#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QByteArray>
#include <QCloseEvent>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QKeySequence>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include "clickhle/core/emulator.h"
#include "clickhle/hle/modules.h"

namespace {

using clickhle::AddressSpace;
using clickhle::Button;
using clickhle::Emulator;
using clickhle::GuestAddr;
using clickhle::u8;
using clickhle::u16;
using clickhle::u32;
using clickhle::u64;

QString hex(u64 value, int digits = 8) {
    return QStringLiteral("0x%1").arg(value, digits, 16, QLatin1Char('0'));
}

QString decode_arm(u32 instruction, GuestAddr address) {
    const unsigned condition = instruction >> 28;
    const QString cond = condition == 14 ? QString() : QStringLiteral("cond%1 ").arg(condition);
    if ((instruction & 0x0ffffff0u) == 0x012fff10u) {
        return cond + QStringLiteral("bx r%1").arg(instruction & 0xfu);
    }
    if ((instruction & 0x0e000000u) == 0x0a000000u) {
        const bool link = (instruction & 0x01000000u) != 0;
        std::int32_t offset = static_cast<std::int32_t>(instruction & 0x00ffffffu);
        if ((offset & 0x00800000) != 0) offset |= static_cast<std::int32_t>(0xff000000u);
        const auto target = static_cast<u32>(address + 8u + static_cast<u32>(offset * 4));
        return cond + (link ? QStringLiteral("bl ") : QStringLiteral("b ")) + hex(target);
    }
    if ((instruction & 0x0c000000u) == 0x04000000u) {
        const bool load = (instruction & (1u << 20)) != 0;
        const unsigned rd = (instruction >> 12) & 0xfu;
        const unsigned rn = (instruction >> 16) & 0xfu;
        return cond + (load ? QStringLiteral("ldr r%1, [r%2]")
                             : QStringLiteral("str r%1, [r%2]")).arg(rd).arg(rn);
    }
    if ((instruction & 0x0c000000u) == 0) {
        const unsigned op = (instruction >> 21) & 0xfu;
        static const std::array<const char*, 16> names{
            "and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc",
            "tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn"};
        const unsigned rd = (instruction >> 12) & 0xfu;
        const unsigned rn = (instruction >> 16) & 0xfu;
        return cond + QString::fromLatin1(names[op]) + QStringLiteral(" r%1, r%2").arg(rd).arg(rn);
    }
    return QStringLiteral(".word %1").arg(hex(instruction));
}

class GameCanvas final : public QWidget {
public:
    explicit GameCanvas(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(480, 360);
        setFocusPolicy(Qt::StrongFocus);
        setAutoFillBackground(false);
    }

    void setFrame(QImage frame) {
        frame_ = std::move(frame);
        update();
    }

    [[nodiscard]] const QImage& frame() const noexcept { return frame_; }

    void setIntegerScaling(bool enabled) {
        integer_scaling_ = enabled;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(8, 10, 13));
        if (frame_.isNull()) {
            painter.setPen(QColor(145, 153, 165));
            painter.drawText(rect(), Qt::AlignCenter,
                             tr("Open a click-wheel game executable to begin"));
            return;
        }
        QSize target = frame_.size();
        const QSize available = size();
        const qreal scale = std::min(qreal(available.width()) / target.width(),
                                     qreal(available.height()) / target.height());
        qreal applied = scale;
        if (integer_scaling_ && scale >= 1.0) {
            applied = std::max<qreal>(1.0, std::floor(scale));
        }
        target = QSize(qRound(frame_.width() * applied),
                       qRound(frame_.height() * applied));
        const QRect destination((width() - target.width()) / 2,
                                (height() - target.height()) / 2,
                                target.width(), target.height());
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(destination, frame_);
    }

private:
    QImage frame_;
    bool integer_scaling_ = true;
};

class MainWindow final : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle(tr("ClickHLE"));
        resize(920, 700);
        canvas_ = new GameCanvas(this);
        setCentralWidget(canvas_);
        createActions();
        createMenus();
        createTools();
        statusBar()->showMessage(tr("Ready — open a game executable"));

        frame_timer_ = new QTimer(this);
        frame_timer_->setInterval(16);
        connect(frame_timer_, &QTimer::timeout, this, [this] { advanceFrame(); });
        fps_timer_.start();

        QSettings settings;
        restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
        recent_games_ = settings.value(QStringLiteral("games/recent")).toStringList();
        rebuildRecentMenu();
        updateActions();
    }

    ~MainWindow() override {
        frame_timer_->stop();
        emulator_.reset();
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (!event->isAutoRepeat() && emulator_) {
            if (const auto button = mapButton(event->key())) {
                emulator_->display().inject(*button, true);
                event->accept();
                return;
            }
        }
        QMainWindow::keyPressEvent(event);
    }

    void keyReleaseEvent(QKeyEvent* event) override {
        if (!event->isAutoRepeat() && emulator_) {
            if (const auto button = mapButton(event->key())) {
                emulator_->display().inject(*button, false);
                event->accept();
                return;
            }
        }
        QMainWindow::keyReleaseEvent(event);
    }

    void closeEvent(QCloseEvent* event) override {
        QSettings settings;
        settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
        settings.setValue(QStringLiteral("games/recent"), recent_games_);
        QMainWindow::closeEvent(event);
    }

private:
    static std::optional<Button> mapButton(int key) {
        switch (key) {
            case Qt::Key_Up: case Qt::Key_S: return Button::ActionWest;
            case Qt::Key_Down: case Qt::Key_X: return Button::ActionEast;
            case Qt::Key_Left: return Button::JoypadWest;
            case Qt::Key_Right: return Button::JoypadEast;
            case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Space:
            case Qt::Key_Z: case Qt::Key_A: return Button::ActionNorth;
            case Qt::Key_M: return Button::JoypadNorth;
            case Qt::Key_P: return Button::JoypadSouth;
            default: return std::nullopt;
        }
    }

    void createActions() {
        open_action_ = new QAction(tr("Load Game…"), this);
        open_action_->setShortcut(QKeySequence::Open);
        connect(open_action_, &QAction::triggered, this, [this] { openGame(); });

        close_action_ = new QAction(tr("Close Game"), this);
        close_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W));
        connect(close_action_, &QAction::triggered, this, [this] { closeGame(); });

        screenshot_action_ = new QAction(tr("Save Screenshot…"), this);
        screenshot_action_->setShortcut(QKeySequence(Qt::Key_F12));
        connect(screenshot_action_, &QAction::triggered, this, [this] { saveScreenshot(); });

        quit_action_ = new QAction(tr("Quit"), this);
        quit_action_->setShortcut(QKeySequence::Quit);
        connect(quit_action_, &QAction::triggered, this, &QWidget::close);

        pause_action_ = new QAction(tr("Pause"), this);
        pause_action_->setShortcut(QKeySequence(Qt::Key_F5));
        connect(pause_action_, &QAction::triggered, this, [this] { pauseGame(); });

        resume_action_ = new QAction(tr("Resume"), this);
        resume_action_->setShortcut(QKeySequence(Qt::Key_F6));
        connect(resume_action_, &QAction::triggered, this, [this] { resumeGame(); });

        restart_action_ = new QAction(tr("Restart Game"), this);
        restart_action_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
        connect(restart_action_, &QAction::triggered, this, [this] { restartGame(); });

        integer_scale_action_ = new QAction(tr("Integer Scaling"), this);
        integer_scale_action_->setCheckable(true);
        integer_scale_action_->setChecked(true);
        connect(integer_scale_action_, &QAction::toggled, canvas_,
                [this](bool checked) { canvas_->setIntegerScaling(checked); });

        about_action_ = new QAction(tr("About ClickHLE"), this);
        connect(about_action_, &QAction::triggered, this, [this] {
            QMessageBox::about(this, tr("About ClickHLE"),
                tr("ClickHLE is an emulator for iPod click-wheel games.\n\n"
                   "CPU: Unicorn Engine\n"
                   "Keys: arrows and Enter/Space for click-wheel controls."));
        });
    }

    void createMenus() {
        QMenuBar* bar = menuBar();
        bar->setNativeMenuBar(true);

        QMenu* file = bar->addMenu(tr("File"));
        file->addAction(open_action_);
        recent_menu_ = file->addMenu(tr("Recent Games"));
        file->addAction(close_action_);
        file->addSeparator();
        file->addAction(screenshot_action_);
        file->addSeparator();
        file->addAction(quit_action_);

        QMenu* options = bar->addMenu(tr("Options"));
        options->addAction(integer_scale_action_);

        QMenu* emulation = bar->addMenu(tr("Emulation"));
        emulation->addAction(pause_action_);
        emulation->addAction(resume_action_);
        emulation->addSeparator();
        emulation->addAction(restart_action_);

        tools_menu_ = bar->addMenu(tr("Tools"));
        debug_menu_ = bar->addMenu(tr("Debug"));

        QMenu* help = bar->addMenu(tr("Help"));
        help->addAction(about_action_);
    }

    QDockWidget* makeTool(const QString& title, QWidget* content) {
        auto* dock = new QDockWidget(title, this);
        dock->setObjectName(title);
        dock->setFeatures(QDockWidget::DockWidgetClosable |
                          QDockWidget::DockWidgetFloatable);
        dock->setAllowedAreas(Qt::NoDockWidgetArea);
        dock->setWidget(content);
        addDockWidget(Qt::RightDockWidgetArea, dock);
        dock->setFloating(true);
        dock->hide();
        tools_menu_->addAction(dock->toggleViewAction());
        debug_menu_->addAction(dock->toggleViewAction());
        return dock;
    }

    void createTools() {
        cpu_view_ = new QPlainTextEdit;
        cpu_view_->setReadOnly(true);
        cpu_view_->setMinimumSize(320, 280);
        makeTool(tr("CPU Registers & Disassembly"), cpu_view_);

        auto* memory_panel = new QWidget;
        auto* memory_layout = new QVBoxLayout(memory_panel);
        auto* address_row = new QWidget(memory_panel);
        auto* address_layout = new QHBoxLayout(address_row);
        address_layout->setContentsMargins(0, 0, 0, 0);
        memory_address_ = new QLineEdit(QStringLiteral("0x18000000"), address_row);
        memory_address_->setPlaceholderText(tr("Guest address"));
        address_layout->addWidget(memory_address_);
        auto* refresh_button = new QPushButton(tr("Go"), address_row);
        address_layout->addWidget(refresh_button);
        memory_layout->addWidget(address_row);
        memory_view_ = new QPlainTextEdit(memory_panel);
        memory_view_->setReadOnly(true);
        memory_view_->setLineWrapMode(QPlainTextEdit::NoWrap);
        memory_layout->addWidget(memory_view_);
        connect(refresh_button, &QPushButton::clicked, this,
                [this] { refreshMemory(); });
        connect(memory_address_, &QLineEdit::returnPressed, this,
                [this] { refreshMemory(); });
        makeTool(tr("Memory Viewer"), memory_panel);

        hle_view_ = new QPlainTextEdit;
        hle_view_->setReadOnly(true);
        hle_view_->setMinimumSize(400, 300);
        makeTool(tr("HLE Activity"), hle_view_);

        map_view_ = new QPlainTextEdit;
        map_view_->setReadOnly(true);
        map_view_->setMinimumSize(360, 280);
        makeTool(tr("Memory Map"), map_view_);

        log_view_ = new QPlainTextEdit;
        log_view_->setReadOnly(true);
        log_view_->setMinimumSize(360, 180);
        makeTool(tr("Frontend Log"), log_view_);
    }

    void openGame() {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Load ClickHLE Game"), {},
            tr("iPod Game Executables (*.bin *.exe);;All Files (*)"));
        if (!path.isEmpty()) loadGame(path);
    }

    void loadGame(const QString& path) {
        frame_timer_->stop();
        emulator_.reset();
        current_game_.clear();
        auto next = std::make_unique<Emulator>();
        next->display().set_headless(true);
        Emulator::Config config;
        const QByteArray encoded_path = path.toUtf8();
        const auto* path_bytes =
            reinterpret_cast<const char8_t*>(encoded_path.constData());
        config.executable = std::filesystem::path(std::u8string(
            path_bytes, static_cast<std::size_t>(encoded_path.size())));
        std::string error;
        if (!next->load(std::move(config), &error)) {
            appendLog(tr("Load failed: %1").arg(QString::fromStdString(error)));
            QMessageBox::critical(this, tr("Could Not Load Game"),
                                  QString::fromStdString(error));
            updateActions();
            return;
        }
        emulator_ = std::move(next);
        current_game_ = path;
        addRecent(path);
        canvas_->setFrame({});
        appendLog(tr("Loaded %1").arg(QFileInfo(path).fileName()));
        updateActions();
        resumeGame();
        canvas_->setFocus();
    }

    void closeGame() {
        frame_timer_->stop();
        emulator_.reset();
        current_game_.clear();
        canvas_->setFrame({});
        statusBar()->showMessage(tr("Game closed"));
        updateActions();
        refreshTools();
    }

    void restartGame() {
        if (!current_game_.isEmpty()) loadGame(current_game_);
    }

    void pauseGame() {
        if (!emulator_) return;
        frame_timer_->stop();
        paused_ = true;
        statusBar()->showMessage(tr("Paused"));
        updateActions();
    }

    void resumeGame() {
        if (!emulator_) return;
        paused_ = false;
        frame_timer_->start();
        statusBar()->showMessage(tr("Running — F5 pauses, F6 resumes"));
        updateActions();
    }

    void advanceFrame() {
        if (!emulator_) return;
        constexpr u64 kGuiInstructionBudget = 10'000'000;
        const clickhle::RunResult result =
            emulator_->run_game_frame(kGuiInstructionBudget);
        int width = 0;
        int height = 0;
        std::vector<u8> pixels = emulator_->display().read_rgba(width, height);
        if (!pixels.empty() && width > 0 && height > 0) {
            // The emulated LCD is an opaque surface. The GL front buffer's
            // alpha may be unset or affected by guest blending; letting the
            // host window manager composite that alpha makes transparent game
            // texels turn into black holes in the captured frame.
            for (std::size_t alpha = 3; alpha < pixels.size(); alpha += 4) {
                pixels[alpha] = 255;
            }
            QImage image(pixels.data(), width, height, width * 4,
                         QImage::Format_RGBA8888);
            canvas_->setFrame(image.mirrored(false, true).copy());
        }
        refreshTools();
        u64 frames = 0;
        u64 draws = 0;
        clickhle::gles_statistics(frames, draws);
        (void)frames;
        ++fps_window_frames_;
        const qint64 fps_elapsed_ms = fps_timer_.elapsed();
        if (fps_elapsed_ms >= 500) {
            fps_ = static_cast<double>(fps_window_frames_) * 1000.0 /
                   static_cast<double>(fps_elapsed_ms);
            fps_window_frames_ = 0;
            fps_timer_.restart();
        }
        statusBar()->showMessage(
            tr("%1  |  %2 FPS  |  %3 draws  |  %4 HLE calls")
                .arg(QFileInfo(current_game_).fileName())
                .arg(fps_, 0, 'f', 1).arg(draws)
                .arg(emulator_->hle_call_count()));
        if (result.status == clickhle::RunStatus::Fault ||
            result.status == clickhle::RunStatus::Unsupported ||
            !emulator_->last_missing_import().empty()) {
            frame_timer_->stop();
            paused_ = true;
            const QString message = result.fault.empty()
                ? QString::fromStdString(emulator_->last_missing_import())
                : QString::fromStdString(result.fault);
            appendLog(tr("Stopped: %1").arg(message));
            updateActions();
        }
        updateActions();
    }

    void refreshTools() {
        if (!emulator_) {
            cpu_view_->setPlainText(tr("No game loaded"));
            hle_view_->setPlainText(tr("No game loaded"));
            map_view_->setPlainText(tr("No game loaded"));
            memory_view_->setPlainText(tr("No game loaded"));
            return;
        }
        if (const auto* cpu = emulator_->cpu()) {
            const auto context = cpu->save_context();
            QString text;
            for (unsigned i = 0; i < 16; ++i) {
                text += QStringLiteral("r%1  %2%3")
                    .arg(i, 2, 10, QLatin1Char('0'))
                    .arg(hex(context.r[i]))
                    .arg(i % 2 == 1 ? QLatin1Char('\n') : QLatin1Char(' '));
            }
            text += tr("\nCPSR  %1   mode %2   %3\nInstructions  %4\n")
                .arg(hex(context.cpsr)).arg(context.cpsr & 0x1fu)
                .arg(context.thumb() ? tr("Thumb") : tr("ARM"))
                .arg(cpu->instruction_count());
            text += tr("\nDisassembly at PC\n");
            const u32 step = context.thumb() ? 2u : 4u;
            const GuestAddr start = context.r[clickhle::reg::kPc] - step * 6u;
            for (u32 i = 0; i < 16; ++i) {
                const GuestAddr address = start + step * i;
                const auto word = emulator_->memory().read<u32>(address);
                const auto half = emulator_->memory().read<u16>(address);
                text += QStringLiteral("%1  %2  %3%4")
                    .arg(hex(address))
                    .arg(context.thumb() ? hex(half.value_or(0), 4)
                                         : hex(word.value_or(0)))
                    .arg(context.thumb() ? tr("Thumb instruction")
                                         : decode_arm(word.value_or(0), address))
                    .arg(address == context.r[clickhle::reg::kPc]
                             ? QStringLiteral("  < PC") : QString());
                text += QLatin1Char('\n');
            }
            cpu_view_->setPlainText(text);
        }

        const auto coverage = emulator_->coverage();
        QString hle = tr("HLE coverage: %1 / %2 imports\nCalls serviced: %3\n")
                          .arg(coverage.implemented).arg(coverage.total)
                          .arg(emulator_->hle_call_count());
        if (!emulator_->last_missing_import().empty()) {
            hle += tr("\nStopped at: %1\n")
                       .arg(QString::fromStdString(emulator_->last_missing_import()));
        }
        hle += tr("\nRecent calls\n");
        for (const std::string& call : emulator_->recent_hle_calls()) {
            hle += QString::fromStdString(call) + QLatin1Char('\n');
        }
        hle_view_->setPlainText(hle);

        QString map;
        for (const auto& region : emulator_->memory().regions()) {
            if (!region) continue;
            QString flags;
            if ((region->prot & AddressSpace::kRead) != 0) flags += QLatin1Char('R');
            if ((region->prot & AddressSpace::kWrite) != 0) flags += QLatin1Char('W');
            if ((region->prot & AddressSpace::kExec) != 0) flags += QLatin1Char('X');
            map += QStringLiteral("%1  +%2  %3  %4%5\n")
                .arg(hex(region->base))
                .arg(hex(region->size))
                .arg(flags.leftJustified(3))
                .arg(QString::fromStdString(region->name))
                .arg(region->is_mirror
                         ? tr(" → %1").arg(hex(region->mirror_source)) : QString());
        }
        map_view_->setPlainText(map);
    }

    void refreshMemory() {
        if (!emulator_) return;
        QString value = memory_address_->text().trimmed();
        if (!value.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
            value.prepend(QStringLiteral("0x"));
        bool ok = false;
        const qulonglong parsed = value.toULongLong(&ok, 0);
        if (!ok || parsed > 0xffffffffull) {
            memory_view_->setPlainText(tr("Enter a valid 32-bit guest address."));
            return;
        }
        const GuestAddr start = static_cast<GuestAddr>(parsed);
        constexpr u32 count = 256;
        std::array<u8, count> bytes{};
        QString text;
        for (u32 row = 0; row < count; row += 16) {
            text += hex(start + row) + QStringLiteral("  ");
            bool readable = emulator_->memory().read_bytes(
                start + row, bytes.data() + row, 16);
            QString ascii;
            for (u32 column = 0; column < 16; ++column) {
                const u8 byte = readable ? bytes[row + column] : 0;
                text += readable ? QStringLiteral("%1 ").arg(byte, 2, 16,
                            QLatin1Char('0')) : QStringLiteral("-- ");
                ascii += readable && byte >= 32 && byte < 127
                    ? QChar(static_cast<ushort>(byte)) : QLatin1Char('.');
            }
            text += QStringLiteral(" |%1|\n").arg(ascii);
        }
        memory_view_->setPlainText(text);
    }

    void saveScreenshot() {
        if (!canvas_ || canvas_->frame().isNull()) return;
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Save Screenshot"), tr("clickhle.png"),
            tr("PNG Image (*.png);;BMP Image (*.bmp)"));
        if (!path.isEmpty() && !canvas_->frame().save(path))
            QMessageBox::warning(this, tr("Screenshot Failed"),
                                 tr("Could not write the image file."));
    }

    void appendLog(const QString& message) {
        if (log_view_) {
            log_view_->appendPlainText(message);
            log_view_->verticalScrollBar()->setValue(log_view_->verticalScrollBar()->maximum());
        }
    }

    void addRecent(const QString& path) {
        recent_games_.removeAll(path);
        recent_games_.prepend(path);
        while (recent_games_.size() > 8) recent_games_.removeLast();
        rebuildRecentMenu();
    }

    void rebuildRecentMenu() {
        if (!recent_menu_) return;
        recent_menu_->clear();
        for (const QString& path : recent_games_) {
            QAction* action = recent_menu_->addAction(QFileInfo(path).fileName());
            action->setToolTip(path);
            connect(action, &QAction::triggered, this,
                    [this, path] { loadGame(path); });
        }
        if (recent_games_.isEmpty()) {
            QAction* empty = recent_menu_->addAction(tr("No Recent Games"));
            empty->setEnabled(false);
        }
    }

    void updateActions() {
        const bool loaded = static_cast<bool>(emulator_);
        open_action_->setEnabled(true);
        close_action_->setEnabled(loaded);
        screenshot_action_->setEnabled(loaded && !canvas_->frame().isNull());
        pause_action_->setEnabled(loaded && !paused_);
        resume_action_->setEnabled(loaded && paused_);
        restart_action_->setEnabled(loaded);
    }

    std::unique_ptr<Emulator> emulator_;
    QString current_game_;
    QStringList recent_games_;
    bool paused_ = true;
    GameCanvas* canvas_ = nullptr;
    QTimer* frame_timer_ = nullptr;
    QElapsedTimer fps_timer_;
    u64 fps_window_frames_ = 0;
    double fps_ = 0.0;
    QMenu* recent_menu_ = nullptr;
    QMenu* tools_menu_ = nullptr;
    QMenu* debug_menu_ = nullptr;
    QAction* open_action_ = nullptr;
    QAction* close_action_ = nullptr;
    QAction* screenshot_action_ = nullptr;
    QAction* quit_action_ = nullptr;
    QAction* pause_action_ = nullptr;
    QAction* resume_action_ = nullptr;
    QAction* restart_action_ = nullptr;
    QAction* integer_scale_action_ = nullptr;
    QAction* about_action_ = nullptr;
    QPlainTextEdit* cpu_view_ = nullptr;
    QPlainTextEdit* memory_view_ = nullptr;
    QPlainTextEdit* hle_view_ = nullptr;
    QPlainTextEdit* map_view_ = nullptr;
    QPlainTextEdit* log_view_ = nullptr;
    QLineEdit* memory_address_ = nullptr;
};

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ClickHLE"));
    QCoreApplication::setApplicationName(QStringLiteral("ClickHLE"));
    MainWindow window;
    window.show();
    return application.exec();
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <QAction>
#include <QActionGroup>
#include <QAbstractItemView>
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QThread>
#include <QScrollBar>
#include <QSettings>
#include <QStackedWidget>
#include <QSpinBox>
#include <QStatusBar>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include "clickhle/common/log.h"
#include "clickhle/core/emulator.h"
#include "clickhle/hle/modules.h"
#include "clickhle/loader/game_manifest.h"
#if CLICKHLE_HAVE_INSTALLER
#include "clickhle/installer/package.h"
#endif

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
        if (!frame_.isNull()) message_.clear();
        update();
    }

    void setMessage(QString message) {
        message_ = std::move(message);
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
            painter.drawText(rect().adjusted(24, 24, -24, -24),
                             Qt::AlignCenter | Qt::TextWordWrap,
                             message_.isEmpty()
                                 ? tr("Open a click-wheel game executable to begin")
                                 : message_);
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
    QString message_;
    bool integer_scaling_ = true;
};

class ClickWheelWidget final : public QWidget {
public:
    std::function<void(u8, bool)> button;
    std::function<void(u8)> wheel;
    std::function<void()> release_wheel;

    explicit ClickWheelWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(190, 190);
        setMaximumSize(280, 280);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPointF center = rect().center();
        const qreal radius = std::min(width(), height()) * 0.46;
        p.setPen(QPen(QColor(80, 88, 100), 2));
        p.setBrush(QColor(214, 217, 222));
        p.drawEllipse(center, radius, radius);
        p.setBrush(QColor(60, 66, 77));
        p.drawEllipse(center, radius * 0.36, radius * 0.36);
        p.setPen(Qt::white);
        p.drawText(QRectF(center.x() - 30, center.y() - 12, 60, 24),
                   Qt::AlignCenter, tr("Select"));
        p.setPen(QColor(25, 30, 38));
        for (const auto& item : std::array<std::pair<const char*, QPointF>, 4>{
                 {{"Menu", {0, -0.71}}, {">|", {0.72, 0}},
                  {">||", {0, 0.71}}, {"|<", {-0.72, 0}}}}) {
            const QPointF pos = center + QPointF(item.second.x() * radius,
                                                 item.second.y() * radius);
            p.drawText(QRectF(pos.x() - 30, pos.y() - 12, 60, 24),
                       Qt::AlignCenter, QString::fromLatin1(item.first));
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) return;
        press_point_ = event->position();
        const QPointF delta = event->position() - rect().center();
        const qreal radius = std::min(width(), height()) * 0.46;
        const qreal distance = std::hypot(delta.x(), delta.y());
        if (distance < radius * 0.36) {
            held_bit_ = 0;
        } else if (distance < radius) {
            if (std::abs(delta.x()) > std::abs(delta.y()))
                held_bit_ = delta.x() > 0 ? 1 : 2;
            else
                held_bit_ = delta.y() > 0 ? 3 : 4;
        } else {
            return;
        }
        if (button) button(*held_bit_, true);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!(event->buttons() & Qt::LeftButton) || !wheel) return;
        if (held_bit_ &&
            std::hypot(event->position().x() - press_point_.x(),
                       event->position().y() - press_point_.y()) > 8) {
            if (button) button(*held_bit_, false);
            held_bit_.reset();
        }
        const QPointF delta = event->position() - rect().center();
        const qreal radius = std::min(width(), height()) * 0.46;
        const qreal distance = std::hypot(delta.x(), delta.y());
        if (distance < radius * 0.40 || distance > radius * 1.15) return;
        qreal angle = std::atan2(delta.x(), -delta.y());
        if (angle < 0) angle += 2 * 3.14159265358979323846;
        wheel(static_cast<u8>(std::floor(angle * 96 /
                   (2 * 3.14159265358979323846))) % 96);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) return;
        if (held_bit_ && button) button(*held_bit_, false);
        held_bit_.reset();
        if (release_wheel) release_wheel();
        event->accept();
    }

private:
    std::optional<u8> held_bit_;
    QPointF press_point_;
};

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QString startup_game = {}) {
        setWindowTitle(tr("ClickHLE"));
        resize(920, 700);
        canvas_ = new GameCanvas(this);
        central_stack_ = new QStackedWidget(this);
        createGameLibrary();
        central_stack_->addWidget(canvas_);
        setCentralWidget(central_stack_);
        createActions();
        createMenus();
        auto* toolbar = addToolBar(tr("Main"));
        toolbar->setMovable(false);
        toolbar->addAction(library_action_);
        toolbar->addAction(install_action_);
        toolbar->addAction(open_action_);
        toolbar->addSeparator();
        toolbar->addAction(pause_action_);
        toolbar->addAction(resume_action_);
        toolbar->addAction(restart_action_);
        createTools();
        auto* wheel_dock = new QDockWidget(tr("Click Wheel"), this);
        wheel_dock->setObjectName(QStringLiteral("clickWheelDock"));
        wheel_dock->setFeatures(QDockWidget::DockWidgetMovable |
                                QDockWidget::DockWidgetFloatable);
        auto* wheel = new ClickWheelWidget(wheel_dock);
        wheel->button = [this](u8 bit, bool down) {
            if (!emulator_) return;
            emulator_->set_hardware_button(bit, down);
            clickhle::poll_hardware_input(*emulator_);
        };
        wheel->wheel = [this](u8 raw) {
            if (!emulator_) return;
            emulator_->set_hardware_wheel(raw);
            clickhle::poll_hardware_input(*emulator_);
        };
        wheel->release_wheel = [this] {
            if (!emulator_) return;
            emulator_->release_hardware_wheel();
            clickhle::poll_hardware_input(*emulator_);
        };
        wheel_dock->setWidget(wheel);
        addDockWidget(Qt::RightDockWidgetArea, wheel_dock);
        tools_menu_->addAction(wheel_dock->toggleViewAction());
        refreshTools();
        statusBar()->showMessage(tr("Select an installed game"));

        frame_timer_ = new QTimer(this);
        frame_timer_->setInterval(16);
        connect(frame_timer_, &QTimer::timeout, this, [this] { advanceFrame(); });
        fps_timer_.start();

        QSettings settings;
        device_settings_.language = settings.value(QStringLiteral("device/language"), 0).toUInt();
        device_settings_.time_format_24 = settings.value(QStringLiteral("device/timeFormat24"), false).toBool();
        device_settings_.hold_switch = settings.value(QStringLiteral("device/holdSwitch"), false).toBool();
        restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
        if (settings.value(QStringLiteral("video/engine")).toString() == QStringLiteral("interpreter"))
            interpreter_action_->setChecked(true);
        recent_games_ = settings.value(QStringLiteral("games/recent")).toStringList();
        rebuildRecentMenu();
        updateActions();
        if (!startup_game.isEmpty()) {
            QTimer::singleShot(0, this, [this, startup_game = std::move(startup_game)] {
                loadGame(startup_game);
            });
        }
    }

    ~MainWindow() override {
        frame_timer_->stop();
        emulator_.reset();
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (emulator_) {
            if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) {
                emulator_->move_hardware_wheel(event->key() == Qt::Key_Up ? -3 : 3);
                clickhle::poll_hardware_input(*emulator_);
                event->accept();
                return;
            }
            if (!event->isAutoRepeat()) {
                if (const auto bit = keyHardwareBit(event->key())) {
                    emulator_->set_hardware_button(*bit, true);
                    clickhle::poll_hardware_input(*emulator_);
                    event->accept();
                    return;
                }
            }
        }
        QMainWindow::keyPressEvent(event);
    }

    void keyReleaseEvent(QKeyEvent* event) override {
        if (!event->isAutoRepeat() && emulator_) {
            if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) {
                emulator_->release_hardware_wheel();
                clickhle::poll_hardware_input(*emulator_);
                event->accept();
                return;
            }
            if (const auto bit = keyHardwareBit(event->key())) {
                emulator_->set_hardware_button(*bit, false);
                clickhle::poll_hardware_input(*emulator_);
                event->accept();
                return;
            }
        }
        QMainWindow::keyReleaseEvent(event);
    }

    void closeEvent(QCloseEvent* event) override {
        if (install_thread_) {
            event->ignore();
            statusBar()->showMessage(tr("Wait for installation to finish"), 8000);
            return;
        }
        QSettings settings;
        settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
        settings.setValue(QStringLiteral("games/recent"), recent_games_);
        settings.setValue(QStringLiteral("video/engine"),
                          jit_action_->isChecked() ? QStringLiteral("jit")
                                                   : QStringLiteral("interpreter"));
        QMainWindow::closeEvent(event);
    }

private:
    void createGameLibrary() {
        auto* page = new QWidget(this);
        auto* layout = new QVBoxLayout(page);
        auto* heading = new QLabel(tr("Installed Games"), page);
        QFont font = heading->font();
        font.setPointSize(font.pointSize() + 5);
        font.setBold(true);
        heading->setFont(font);
        layout->addWidget(heading);

        auto* controls = new QHBoxLayout;
        controls->addWidget(new QLabel(tr("iPod platform:"), page));
        platform_selector_ = new QComboBox(page);
        controls->addWidget(platform_selector_);
        controls->addStretch();
        library_search_ = new QLineEdit(page);
        library_search_->setPlaceholderText(tr("Search name or game ID"));
        controls->addWidget(library_search_);
        auto* refresh = new QPushButton(tr("Refresh"), page);
        controls->addWidget(refresh);
        layout->addLayout(controls);

        library_table_ = new QTableWidget(page);
        library_table_->setColumnCount(6);
        library_table_->setHorizontalHeaderLabels(
            {tr("Game"), tr("Version"), tr("Game ID"), tr("Build ID"),
             tr("EXE size"), tr("Platform version")});
        auto* header = library_table_->horizontalHeader();
        header->setSectionResizeMode(QHeaderView::ResizeToContents);
        header->setSectionResizeMode(0, QHeaderView::Stretch);
        library_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        library_table_->setSelectionMode(QAbstractItemView::SingleSelection);
        library_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        layout->addWidget(library_table_);
        library_message_ = new QLabel(page);
        layout->addWidget(library_message_);
        central_stack_->addWidget(page);

        connect(refresh, &QPushButton::clicked, this, [this] { refreshGameLibrary(); });
        connect(platform_selector_, &QComboBox::currentIndexChanged, this,
                [this] { populateGameLibrary(); });
        connect(library_search_, &QLineEdit::textChanged, this,
                [this] { populateGameLibrary(); });
        connect(library_table_, &QTableWidget::cellDoubleClicked, this,
                [this](int row, int) { launchInstalledGame(row); });
        refreshGameLibrary();
    }

    void refreshGameLibrary() {
        const auto app = std::filesystem::path(
            QCoreApplication::applicationDirPath().toStdString());
        hdd_root_ = app / "hdd";
        installed_games_ = clickhle::scan_installed_games(hdd_root_);
        const u32 previous = platform_selector_->currentData().toUInt();
        platform_selector_->blockSignals(true);
        platform_selector_->clear();
        std::vector<u32> ids;
        for (const auto& game : installed_games_)
            for (const auto& variant : game.variants)
                if (std::find(ids.begin(), ids.end(), variant.platform_id) == ids.end())
                    ids.push_back(variant.platform_id);
        std::sort(ids.begin(), ids.end());
        for (u32 id : ids)
            platform_selector_->addItem(tr("Platform %1").arg(id), id);
        const int selected = platform_selector_->findData(previous ? previous : 1);
        if (selected >= 0) platform_selector_->setCurrentIndex(selected);
        platform_selector_->blockSignals(false);
        populateGameLibrary();
    }

    void populateGameLibrary() {
        library_table_->setRowCount(0);
        library_rows_.clear();
        const u32 platform = platform_selector_->currentData().toUInt();
        for (const auto& game : installed_games_) {
            const QString name = QString::fromStdString(game.name);
            const QString id = QString::fromStdString(game.guid);
            const QString query = library_search_->text();
            if (!name.contains(query, Qt::CaseInsensitive) &&
                !id.contains(query, Qt::CaseInsensitive))
                continue;
            const auto variant = std::find_if(game.variants.begin(), game.variants.end(),
                [platform](const auto& item) { return item.platform_id == platform; });
            if (variant == game.variants.end()) continue;
            const int row = library_table_->rowCount();
            library_table_->insertRow(row);
            const QString details = tr("Build: %1\nExecutable: %2")
                .arg(QString::fromStdString(game.build_identifier),
                     QString::fromStdString(
                         variant->executable.lexically_relative(game.directory).generic_string()));
            const QStringList columns{
                name,
                QString::fromStdString(game.version),
                id,
                variant->build_id ? QString::number(variant->build_id) : QStringLiteral("—"),
                variant->size ? QLocale().formattedDataSize(static_cast<qint64>(variant->size))
                              : QStringLiteral("—"),
                QString::number(variant->platform_version)};
            for (int column = 0; column < columns.size(); ++column) {
                auto* item = new QTableWidgetItem(columns[column]);
                item->setToolTip(details);
                library_table_->setItem(row, column, item);
            }
            library_rows_.push_back(*variant);
        }
        library_message_->setText(library_rows_.empty()
            ? tr("No installed games found in the hdd folder.")
            : tr("%1 games available. Double-click a game to start it.")
                  .arg(static_cast<int>(library_rows_.size())));
    }

    void launchInstalledGame(int row) {
        if (row < 0 || static_cast<std::size_t>(row) >= library_rows_.size()) return;
        const auto& variant = library_rows_[static_cast<std::size_t>(row)];
        loadGame(QString::fromStdString(variant.executable.string()), variant.platform_id);
    }

    void showGameLibrary() {
        central_stack_->setCurrentIndex(0);
        statusBar()->showMessage(tr("Select an installed game"));
    }

    void installGame() {
        if (install_thread_) return;
#if !CLICKHLE_HAVE_INSTALLER
        QMessageBox::critical(this, tr("Installer Unavailable"),
            tr("Rebuild ClickHLE with libzip and OpenSSL to install packages."));
        return;
#else
        const QString package = QFileDialog::getOpenFileName(
            this, tr("Install iPod Game"), {}, tr("iPod Games (*.ipg *.zip);;All Files (*)"));
        if (package.isEmpty()) return;

        const auto app = std::filesystem::path(
            QCoreApplication::applicationDirPath().toStdString());
        const auto keys = app / "keys";
        const QByteArray encoded_path = package.toUtf8();
        const auto* path_bytes =
            reinterpret_cast<const char8_t*>(encoded_path.constData());
        const auto package_path = std::filesystem::path(std::u8string(
            path_bytes, static_cast<std::size_t>(encoded_path.size())));
        const auto hdd = hdd_root_;
        auto error = std::make_shared<std::string>();
        install_thread_ = QThread::create([package_path, hdd, keys, error] {
            try {
                clickhle::install_game_package(package_path, hdd, keys);
            } catch (const std::exception& failure) {
                *error = failure.what();
            }
        });
        connect(install_thread_, &QThread::finished, this, [this, error] {
            install_thread_->deleteLater();
            install_thread_ = nullptr;
            install_action_->setEnabled(true);
            if (error->empty()) {
                refreshGameLibrary();
                showGameLibrary();
                statusBar()->showMessage(tr("Game installed"), 8000);
                appendLog(tr("Installed %1").arg(package_name_));
                QMessageBox::information(this, tr("Game Installed"),
                                         tr("The game is ready in Installed Games."));
            } else {
                statusBar()->showMessage(tr("Installation failed"), 10000);
                QMessageBox::critical(this, tr("Could Not Install Game"),
                                      QString::fromStdString(*error));
            }
        });
        package_name_ = QFileInfo(package).fileName();
        install_action_->setEnabled(false);
        statusBar()->showMessage(tr("Installing %1…").arg(package_name_));
        install_thread_->start();
#endif
    }

    static std::optional<u8> keyHardwareBit(int key) {
        switch (key) {
            case Qt::Key_Left: return 2;
            case Qt::Key_Right: return 1;
            case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Space:
            case Qt::Key_Z: case Qt::Key_A: return 0;
            case Qt::Key_M: return 4;
            case Qt::Key_P: return 3;
            default: return std::nullopt;
        }
    }

    void createActions() {
        library_action_ = new QAction(tr("Installed Games"), this);
        connect(library_action_, &QAction::triggered, this,
                [this] { showGameLibrary(); });
        install_action_ = new QAction(tr("Install Game Package…"), this);
        connect(install_action_, &QAction::triggered, this,
                [this] { installGame(); });
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

        device_settings_action_ = new QAction(tr("iPod Settings…"), this);
        connect(device_settings_action_, &QAction::triggered, this,
                [this] { showDeviceSettings(); });

        auto* engine_actions = new QActionGroup(this);
        engine_actions->setExclusive(true);
        interpreter_action_ = new QAction(tr("Interpreter"), engine_actions);
        jit_action_ = new QAction(tr("JIT"), engine_actions);
        interpreter_action_->setCheckable(true);
        jit_action_->setCheckable(true);
        const bool jit_available = clickhle::videocore_jit_available();
        interpreter_action_->setChecked(!jit_available);
        jit_action_->setChecked(jit_available);
        jit_action_->setEnabled(jit_available);
        connect(interpreter_action_, &QAction::triggered, this, [this] {
            if (emulator_) emulator_->set_videocore_engine(clickhle::VideoCoreEngine::Interpreter);
        });
        connect(jit_action_, &QAction::triggered, this, [this] {
            if (emulator_) emulator_->set_videocore_engine(clickhle::VideoCoreEngine::Jit);
        });

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
        bar->setNativeMenuBar(false);
        bar->setVisible(true);

        QMenu* file = bar->addMenu(tr("File"));
        file->addAction(library_action_);
        file->addAction(install_action_);
        file->addAction(open_action_);
        recent_menu_ = file->addMenu(tr("Recent Games"));
        file->addAction(close_action_);
        file->addSeparator();
        file->addAction(screenshot_action_);
        file->addSeparator();
        file->addAction(quit_action_);

        QMenu* options = bar->addMenu(tr("Options"));
        options->addAction(device_settings_action_);
        options->addSeparator();
        options->addAction(integer_scale_action_);
        QMenu* video_engine = options->addMenu(tr("VideoCore Engine"));
        video_engine->addAction(interpreter_action_);
        video_engine->addAction(jit_action_);

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

    void showDeviceSettings() {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("iPod Settings"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* form = new QFormLayout;
        auto* language = new QSpinBox(&dialog);
        language->setRange(0, 65535);
        language->setSpecialValueText(tr("English (0)"));
        language->setValue(static_cast<int>(device_settings_.language.value_or(0)));
        language->setToolTip(tr("Language index 0 selects English. Available languages depend on the game."));
        form->addRow(tr("Language index:"), language);
        auto* time_format = new QComboBox(&dialog);
        time_format->addItem(tr("12-hour"));
        time_format->addItem(tr("24-hour"));
        time_format->setCurrentIndex(device_settings_.time_format_24.value_or(false) ? 1 : 0);
        form->addRow(tr("Time format:"), time_format);
        auto* hold = new QCheckBox(tr("Hold switch on"), &dialog);
        hold->setChecked(device_settings_.hold_switch);
        form->addRow(hold);
        layout->addLayout(form);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted) return;

        device_settings_.language = static_cast<u32>(language->value());
        device_settings_.time_format_24 = time_format->currentIndex() == 1;
        device_settings_.hold_switch = hold->isChecked();
        if (emulator_) emulator_->device_settings() = device_settings_;
        QSettings settings;
        settings.setValue(QStringLiteral("device/language"), language->value());
        settings.setValue(QStringLiteral("device/timeFormat24"), *device_settings_.time_format_24);
        settings.setValue(QStringLiteral("device/holdSwitch"), device_settings_.hold_switch);
    }

    QDockWidget* makeTool(const QString& title, QWidget* content) {
        auto* dock = new QDockWidget(title, this);
        dock->setObjectName(title);
        dock->setFeatures(QDockWidget::DockWidgetClosable |
                          QDockWidget::DockWidgetFloatable);
        dock->setAllowedAreas(Qt::NoDockWidgetArea);
        dock->setWidget(content);
        addDockWidget(Qt::RightDockWidgetArea, dock);
        dock->setVisible(false);
        QAction* view_action = dock->toggleViewAction();
        view_action->setChecked(false);
        connect(dock, &QDockWidget::visibilityChanged, this,
                [this, dock](bool visible) {
                    if (!visible) return;
                    if (!dock->isFloating()) dock->setFloating(true);
                    refreshTools();
                    if (dock == memory_dock_) refreshMemory();
                });
        tools_menu_->addAction(view_action);
        debug_menu_->addAction(view_action);
        return dock;
    }

    void createTools() {
        cpu_view_ = new QPlainTextEdit;
        cpu_view_->setReadOnly(true);
        cpu_view_->setMinimumSize(320, 280);
        cpu_dock_ = makeTool(tr("CPU Registers & Disassembly"), cpu_view_);

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
        memory_dock_ = makeTool(tr("Memory Viewer"), memory_panel);

        hle_view_ = new QPlainTextEdit;
        hle_view_->setReadOnly(true);
        hle_view_->setMinimumSize(400, 300);
        hle_dock_ = makeTool(tr("HLE Activity"), hle_view_);

        auto* videocore_panel = new QWidget;
        auto* videocore_layout = new QVBoxLayout(videocore_panel);
        auto* videocore_controls = new QWidget(videocore_panel);
        auto* videocore_control_layout = new QHBoxLayout(videocore_controls);
        videocore_control_layout->setContentsMargins(0, 0, 0, 0);
        videocore_trace_enabled_ = new QCheckBox(tr("Trace render-server calls"));
        videocore_trace_enabled_->setChecked(false);
        videocore_pause_on_call_ = new QCheckBox(tr("Pause after a call"));
        auto* clear_videocore = new QPushButton(tr("Clear"), videocore_controls);
        videocore_control_layout->addWidget(videocore_trace_enabled_);
        videocore_control_layout->addWidget(videocore_pause_on_call_);
        videocore_control_layout->addStretch();
        videocore_control_layout->addWidget(clear_videocore);
        videocore_layout->addWidget(videocore_controls);
        auto* videocore_note = new QLabel(
            tr("Watches ARM calls at the OpenGLES render-server boundary. "
               "Enable tracing only when inspecting a game."),
            videocore_panel);
        videocore_note->setWordWrap(true);
        videocore_layout->addWidget(videocore_note);
        videocore_table_ = new QTableWidget(0, 6, videocore_panel);
        videocore_table_->setHorizontalHeaderLabels(
            {tr("#"), tr("Frame"), tr("API"), tr("Return PC"),
             tr("Arguments"), tr("Result")});
        videocore_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        videocore_table_->setSelectionMode(QAbstractItemView::SingleSelection);
        videocore_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        videocore_table_->horizontalHeader()->setStretchLastSection(true);
        videocore_table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
        videocore_layout->addWidget(videocore_table_, 2);
        videocore_detail_ = new QPlainTextEdit(videocore_panel);
        videocore_detail_->setReadOnly(true);
        videocore_detail_->setMaximumBlockCount(20);
        videocore_detail_->setPlaceholderText(
            tr("Select a call to inspect its arguments and payload."));
        videocore_layout->addWidget(videocore_detail_, 1);
        videocore_dock_ = makeTool(tr("RenderServer / VideoCore Watch"), videocore_panel);
        connect(videocore_trace_enabled_, &QCheckBox::toggled, this,
                [this](bool enabled) {
                    if (emulator_) emulator_->set_videocore_trace_enabled(enabled);
                });
        connect(videocore_pause_on_call_, &QCheckBox::toggled, this,
                [this](bool enabled) {
                    if (emulator_) emulator_->set_videocore_pause_on_call(enabled);
                });
        connect(clear_videocore, &QPushButton::clicked, this, [this] {
            if (emulator_) emulator_->clear_videocore_events();
            videocore_last_sequence_ = 0;
            videocore_table_->setRowCount(0);
            videocore_detail_->clear();
        });
        connect(videocore_table_, &QTableWidget::itemSelectionChanged, this,
                [this] { showSelectedVideoCoreEvent(); });

        map_view_ = new QPlainTextEdit;
        map_view_->setReadOnly(true);
        map_view_->setMinimumSize(360, 280);
        map_dock_ = makeTool(tr("Memory Map"), map_view_);

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

    void loadGame(const QString& path, u32 platform_id = 0) {
        frame_timer_->stop();
        emulator_.reset();
        current_game_.clear();
        canvas_->setFrame({});
        canvas_->setMessage(tr("Loading %1…").arg(QFileInfo(path).fileName()));
        statusBar()->showMessage(tr("Loading %1…").arg(QFileInfo(path).fileName()));
        auto next = std::make_unique<Emulator>();
        next->device_settings() = device_settings_;
        next->display().set_headless(true);
        Emulator::Config config;
        const QByteArray encoded_path = path.toUtf8();
        const auto* path_bytes =
            reinterpret_cast<const char8_t*>(encoded_path.constData());
        config.executable = std::filesystem::path(std::u8string(
            path_bytes, static_cast<std::size_t>(encoded_path.size())));
        config.hdd_root = hdd_root_;
        config.platform_id = platform_id;
        config.videocore_engine = jit_action_->isChecked()
            ? clickhle::VideoCoreEngine::Jit : clickhle::VideoCoreEngine::Interpreter;
        std::string error;
        if (!next->load(std::move(config), &error)) {
            const QString reason = QString::fromStdString(error);
            const QString message = tr("Could not load %1\n\n%2")
                                        .arg(QFileInfo(path).fileName(), reason);
            canvas_->setMessage(message);
            statusBar()->showMessage(message, 15000);
            appendLog(tr("Load failed: %1").arg(QString::fromStdString(error)));
            QMessageBox::critical(this, tr("Could Not Load Game"),
                                  message);
            updateActions();
            return;
        }
        emulator_ = std::move(next);
        audio_error_reported_ = false;
        emulator_->set_videocore_trace_enabled(videocore_trace_enabled_->isChecked());
        emulator_->set_videocore_pause_on_call(videocore_pause_on_call_->isChecked());
        videocore_last_sequence_ = 0;
        videocore_table_->setRowCount(0);
        videocore_detail_->clear();
        current_game_ = path;
        current_platform_id_ = emulator_->platform_id();
        central_stack_->setCurrentWidget(canvas_);
        addRecent(path);
        canvas_->setFrame({});
        canvas_->setMessage(tr("Game loaded. Waiting for the first frame…"));
        appendLog(tr("Loaded %1").arg(QFileInfo(path).fileName()));
        updateActions();
        resumeGame();
        canvas_->setFocus();
    }

    void closeGame() {
        frame_timer_->stop();
        emulator_.reset();
        current_game_.clear();
        current_platform_id_ = 0;
        showGameLibrary();
        canvas_->setFrame({});
        canvas_->setMessage({});
        statusBar()->showMessage(tr("Game closed"));
        updateActions();
        if (cpu_dock_->isVisible() || hle_dock_->isVisible() ||
            map_dock_->isVisible() || videocore_dock_->isVisible())
            refreshTools();
    }

    void restartGame() {
        if (!current_game_.isEmpty()) loadGame(current_game_, current_platform_id_);
    }

    void pauseGame() {
        if (!emulator_) return;
        frame_timer_->stop();
        if (emulator_->audio() != nullptr) {
            emulator_->audio()->set_paused(true);
        }
        paused_ = true;
        statusBar()->showMessage(tr("Paused"));
        updateActions();
    }

    void resumeGame() {
        if (!emulator_) return;
        paused_ = false;
        if (emulator_->audio() != nullptr) {
            emulator_->audio()->set_paused(false);
        }
        frame_timer_->start();
        statusBar()->showMessage(tr("Running — F5 pauses, F6 resumes"));
        updateActions();
    }

    void advanceFrame() {
        if (!emulator_) return;
        // Loading callbacks can convert whole textures on the ARM CPU.
        // Keep a finite guard, with room for the verified loading paths.
        constexpr u64 kGuiInstructionBudget = 50'000'000;
        const clickhle::RunResult result =
            emulator_->run_game_frame(kGuiInstructionBudget);
        if (!audio_error_reported_ && !emulator_->audio_error().empty()) {
            appendLog(tr("Audio unavailable: %1")
                .arg(QString::fromStdString(emulator_->audio_error())));
            audio_error_reported_ = true;
        }
        int width = 0;
        int height = 0;
        std::vector<u8> pixels = emulator_->display().read_rgba(width, height);
        if (!pixels.empty() && width > 0 && height > 0) {
            QImage image(pixels.data(), width, height, width * 4,
                         QImage::Format_RGBA8888);
            canvas_->setFrame(image.mirrored(false, true).copy());
        }
        if (cpu_dock_->isVisible() || hle_dock_->isVisible() ||
            map_dock_->isVisible() || videocore_dock_->isVisible())
            refreshTools();
        u64 frames = 0;
        u64 draws = 0;
        clickhle::gles_statistics(*emulator_, frames, draws);
        (void)frames;
        ++fps_window_frames_;
        const qint64 fps_elapsed_ms = fps_timer_.elapsed();
        if (fps_elapsed_ms >= 500) {
            fps_ = static_cast<double>(fps_window_frames_) * 1000.0 /
                   static_cast<double>(fps_elapsed_ms);
            fps_window_frames_ = 0;
            fps_timer_.restart();
        }
        QString status =
            tr("%1  |  %2 FPS  |  %3 draws  |  %4 HLE calls")
                .arg(QFileInfo(current_game_).fileName())
                .arg(fps_, 0, 'f', 1).arg(draws)
                .arg(emulator_->hle_call_count());
        if (!emulator_->audio_error().empty()) status += tr("  |  Audio unavailable");
        statusBar()->showMessage(status);
        if (result.status == clickhle::RunStatus::Fault ||
            result.status == clickhle::RunStatus::Unsupported ||
            !emulator_->last_missing_import().empty()) {
            frame_timer_->stop();
            paused_ = true;
            const QString message = result.fault.empty()
                ? QString::fromStdString(emulator_->last_missing_import())
                : QString::fromStdString(result.fault);
            appendLog(tr("Stopped: %1").arg(message));
            statusBar()->showMessage(tr("Stopped: %1").arg(message));
            if (!emulator_->last_missing_import().empty() && hle_dock_) {
                hle_dock_->show();
                hle_dock_->raise();
            }
            updateActions();
        }
        if (auto event = emulator_->take_videocore_break()) {
            frame_timer_->stop();
            if (emulator_->audio() != nullptr) emulator_->audio()->set_paused(true);
            paused_ = true;
            statusBar()->showMessage(
                tr("Paused after %1 at %2")
                    .arg(QString::fromStdString(event->name), hex(event->return_pc)));
            appendLog(tr("Render-server trigger: %1 at %2")
                          .arg(QString::fromStdString(event->name), hex(event->return_pc)));
            videocore_dock_->show();
            videocore_dock_->raise();
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
            videocore_detail_->setPlainText(tr("No game loaded"));
            return;
        }
        if (const auto* cpu = emulator_->cpu(); cpu_dock_->isVisible() && cpu) {
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

        if (hle_dock_->isVisible()) {
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
        }

        if (map_dock_->isVisible()) {
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
        if (videocore_dock_->isVisible()) refreshVideoCoreEvents();
    }

    void refreshVideoCoreEvents() {
        const auto& events = emulator_->videocore_events();
        bool added_event = false;
        for (const auto& event : events) {
            if (event.sequence <= videocore_last_sequence_) continue;
            const int row = videocore_table_->rowCount();
            videocore_table_->insertRow(row);
            QString arguments;
            for (u8 i = 0; i < event.arg_count; ++i) {
                if (!arguments.isEmpty()) arguments += QStringLiteral(", ");
                arguments += hex(event.args[i]);
            }
            const QStringList columns{
                QString::number(event.sequence), QString::number(event.frame),
                QString::fromStdString(event.name), hex(event.return_pc), arguments,
                hex(event.result)};
            for (int column = 0; column < columns.size(); ++column) {
                videocore_table_->setItem(
                    row, column, new QTableWidgetItem(columns[column]));
            }
            videocore_last_sequence_ = event.sequence;
            added_event = true;
            while (videocore_table_->rowCount() > 1024)
                videocore_table_->removeRow(0);
        }
        if (videocore_table_->rowCount() > 0 &&
            videocore_table_->currentRow() < 0)
            videocore_table_->selectRow(videocore_table_->rowCount() - 1);
        if (added_event && videocore_pause_on_call_->isChecked() &&
            videocore_table_->rowCount() > 0)
            videocore_table_->selectRow(videocore_table_->rowCount() - 1);
    }

    void showSelectedVideoCoreEvent() {
        if (!emulator_ || videocore_table_->currentRow() < 0) return;
        const auto* item =
            videocore_table_->item(videocore_table_->currentRow(), 0);
        if (!item) return;
        bool ok = false;
        const u64 sequence = item->text().toULongLong(&ok);
        if (!ok) return;
        for (const auto& event : emulator_->videocore_events()) {
            if (event.sequence != sequence) continue;
            QString detail = tr("%1\nFrame: %2   Instruction: %3\nReturn PC: %4\n")
                .arg(QString::fromStdString(event.name)).arg(event.frame)
                .arg(event.instructions).arg(hex(event.return_pc));
            for (u8 i = 0; i < event.arg_count; ++i)
                detail += tr("r%1 = %2\n").arg(i).arg(hex(event.args[i]));
            detail += tr("Result: %1\nPayload size: %2 bytes")
                .arg(hex(event.result)).arg(event.payload_size);
            if (event.payload_prefix_size != 0) {
                detail += tr("\nPayload prefix:");
                for (u8 i = 0; i < event.payload_prefix_size; ++i) {
                    if (i % 16 == 0) detail += QLatin1Char('\n');
                    detail += QStringLiteral("%1 ").arg(event.payload_prefix[i], 2, 16,
                                                         QLatin1Char('0'));
                }
            }
            videocore_detail_->setPlainText(detail);
            return;
        }
        videocore_detail_->setPlainText(tr("Event has aged out of the trace buffer."));
    }

    void refreshMemory() {
        if (!emulator_) {
            memory_view_->setPlainText(tr("No game loaded"));
            return;
        }
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
    Emulator::DeviceSettings device_settings_;
    QString current_game_;
    QString package_name_;
    u32 current_platform_id_ = 0;
    std::filesystem::path hdd_root_;
    QThread* install_thread_ = nullptr;
    std::vector<clickhle::InstalledGame> installed_games_;
    std::vector<clickhle::GameVariant> library_rows_;
    QStackedWidget* central_stack_ = nullptr;
    QTableWidget* library_table_ = nullptr;
    QComboBox* platform_selector_ = nullptr;
    QLineEdit* library_search_ = nullptr;
    QLabel* library_message_ = nullptr;
    QStringList recent_games_;
    bool paused_ = true;
    bool audio_error_reported_ = false;
    GameCanvas* canvas_ = nullptr;
    QTimer* frame_timer_ = nullptr;
    QElapsedTimer fps_timer_;
    u64 fps_window_frames_ = 0;
    double fps_ = 0.0;
    QMenu* recent_menu_ = nullptr;
    QMenu* tools_menu_ = nullptr;
    QMenu* debug_menu_ = nullptr;
    QAction* open_action_ = nullptr;
    QAction* library_action_ = nullptr;
    QAction* install_action_ = nullptr;
    QAction* close_action_ = nullptr;
    QAction* screenshot_action_ = nullptr;
    QAction* quit_action_ = nullptr;
    QAction* pause_action_ = nullptr;
    QAction* resume_action_ = nullptr;
    QAction* restart_action_ = nullptr;
    QAction* integer_scale_action_ = nullptr;
    QAction* device_settings_action_ = nullptr;
    QAction* interpreter_action_ = nullptr;
    QAction* jit_action_ = nullptr;
    QAction* about_action_ = nullptr;
    QPlainTextEdit* cpu_view_ = nullptr;
    QPlainTextEdit* memory_view_ = nullptr;
    QPlainTextEdit* hle_view_ = nullptr;
    QDockWidget* cpu_dock_ = nullptr;
    QDockWidget* map_dock_ = nullptr;
    QDockWidget* hle_dock_ = nullptr;
    QDockWidget* memory_dock_ = nullptr;
    QDockWidget* videocore_dock_ = nullptr;
    QPlainTextEdit* map_view_ = nullptr;
    QPlainTextEdit* log_view_ = nullptr;
    QLineEdit* memory_address_ = nullptr;
    QTableWidget* videocore_table_ = nullptr;
    QPlainTextEdit* videocore_detail_ = nullptr;
    QCheckBox* videocore_trace_enabled_ = nullptr;
    QCheckBox* videocore_pause_on_call_ = nullptr;
    u64 videocore_last_sequence_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    const QString startup_game = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                          : QString{};
    clickhle::log::set_level(clickhle::log::Level::Warn);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ClickHLE"));
    QCoreApplication::setApplicationName(QStringLiteral("ClickHLE"));
    MainWindow window(startup_game);
    window.show();
    return application.exec();
}

#include "clickhle/video/videocore.h"
#include "clickhle/core/emulator.h"
#include "clickhle/hle/call_frame.h"
#include "vc2/core.h"
#include "vc2/png.h"
#include "graphics_api.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace clickhle {
namespace {
std::vector<u8> read_file(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot open " + path.string());
    return {std::istreambuf_iterator<char>(file), {}};
}
std::filesystem::path default_firmware_path() {
    if (const char *value = std::getenv("CLICKHLE_VC2_FIRMWARE"); value && *value)
        return value;
    std::filesystem::path executable;
#if defined(_WIN32)
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!size) break;
        if (size < buffer.size()) {
            executable = std::wstring(buffer.data(), size);
            break;
        }
        if (buffer.size() >= 32768) break;
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (size && _NSGetExecutablePath(buffer.data(), &size) == 0)
        executable = std::filesystem::canonical(buffer.data());
#elif defined(__linux__)
    executable = std::filesystem::read_symlink("/proc/self/exe");
#endif
    if (executable.empty())
        throw std::runtime_error("Cannot locate the executable; set CLICKHLE_VC2_FIRMWARE");
    return executable.parent_path() / "rserver.bin";
}
} // namespace
struct VideoCore::Impl {
    VideoCore &owner;
    vc2::Core chip;
    std::vector<u8> firmware;
    GraphicsApi api;
    bool started = false;
    u32 device = 0, host = 0, ring = 0, sequence = 0, completion = 0;
    // Experimental panel timing. GPIO49/source 11 follows firmware evidence;
    // the 60 Hz period is a fixture until panel timing is independently known.
    static constexpr u32 kDisplaySyncPeriod = 16667;
    u32 next_display_sync = 0;
    std::filesystem::path output;

    explicit Impl(VideoCore &instance) : owner(instance), api([this](std::span<const u8> bytes) { send(bytes); }) {
        const char *path = std::getenv("CLICKHLE_VC2_OUTPUT");
        output = path ? path : "work/vc2";
        std::filesystem::create_directories(output);
        // A replay is one emulator session, not a concatenation of previous
        // launches. send() appends batches only after this file is reset.
        std::ofstream packets(output / "native-packets.bin", std::ios::binary | std::ios::trunc);
        if (!packets)
            throw std::runtime_error("Cannot create VC2 packet capture");
    }
    void boot() {
        if (started)
            return;
        if (firmware.empty())
            firmware = read_file(default_firmware_path());
        if (firmware.size() < 0x206 || firmware.size() > 0x140000 || firmware.size() % 4)
            throw std::runtime_error("Invalid VC2 firmware upload");
        // Upload bytes to VC SRAM and execute the firmware entry. Host-port
        // cache/clock setup is abstracted by HLE, not replayed as ARM code.
        chip.load(firmware);
        // The game-image loader clears readiness after uploading the image.
        // Only executing the new firmware may publish a ready state.
        chip.write(0x1f8, 0);
        if (!chip.run()) throw std::runtime_error(chip.error);
        if (chip.read(0x1f8) != 1)
            throw std::runtime_error("VC2 firmware readiness handshake failed");
        device = chip.read(0xc0000000);
        host = chip.read(0xc0000004);
        ring = chip.read(0xc0000008);
        if (!device || !host || !ring || !chip.sleeping)
            throw std::runtime_error("VC2 ring initialization failed");
        next_display_sync = chip.timer + kDisplaySyncPeriod;
        started = true;
        std::ofstream report(output / "native-startup.txt");
        report << "Native HLE uploaded " << firmware.size()
               << " bytes; VC2 firmware published readiness and command-ring addresses.\n"
               << "Host transport and panel timing remain HLE fixtures.\n";
    }
    void capture_pixels() {
        if (chip.descriptors.size() < 15)
            return;
        const u32 address = chip.descriptors[0].words[2];
        for (unsigned i = 0; i < 15; ++i) {
            const auto &d = chip.descriptors[i].words;
            if (d[2] != address + i * 10240 || d[3] != 320 || d[4] != 0x1025151)
                return;
        }
        auto raw = chip.copy_out(address, 320 * 240 * 2);
        std::vector<u8> rgb;
        rgb.reserve(320 * 240 * 3);
        for (std::size_t i = 0; i < raw.size(); i += 2) {
            u32 pixel = raw[i] | static_cast<u32>(raw[i + 1]) << 8;
            u32 red = pixel >> 11, green = (pixel >> 5) & 63, blue = pixel & 31;
            rgb.push_back(static_cast<u8>((red << 3) | (red >> 2)));
            rgb.push_back(static_cast<u8>((green << 2) | (green >> 4)));
            rgb.push_back(static_cast<u8>((blue << 3) | (blue >> 2)));
        }
        if (rgb == owner.pixels_)
            return;
        owner.pixels_ = std::move(rgb);
        ++owner.pixel_revision_;
        std::ostringstream label;
        label << "native-api-" << std::setfill('0') << std::setw(6) << sequence << "-RGB565";
        vc2::write_rgb_png(output / (label.str() + ".png"), 320, 240, owner.pixels_);
        std::ofstream file(output / (label.str() + ".rgb565"), std::ios::binary);
        file.write(reinterpret_cast<const char *>(raw.data()),
                   static_cast<std::streamsize>(raw.size()));
    }
    void send(std::span<const u8> packets) {
        if (packets.empty())
            return;
        boot();
        if (!chip.sleeping || chip.read(chip.r[31], 2) != 0x4330)
            throw std::runtime_error("VC2 is not idle");
        u32 consumer = chip.read(device + 4), producer = chip.read(host + 4);
        if (consumer != producer || producer >= 0x6000 || packets.size() > 0x5f00)
            throw std::runtime_error("VC2 command ring busy or batch too large");
        const auto first = std::min<std::size_t>(packets.size(), 0x6000 - producer);
        chip.copy_in(ring + producer, packets.first(first));
        chip.copy_in(ring, packets.subspan(first));
        chip.write(host + 4, (producer + static_cast<u32>(packets.size())) % 0x6000);
        std::ofstream log(output / "native-packets.bin", std::ios::binary | std::ios::app);
        log.write(reinterpret_cast<const char *>(packets.data()),
                  static_cast<std::streamsize>(packets.size()));
        if (!log)
            throw std::runtime_error("Cannot write VC2 packet capture");
        // RetailOS 0x10275028 publishes the producer block, then writes
        // 0xa5a50002 to VC address 0x10000400. Resuming after sleep models
        // its observed purpose; the register's hardware semantics remain
        // provisional. RenderServer does not enable the VMCS host IRQ here.
        chip.r[31] += 2;
        bool ok = chip.run();
        for (unsigned i = 0; i < 512 && ok &&
                             (chip.dma_busy() || chip.read(device + 4) != chip.read(host + 4)); ++i) {
            chip.advance(100);
            chip.progress_dma();
            if (chip.timer - next_display_sync < 0x80000000u) {
                chip.set_gpio_input(49, true);
                chip.set_gpio_input(49, false);
                next_display_sync = chip.timer + kDisplaySyncPeriod;
            }
            // Resume only through a pending, unmasked hardware interrupt.
            // The firmware itself acknowledges events and advances its queues.
            if (chip.sleeping && chip.interrupt(chip.r[31] + 2))
                ok = chip.run();
        }
        capture_pixels();
        if (!ok)
            throw std::runtime_error(chip.error);
        if (chip.read(device + 4) != chip.read(host + 4))
            throw std::runtime_error("VC2 commands remain pending after consumer sleep");
    }
    void packet(std::array<u32, 4> words) {
        std::array<u8, 16> bytes{};
        for (unsigned i = 0; i < 4; ++i)
            endian::store_le<u32>(reinterpret_cast<std::byte *>(bytes.data() + 4 * i), words[i]);
        send(bytes);
    }
    void finish(u32 marker = 0) {
        api.flush();
        if (++completion == 0) ++completion;
        if (!marker) marker = completion;
        packet({9, marker, 0, 0});
        if (chip.read(device) != marker)
            throw std::runtime_error("VC2 completion is pending");
    }
    u32 call(u32 ordinal, HleCallFrame &frame) {
        ++sequence;
        chip.descriptors.clear();
        switch (ordinal) {
        case 164: {
            if (started) throw std::runtime_error("Stop VC2 before replacing its firmware");
            const u32 enabled = frame.args.next_u32();
            const u32 address = frame.args.next_ptr();
            const u32 size = frame.args.next_u32();
            // RetailOS 0x101a892c clears the saved image first and copies only
            // when both the pointer and signed length are positive. Games use
            // a zero-length call to select the default RenderServer image.
            if (!enabled || !address || static_cast<i32>(size) <= 0) firmware.clear();
            else {
                const auto *data = frame.memory.host_ptr(address, size);
                if (!data || size < 0x206 || size > 0x140000)
                    throw std::runtime_error("Invalid guest VC2 firmware: address=" + std::to_string(address) + " size=" + std::to_string(size));
                const auto *bytes = reinterpret_cast<const u8 *>(data);
                firmware.assign(bytes, bytes + size);
            }
            return 1;
        }
        case 152: {
            (void)frame.args.next_u32();
            const u32 first = frame.args.next_ptr(), second = frame.args.next_ptr();
            boot();
            packet({5, 0, 0, 0});
            if (!frame.store<u32>(first, 1) || !frame.store<u32>(second, 2))
                throw std::runtime_error("Invalid render-server output pointers");
            // RetailOS API 152 clears the graphics state and restores its
            // defaults after opening the render service (0x100cdb88).
            api.reset();
            return 1;
        }
        case 153:
            // RetailOS closes the service only after glFinish acknowledges
            // its completion marker, not merely after submitting the batch.
            finish();
            started = false;
            return 1;
        case 41:
            finish();
            return 0;
        case 42:
            api.flush();
            return 0;
        case 157:
            api.queue({6, 0, 0, 0});
            finish(0x40000000 | (sequence & 0x3fffffff));
            ++owner.frames_;
            return 1;
        default: {
            const u32 result = api.call(ordinal, frame);
            if (ordinal == 37 || ordinal == 38) ++owner.draws_;
            return result;
        }
        }
    }
    void failure() {
        std::ofstream report(output / "native-failure.txt");
        report << owner.error_ << "\nVC PC 0x" << std::hex << chip.r[31] << "\n";
        report << "Recent instruction PCs:";
        const u64 first = chip.recent_pc_count > chip.recent_pcs.size() ? chip.recent_pc_count - chip.recent_pcs.size() : 0;
        for (u64 i = first; i < chip.recent_pc_count; ++i)
            report << " 0x" << chip.recent_pcs[i % chip.recent_pcs.size()];
        report << "\n";
        const u32 pc = chip.r[31];
        if (pc >= 32 && pc < 0x4000000 - 64) {
            report << "Instruction window from 0x" << pc - 32 << ":";
            for (u32 address = pc - 32; address < pc + 64; address += 2)
                report << " 0x" << chip.read(address, 2);
            report << "\n";
        }
        for (unsigned i = 0; i < 32; ++i)
            report << "r" << std::dec << i << "=0x" << std::hex << chip.r[i] << "\n";
        report << "Instructions " << std::dec << chip.instructions << ", DMA words "
               << chip.dma_words << "\n";
        report << "Sleeping " << chip.sleeping << ", DMA busy " << chip.dma_busy()
               << ", VC error: " << chip.error << "\n";
        if (started)
            report << "Ring consumer 0x" << std::hex << chip.read(device + 4)
                   << ", producer 0x" << chip.read(host + 4) << "\n";
        if (!chip.descriptors.empty()) {
            report << "Last DMA descriptor:" << std::hex;
            for (u32 value : chip.descriptors.back().words)
                report << " 0x" << value;
            report << "\n";
        }
    }
};
VideoCore::VideoCore() = default;
VideoCore::~VideoCore() = default;
bool VideoCore::initialize() {
    if (!error_.empty())
        return false;
    if (!impl_)
        impl_ = std::make_unique<Impl>(*this);
    return true;
}
bool VideoCore::call(u32 ordinal, HleCallFrame &frame) {
    bool success = false;
    try {
        if (!initialize()) return false;
        frame.ret(impl_->call(ordinal, frame));
        success = true;
    } catch (const std::exception &e) {
        error_ = "OpenGLES#" + std::to_string(ordinal) + ": " + e.what();
        if (impl_) impl_->failure();
    }
    // Keep partial firmware output visible when execution stops on a fault.
    if (!pixels_.empty() && (pixel_revision_ != published_revision_ || ordinal == 157)) {
        auto& display = frame.emulator.display();
        display.set_frame_rgb(pixels_, 320, 240);
        published_revision_ = pixel_revision_;
        if (display.is_open() || display.open(320, 240, "ClickHLE")) display.present();
        if (!frame.emulator.screenshot_path().empty())
            static_cast<void>(display.screenshot(frame.emulator.screenshot_path()));
    }
    if (ordinal == 157 && success && !frame.emulator.display().pump())
        frame.cpu.request_stop();
    return success;
}
bool VideoCore::submit(std::span<const u8> packets) {
    try {
        if (!initialize())
            return false;
        ++impl_->sequence;
        impl_->chip.descriptors.clear();
        impl_->send(packets);
        return true;
    } catch (const std::exception &e) {
        error_ = e.what();
        if (impl_)
            impl_->failure();
        return false;
    }
}
} // namespace clickhle

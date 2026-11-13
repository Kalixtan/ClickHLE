#include "clickhle/video/videocore.h"
#include "clickhle/core/emulator.h"
#include "clickhle/hle/call_frame.h"
#include "vc2/core.h"
#include "vc2/worker.h"
#include "vc2/png.h"
#include "graphics_api.h"
#include "g5/api.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>
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
struct CpuTimer {
    double *total;
    std::clock_t start;
    explicit CpuTimer(double *value) : total(value), start(value ? std::clock() : 0) {}
    ~CpuTimer() {
        if (total) *total += static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
    }
};

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
    std::unique_ptr<vc2::Worker> worker;
    std::vector<std::future<void>> pending_work;
    std::exception_ptr worker_failure;
    bool worker_stats = std::getenv("CLICKHLE_VC2_WORKER_STATS") != nullptr;
    u64 worker_submissions = 0, worker_overlaps = 0, worker_waits = 0;
    std::size_t worker_max_pending = 0;
    std::vector<u8> firmware;
    GraphicsApi api;
    bool started = false;
    u32 device = 0, host = 0, ring = 0, sequence = 0;
    // Experimental panel timing. GPIO49/source 11 follows firmware evidence;
    // the 60 Hz period is a fixture until panel timing is independently known.
    static constexpr u32 kDisplaySyncPeriod = 16667;
    // A batch can queue a large DMA transfer after substantial rendering work.
    // Allow its firmware wait loop to finish while retaining a finite guard.
    static constexpr u32 kSubmissionInstructionLimit = 4'000'000;
    u32 next_display_sync = 0;
    std::filesystem::path output;
    bool timing = std::getenv("CLICKHLE_VC2_TIMING") != nullptr;
    bool verify_g5 = owner.capture_ && std::getenv("CLICKHLE_G5_VERIFY") != nullptr;
    u64 verified_runs = 0;
    double boot_cpu = 0, send_cpu = 0, run_cpu = 0, pixels_cpu = 0;
    double run_compile_cpu = 0;
    u64 send_count = 0;
    u64 pixel_scans = 0, complete_frames = 0, changed_frames = 0;
    u64 last_complete_sequence = 0, last_changed_sequence = 0;
    u64 last_scene_sequence = 0, last_scene_pixels = 0;
    std::size_t maximum_descriptors = 0;
    u64 trace_from = [] {
        if (const char *value = std::getenv("CLICKHLE_VC2_TRACE_FROM"))
            return std::strtoull(value, nullptr, 0);
        return 0ull;
    }();
    u32 current_ordinal = 0;
    u64 trace_count = [] {
        if (const char *value = std::getenv("CLICKHLE_VC2_TRACE_COUNT"))
            return std::strtoull(value, nullptr, 0);
        return 200ull;
    }();

    bool tracing() const {
        return trace_from && sequence >= trace_from && sequence - trace_from < trace_count;
    }

    explicit Impl(VideoCore &instance) : owner(instance), api(
        [this](std::span<const u8> bytes) { send(bytes); },
        {[this](u32 size) {
            wait_pending();
            return chip.copy_out(chip.read(0xc000000c), size);
        }, [this] { wait_pending(); boot(); return chip.read(device); }, [this](u32 marker) {
            wait_pending();
            boot();
            if (chip.read(device) < marker)
                throw std::runtime_error("VC2 completion is pending");
        }}) {
        if (std::getenv("CLICKHLE_VC2_WORKER"))
            worker = std::make_unique<vc2::Worker>();
        if (!owner.capture_) return;
        const char *path = std::getenv("CLICKHLE_VC2_OUTPUT");
        output = path ? path : "work/vc2";
        std::filesystem::create_directories(output);
        // A replay is one emulator session, not a concatenation of previous
        // launches. send() appends batches only after this file is reset.
        std::ofstream packets(output / "native-packets.bin", std::ios::binary | std::ios::trunc);
        if (!packets)
            throw std::runtime_error("Cannot create VC2 packet capture");
    }
    ~Impl() {
        if (worker_stats)
            std::cerr << "VC2 worker: submissions=" << worker_submissions
                      << ", queued while busy=" << worker_overlaps
                      << ", waits=" << worker_waits
                      << ", max pending=" << worker_max_pending
                      << ", JIT compile wall us=" << chip.jit_compile_us << '\n';
        if (verify_g5) std::cerr << "G5 verified runs: " << verified_runs << '\n';
        if (!timing) return;
        std::cerr << "VC2 CPU seconds: boot=" << boot_cpu
                  << " send=" << send_cpu << " run=" << run_cpu
                  << " run excluding compilation=" << run_cpu - run_compile_cpu
                  << " compilation=" << run_compile_cpu
                  << " pixels=" << pixels_cpu << ", submissions=" << send_count
                  << ", instructions=" << chip.instructions
                  << ", G5 instructions=" << chip.replacement_instructions
                  << ", JIT instructions=" << chip.jit_instructions
                  << ", JIT executions=" << chip.jit_executions
                  << ", DMA words=" << chip.dma_words
                  << ", DMA CPU seconds=" << chip.dma_cpu_seconds
                  << ", DMA passes=" << chip.dma_passes
                  << ", JIT materializations=" << chip.jit_compilations
                  << ", JIT setup wall us=" << chip.jit_compile_us << '\n';
        std::cerr << "VC2 pixels: scans=" << pixel_scans
                  << ", complete=" << complete_frames
                  << ", changed=" << changed_frames
                  << ", max descriptors=" << maximum_descriptors
                  << ", last complete sequence=" << last_complete_sequence
                  << ", last change sequence=" << last_changed_sequence
                  << ", last scene sequence=" << last_scene_sequence
                  << ", last scene pixels=" << last_scene_pixels << '\n';
    }
    bool run_chip(u64 budget = 2'000'000) {
        CpuTimer timer(timing ? &run_cpu : nullptr);
        auto reference = verify_g5 && owner.engine_ == VideoCoreEngine::G5
            ? chip.fork_reference() : nullptr;
        const double compile_before = chip.jit_compile_cpu;
        const bool result = chip.run(budget);
        run_compile_cpu += chip.jit_compile_cpu - compile_before;
        if (reference) {
            const bool expected = reference->run(budget);
            const auto difference = chip.state_difference(*reference);
            if (result != expected || !difference.empty() || chip.instructions != reference->instructions)
                throw std::runtime_error("G5 reference mismatch at API #" +
                    std::to_string(current_ordinal) + ": " +
                    (difference.empty() ? "execution count/result" : difference));
            ++verified_runs;
        }
        return result;
    }
    void boot() {
        if (started)
            return;
        CpuTimer timer(timing ? &boot_cpu : nullptr);
        if (firmware.empty())
            firmware = read_file(default_firmware_path());
        if (firmware.size() < 0x206 || firmware.size() > 0x140000 || firmware.size() % 4)
            throw std::runtime_error("Invalid VC2 firmware upload");
        // Upload bytes to VC SRAM and execute the firmware entry. Host-port
        // cache/clock setup is abstracted by HLE, not replayed as ARM code.
        chip.load(firmware);
        chip.set_jit_enabled(owner.engine_ != VideoCoreEngine::Interpreter);
        chip.set_g5_enabled(owner.engine_ == VideoCoreEngine::G5);
        // The game-image loader clears readiness after uploading the image.
        // Only executing the new firmware may publish a ready state.
        chip.write(0x1f8, 0);
        if (!run_chip()) throw std::runtime_error(chip.error);
        if (chip.read(0x1f8) != 1)
            throw std::runtime_error("VC2 firmware readiness handshake failed");
        device = chip.read(0xc0000000);
        host = chip.read(0xc0000004);
        ring = chip.read(0xc0000008);
        if (!device || !host || !ring || !chip.sleeping)
            throw std::runtime_error("VC2 ring initialization failed");
        next_display_sync = chip.timer + kDisplaySyncPeriod;
        started = true;
        if (!owner.capture_) return;
        std::ofstream report(output / "native-startup.txt");
        report << "Native HLE uploaded " << firmware.size()
               << " bytes; VC2 firmware published readiness and command-ring addresses.\n"
               << "Host transport and panel timing remain HLE fixtures.\n";
    }
    void capture_pixels() {
        CpuTimer timer(timing ? &pixels_cpu : nullptr);
        if (timing) {
            ++pixel_scans;
            maximum_descriptors = std::max(maximum_descriptors, chip.descriptors.size());
        }
        if (chip.descriptors.size() < 15)
            return;
        // A submission may upload textures before writing the framebuffer.
        // Find the latest complete set of 15 RGB565 strips, wherever it
        // occurs in the descriptor history for this API call.
        u32 address = 0;
        bool found = false;
        for (std::size_t end = chip.descriptors.size(); end >= 15; --end) {
            const auto start = end - 15;
            const u32 candidate = chip.descriptors[start].words[2];
            bool complete = true;
            for (unsigned i = 0; i < 15; ++i) {
                const auto &d = chip.descriptors[start + i].words;
                if (d[2] != candidate + i * 10240 || d[3] != 320 || d[4] != 0x1025151) {
                    complete = false;
                    break;
                }
            }
            if (complete) {
                address = candidate;
                found = true;
                break;
            }
        }
        if (!found) return;
        if (timing) {
            ++complete_frames;
            last_complete_sequence = sequence;
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
        if (timing) {
            ++changed_frames;
            last_changed_sequence = sequence;
            u64 scene_pixels = 0;
            for (std::size_t pixel = 16 * 320; pixel < 320 * 240; ++pixel) {
                const auto offset = pixel * 3;
                scene_pixels += rgb[offset] || rgb[offset + 1] || rgb[offset + 2];
            }
            if (last_scene_pixels && !scene_pixels)
                std::cerr << "VC2 scene became blank at sequence " << sequence
                          << ", ordinal " << current_ordinal << '\n';
            last_scene_pixels = scene_pixels;
            if (scene_pixels) last_scene_sequence = sequence;
        }
        owner.pixels_ = std::move(rgb);
        ++owner.pixel_revision_;
        if (!owner.capture_)
            return;
        std::ostringstream label;
        label << "native-api-" << std::setfill('0') << std::setw(6) << sequence << "-RGB565";
        vc2::write_rgb_png(output / (label.str() + ".png"), 320, 240, owner.pixels_);
        std::ofstream file(output / (label.str() + ".rgb565"), std::ios::binary);
        file.write(reinterpret_cast<const char *>(raw.data()),
                   static_cast<std::streamsize>(raw.size()));
    }
    void wait_pending() {
        if (worker_stats && !pending_work.empty()) ++worker_waits;
        std::exception_ptr failure;
        for (auto &job : pending_work) {
            try { job.get(); }
            catch (...) { if (!failure) failure = std::current_exception(); }
        }
        pending_work.clear();
        if (!failure) failure = std::exchange(worker_failure, {});
        else worker_failure = {};
        if (failure) std::rethrow_exception(failure);
    }
    void send(std::span<const u8> packets) {
        if (!worker) {
            send_now(packets);
            return;
        }
        if (packets.empty()) return;
        // Own the bytes independently of GraphicsApi's reusable staging buffer.
        // A bounded queue lets the producer prepare later fragments while the
        // worker consumes earlier ones. The public API call still waits before
        // returning to the guest or publishing a framebuffer.
        if (pending_work.size() >= 4) wait_pending();
        if (worker_stats) {
            ++worker_submissions;
            if (!pending_work.empty() &&
                pending_work.back().wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                ++worker_overlaps;
        }
        std::vector<u8> owned(packets.begin(), packets.end());
        auto job = worker->submit([this, owned = std::move(owned)] {
            if (worker_failure) return;
            try { send_now(owned); }
            catch (...) { worker_failure = std::current_exception(); }
        });
        try { pending_work.push_back(std::move(job)); }
        catch (...) { if (job.valid()) job.wait(); throw; }
        if (worker_stats) worker_max_pending = std::max(worker_max_pending, pending_work.size());
        // std::clock measures the entire process, not just the worker. Retain
        // synchronous submission when CPU timing diagnostics are requested.
        if (timing) wait_pending();
    }
    void send_now(std::span<const u8> packets) {
        if (packets.empty())
            return;
        CpuTimer timer(timing ? &send_cpu : nullptr);
        ++send_count;
        boot();
        const auto before_instructions = chip.instructions;
        const auto before_dma = chip.dma_words;
        if (tracing()) {
            std::cerr << "VC2 packet seq=" << sequence << " ordinal=" << current_ordinal
                      << " bytes=" << packets.size();
            for (unsigned i = 0; i < 4 && 4 * (i + 1) <= packets.size(); ++i)
                std::cerr << " word" << i << "=0x" << std::hex
                          << endian::load_le<u32>(reinterpret_cast<const std::byte *>(
                              packets.data() + 4 * i)) << std::dec;
            std::cerr << '\n';
        }
        if (!chip.sleeping || chip.read(chip.r[31], 2) != 0x4330)
            throw std::runtime_error("VC2 is not idle");
        u32 consumer = chip.read(device + 4), producer = chip.read(host + 4);
        if (consumer != producer || producer >= 0x6000 || packets.size() > 0x5f00)
            throw std::runtime_error("VC2 command ring busy or batch too large");
        const auto first = std::min<std::size_t>(packets.size(), 0x6000 - producer);
        chip.copy_in(ring + producer, packets.first(first));
        chip.copy_in(ring, packets.subspan(first));
        chip.write(host + 4, (producer + static_cast<u32>(packets.size())) % 0x6000);
        if (owner.capture_) {
            std::ofstream log(output / "native-packets.bin", std::ios::binary | std::ios::app);
            log.write(reinterpret_cast<const char *>(packets.data()),
                      static_cast<std::streamsize>(packets.size()));
            if (!log) throw std::runtime_error("Cannot write VC2 packet capture");
        }
        // RetailOS 0x10275028 publishes the producer block, then writes
        // 0xa5a50002 to VC address 0x10000400. Resuming after sleep models
        // its observed purpose; the register's hardware semantics remain
        // provisional. RenderServer does not enable the VMCS host IRQ here.
        chip.r[31] += 2;
        bool ok = run_chip(kSubmissionInstructionLimit);
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
                ok = run_chip(kSubmissionInstructionLimit);
        }
        capture_pixels();
        if (tracing())
            std::cerr << "VC2 result seq=" << sequence
                      << " instructions=" << chip.instructions - before_instructions
                      << " DMA words=" << chip.dma_words - before_dma
                      << " descriptors=" << chip.descriptors.size()
                      << " scene pixels=" << last_scene_pixels << '\n';
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
    u32 call(u32 ordinal, HleCallFrame &frame) {
        ++sequence;
        current_ordinal = ordinal;
        if (tracing()) {
            std::cerr << "VC2 call seq=" << sequence << " ordinal=" << ordinal
                      << " lr=0x" << std::hex << frame.cpu.lr() << std::dec;
            ArgReader snapshot(frame.cpu, frame.memory);
            for (unsigned i = 0; i < 8; ++i) {
                const auto value = snapshot.next_u32();
                if (!snapshot.ok()) break;
                std::cerr << " arg" << i << "=0x" << std::hex << value << std::dec;
            }
            std::cerr << '\n';
        }
        chip.descriptors.clear();
        if (owner.engine_ == VideoCoreEngine::G5)
            if (const auto result = g5_graphics_call(ordinal, frame))
                return *result;
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
            api.reset(frame);
            return 1;
        }
        case 153:
            // RetailOS closes the service only after glFinish acknowledges
            // its completion marker, not merely after submitting the batch.
            api.finish();
            started = false;
            return 1;
        case 41:
            api.finish();
            return 0;
        case 42:
            api.flush();
            return 0;
        case 157:
            api.call(ordinal, frame);
            ++owner.frames_;
            return 1;
        default: {
            const u32 result = api.call(ordinal, frame);
            if (ordinal == 37 || ordinal == 38) ++owner.draws_;
            return result;
        }
        }
    }
    void failure(const HleCallFrame *frame = nullptr) {
        if (!owner.capture_) return;
        std::ofstream report(output / "native-failure.txt");
        report << owner.error_ << "\nVC PC 0x" << std::hex << chip.r[31] << "\n";
        if (frame) {
            report << "ARM fault PC 0x" << frame->cpu.pc() << ", LR 0x" << frame->cpu.lr()
                   << ", SP 0x" << frame->cpu.sp() << "\nArgument words at fault:";
            ArgReader args(frame->cpu, frame->memory);
            for (unsigned i = 0; i < 12; ++i) {
                const u32 value = args.next_u32();
                if (!args.ok()) {
                    report << " [unmapped]";
                    break;
                }
                report << " 0x" << value;
            }
            report << "\n";
        }
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
               << chip.dma_words << ", JIT instructions "
               << chip.jit_instructions << ", JIT materializations " << chip.jit_compilations
               << ", JIT setup time (us) " << chip.jit_compile_us << "\n";
        report << "API sequence " << sequence << ", VC timer " << chip.timer << "\n";
        report << "Vector lanes: lane acc acclo Z N C\n";
        for (unsigned i = 0; i < chip.acc.size(); ++i)
            report << std::dec << i << " " << chip.acc[i] << " 0x" << std::hex << chip.acclo[i]
                   << std::dec << " " << chip.vz[i] << " " << chip.vn[i] << " " << chip.vc[i] << "\n";
        std::ofstream vrf(output / "native-vrf.bin", std::ios::binary);
        vrf.write(reinterpret_cast<const char *>(chip.vrf.data()),
                  static_cast<std::streamsize>(chip.vrf.size()));
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
bool videocore_jit_available() noexcept {
#if defined(CLICKHLE_HAVE_VC2_JIT) && CLICKHLE_HAVE_VC2_JIT
    return true;
#else
    return false;
#endif
}
VideoCore::VideoCore(bool capture, VideoCoreEngine engine)
    : capture_(capture), engine_(engine) {}
VideoCore::~VideoCore() = default;
void VideoCore::set_engine(VideoCoreEngine engine) {
    engine_ = engine;
    if (impl_) {
        impl_->chip.set_jit_enabled(engine != VideoCoreEngine::Interpreter);
        impl_->chip.set_g5_enabled(engine == VideoCoreEngine::G5);
    }
}
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
        const u32 result = impl_->call(ordinal, frame);
        impl_->wait_pending();
        frame.ret(result);
        success = true;
    } catch (const std::exception &e) {
        if (impl_) { try { impl_->wait_pending(); } catch (...) {} }
        error_ = "OpenGLES#" + std::to_string(ordinal) + ": " + e.what();
        if (impl_) impl_->failure(&frame);
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
    if (ordinal == 157 && success) {
        if (!frame.emulator.display().pump())
            frame.cpu.request_stop();
    }
    return success;
}
bool VideoCore::submit(std::span<const u8> packets) {
    try {
        if (!initialize())
            return false;
        ++impl_->sequence;
        impl_->chip.descriptors.clear();
        impl_->send(packets);
        impl_->wait_pending();
        return true;
    } catch (const std::exception &e) {
        if (impl_) { try { impl_->wait_pending(); } catch (...) {} }
        error_ = e.what();
        if (impl_)
            impl_->failure();
        return false;
    }
}
} // namespace clickhle

#include "clickhle/core/emulator.h"

#include <algorithm>
#include <iterator>
#include <type_traits>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

#include "clickhle/common/log.h"
#include "clickhle/hle/modules.h"

namespace clickhle {
namespace {

constexpr std::string_view kCategory = "emu";

bool is_videocore_boundary_call(const HleFunction& function) {
    if (function.module != "OpenGLES" || !function.has_ordinal) return false;
    // Known render-server control/data exports, plus the uniform packet
    // exports confirmed as inputs to rserver's command queue.
    switch (function.ordinal) {
        case 147:
        case 148:
        case 149:
        case 152:
        case 153:
        case 155:
        case 156:
        case 158:
        case 160:
        case 164:
            return true;
        default:
            return false;
    }
}

/// Leaves a little headroom below the top of the stack region so that a guest
/// prologue writing above sp cannot immediately fall off the mapping.
constexpr u32 kStackHeadroom = 64;

/// Handle of the thread the loader starts. Distinct from the handles the
/// scheduler hands out, so a game can tell its main thread from its workers.
constexpr u32 kMainThreadHandle = 0x0005'0000;

/// The PP502x mirrors SDRAM at 0x00000000 as well as its real base. A guest
/// pointer that is small -- an uninitialised or never-written register --
/// lands here on real hardware and reads back real data; it does not fault.
/// Under HLE there is no firmware image to genuinely mirror, so this backs
/// only what oracle measured directly against real firmware (2026-09-12,
/// this region byte-for-byte, `slotprobe` against the shared ipod-machine
/// crate) rather than a guess: the ARM vector table plus the start of a
/// version/identifier string table ("portalplayer\0" at +0x20). Beyond this
/// window the region reads as zero, same as any other never-written guest
/// memory -- there is no real data to back it further, and a first attempt
/// at zero-filling the WHOLE window hung Vortex's own error-reporting print
/// loop, which walks byte-by-byte from a bad pointer looking for a genuine
/// null terminator: real vector-table words don't happen to contain one
/// where zero-fill does everywhere, so the loop over- or under-shoots
/// depending on what it's scanning. Getting the real bytes right, not just
/// "some value", is what makes this behave the way real hardware does
/// (confirmed: oracle re-ran Vortex's exact crash reproduction through this
/// same real content and got a clean `Exited` via ARM semihosting, the same
/// controlled mechanism RetailOS uses for its own fatal asserts elsewhere in
/// this project -- not a fault, and not a hang).
constexpr GuestAddr kLowMirrorBase = 0x0;
constexpr u32 kLowMirrorSize = 0x1000;
constexpr u8 kLowMirrorRealBytes[] = {
    0xea, 0x00, 0x00, 0x7a, 0xea, 0x00, 0x00, 0x67,
    0xe5, 0xf0, 0x9f, 0xdc, 0xea, 0x00, 0x00, 0x6b,
    0xea, 0x00, 0x00, 0x70, 0xea, 0xff, 0xff, 0xfe,
    0xea, 0x00, 0x00, 0x58, 0xea, 0x00, 0x00, 0x4e,
    0x70, 0x6f, 0x72, 0x74, 0x61, 0x6c, 0x70, 0x6c,
    0x61, 0x79, 0x65, 0x72, 0x00, 0x30, 0x2e, 0x30,
    0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/// EXPLICIT DEVIATION FROM HARDWARE FIDELITY, scoped to one title -- see
/// load()'s use of this below for the full writeup. Not a hardware-accurate
/// fix like kLowMirror* above; a deliberate workaround the user asked for
/// after the real root cause turned out to need firmware/OS scheduler
/// knowledge neither this project's static analysis nor its real-hardware
/// execution tooling could supply (2026-09-12, see agent-comms/STATUS.md).
///
/// Vortex divides by `asr(*0x18063e3c, #0xa)` during its first per-frame
/// update. That field (object `0x18063e20`, offset `+0x1c`) has exactly one
/// writer anywhere in Vortex's own binary (`0x1801a318`) -- and that
/// function has ZERO callers anywhere in the binary, by direct `bl`,
/// literal-pool reference, or computed PC-relative address (confirmed
/// independently by two different static-analysis passes). So on a genuine
/// real device, something OUTSIDE the game's own compiled code drives this
/// value -- almost certainly a RetailOS scheduler/interrupt mechanism ClickHLE
/// does not model, the same general shape as the vtable-constructor gap
/// documented in RENDERER.md, just one level further into the firmware than
/// either static reading or live firmware execution could resolve this
/// session. The field's value at the point of the crash is a small,
/// unremarkable-looking number (as low as 4 in one observed run) -- >> 10
/// zeroes anything under 1024, so this is the field failing to have
/// accumulated past that threshold yet, not corruption.
///
/// Identifies Vortex by BOTH its entry point and its exact image size,
/// deliberately over-specific: this must never fire for a different title
/// that happens to load at the same base address with unrelated content at
/// this same offset.
///
/// Tried seeding the DATA first (writing a plausible value to the field
/// itself, both once at load and again every frame) and it did not survive
/// to the crash either way -- `--watch-read` showed the field still reading
/// its own small, oscillating values right up to the divide, meaning
/// something inside the SAME frame's execution overwrites our write before
/// the read, and our own code only regains control between frames, not
/// mid-frame. So patching the CODE instead: the single instruction that
/// computes the divisor, replaced with one that always loads a safe value.
/// A code patch doesn't have this race -- Vortex's own CRT never
/// re-initialises its .text section the way it clears .bss, so once
/// written this simply is the instruction from then on.
///
/// `0x18010aa0` is `asr r1, r8, #0xa` (confirmed against the raw file
/// bytes: `0xe1a01548`) -- replaced with `mov r1, #1` (`0xe3a01001`), a
/// standard, unambiguous ARM encoding. Guarantees a nonzero divisor
/// regardless of what real mechanism was supposed to populate `r8`'s
/// source, without needing to win a timing race against it.
constexpr GuestAddr kVortexEntryPoint = 0x18021798;
constexpr u32 kVortexImageSize = 414600;
constexpr GuestAddr kVortexDivisorInstruction = 0x18010aa0;
constexpr u32 kVortexDivisorInstructionOriginal = 0xe1a01548;
constexpr u32 kVortexDivisorInstructionPatched = 0xe3a01001;

/// Size of the structure handed to vector[4]. A guess; see run_game().
constexpr u32 kAppBlockBytes = 1024;
/// The second vec4 argument is the first plus this. See run_game().
constexpr u32 kArgBlockOffset = 0x100;

/// ARM semihosting, `SVC 0x123456` in ARM state.
///
/// Not an iPod thing at all -- it is the debug channel armcc emits, and these
/// games shipped with it left in. The Sims titles reach it during startup and
/// simply stopped, because an unrecognised SVC ends the run.
///
/// Worth implementing properly rather than swallowing: this is the guest's
/// stdout, so assertion messages and library diagnostics come through it. It is
/// a documented, stable interface, which makes it one of the few things in this
/// project that does not have to be guessed at.
constexpr u32 kSemihostSvc = 0x123456;

/// The operations a statically linked armcc runtime actually issues.
enum SemihostOp : u32 {
    kSysOpen = 0x01,
    kSysClose = 0x02,
    kSysWriteC = 0x03,
    kSysWrite0 = 0x04,
    kSysWrite = 0x05,
    kSysRead = 0x06,
    kSysIsTty = 0x09,
    kSysSeek = 0x0A,
    kSysFlen = 0x0C,
    kSysClock = 0x10,
    kSysTime = 0x11,
    kSysErrno = 0x13,
    kSysGetCmdline = 0x15,
    kSysHeapInfo = 0x16,
    kSysExit = 0x18,
};

void fail(std::string* error, std::string message) {
    CLICKHLE_ERROR(kCategory) << message;
    if (error != nullptr) {
        *error = std::move(message);
    }
}

}  // namespace

void Emulator::set_videocore_engine(VideoCoreEngine engine) {
    config_.videocore_engine = engine;
    if (videocore_) videocore_->set_engine(engine);
}

bool Emulator::load(Config config, std::string* error) {
    config_ = std::move(config);
    videocore_ = std::make_unique<VideoCore>(config_.capture_videocore,
                                              config_.videocore_engine);
    system_clock_start_ = std::chrono::steady_clock::now();
    last_system_clock_us_ = 0;
    metadata_state_ = {};
    audio_state_ = {};
    input_state_ = {};
    misc_state_ = {};
    filesystem_state_ = {};
    game_driver_ready_ = false;
    single_frame_mode_ = false;
    game_main_vector_ = 0;
    game_app_block_ = 0;
    game_event_pool_ = 0;
    game_io_pool_ = 0;
    game_frame_index_ = 0;
    videocore_sequence_ = 0;
    videocore_events_.clear();
    videocore_break_event_.reset();

    content_root_ = config_.content_root;
    if (content_root_.empty()) {
        content_root_ = config_.executable.parent_path();
        if (content_root_.empty()) {
            content_root_ = std::filesystem::current_path();
        }
    }

    image_ = EappImage::load_file(config_.executable, error, config_.platform_id);
    if (!image_.has_value()) {
        return false;
    }

    if (!image_->map_into(memory_, config_.image_arena_end, error)) {
        return false;
    }

    // See kVortexDivisorInstructionPatched for the full writeup. A one-shot
    // code patch, applied once here rather than every frame like the data
    // approach that didn't work -- code isn't re-initialised the way .bss
    // is, so this doesn't need reapplying.
    if (image_->entry_point() == kVortexEntryPoint &&
        image_->size_of_image() == kVortexImageSize) {
        const auto original =
            memory_.read<u32>(kVortexDivisorInstruction);
        if (original.value_or(0) != kVortexDivisorInstructionOriginal) {
            fail(error,
                 "Vortex divisor-patch site doesn't match the expected "
                 "instruction -- refusing to patch a build this wasn't "
                 "verified against");
            return false;
        }
        if (!memory_.write<u32>(kVortexDivisorInstruction,
                                kVortexDivisorInstructionPatched)) {
            fail(error, "failed to apply Vortex's divisor-safety patch");
            return false;
        }
        CLICKHLE_WARN(kCategory)
            << "Vortex-specific workaround active: patched the divisor "
               "computation at 0x"
            << std::hex << kVortexDivisorInstruction << std::dec
            << " to always be nonzero (real producer unidentified -- see "
               "emulator.cpp)";
    }

    // See kLowMirrorBase: real hardware mirrors SDRAM here rather than
    // faulting. Mapped read-only (a title should never legitimately write
    // its own reset vector) and seeded with the real bytes oracle measured;
    // the rest of the window stays zero, same as any freshly mapped region.
    if (!memory_.map(kLowMirrorBase, kLowMirrorSize, AddressSpace::kRead,
                     "low-mirror")) {
        fail(error, "failed to map the low-memory mirror");
        return false;
    }
    if (!memory_.write_bytes(kLowMirrorBase, kLowMirrorRealBytes,
                             sizeof(kLowMirrorRealBytes))) {
        fail(error, "failed to seed the low-memory mirror");
        return false;
    }

    if (!memory_.map(config_.stack_base, config_.stack_size,
                     AddressSpace::kRW, "stack")) {
        fail(error, "failed to map the guest stack");
        return false;
    }
    if (!memory_.map(config_.heap_base, config_.heap_size, AddressSpace::kRW,
                     "heap")) {
        fail(error, "failed to map the guest heap");
        return false;
    }
    // Windows CE's user-visible kernel data page. Mapped writable because the
    // TLS slots the guest reaches through it live inside the same page.
    if (!memory_.map(Config::kKDataBase, Config::kKDataSize, AddressSpace::kRW,
                     "ce-kdata")) {
        fail(error, "failed to map the CE kernel data page");
        return false;
    }
    if (!memory_.write<u32>(Config::kKDataTlsPointer, Config::kTlsSlots)) {
        fail(error, "failed to install the TLS pointer");
        return false;
    }
    // The main thread's handle, published before any guest code runs: a game's
    // CRT registers its first per-thread data block during startup, keyed on
    // whatever it reads here, and every later lookup from the main thread has
    // to agree with it.
    if (!memory_.write<u32>(Config::kKDataCurrentThread, kMainThreadHandle)) {
        fail(error, "failed to install the current thread handle");
        return false;
    }

    if (!memory_.map(config_.thread_stack_base, config_.thread_stack_arena,
                     AddressSpace::kRW, "thread-stacks")) {
        fail(error, "failed to map the thread stack arena");
        return false;
    }

    // The guest filesystem. Rooted at the game directory rather than the
    // Executables/ directory the image sits in -- titles ask for assets by
    // bare name and those live one level up. See Vfs::mount_for_executable.
    if (config_.content_root.empty()) {
        vfs_.mount_for_executable(config_.executable);
    } else {
        vfs_.mount(config_.content_root);
    }
    // Saves go beside the emulator, never into the game directory. A corpus
    // that quietly diverges from the one that was dumped is a corpus you can no
    // longer trust, and a title saving over an asset it shipped with would do
    // exactly that.
    if (config_.save_root.empty()) {
        vfs_.set_save_root(std::filesystem::current_path() / "saves" /
                           vfs_.root().filename());
    } else {
        vfs_.set_save_root(config_.save_root);
    }

    heap_.initialise(config_.heap_base, config_.heap_size);
    scheduler_.initialise(config_.thread_stack_base, config_.thread_stack_arena,
                          config_.thread_stack_size, Config::kKDataTlsPointer,
                          Config::kKDataCurrentThread,
                          Config::kTlsSlotCount * 4);

    if (!thunks_.initialise(memory_)) {
        fail(error, "failed to map the HLE thunk page");
        return false;
    }

    register_all_modules(registry_);
    if (videocore_) {
        // Every firmware export can be dispatched without a host GL handler.
        // Execution may still stop at an unsupported firmware dependency.
        for (u32 ordinal = 0; ordinal < 179; ++ordinal) {
            if (!registry_.find("OpenGLES", ordinal)) {
                registry_.add("OpenGLES", ordinal, "native_graphics_" + std::to_string(ordinal), 0,
                              [](HleCallFrame& frame) { frame.cpu.request_stop(); });
            }
        }
    }

    if (!bind_imports(error)) {
        return false;
    }

    cpu_ = make_cpu_core(config_.cpu_backend, memory_);
    if (cpu_ == nullptr) {
        fail(error, "no usable CPU backend");
        return false;
    }
    cpu_->reset();
    cpu_->set_svc_handler([this](u32 immediate) { return on_svc(immediate); });
    cpu_->set_pc(image_->entry_point());
    cpu_->set_sp(config_.stack_base + config_.stack_size - kStackHeadroom);
    cpu_->set_lr(ThunkTable::kReturnSentinel);

    scheduler_.adopt_main_thread(kMainThreadHandle, Config::kTlsSlots, *cpu_);

    CLICKHLE_INFO(kCategory)
        << "loaded " << config_.executable.filename().string() << " entry=0x"
        << std::hex << image_->entry_point() << std::dec << " cpu="
        << cpu_->name();
    return true;
}

namespace {

/// Reads a NUL-terminated guest string, bounded so a bad pointer cannot make us
/// walk the whole address space.
std::string read_guest_string(const AddressSpace& memory, GuestAddr address,
                              u32 limit = 4096) {
    std::string out;
    for (u32 i = 0; i < limit; ++i) {
        const auto byte = memory.read<u8>(address + i);
        if (!byte.has_value() || *byte == 0) {
            break;
        }
        out.push_back(static_cast<char>(*byte));
    }
    return out;
}

}  // namespace

bool Emulator::on_semihost() {
    const u32 op = cpu_->reg(0);
    const GuestAddr block = cpu_->reg(1);

    switch (op) {
        case kSysWriteC: {
            const auto byte = memory_.read<u8>(block);
            if (byte.has_value()) {
                semihost_line_.push_back(static_cast<char>(*byte));
                flush_semihost_line(false);
            }
            cpu_->set_reg(0, 0);
            return true;
        }
        case kSysWrite0: {
            semihost_line_ += read_guest_string(memory_, block);
            flush_semihost_line(false);
            cpu_->set_reg(0, 0);
            return true;
        }
        case kSysWrite: {
            // { handle, buffer, length } -- returns bytes NOT written, so zero
            // means everything was.
            const auto buffer = memory_.read<u32>(block + 4);
            const auto length = memory_.read<u32>(block + 8);
            if (buffer.has_value() && length.has_value()) {
                for (u32 i = 0; i < *length; ++i) {
                    const auto byte = memory_.read<u8>(*buffer + i);
                    if (!byte.has_value()) {
                        break;
                    }
                    semihost_line_.push_back(static_cast<char>(*byte));
                }
                flush_semihost_line(false);
            }
            cpu_->set_reg(0, 0);
            return true;
        }
        case kSysIsTty:
            cpu_->set_reg(0, 1);  // yes: keeps the runtime unbuffered
            return true;
        case kSysOpen:
            // Only the console is offered. A real file open goes through the
            // firmware's own file API, not through the debug channel, so a
            // guest reaching here wants stderr and nothing more.
            cpu_->set_reg(0, 1);
            return true;
        case kSysClose:
        case kSysFlen:
        case kSysSeek:
            cpu_->set_reg(0, 0);
            return true;
        case kSysErrno:
            cpu_->set_reg(0, 0);
            return true;
        case kSysClock:
            cpu_->set_reg(0, tick_count() / 10);  // centiseconds, per the spec
            return true;
        case kSysTime:
            // Seconds since the epoch. Derived from the emulated tick rather
            // than the host clock, so runs stay reproducible.
            cpu_->set_reg(0, tick_count() / 1000);
            return true;
        case kSysExit:
            flush_semihost_line(true);
            CLICKHLE_INFO(kCategory) << "guest exited via semihosting";
            return false;
        case kSysHeapInfo:
        case kSysGetCmdline:
            // Both write through a caller-supplied block. Left alone: the
            // caller zeroes it first, and a wrong answer here is worse than
            // none.
            cpu_->set_reg(0, 0);
            return true;
        default:
            CLICKHLE_WARN(kCategory)
                << "unimplemented semihosting op 0x" << std::hex << op
                << std::dec;
            cpu_->set_reg(0, 0);
            return true;
    }
}

void Emulator::flush_semihost_line(bool force) {
    // Emitted a line at a time. The guest writes a character at a time, and one
    // log line per character would bury everything else.
    for (;;) {
        const auto newline = semihost_line_.find('\n');
        if (newline == std::string::npos) {
            break;
        }
        CLICKHLE_INFO("guest") << semihost_line_.substr(0, newline);
        semihost_line_.erase(0, newline + 1);
    }
    if (force && !semihost_line_.empty()) {
        CLICKHLE_INFO("guest") << semihost_line_;
        semihost_line_.clear();
    }
}

void Emulator::queue_completion(GuestAddr request, const char* origin) {
    CLICKHLE_DEBUG(kCategory)
        << "queue completion for 0x" << std::hex << request << std::dec
        << " from " << (origin != nullptr ? origin : "?");
    pending_completions_.push_back(request);
}

void Emulator::deliver_completions(u64 max_instructions) {
    if (!completions_enabled_) {
        pending_completions_.clear();
        return;
    }
    if (pending_completions_.empty()) {
        return;
    }
    // Swapped out first: a callback may itself start another operation, and
    // appending to the container being iterated would invalidate the loop. The
    // new completion is picked up on the next drain, which is also when the
    // device would have delivered it.
    std::vector<GuestAddr> ready;
    ready.swap(pending_completions_);
    const GuestAddr code_low = image_.has_value() ? image_->image_base() : 0;
    const GuestAddr code_high =
        image_.has_value() ? code_low + image_->size_of_image() : 0;

    for (const GuestAddr request : ready) {
        const auto trampoline = memory_.read<u32>(request + 0x34);
        if (!trampoline.has_value() || *trampoline == 0) {
            continue;
        }
        // The word has to actually be code in this image, and it has to be
        // aligned. A request the game has already finished with gets recycled
        // as an ordinary buffer, and +0x34 of a recycled request is whatever
        // the game wrote there -- Tetris put sample data in one and the run
        // branched into the middle of a function and never returned.
        if (*trampoline < code_low || *trampoline >= code_high ||
            (*trampoline & 3u) != 0) {
            CLICKHLE_WARN(kCategory)
                << "request 0x" << std::hex << request
                << " has no usable completion (0x" << *trampoline << ")"
                << std::dec;
            continue;
        }
        // The trampoline dereferences the file object without checking it:
        // `ldr r4, [r0, #8]` and then `str r3, [r0, #8]` on the object itself.
        // A request the game has torn down between issuing it and this drain
        // has a null object there, and calling the trampoline then writes to
        // guest address 8. Texas Hold'em does exactly that -- it recycles the
        // request within the frame, and the dump shows ASCII text where the
        // structure used to be.
        const auto object = memory_.read<u32>(request + 0x08);
        if (!object.has_value() || *object == 0) {
            CLICKHLE_DEBUG(kCategory)
                << "request 0x" << std::hex << request
                << " was torn down before its completion; dropped" << std::dec;
            continue;
        }
        CLICKHLE_DEBUG(kCategory)
            << "completion for request 0x" << std::hex << request << " -> 0x"
            << *trampoline << std::dec;
        (void)call_guest(*trampoline, {request}, max_instructions);
    }
}

bool Emulator::install_probe(const std::string& module, u32 ordinal, u8 arity,
                             HleHandler handler) {
    const HleFunction* function =
        registry_.add_shadow(module, ordinal, "probe", arity,
                             std::move(handler));
    if (function == nullptr) {
        return false;
    }
    // The key has to match what the loader bound, which is GuestImport::key().
    const std::string key = module + "#" + std::to_string(ordinal);
    if (!thunks_.rebind(key, function)) {
        CLICKHLE_WARN(kCategory)
            << key << " is not imported by this image; probe ignored";
        return false;
    }
    return true;
}

bool Emulator::bind_imports(std::string* error) {
    for (const auto& import : image_->imports()) {
        // Ordinals from another framework version cannot be dispatched to
        // the Platform 1 registry. Keep the thunk so the image loads, but
        // stop on its first call until that ABI has been established.
        const HleFunction* function = import.compatible_abi
            ? registry_.resolve(import) : nullptr;
        const auto thunk = thunks_.bind(import.key(), function, memory_);
        if (!thunk.has_value()) {
            fail(error, "failed to bind thunk for " + import.key());
            return false;
        }
        // Patch the image's import pointer table, exactly where the firmware
        // would have written a real function address. The stub the guest calls
        // is `LDR PC, [PC, #imm]` reading this word, so it lands on our SVC.
        if (!memory_.write<u32>(import.slot_address, *thunk)) {
            fail(error, "failed to patch the import slot for " + import.key());
            return false;
        }
    }
    CLICKHLE_DEBUG(kCategory) << "bound " << thunks_.bound_count() << " thunks";
    return true;
}

bool Emulator::on_svc(u32 immediate) {
    if (immediate == kSemihostSvc) {
        return on_semihost();
    }
    if (immediate == ThunkTable::kReturnSvc) {
        return_sentinel_hit_ = true;
        return false;  // unwind out of run(); call_guest() collects r0
    }
    if (immediate == Scheduler::kThreadExitSvc) {
        // A thread's entry point returned. Switch away from it.
        if (scheduler_.exit_current(*cpu_, tick_count(), memory_)) {
            return true;
        }
        // Nothing was runnable to switch to, which is not the same as the run
        // being over: the other threads may only be sleeping. Park, exactly as
        // ExitThread does, and let the run loop idle until one comes due --
        // any_alive() is what decides the run is finished.
        //
        // Reachable as soon as waits genuinely block. the title that produced this bug starts its
        // card thread, the main thread waits INFINITE for it, and the card
        // thread returns while the main thread is still asleep: the run ended
        // there, 106 HLE calls in, having drawn nothing.
        thread_parked_ = true;
        return false;
    }

    const ThunkTable::Entry* entry = thunks_.lookup(immediate);
    if (entry == nullptr) {
        CLICKHLE_ERROR(kCategory) << "SVC #" << immediate << " has no thunk entry";
        return false;
    }

    ++hle_calls_;
    ++call_counts_[entry->import_key];

    if (entry->function == nullptr) {
        ++hle_missing_calls_;
        last_missing_import_ = entry->import_key;
        CLICKHLE_ERROR(kCategory)
            << "unimplemented import called: " << entry->import_key;
        return false;
    }

    HleCallFrame frame(*cpu_, memory_, *this);
    CLICKHLE_TRACE(kCategory) << "-> " << entry->function->name;
    call_history_[call_history_next_] =
        "t" + std::to_string(scheduler_.current_handle() & 0xFFFF) + ": " +
        entry->function->name;
    call_history_next_ = (call_history_next_ + 1) % kCallHistory;

    std::optional<VideoCoreEvent> videocore_event;
    if (videocore_trace_enabled_ &&
        (is_videocore_boundary_call(*entry->function) ||
         (videocore_ && entry->function->module == "OpenGLES" && entry->function->has_ordinal))) {
        VideoCoreEvent event;
        event.sequence = ++videocore_sequence_;
        event.frame = game_frame_index_;
        event.instructions = cpu_->instruction_count();
        event.return_pc = cpu_->lr();
        event.ordinal = entry->function->ordinal;
        event.name = entry->function->name;
        event.arg_count = videocore_ ? 4 : std::min<u8>(entry->function->arity, 4);
        for (u8 i = 0; i < event.arg_count; ++i) {
            event.args[i] = cpu_->reg(i);
        }

        GuestAddr data = 0;
        switch (event.ordinal) {
            case 148: data = event.args[2]; event.payload_size = 16; break;
            case 149: data = event.args[3]; event.payload_size = 64; break;
            case 160:
                data = event.args[2];
                event.payload_size = event.args[1];
                break;
            case 164:
                data = event.args[1];
                event.payload_size = event.args[2];
                break;
            default: break;
        }
        if (data != 0 && event.payload_size != 0) {
            event.payload_prefix_size = static_cast<u8>(
                std::min<u32>(event.payload_size, event.payload_prefix.size()));
            if (!memory_.read_bytes(data, event.payload_prefix.data(),
                                    event.payload_prefix_size)) {
                event.payload_prefix_size = 0;
            }
        }
        videocore_event = std::move(event);
    }

    // A handler may switch threads (Sleep and the WaitFor* family do). If it
    // does, the CPU is now running a different context whose pc is already
    // correct, and forcing pc = lr below would overwrite it with the *new*
    // thread's return address -- resuming it in the wrong place.
    //
    // The yielding thread does not need the fixup either: its saved pc is the
    // instruction after its SVC, which is exactly where it should resume.
    // CLICKHLE_FIND_PTR=<addr>: report every HLE call that carries <addr> in
    // r0-r3 or the first few stack words.
    //
    // Answers "does this pointer ever cross the ARM/framework boundary at
    // all", which is the question you hit when a guest structure is supposed
    // to be filled in by firmware and never is -- if the game never hands us
    // the address, we were never asked to populate it, and the gap is
    // somewhere else entirely. Scanning for one known value is far cheaper
    // than dumping every argument of ~14000 calls and reading it back.
    {
        static const u32 wanted = [] {
            const char* v = std::getenv("CLICKHLE_FIND_PTR");
            return v != nullptr
                       ? static_cast<u32>(std::strtoul(v, nullptr, 0))
                       : 0u;
        }();
        if (wanted != 0) {
            std::string where;
            for (unsigned i = 0; i < 4; ++i) {
                if (cpu_->reg(i) == wanted) {
                    where += " r" + std::to_string(i);
                }
            }
            for (u32 i = 0; i < 6; ++i) {
                if (memory_.read<u32>(cpu_->sp() + i * 4).value_or(0) ==
                    wanted) {
                    where += " sp+" + std::to_string(i * 4);
                }
            }
            if (!where.empty()) {
                CLICKHLE_INFO(kCategory)
                    << "FIND_PTR 0x" << std::hex << wanted << std::dec
                    << " in " << entry->function->name << " at" << where;
            }
        }
    }
    const u32 thread_before = scheduler_.current_handle();
    const GuestAddr thunk_before = current_thunk_;
    current_thunk_ = entry->address;
    if (videocore_ && entry->function->module == "OpenGLES" && entry->function->has_ordinal) {
        if (!videocore_->call(entry->function->ordinal, frame)) {
            CLICKHLE_ERROR("vc2") << videocore_->error();
            last_guest_call_failure_ = videocore_->error();
            cpu_->request_stop();
        } else if (!single_frame_mode_ && frame_limit_ &&
                   videocore_->frames() >= frame_limit_) {
            // A GUI tick already stops after its callback returns. Stopping
            // inside presentation would replay that same GL call next tick.
            cpu_->request_stop();
        }
    } else if (videocore_ && entry->function->module == "gl") {
        last_guest_call_failure_ = "Named GL entry points are not supported by the VC2 backend";
        cpu_->request_stop();
    } else {
        entry->function->handler(frame);
    }
    current_thunk_ = thunk_before;
    if (videocore_event.has_value()) {
        videocore_event->result = cpu_->reg(reg::kR0);
        videocore_events_.push_back(*videocore_event);
        constexpr std::size_t kVideoCoreHistory = 1024;
        if (videocore_events_.size() > kVideoCoreHistory) {
            videocore_events_.pop_front();
        }
        if (videocore_pause_on_call_) {
            videocore_break_event_ = *videocore_event;
        }
    }
    if (!frame.args.ok()) {
        CLICKHLE_WARN(kCategory)
            << entry->function->name
            << ": argument read ran off the stack (declared arity likely wrong)";
    }
    // A handler that blocked has already pointed pc at this same thunk so the
    // call is re-issued; leave it alone.
    if (scheduler_.current_handle() != thread_before || call_reissued_) {
        call_reissued_ = false;
        return true;
    }

    // Classify what this call handed back, for the opaque-handle audit.
    {
        const u32 result = cpu_->reg(reg::kR0);
        ++return_audit_.calls;
        if (result == 0) {
            ++return_audit_.zero;
        } else if (memory_.read<u8>(result).has_value()) {
            // The case that matters: a value a title could dereference and get
            // something other than a fault.
            ++return_audit_.mapped;
            ++return_audit_.mapped_by_import[entry->import_key];
        } else {
            ++return_audit_.small;
        }
    }

    // Return to the caller. The guest reached the thunk via a branch-with-link,
    // so lr holds the return address.
    cpu_->set_pc(cpu_->lr());
    return true;
}

bool Emulator::yield_thread() {
    if (cpu_ == nullptr || call_depth_ > 0) {
        return false;
    }
    const CpuContext before = cpu_->save_context();
    cpu_->set_pc(cpu_->lr());
    if (scheduler_.yield_now(*cpu_, tick_count(), memory_)) {
        return true;
    }
    // No switch happened, so leave the CPU exactly as the handler found it and
    // let on_svc do its usual pc = lr.
    cpu_->restore_context(before);
    return false;
}

void Emulator::skip_idle_to(u32 due, u32 tick) {
    // Credit the cycles the guest would have burned reaching `due`. Runs stay
    // reproducible because this depends only on guest state, never on the host
    // clock -- the same executable skips the same intervals every time.
    if (static_cast<i32>(due - tick) <= 0) {
        // Nothing to skip, but the caller expects forward progress; charge a
        // single millisecond so an idle loop cannot spin forever.
        idle_cycles_ += config_.emulated_hz / 1000;
        return;
    }
    const u64 milliseconds = static_cast<u32>(due - tick);
    idle_cycles_ += milliseconds * config_.emulated_hz / 1000;
}

bool Emulator::block_thread(u32 milliseconds) {
    if (cpu_ == nullptr || call_depth_ > 0 || current_thunk_ == 0) {
        return false;
    }
    // Send the thread back to its own SVC instruction rather than to lr, so it
    // re-enters this handler with its arguments still in place.
    cpu_->set_pc(current_thunk_);
    scheduler_.sleep_current(milliseconds, tick_count());
    call_reissued_ = true;
    if (scheduler_.yield_now(*cpu_, tick_count(), memory_)) {
        return true;
    }
    // Nothing else is runnable this instant, but that does not make the wait
    // satisfiable -- the other threads are only sleeping. Park the CPU rather
    // than pretend the wait timed out: the run loop idles until a thread comes
    // due and re-enters, and this call is issued again. Falling back to
    // WAIT_TIMEOUT here is what left one title showing "SD card removed"
    // over its language menu.
    thread_parked_ = true;
    cpu_->request_stop();
    return true;
}

void Emulator::exit_current_thread() {
    if (cpu_ == nullptr) {
        return;
    }
    if (!scheduler_.exit_current(*cpu_, tick_count(), memory_)) {
        thread_parked_ = true;
        cpu_->request_stop();
    }
}

namespace {

/// A save state is a private format read only by the matching build, so it is
/// deliberately plain: a magic and version, then length-prefixed blocks. No
/// attempt at forward compatibility -- a version bump refuses to load rather
/// than guessing at a layout it does not know.
constexpr u32 kStateMagic = 0x594B5453;  // "STKY"
constexpr u32 kStateVersion = 1;

template <typename T>
void put(std::vector<std::byte>& out, T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* bytes = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

void put_bytes(std::vector<std::byte>& out, const void* data, std::size_t n) {
    const auto* bytes = static_cast<const std::byte*>(data);
    out.insert(out.end(), bytes, bytes + n);
}

template <typename T>
bool take(const std::vector<std::byte>& in, std::size_t& at, T& value) {
    if (at + sizeof(T) > in.size()) return false;
    std::memcpy(&value, in.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
}

}  // namespace

std::string Emulator::save_state(const std::filesystem::path& path) {
    if (videocore_) return "Save states are not supported by the experimental VC2 backend";
    if (cpu_ == nullptr) {
        return "nothing loaded";
    }
    // Modules holding host resources -- an open FILE*, a queued audio buffer
    // -- must refuse a snapshot rather than write a pointer out as a number.
    // Nothing holds one yet; the check comes back with the module that does.

    std::vector<std::byte> out;
    put(out, kStateMagic);
    put(out, kStateVersion);

    const CpuContext context = cpu_->save_context();
    put_bytes(out, &context, sizeof(context));
    put(out, idle_cycles_);
    put(out, hle_calls_);
    put(out, random_seed_);

    const auto& regions = memory_.regions();
    put(out, static_cast<u32>(regions.size()));
    for (const auto& region : regions) {
        put(out, region->base);
        put(out, region->size);
        put(out, region->prot);
        put(out, static_cast<u32>(region->data.size()));
        put_bytes(out, region->data.data(), region->data.size());
    }

    const auto& threads = scheduler_.threads();
    put(out, static_cast<u32>(threads.size()));
    put(out, static_cast<u32>(scheduler_.current_index()));
    for (const auto& thread : threads) {
        put(out, thread.handle);
        put_bytes(out, &thread.context, sizeof(thread.context));
        put(out, thread.stack_base);
        put(out, thread.stack_size);
        put(out, thread.tls_slots);
        put(out, static_cast<u8>(thread.started));
        put(out, static_cast<u8>(thread.finished));
        put(out, thread.wake_at);
        put(out, thread.suspended);
        put(out, thread.exit_code);
    }

    const auto blocks = heap_.snapshot();
    put(out, static_cast<u32>(blocks.size()));
    for (const auto& [address, block] : blocks) {
        put(out, address);
        put(out, block.first);
        put(out, static_cast<u8>(block.second));
    }

    // Open-file count, always zero until a file module exists again. Written
    // so the format does not have to change when one does.
    put(out, static_cast<u32>(0));

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return "cannot open " + path.string() + " for writing";
    }
    file.write(reinterpret_cast<const char*>(out.data()),
               static_cast<std::streamsize>(out.size()));
    if (!file) {
        return "write failed";
    }
    CLICKHLE_INFO(kCategory) << "saved state to " << path << " ("
                           << out.size() / 1024 << " KB)";
    return {};
}

std::string Emulator::load_state(const std::filesystem::path& path) {
    if (videocore_) return "Save states are not supported by the experimental VC2 backend";
    if (cpu_ == nullptr) {
        return "nothing loaded";
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return "cannot open " + path.string();
    }
    const std::string raw((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
    std::vector<std::byte> in(raw.size());
    std::memcpy(in.data(), raw.data(), raw.size());

    std::size_t at = 0;
    u32 magic = 0;
    u32 version = 0;
    if (!take(in, at, magic) || !take(in, at, version)) return "truncated";
    if (magic != kStateMagic) return "not a ClickHLE save state";
    if (version != kStateVersion) {
        return "save state version " + std::to_string(version) +
               ", this build reads " + std::to_string(kStateVersion);
    }

    CpuContext context;
    if (at + sizeof(context) > in.size()) return "truncated";
    std::memcpy(&context, in.data() + at, sizeof(context));
    at += sizeof(context);
    if (!take(in, at, idle_cycles_) || !take(in, at, hle_calls_) ||
        !take(in, at, random_seed_)) {
        return "truncated";
    }

    u32 region_count = 0;
    if (!take(in, at, region_count)) return "truncated";
    for (u32 i = 0; i < region_count; ++i) {
        GuestAddr base = 0;
        u32 size = 0;
        u32 prot = 0;
        u32 bytes = 0;
        if (!take(in, at, base) || !take(in, at, size) || !take(in, at, prot) ||
            !take(in, at, bytes) || at + bytes > in.size()) {
            return "truncated";
        }
        // Regions are recreated by load(), so this restores contents into the
        // mapping that is already there rather than remapping.
        if (!memory_.write_bytes(base, in.data() + at, bytes)) {
            return "state describes a region this build does not map at 0x" +
                   std::to_string(base);
        }
        at += bytes;
    }

    u32 thread_count = 0;
    u32 current = 0;
    if (!take(in, at, thread_count) || !take(in, at, current)) return "truncated";
    std::vector<Scheduler::Thread> threads;
    threads.reserve(thread_count);
    for (u32 i = 0; i < thread_count; ++i) {
        Scheduler::Thread thread;
        u8 started = 0;
        u8 finished = 0;
        if (!take(in, at, thread.handle)) return "truncated";
        if (at + sizeof(thread.context) > in.size()) return "truncated";
        std::memcpy(&thread.context, in.data() + at, sizeof(thread.context));
        at += sizeof(thread.context);
        if (!take(in, at, thread.stack_base) ||
            !take(in, at, thread.stack_size) ||
            !take(in, at, thread.tls_slots) || !take(in, at, started) ||
            !take(in, at, finished) || !take(in, at, thread.wake_at) ||
            !take(in, at, thread.suspended) || !take(in, at, thread.exit_code)) {
            return "truncated";
        }
        thread.started = started != 0;
        thread.finished = finished != 0;
        threads.push_back(std::move(thread));
    }
    scheduler_.restore_threads(std::move(threads), current);

    u32 block_count = 0;
    if (!take(in, at, block_count)) return "truncated";
    std::vector<std::pair<GuestAddr, std::pair<u32, bool>>> blocks;
    blocks.reserve(block_count);
    for (u32 i = 0; i < block_count; ++i) {
        GuestAddr address = 0;
        u32 size = 0;
        u8 used = 0;
        if (!take(in, at, address) || !take(in, at, size) ||
            !take(in, at, used)) {
            return "truncated";
        }
        blocks.emplace_back(address, std::make_pair(size, used != 0));
    }
    heap_.restore(blocks);

    u32 file_count = 0;
    if (!take(in, at, file_count)) return "truncated";
    if (file_count != 0) {
        return "state holds open files but no file module is registered";
    }

    cpu_->restore_context(context);
    CLICKHLE_INFO(kCategory) << "restored state from " << path;
    return {};
}

AudioDevice* Emulator::open_audio(u32 sample_rate, u16 channels, u16 bits) {
    // Mix all voices in one stable host format. Each source is converted by
    // AudioDevice before it enters the mixer, so a title can play different
    // WAV rates and channel layouts without reopening the output device.
    (void)sample_rate;
    (void)channels;
    (void)bits;
    constexpr u32 kMixRate = 44100;
    constexpr u16 kMixChannels = 2;
    constexpr u16 kMixBits = 16;
    if (!audio_) {
        auto device = AudioDevice::create();
        if (!device) {
            return nullptr;
        }
        if (!device->open(kMixRate, kMixChannels, kMixBits)) {
            return nullptr;
        }
        audio_ = std::move(device);
        audio_rate_ = kMixRate;
        audio_channels_ = kMixChannels;
        audio_bits_ = kMixBits;
        CLICKHLE_INFO(kCategory)
            << "audio opened " << kMixRate << " Hz, " << kMixChannels
            << " ch, " << kMixBits << "-bit mix (" << audio_->backend() << ")";
    }
    return audio_.get();
}

RunResult Emulator::run_game(u64 max_instructions) {
    // Startup, as the vectors describe it.
    //
    // vector[0] is the static-constructor runner: the same compiler-generated
    // prologue appears at it in every title, and it returns after a few tens of
    // thousands of instructions with nothing left to do.
    //
    // vector[4] is the game proper, and takes two arguments. Its prologue is
    // also identical across titles -- `push {r2-sl, lr}; mov r4, r0;
    // add r0, r0, #4; mov r5, r1` -- so it reads a structure through r0 and
    // keeps r1.
    //
    // vector[1] is optional: real code in Lost, a bare `BX LR` in Tetris and
    // Pac-Man. Treating it as "the frame hook" and calling it in a loop is what
    // made nine titles look like they were running while doing nothing at all.
    constexpr u32 kMaxEventsPerFrame = 8;
    constexpr u32 kEventRecordBytes = 0x0c;
    RunResult total;
    if (!game_driver_ready_) {
        total = run(max_instructions);
        if (total.status == RunStatus::Fault ||
            total.status == RunStatus::Unsupported) {
            return total;
        }
        if (!image_.has_value() || hle_missing_calls_ > 0) {
        // Construction did not finish. Starting the game on top of a half-built
        // static state makes every later failure a consequence of this one.
            return total;
        }

        game_main_vector_ = image_->vectors()[4];
        if (game_main_vector_ == 0) {
            CLICKHLE_INFO(kCategory)
                << "image declares no vector[4]; nothing to run after construction";
            return total;
        }

        CLICKHLE_INFO(kCategory)
            << "constructors returned after " << total.instructions_executed
            << " instructions; entering vector[4] at 0x" << std::hex
            << game_main_vector_ << std::dec;

    // vector[4](struct*, u32).
    //
    // r0 is a structure the firmware owns and the game writes into: every title
    // begins `mov r4, r0; add r0, r0, #4` and passes that to the clock, so it
    // stores the current time at +4. Passing null faults on the first store, at
    // guest address 0x4, which is how the argument was identified at all.
    //
    // Its size and the meaning of its other fields are unknown, and so is r1.
    // A zeroed block is enough while the game only writes to it; the moment it
    // reads a field back expecting something, this becomes wrong and the fault
    // will say where. Sized generously because the cost of being short is a
    // silent overwrite of whatever follows.
    game_app_block_ = heap_.allocate(kAppBlockBytes);
    if (game_app_block_ == kNullGuest) {
        total.status = RunStatus::Fault;
        total.fault = "could not allocate the vector[4] argument block";
        return total;
    }
    const GuestAddr app_block = game_app_block_;
    for (u32 offset = 0; offset < kAppBlockBytes; offset += 4) {
        (void)memory_.write<u32>(app_block + offset, 0);
    }

    // app[0x249] -- the status byte that selects WHICH vector the frame driver
    // calls: 1 -> vec2, 2 -> vec4, anything else -> the frame does nothing.
    //
    // The firmware derives it from which vectors the image declares:
    //
    //     if      (vec2) app[0x249] = 1;
    //     else if (vec4) app[0x249] = 2;
    //
    // Every title in this corpus has vec2 == 0 and vec4 != 0, so all of them
    // are status 2 -- which is why driving vec4 unconditionally has worked. It
    // is still a state the firmware has and we did not, so it is written here
    // and honoured below rather than assumed.
    const GuestAddr vec2 = image_->vectors()[2];
    const u8 status_byte =
        vec2 != 0 ? 1 : (game_main_vector_ != 0 ? 2 : 0);
    (void)memory_.write<u8>(app_block + 0x249, status_byte);
    if (status_byte != 2) {
        CLICKHLE_WARN(kCategory)
            << "image is status " << static_cast<u32>(status_byte)
            << "; the frame driver would call vec2, which nothing in this "
               "corpus does and which is not implemented";
    }

    // Called once per frame.
    //
    // The callback does a whole frame and returns: Tetris allocates, polls
    // input, clears, swaps and comes back, all in 22 firmware calls. Nothing
    // animates unless it is called again, and the firmware is what calls it.
    //
    // An earlier revision called it once, after looping it made several titles
    // spin. That was with five ordinals implemented and the input poll handing
    // back uninitialised stack -- a game re-entered with a randomly flicking
    // wheel does spin. With input and the clock behaving, looping is what the
    // vector is for.
    //
    // r1 is a pointer too, not a constant: Tetris reads through it at
    // 0x180222c0 and dies at guest address 0 when it is null. Both blocks are
    // allocated once and reused across frames, because a game that stored
    // something in them last frame must find it there this frame.
    // The second argument is NOT a separate allocation. The firmware's frame
    // driver calls `vec4(app, app + 0x100)` -- one structure, and the second
    // pointer is 256 bytes into the first (LLE track, `0x10243ba4`).
    //
    // We had been passing two independent blocks. Nothing had visibly broken,
    // because no title had yet written through one and read it back through the
    // other -- but they alias on hardware and any title relying on that would
    // have seen two disjoint buffers.

    // The per-frame event list, exactly as the firmware's frame driver builds
    // it (LLE track, `0x10243ba4` and `FUN_10243a3c`):
    //
    //     app[0x2c] = <derived value>;
    //     app[0x30] = head of a singly-linked list of 12-byte records;
    //     vec4(app, app + 0x100);
    //     app[0x2c] = 0; app[0x30] = 0; free every record;
    //
    // A record is type:u8 at +0, gate:u8 at +1 (must be 1), payload:u32 at +4,
    // next:u32 at +8 with 0 terminating. The list lives for exactly one call,
    // which is why +0x30 reads as firmware-supplied and is never game-written.
    //
    // Nodes come from a fixed pool rather than the guest heap: the firmware
    // allocates and frees each one per frame, and reproducing that churn would
    // fragment a heap we do not need to fragment.
    constexpr u32 kIoNodeBytes = 0x20;
    constexpr u32 kIoPathBytes = 128;
    game_event_pool_ =
        heap_.allocate(kEventRecordBytes * kMaxEventsPerFrame);
    game_io_pool_ = heap_.allocate(kIoNodeBytes * kMaxEventsPerFrame);
    (void)heap_.allocate(kIoPathBytes * kMaxEventsPerFrame);
    const GuestAddr io_pool = game_io_pool_;

    CLICKHLE_INFO(kCategory)
        << "app_block 0x" << std::hex << app_block << " io_pool 0x" << io_pool
        << " event_pool 0x" << game_event_pool_ << std::dec;

    game_driver_ready_ = true;
    } else {
        total.status = RunStatus::Stopped;
    }

    const GuestAddr app_block = game_app_block_;
    const GuestAddr arg_block = app_block + kArgBlockOffset;
    const GuestAddr event_pool = game_event_pool_;
    const GuestAddr main_vector = game_main_vector_;

    u64 frames = game_frame_index_;
    // The GUI advances one callback per tick, including loading callbacks
    // that present nothing. Its presentation limit cannot bound the callback
    // index: after a non-presenting callback those counters diverge.
    for (; single_frame_mode_ || frame_limit_ == 0 || frames < frame_limit_; ++frames) {
        // RetailOS samples the device before publishing its button-event list.
        poll_hardware_input(*this);
        const u64 missing_before = hle_missing_calls_;
        // Investigation hook: inject one correctly-shaped event on a cadence,
        // independent of the button plumbing, so a type/gate sweep tests the
        // record contract on its own.
        if (const char* inject = std::getenv("CLICKHLE_INJECT_EVENT")) {
            // "wheel then button": type 1 for a while, then the swept type.
            const bool sweep_phase =
                std::getenv("CLICKHLE_INJECT_AFTER_WHEEL") != nullptr;
            if (sweep_phase && frames < 120) {
                if (frames % 10 == 5) {
                    queue_input_event(1, 0, 1);
                }
            } else if (frames % 60 == 30) {
                const u32 type = static_cast<u32>(
                    std::strtoul(inject, nullptr, 0));
                const char* colon = std::strchr(inject, ':');
                const u32 gate = colon != nullptr
                    ? static_cast<u32>(std::strtoul(colon + 1, nullptr, 0))
                    : 1u;
                queue_input_event(static_cast<u8>(type), 0, static_cast<u8>(gate));
            }
        }

        // Publish this frame's input as an event list.
        u32 event_count = 0;
        if (event_pool != kNullGuest) {
            GuestAddr previous = kNullGuest;
            for (const InputEvent& event : pending_input_) {
                if (event_count >= kMaxEventsPerFrame) {
                    break;
                }
                const GuestAddr node =
                    event_pool + kEventRecordBytes * event_count;
                (void)memory_.write<u8>(node + 0, event.type);
                (void)memory_.write<u8>(node + 1, event.gate);
                // The payload is a millisecond timestamp, not a keycode -- a
                // title timing a double-click or a hold reads it, so a constant
                // would make a game behave oddly rather than not at all.
                (void)memory_.write<u32>(
                    node + 4, static_cast<u32>(microsecond_count() / 1000));
                (void)memory_.write<u32>(node + 8, 0);  // terminator
                if (previous != kNullGuest) {
                    (void)memory_.write<u32>(previous + 8, node);
                }
                previous = node;
                ++event_count;
            }
            pending_input_.clear();
        }
        if (event_count != 0) {
            CLICKHLE_DEBUG(kCategory)
                << "vec4 event list: " << event_count << " record(s) at 0x"
                << std::hex << event_pool << std::dec;
        }
        // app[0x2c] -- the completed-file-IO list for this frame.
        //
        // It is a chain of the GAME'S OWN request objects, linked through
        // request[0x00], not firmware-allocated nodes. The firmware's internal
        // node is scaffolding: the drain relinks the requests out of it and
        // destroys it, so the guest never sees it.
        //
        // That is why Mahjong reads request+0x08 and never reads a character
        // from it -- +0x08 is the *file object*, which we have documented since
        // section 24, not a path string. An earlier revision here published
        // synthetic nodes carrying a path; that was built on a layout the LLE
        // track has since retracted.
        //
        // The fields the drain relies on -- status at +0x20 and the handle at
        // +0x2c -- are already written by the open handlers, so publishing is
        // just chaining and pointing.
        GuestAddr io_head = kNullGuest;
        GuestAddr io_tail = kNullGuest;
        // A title reuses ONE request object for every operation, so the same
        // pointer can complete twice in a frame. Chaining it twice makes
        // request[0x00] point at itself, and the guest's walker then processes
        // the same completion twice -- once for an operation that has already
        // been superseded.
        std::vector<GuestAddr> published;
        for (const IoCompletion& done : pending_io_) {
            if (std::find(published.begin(), published.end(), done.request) !=
                published.end()) {
                continue;
            }
            published.push_back(done.request);
            (void)memory_.write<u32>(done.request + 0x00, 0);
            if (io_tail != kNullGuest) {
                (void)memory_.write<u32>(io_tail + 0x00, done.request);
            } else {
                io_head = done.request;
            }
            io_tail = done.request;
        }
        pending_io_.clear();
        (void)memory_.write<u32>(app_block + 0x2c,
                                 io_head == kNullGuest ? 0u : io_head);

        // app[0x00] -- the per-frame reason code, written immediately before
        // every vec4 by the firmware's frame driver:
        //
        //     app[0x230] == 1  ->  app[0x00] = 5
        //     app[0x230] == 3  ->  app[0x00] = 4
        //     otherwise        ->  left as it was
        //
        // Besides the two list pointers this is the only thing that changes
        // between one vec4 and the next, and Mahjong reads app_block+0 as a
        // byte -- so a title dispatching on it was seeing a value that never
        // moved. We were not writing it at all.
        const auto lifecycle = memory_.read<u8>(app_block + 0x230);
        if (lifecycle.value_or(0) == 1) {
            (void)memory_.write<u8>(app_block + 0x00, 5);
        } else if (lifecycle.value_or(0) == 3) {
            (void)memory_.write<u8>(app_block + 0x00, 4);
        }
        (void)memory_.write<u32>(app_block + 0x30,
                                 event_count != 0 ? event_pool : 0u);
        // Before the frame, not after: a title that starts a load during frame
        // N expects to use the data in frame N+1, and delivering afterwards
        // would leave the callback a frame behind the game's state machine.
        //
        // Delivering at the end of the HLE call instead -- so the request is
        // certainly still intact -- was tried and is worse. It does not fix
        // Texas Hold'em (it faults sooner) and it costs Cubis 2 most of its
        // frame, 57 draws down to 1. Whatever these titles want, it is not
        // "as soon as possible".
        deliver_completions(max_instructions);
        last_guest_call_failure_.clear();
        const auto result =
            call_guest(main_vector, {app_block, arg_block}, max_instructions);
        // An unserviced import stops the run before the callback can reach the
        // return sentinel, so it is indistinguishable from a hang unless it is
        // checked first -- and only this branch knows the ordinal's name.
        // Torn down after the call, as the firmware does.
        (void)memory_.write<u32>(app_block + 0x2c, 0);
        (void)memory_.write<u32>(app_block + 0x30, 0);
        // NOT pending_io_.clear() -- a completion queued *during* this frame's
        // callback belongs to the next frame's list. Clearing here discarded it
        // before it was ever published, and every read of app+0x2c returned 0.

        if (hle_missing_calls_ > missing_before) {
            total.status = RunStatus::Stopped;
            break;
        }
        if (!result.has_value()) {
            // Reaching --frames stops the presenter mid-callback, so the guest
            // never gets back to the return sentinel and call_guest reports
            // nothing. That is a bounded run ending exactly as asked, not a
            // fault, and reporting it as one made every successful long run
            // look broken.
            u64 presented = 0;
            u64 draws = 0;
            gles_statistics(*this, presented, draws);
            if (frame_limit_ != 0 && presented >= frame_limit_) {
                game_frame_index_ = frames + 1;
                total.status = RunStatus::Stopped;
                break;
            }
            total.status = RunStatus::Fault;
            if (videocore_ && !videocore_->error().empty()) total.fault = videocore_->error();
            if (total.fault.empty() && !last_guest_call_failure_.empty()) {
                total.fault = last_guest_call_failure_;
            }
            if (total.fault.empty()) {
                // Where it was is the whole diagnostic. A callback that never
                // returns is either spinning or waiting, and the pc says which
                // code is doing it -- without that the message names a symptom
                // and withholds the only actionable thing.
                std::ostringstream where;
                where << "the frame callback did not return (pc 0x" << std::hex
                      << cpu_->pc() << ", lr 0x" << cpu_->reg(14) << std::dec
                      << ", after " << frames << " frame(s))";
                total.fault = where.str();
            }
            break;
        }
        game_frame_index_ = frames + 1;
        if (single_frame_mode_) {
            break;
        }
    }
    CLICKHLE_INFO(kCategory) << "ran " << frames << " frame callback(s)";
    return total;
}

RunResult Emulator::run_game_frame(u64 max_instructions) {
    u64 presented = 0;
    u64 draws = 0;
    gles_statistics(*this, presented, draws);
    frame_limit_ = presented + 1;
    single_frame_mode_ = true;
    RunResult result = run_game(max_instructions);
    single_frame_mode_ = false;
    return result;
}

RunResult Emulator::run(u64 max_instructions) {
    if (cpu_ == nullptr) {
        RunResult result;
        result.status = RunStatus::Unsupported;
        result.fault = "no CPU backend";
        return result;
    }
    // Threaded: run the current thread for a quantum, then rotate. This is the
    // only pre-emption point -- a thread spinning on a flag another thread sets
    // would otherwise never give way, and games do exactly that while loading.
    RunResult total;
    total.status = RunStatus::Stopped;

    if (!scheduler_.active()) {
        // Single-threaded: no quantum, no scheduling overhead.
        const RunResult single = cpu_->run(max_instructions);
        if (single.status == RunStatus::Fault) {
            dump_call_history();
            return single;
        }
        // Unless the guest created its first thread during that slice and then
        // parked, which the fast path has no way to service. that title does
        // exactly this: it starts its SD card thread and immediately waits for
        // it, so the first park happened inside the fast path and ended the run
        // 106 HLE calls in, having drawn nothing. Carry the count and fall
        // through to the scheduler.
        if (!thread_parked_ || !scheduler_.active()) {
            return single;
        }
        total.instructions_executed = single.instructions_executed;
    }

    while (true) {
        u64 quantum = config_.thread_quantum;
        if (max_instructions != 0) {
            const u64 remaining = max_instructions - total.instructions_executed;
            if (remaining == 0) {
                total.status = RunStatus::BudgetExhausted;
                return total;
            }
            quantum = std::min(quantum, remaining);
        }

        const RunResult slice = cpu_->run(quantum);
        total.instructions_executed += slice.instructions_executed;

        if (slice.status == RunStatus::Fault) {
            total.status = RunStatus::Fault;
            total.fault = slice.fault;
            dump_call_history();
            return total;
        }
        const u32 tick = tick_count();

        // Deadlock watchdog.
        //
        // Reset whenever anything is runnable *or merely sleeping*. Testing
        // only "is anything runnable right now" fires on healthy games: a menu
        // loop is Sleep(n) then poll-input, so with nobody pressing anything it
        // has no runnable thread almost all of the time. The emulator this one
        // forked from killed a fully-rendered language-select screen 101 frames
        // in, and reported it as a deadlock. A sleeping thread has a wake time
        // that arrives on its own, so the guest is idle, not stuck; only a
        // guest where every live thread waits on something nothing will deliver
        // is deadlocked.
        if (scheduler_.runnable_count(tick) > 0 ||
            scheduler_.any_sleeping(tick)) {
            blocked_since_ = 0;
        } else if (blocked_since_ == 0) {
            blocked_since_ = tick == 0 ? 1 : tick;
            // Signed, and deliberately. blocked_since_ uses 1 as its "not
            // set" sentinel, so a guest that blocks at tick 0 -- which a game
            // waiting on its own startup thread does -- computes 0 - 1 and
            // wraps to four billion milliseconds, reporting a deadlock on the
            // first idle tick. Supernaturals faulted 1320 instructions in.
        } else if (config_.deadlock_timeout_ms != 0 &&
                   static_cast<i32>(tick - blocked_since_) >
                       static_cast<i32>(config_.deadlock_timeout_ms)) {
            total.status = RunStatus::Fault;
            total.fault = "all guest threads blocked for " +
                          std::to_string(config_.deadlock_timeout_ms) +
                          " ms (deadlock)";
            CLICKHLE_ERROR(kCategory) << total.fault;
            // Name every thread and say where it is parked. "All threads
            // blocked" states the symptom and withholds the only thing that
            // identifies the cause: which thread is waiting, at what pc, and
            // for how much longer it thinks it is sleeping. A thread whose
            // wake_at is far in the future is sleeping normally and is not the
            // culprit; the ones due now, or never, are.
            for (const auto& thread : scheduler_.threads()) {
                const bool current = thread.handle == scheduler_.current_handle();
                std::string state;
                if (thread.finished) {
                    state = "finished";
                } else if (thread.suspended > 0) {
                    state = "suspended x" + std::to_string(thread.suspended);
                } else if (static_cast<i32>(thread.wake_at - tick) > 0) {
                    state = "sleeping " +
                            std::to_string(static_cast<i32>(thread.wake_at - tick)) +
                            " ms more";
                } else {
                    state = "runnable-but-not-run";
                }
                CLICKHLE_ERROR(kCategory)
                    << "  thread 0x" << std::hex << thread.handle << " pc=0x"
                    << thread.context.r[reg::kPc] << " lr=0x"
                    << thread.context.r[reg::kLr] << std::dec << " " << state
                    << (thread.name.empty() ? "" : " " + thread.name)
                    << (current ? "  <- current" : "");
            }
            dump_call_history();
            return total;
        }

        if (slice.status == RunStatus::Stopped) {
            // Two very different things end a slice as "stopped", and
            // conflating them either hangs the emulator or ends the game early:
            //
            //   - a real stop request (frame limit, window closed) -- terminal;
            //   - a thread exiting with nothing else runnable -- resumable once
            //     a sleeping thread comes due.
            //
            // thread_parked_ distinguishes them; only on_svc sets it.
            if (!thread_parked_) {
                return total;
            }
            thread_parked_ = false;
            if (!scheduler_.any_alive()) {
                return total;  // every thread has finished
            }
            if (scheduler_.runnable_count(tick) == 0) {
                skip_idle_to(scheduler_.earliest_wake(tick), tick);
                thread_parked_ = true;
                continue;  // the watchdog above bounds this
            }
            static_cast<void>(scheduler_.yield_now(*cpu_, tick, memory_));
            continue;
        }

        if (scheduler_.runnable_count(tick) == 0) {
            // Every thread is sleeping, and the clock is ours, so there is
            // nothing to wait for -- jump straight to when the first one is
            // due. Sleeping the host here instead used to cost real seconds
            // per run and, worse, let a blocked thread re-issue its wait about
            // a hundred times per emulated millisecond, because an HLE call
            // advances the clock by only hle_call_cycles.
            skip_idle_to(scheduler_.earliest_wake(tick), tick);
            continue;
        }
        static_cast<void>(scheduler_.yield_now(*cpu_, tick, memory_));
    }
}

std::optional<u32> Emulator::call_guest(GuestAddr function,
                                        const std::vector<u32>& args,
                                        u64 max_instructions) {
    if (cpu_ == nullptr) {
        return std::nullopt;
    }
    if (args.size() > 4) {
        // Everything that currently needs this takes four or fewer: WndProc
        // takes exactly four, qsort comparators two, thread entry points one.
        // Stack-passed arguments arrive with the calling convention work in
        // roadmap task #11.
        CLICKHLE_ERROR(kCategory)
            << "call_guest with " << args.size()
            << " arguments; only register-passed arguments are supported";
        return std::nullopt;
    }

    const CpuContext saved = cpu_->save_context();
    const bool saved_sentinel = return_sentinel_hit_;
    return_sentinel_hit_ = false;
    ++call_depth_;

    for (std::size_t i = 0; i < args.size(); ++i) {
        cpu_->set_reg(static_cast<unsigned>(i), args[i]);
    }
    cpu_->set_pc(function);
    cpu_->set_lr(ThunkTable::kReturnSentinel);

    const RunResult result = cpu_->run(max_instructions);

    std::optional<u32> value;
    if (return_sentinel_hit_) {
        value = cpu_->reg(reg::kR0);
    } else if (result.status == RunStatus::BudgetExhausted) {
        std::ostringstream message;
        message << "guest callback exceeded its instruction budget ("
                << result.instructions_executed << " instructions; pc 0x"
                << std::hex << cpu_->pc() << ", lr 0x" << cpu_->reg(14) << ')';
        last_guest_call_failure_ = message.str();
        CLICKHLE_ERROR(kCategory) << message.str();
    } else if (result.status == RunStatus::Stopped) {
        // Asked to stop, so of course it did not reach the sentinel.
        //
        // `--frames N` trips this on every single run: the frame-limit check
        // lives in the swap-buffers path, which is INSIDE the frame callback,
        // so request_stop() unwinds out of vec4 mid-call. Reporting that at
        // error level made every bounded run look like it had crashed, and it
        // cost real time twice -- once reading it as a Zuma fault that did not
        // exist, and once as a Mahjong one.
        //
        // A deliberate stop is not a failure to return.
        CLICKHLE_DEBUG(kCategory)
            << "guest call to 0x" << std::hex << function << std::dec
            << " stopped before returning, by request";
    } else {
        CLICKHLE_ERROR(kCategory)
            << "guest call to 0x" << std::hex << function << std::dec
            << " did not return (" << result.fault << ")";
    }

    --call_depth_;
    // A returned host call is transparent to its caller, so put the saved
    // context back. If guest code faulted or exhausted its budget, keep the
    // live context: restoring it here hid the actual stop PC and registers
    // from the frame runner and debugger.
    if (return_sentinel_hit_) {
        cpu_->restore_context(saved);
    }
    return_sentinel_hit_ = saved_sentinel;
    return value;
}

std::vector<std::string> Emulator::recent_hle_calls() const {
    std::vector<std::string> out;
    out.reserve(kCallHistory);
    for (std::size_t i = 0; i < kCallHistory; ++i) {
        const auto& entry =
            call_history_[(call_history_next_ + i) % kCallHistory];
        if (!entry.empty()) {
            out.push_back(entry);
        }
    }
    return out;
}

void Emulator::dump_call_history() const {
    const auto history = recent_hle_calls();
    if (history.empty()) {
        return;
    }
    std::string line;
    for (const auto& entry : history) {
        if (!line.empty()) {
            line += "  ->  ";
        }
        line += entry;
    }
    CLICKHLE_ERROR(kCategory) << "last HLE calls: " << line;
}

Emulator::Coverage Emulator::coverage() const {
    Coverage result;
    if (!image_.has_value()) {
        return result;
    }
    std::set<std::string> seen;
    std::set<std::string> missing;
    for (const auto& import : image_->imports()) {
        const std::string key = import.key();
        if (!seen.insert(key).second) {
            continue;
        }
        if (import.compatible_abi && registry_.resolve(import) != nullptr) {
            ++result.implemented;
        } else {
            missing.insert(key);
        }
    }
    result.total = seen.size();
    result.missing.assign(missing.begin(), missing.end());
    return result;
}

u64 Emulator::microsecond_count() const {
    // Guest instructions plus a fixed charge per HLE call, so time advances
    // during host-side work too -- without that, a game busy-waiting on
    // QueryPerformanceCounter retires one instruction per poll and never
    // progresses.
    const u64 cycles = (cpu_ == nullptr ? 0 : cpu_->instruction_count()) +
                       hle_calls_ * config_.hle_call_cycles + idle_cycles_;
    const u64 hz = config_.emulated_hz == 0 ? 1 : config_.emulated_hz;
    return cycles * 1'000'000ull / hz;
}

void Emulator::set_hardware_buttons(u8 mask) {
    input_state_.hardware_port_word =
        (input_state_.hardware_port_word & ~(0x3fu << 8)) |
        (static_cast<u32>(mask & 0x3f) << 8);
}

void Emulator::set_hardware_button(u8 bit, bool pressed) {
    if (bit >= 6) return;
    const u32 mask = 1u << (bit + 8);
    if (pressed) input_state_.hardware_port_word |= mask;
    else input_state_.hardware_port_word &= ~mask;
}

void Emulator::set_hardware_wheel(u8 raw_position) {
    if (raw_position >= 96) return;
    hardware_wheel_position_ = raw_position;
    input_state_.hardware_port_word =
        (input_state_.hardware_port_word & ~(0x3ffu << 16)) |
        (static_cast<u32>(raw_position) << 16) | (1u << 30);
}

void Emulator::release_hardware_wheel() {
    input_state_.hardware_port_word &= ~((0x3ffu << 16) | (1u << 30));
}

void Emulator::move_hardware_wheel(i32 detents) {
    const i64 next = (static_cast<i64>(hardware_wheel_position_) +
                      static_cast<i64>(detents) % 96 + 96) % 96;
    set_hardware_wheel(static_cast<u8>(next));
}

void Emulator::set_replay(std::vector<ReplayEntry> entries) {
    replay_entries_ = std::move(entries);
    replay_next_entry_ = 0;
    replay_polls_left_ = 0;
    input_state_.hardware_button_mask = 0;
    hardware_wheel_position_ = 0;
    input_state_.hardware_port_word = 0x8000001a;
}

u32 Emulator::sample_hardware_port() {
    if (!replay_entries_.empty()) {
        if (replay_polls_left_ == 0 && replay_next_entry_ < replay_entries_.size()) {
            const ReplayEntry& entry = replay_entries_[replay_next_entry_++];
            input_state_.hardware_port_word = entry.port_word;
            replay_polls_left_ = entry.polls;
        }
        if (replay_polls_left_ != 0) --replay_polls_left_;
    }
    return input_state_.hardware_port_word;
}

u64 Emulator::system_clock_microsecond_count() {
    u64 measured = 0;
    if (config_.deterministic_system_clock) {
        measured = microsecond_count();
    } else {
        const auto elapsed = std::chrono::steady_clock::now() - system_clock_start_;
        measured = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    }
    // Some titles divide by the delta between consecutive reads. Keep the
    // timer strictly increasing when polls land in the same host microsecond.
    last_system_clock_us_ = std::max(measured, last_system_clock_us_ + 1);
    return last_system_clock_us_;
}

u32 Emulator::tick_count() const {
    return static_cast<u32>((microsecond_count() / 1000) & 0xFFFFFFFF);
}

}  // namespace clickhle

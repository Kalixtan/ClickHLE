#pragma once

#include <array>
#include <any>
#include <chrono>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "clickhle/core/address_space.h"
#include "clickhle/audio/audio_device.h"
#include "clickhle/audio/classic_audio.h"
#include "clickhle/core/cpu.h"
#include "clickhle/hle/guest_heap.h"
#include "clickhle/hle/classic_memory.h"
#include "clickhle/hle/registry.h"
#include "clickhle/hle/scheduler.h"
#include "clickhle/hle/vfs.h"
#include "clickhle/input/replay.h"
#include "clickhle/loader/eapp_image.h"
#include "clickhle/loader/music_database.h"
#include "clickhle/video/display.h"
#include "clickhle/video/classic_compositor.h"
#include "clickhle/video/classic_graphics.h"
#include "clickhle/video/classic_gpu.h"
#include "clickhle/video/videocore.h"

namespace clickhle {

/// Ties the loader, the address space, the CPU core and the HLE layer together.
///
/// Deliberately holds no platform-specific state. Windowing, audio and input
/// arrive later as separate services injected here, so that this class stays
/// buildable and testable with nothing but a compiler.
class Emulator {
public:
    struct DeviceSettings {
        // Unset values retain the CLI environment overrides and defaults.
        std::optional<u32> language;
        std::optional<u32> display_level;
        std::optional<bool> time_format_24;
        bool hold_switch = false;
        // Unconnected 7G power-service inputs; their hardware mapping and
        // persistence have not yet been modeled.
        bool power_input_a = false;
        bool power_input_b = false;
        i32 power_mode = 0;
        bool power_status_inhibited = false;
        u8 backlight_dim_state = 0;
        std::optional<u32> applied_backlight_level;
    };

    DeviceSettings& device_settings() noexcept { return device_settings_; }
    [[nodiscard]] const DeviceSettings& device_settings() const noexcept { return device_settings_; }

    /// One observed call at the ARM-to-render-server boundary. VideoCore
    /// tracing records the host-visible API traffic
    /// and any small input prefix that can help reverse its contract.
    struct VideoCoreEvent {
        u64 sequence = 0;
        u64 frame = 0;
        u64 instructions = 0;
        GuestAddr return_pc = 0;
        u32 ordinal = 0;
        std::string name;
        std::array<u32, 4> args{};
        u8 arg_count = 0;
        u32 result = 0;
        u32 payload_size = 0;
        std::array<u8, 64> payload_prefix{};
        u8 payload_prefix_size = 0;
    };

    struct Config {
        std::filesystem::path executable;
        /// The emulated iPod volume, normally hdd beside the frontend binary.
        std::filesystem::path hdd_root;
        /// Zero selects the first matching entry in the package manifest.
        u32 platform_id = 0;
        bool classic_gpu = false;

        // Opt-in disk capture for headless reverse-engineering runs.
        bool capture_videocore = false;
        VideoCoreEngine videocore_engine = videocore_jit_available()
            ? VideoCoreEngine::G5 : VideoCoreEngine::Interpreter;

        /// Empty means the backend chosen at configure time.
        std::string cpu_backend;

        /// Emulated clock rate.
        ///
        /// Guest time is derived from work the guest has actually done, never
        /// from the host clock, so a run is reproducible: the same executable
        /// and inputs always produce the same frames. A host-time option
        /// existed briefly and was removed -- it made every timing bug a
        /// heisenbug, and the Supernaturals fault took three runs to even
        /// confirm.
        ///
        /// NOT the hardware's rate, deliberately. The PP5021/5022 runs at up
        /// to 80 MHz per core; the figure here is lower still.
        ///
        /// Guest instruction count badly underestimates how much time hardware
        /// would have spent, because HLE does the heavy lifting in host code:
        /// a memcpy of a megabyte is millions of cycles on the device and one
        /// guest instruction here. Charging the true rate therefore runs games
        /// in slow motion.
        ///
        /// ARBITRARY, inherited, and not yet calibrated against any
        /// click-wheel title -- 20 MHz was tuned for a different platform. It
        /// controls the deterministic virtual scheduler clock. RetailOS
        /// miscTBD#9 uses a separate system clock for elapsed-time queries.
        u64 emulated_hz = 20'000'000;

        /// Cycles charged for each HLE call.
        ///
        /// Without this the emulated clock barely advances during host-side
        /// work: a game busy-waiting on QueryPerformanceCounter retires one
        /// guest instruction per poll and would spin forever. A real firmware
        /// call costs at least this many cycles, so charging for it is both
        /// more accurate and what makes such loops terminate.
        ///
        /// ARBITRARY, and harmless while it is: nothing implemented so far
        /// reads a clock. The number to replace it with is whatever the
        /// firmware's own cheapest export costs, once one is timed.
        u64 hle_call_cycles = 200;

        /// Use emulated guest time for RetailOS system-clock reads. The
        /// default follows the host's monotonic clock; deterministic mode is
        /// useful for replay and bitmap regression runs.
        bool deterministic_system_clock = false;

        /// Host directory for game assets. Empty derives Games_RO/<id> from
        /// an installed executable, or the executable's parent directory.
        std::filesystem::path content_root;

        /// GameData_RW/<id> on the emulated volume unless overridden for tests.
        std::filesystem::path save_root;

        /// The device has 64 MB of SDRAM. Stack and heap are carved out well
        /// away from the image base (0x18000000) and the thunk page
        /// (0xF0000000).
        /// ARBITRARY placement, clear of the image at 0x18000000, the heap at
        /// 0x20000000 and the thunk page at 0xF0000000. What the firmware
        /// actually gives a game, and how much, is unknown -- no export that
        /// reports a stack bound has been identified. 1 MB is a guess that has
        /// not yet been approached: the deepest run so far peaks a few hundred
        /// bytes in.
        GuestAddr stack_base = 0x30000000u;
        u32 stack_size = 1u * 1024u * 1024u;
        /// Per-thread control block page.
        ///
        /// INHERITED AND UNJUSTIFIED for this platform. 0xFFFFC800 is where
        /// Windows CE put its KDataStruct; nothing suggests click-wheel games
        /// see anything at that address. It survives only because the scheduler
        /// publishes the running thread's identity and TLS somewhere, and this
        /// is where it looks. Purely internal bookkeeping unless a firmware
        /// export turns out to expose it.
        ///
        /// Inherited from the Windows CE KDataStruct this scheduler was written
        /// against, and kept because the scheduler still needs somewhere to
        /// publish the running thread's identity and TLS. Whether click-wheel
        /// games see anything equivalent is unknown -- they may not, in which
        /// case this becomes purely internal bookkeeping. The two fields:
        ///
        ///     +0x00  lpvTls    the running thread's TLS slot array
        ///     +0x04  ahSys[]   system handles, so ahSys[SH_CURTHREAD] (index
        ///                      1) is at +0x08: the running thread's handle
        ///
        /// +0x08 is the one every game actually uses, and it must agree with
        /// the thread id CreateThread reports. Each game's statically linked
        /// CRT keeps a table of per-thread data blocks keyed on the thread id
        /// it got from CreateThread, and looks an entry up by comparing that
        /// key against *(+0x08). Holding anything else there -- this used to
        /// hold the TLS array pointer -- means the lookup misses for every
        /// thread but the first, and returns null. that game then took the
        /// null through its allocator and read address 0x84.
        static constexpr GuestAddr kKDataBase = 0xFFFFC800u;
        static constexpr u32 kKDataSize = 0x800u;
        static constexpr GuestAddr kKDataTlsPointer = kKDataBase + 0x00u;
        static constexpr GuestAddr kKDataCurrentThread = kKDataBase + 0x08u;
        static constexpr GuestAddr kTlsSlots = kKDataBase + 0x200u;
        static constexpr u32 kTlsSlotCount = 64u;

        /// Top of the working arena mapped above the game image.
        ///
        /// JUSTIFIED: the image base 0x18000000 is fixed by the format -- the
        /// eapp header has no relocation information and every pointer in every
        /// title is 0x18xxxxxx. Games write
        /// directly above their image: Tetris stores past the end of its file
        /// within eighteen instructions of entry, which is zero-initialised
        /// static data the file does not carry.
        ///
        /// ARBITRARY: the 64 MB extent. How much the firmware actually grants
        /// is unknown. Generous, and costs nothing until a page is touched.
        GuestAddr image_arena_end = 0x1C000000u;

        /// Stacks for guest-created threads, carved out of one arena.
        ///
        /// UNREACHABLE TODAY. Nothing can create a guest thread -- see the note
        /// on Scheduler. Mapped anyway because it is cheap and the question of
        /// whether any ordinal spawns a thread is still open.
        GuestAddr thread_stack_base = 0x31000000u;
        u32 thread_stack_arena = 8u * 1024u * 1024u;
        u32 thread_stack_size = 256u * 1024u;

        /// Instructions a thread runs before the scheduler rotates. Large
        /// enough that switching costs nothing measurable, small enough that a
        /// thread spinning on a flag another thread sets still makes progress.
        u64 thread_quantum = 200000;

        /// Give up if every thread has been blocked this long. A guest can
        /// deadlock -- all threads asleep or waiting on something that never
        /// arrives -- and without a bound the emulator idles forever instead of
        /// reporting it. A hang is a bug; it should look like one.
        ///
        /// ARBITRARY. Also nearly unreachable now: the watchdog only fires when
        /// every live thread waits on something nothing will deliver, and with
        /// the scheduler dormant there is one thread. Kept for when that stops
        /// being true.
        u32 deadlock_timeout_ms = 10000;

        /// The heap behind miscTBD#0 / #1, the allocator every title uses.
        ///
        /// ARBITRARY placement: 0x20000000 simply sits clear of the image
        /// arena below it and the stack above, and of the firmware's own
        /// 0x10000000 load address. The real device's map is not yet known.
        ///
        /// The size is a floor rather than a guess: Lost asks for a single
        /// 0x7ff80 (512 KB) block during startup, and an iPod 5G has 32 or
        /// 64 MB of SDRAM, so a game that ran on hardware fits in 48 MB by
        /// construction.
        GuestAddr heap_base = 0x20000000u;
        u32 heap_size = 48u * 1024u * 1024u;
    };

    struct MetadataState {
        std::vector<bool> music_library_slots;
        std::vector<bool> artwork_library_slots;
        MusicDatabase music_database;
        bool music_database_available = false;
        GuestAddr service_object = 0;
    };

    struct AudioState {
        struct Sound {
            GuestAddr guest_object = 0;
            u32 buffer = 0;
            u32 repeat = 1;
            u32 state = 0;
            u32 next = 0;
            u32 auxiliary_48 = 0;
            bool started = false;
            std::map<u32, u32> fields = {
                {8, 0}, {9, 0}, {10, 0xAC44}, {11, 1}, {12, 0x10},
                {13, 0x7FFF}, {14, 0}, {15, 1000}, {18, 0}, {19, 0},
                {20, 0}, {22, 0x7FFF}};
        };

        // Audio#54/#55 expose the stream service's 16-bit level at +0x14.
        // Its constructor default is not yet identified in the firmware.
        u32 stream_level = 0;
        // Number of stream-registration messages submitted through Audio#40.
        // Metadata#134 reports whether this count is nonzero.
        u32 registered_stream_count = 0;
        std::vector<std::filesystem::path> stream_assets;
        std::filesystem::path pending_stream_asset;
        // State changed by the Audio stream-command messages. The -1 values
        // represent the firmware's "defer to the player service" sentinel;
        // that service is not present in ClickHLE, so its initial fallback is
        // reported as idle by the HLE getter.
        i64 stream_index = -1;
        i32 stream_control = -1;
        u32 stream_repeat_internal_mode = 0;
        bool stream_shuffle = false;
        std::map<u32, Sound> sounds;
    };

    struct InputState {
        u8 pending_event_flag = 0;
        u32 previous_raw_wheel = 0;
        u32 wheel_event_counter = 0;
        u32 wheel_counter_snapshot = 0;
        u32 wheel_threshold = 3;
        i32 wheel_accumulator = 0;
        u8 wheel_raw = 0;
        bool wheel_touched = false;
        std::optional<u8> hardware_wheel_raw;
        bool synthetic_wheel_active = false;
        u32 hardware_button_mask = 0;
        u32 hardware_port_word = 0x8000001a;
        std::array<bool, static_cast<std::size_t>(Button::Count)> held_buttons{};
        u64 poll_count = 0;
    };

    struct MiscState {
        u32 device_level = 0;
        u8 device_level_flag = 0;
    };

    struct FilesystemState {
        struct DirectoryEntry {
            std::string name;
            u8 type = 0;
        };

        struct DirectoryRequest {
            std::string path;
            std::vector<DirectoryEntry> entries;
            u32 next_entry = 0;
            bool completed = false;
            u8 mode = 0;
            u8 flags = 0;
        };

        struct StoreSlot {
            std::string path;
            u32 references = 0;
            u32 size = 0;
            u32 position = 0;
            std::vector<u8> data;
        };

        std::array<u32, 10> handles{};
        // AsyncFileIO's save store is a four-entry indexed table, distinct
        // from the host VFS handles used by Filesytem#0-#3.
        std::array<StoreSlot, 4> stores{};
        u32 store_bytes = 0;
        // AsyncFileIO#7 stores directory-operation objects in a separate
        // per-context table and publishes their zero-based slot in +0x2c.
        std::vector<DirectoryRequest> directories;
    };

    Emulator() = default;
    // Stop a guest call with a diagnostic that survives callback unwinding.
    void stop_with_error(std::string message);
    Emulator(const Emulator&) = delete;
    Emulator& operator=(const Emulator&) = delete;

    /// Loads the executable, maps it, creates the CPU, and binds every import
    /// to a thunk. Imports with no implementation are still bound, so that
    /// calling one produces a named diagnostic rather than a wild branch.
    [[nodiscard]] bool load(Config config, std::string* error = nullptr);

    /// Runs until the guest stops, faults, or the instruction budget runs out.
    /// SVC traps are handled internally and do not end the run.
    [[nodiscard]] RunResult run(u64 max_instructions = 0);

    /// Runs the image the way the firmware does: entry vector once, then the
    /// frame-hook vector until something stops it.
    ///
    /// The header's vector[0] is an *init* function and it returns -- every
    /// title in hand reaches the return sentinel after a few tens of thousands
    /// of instructions with nothing left to do. A game only makes progress
    /// because the firmware then calls vector[1] once per frame. Running only
    /// vector[0] and reporting "stopped" looks like success and is not: the
    /// game has initialised and never been asked to draw anything.
    ///
    /// Stops on a fault, on an unimplemented import, on the frame limit, or
    /// when a hook returns a value asking to quit.
    [[nodiscard]] RunResult run_game(u64 max_instructions = 0);

    /// Advances the game by one firmware-driven frame. Intended for GUI
    /// frontends that need to return to their event loop between frames.
    [[nodiscard]] RunResult run_game_frame(u64 max_instructions = 0);

    /// Calls a function in guest code and returns its r0.
    ///
    /// Needed by WndProc dispatch, thread entry points, and qsort comparators.
    /// Sets lr to the thunk table's return sentinel and runs until the guest
    /// branches there. Nests correctly: a guest callback may re-enter the HLE
    /// layer and call back into the guest again.
    ///
    /// Not usable until a real CPU core is compiled in (roadmap task #11).
    [[nodiscard]] std::optional<u32> call_guest(GuestAddr function,
                                                const std::vector<u32>& args,
                                                u64 max_instructions = 0);

    /// Records that an async request has finished and its completion
    /// trampoline should be run.
    ///
    /// The trampoline is the word at request+0x34. It reads the status and byte
    /// count the operation left in the request, then tail-calls the callback
    /// the game parked in its file object -- so the *only* way a title learns
    /// its load finished is for the firmware to call this.
    ///
    /// Queued rather than called inline. Two reasons: the handler that finishes
    /// the operation has not returned its result to the guest yet, so running
    /// game code in the middle of it re-enters with a half-finished call on the
    /// stack; and on the device the callback genuinely arrived later, off the
    /// back of the I/O completing. Draining between frames models that.
    void queue_completion(GuestAddr request, const char* origin = nullptr);

    /// Stops completions being delivered at all.
    ///
    /// A bisection tool. Completing async operations changed the behaviour of
    /// most of the corpus at once, so when a title starts faulting the first
    /// question is whether the completion broke it or merely let it run far
    /// enough to break on its own. This answers that in one run.
    void set_completions_enabled(bool enabled) { completions_enabled_ = enabled; }

    /// Runs and clears the queued completion trampolines.
    void deliver_completions(u64 max_instructions);

    /// Installs a handler for an ordinal *after* loading, and points its
    /// already-bound thunk at it.
    ///
    /// For investigation, not implementation. Coverage is reported before this
    /// runs, so a probed ordinal stays counted as unimplemented -- which it is.
    /// Returns false if the image does not import that ordinal.
    bool install_probe(const std::string& module, u32 ordinal, u8 arity,
                       HleHandler handler);

    struct Coverage {
        std::size_t total = 0;
        std::size_t implemented = 0;
        std::vector<std::string> missing;  ///< Import keys, sorted.
    };

    /// Which of the loaded image's imports we can actually service. This is the
    /// project's compatibility metric.
    [[nodiscard]] Coverage coverage() const;

    [[nodiscard]] AddressSpace& memory() noexcept { return memory_; }
    [[nodiscard]] const AddressSpace& memory() const noexcept { return memory_; }
    [[nodiscard]] ICpuCore* cpu() noexcept { return cpu_.get(); }
    [[nodiscard]] HleRegistry& registry() noexcept { return registry_; }
    [[nodiscard]] ThunkTable& thunks() noexcept { return thunks_; }
    [[nodiscard]] GuestHeap& heap() noexcept { return heap_; }
    [[nodiscard]] MetadataState& metadata_state() noexcept {
        return metadata_state_;
    }
    [[nodiscard]] AudioState& audio_state() noexcept { return audio_state_; }
    [[nodiscard]] InputState& input_state() noexcept { return input_state_; }
    void set_hardware_buttons(u8 mask);
    void set_hardware_button(u8 bit, bool pressed);
    void set_hardware_wheel(u8 raw_position);
    void release_hardware_wheel();
    void move_hardware_wheel(i32 detents);
    void set_replay(std::vector<ReplayEntry> entries);
    [[nodiscard]] u32 sample_hardware_port();
    [[nodiscard]] MiscState& misc_state() noexcept { return misc_state_; }
    [[nodiscard]] FilesystemState& filesystem_state() noexcept {
        return filesystem_state_;
    }

    [[nodiscard]] Scheduler& scheduler() noexcept { return scheduler_; }
    [[nodiscard]] Vfs& vfs() noexcept { return vfs_; }

    /// True while executing a guest callback via call_guest. The scheduler must
    /// not switch threads here: the nested run shares the CPU with an
    /// unfinished outer one, and swapping the register file underneath it would
    /// return to the wrong place.
    [[nodiscard]] bool in_guest_callback() const noexcept {
        return call_depth_ > 0;
    }

    /// Yields the running thread from inside an HLE handler.
    ///
    /// Must be used instead of Scheduler::yield_now directly. A thread
    /// suspended inside an SVC would otherwise save "the instruction after the
    /// SVC" as its resume point -- but the thunk page is a dense array of SVCs
    /// with no return instruction, so that address is the *next import's* trap.
    /// The thread would come back and call a random guest API with whatever was
    /// in its registers. Returning to lr, exactly as a normal HLE return does,
    /// is the only correct resume point.
    ///
    /// CALLERS MUST SET THEIR RETURN VALUE FIRST. HleCallFrame::ret writes r0
    /// on whichever context the CPU is running, and a successful yield has
    /// already swapped that context -- so returning afterwards stores the
    /// result into the *next* thread's r0. That is how POD's thread parameter
    /// was being zeroed: WaitForSingleObject yielded, then wrote WAIT_OBJECT_0
    /// (which is 0) over the incoming thread's argument.
    ///
    /// Returns true if a switch happened. No-op inside a guest callback.
    bool yield_thread();

    /// Blocks the running thread inside the HLE call it is currently in, and
    /// re-issues that call when the thread next runs.
    ///
    /// This is what makes a genuinely blocking wait possible. Handlers cannot
    /// block the host -- everything runs on one thread -- so a wait that cannot
    /// be satisfied used to return WAIT_TIMEOUT and hope the caller looped.
    /// Plenty of callers do not: libSDProt waits INFINITE on [revalidate,
    /// card-removed] and reads anything but WAIT_OBJECT_0 as the card having
    /// been pulled, so POD reported a removed card a few frames in. An INFINITE
    /// wait must never time out, and now does not: the thread sleeps and
    /// arrives back at the same call, which is what blocking looks like from
    /// the guest's side.
    ///
    /// Sleeping (rather than spinning) is what keeps the deadlock watchdog
    /// honest -- a blocked thread leaves the runnable set, so a guest in which
    /// *everything* is blocked still reports a deadlock instead of hanging.
    ///
    /// CALL THIS BEFORE WRITING A RETURN VALUE. The re-issued call re-reads its
    /// arguments from r0-r3, so an f.ret() beforehand would overwrite the first
    /// argument and the second attempt would see garbage.
    ///
    /// Returns false if nothing else can run, in which case nothing changed and
    /// the handler should return something rather than stall.
    bool block_thread(u32 milliseconds = 1);

    /// Ends the running thread and switches away. Used by ExitThread, which by
    /// definition does not return to its caller.
    void exit_current_thread();

    /// Writes the whole guest state to a file, or explains why it cannot.
    ///
    /// Covers what is genuinely a value: the CPU context, every mapped memory
    /// region, the thread table and the heap partitioning. Between them that is
    /// almost all of a running game, because a game keeps its state in its own
    /// memory.
    ///
    /// What it does not cover is the HLE modules' own state, and rather than
    /// write a snapshot that silently loses it, save_state refuses when a
    /// module reports it is holding something that cannot round-trip -- an open
    /// host FILE*, a queued host audio buffer. Those are references to host
    /// resources, not values, and restoring a guest whose file handles have
    /// quietly become nonsense fails later and somewhere unrelated. Declining
    /// is the honest answer until the modules are refactored (see the task
    /// notes for the plan).
    ///
    /// Returns an empty string on success, otherwise the reason.
    [[nodiscard]] std::string save_state(const std::filesystem::path& path);
    [[nodiscard]] std::string load_state(const std::filesystem::path& path);
    [[nodiscard]] Display& display() noexcept { return *display_; }
    [[nodiscard]] VideoCore* videocore() noexcept { return videocore_.get(); }
    [[nodiscard]] classic::Compositor* classic_compositor() noexcept { return classic_compositor_.get(); }
    [[nodiscard]] classic::GraphicsContext* classic_graphics() noexcept { return classic_graphics_.get(); }
    [[nodiscard]] classic::GpuRenderer* classic_gpu() noexcept { return classic_gpu_.get(); }
    [[nodiscard]] classic::AudioService& classic_audio() noexcept { return classic_audio_; }
    void set_videocore_engine(VideoCoreEngine engine);

    /// Host audio output.
    ///
    /// Lazily opened: the backend is only created once a title actually plays
    /// something, so a silent title (and every headless test) pays nothing and
    /// never touches an audio device. Null until then.
    [[nodiscard]] AudioDevice* audio() noexcept { return audio_.get(); }
    [[nodiscard]] const std::string& audio_error() const noexcept {
        return audio_error_;
    }
    AudioDevice* open_audio(u32 sample_rate, u16 channels, u16 bits);

    /// When set, every presented frame is written here, overwriting. The file
    /// therefore holds the last frame drawn -- which is what you want after a
    /// run that ended somewhere unexpected.
    void set_screenshot_path(std::filesystem::path path) {
        screenshot_path_ = std::move(path);
    }
    [[nodiscard]] const std::filesystem::path& screenshot_path() const noexcept {
        return screenshot_path_;
    }

    /// Stops after this many presented frames. Zero means run until the guest
    /// stops. Makes a run reproducible and bounded, which is what a test needs.
    void set_frame_limit(u64 frames) noexcept { frame_limit_ = frames; }
    [[nodiscard]] u64 frame_limit() const noexcept { return frame_limit_; }

    /// Host directory that guest paths resolve under.
    [[nodiscard]] const std::filesystem::path& content_root() const noexcept {
        return content_root_;
    }
    [[nodiscard]] const Config& config() const noexcept { return config_; }
    [[nodiscard]] const EappImage* image() const noexcept {
        return image_.has_value() ? &*image_ : nullptr;
    }
    [[nodiscard]] u32 platform_id() const noexcept {
        return image_ ? image_->platform_id() : 0;
    }

    /// Milliseconds since load, from whichever clock source is configured.
    [[nodiscard]] u32 tick_count() const;

    /// Microseconds since load. QueryPerformanceCounter is built on this, so it
    /// must not simply be tick_count() * 1000 -- a game measuring a sub-
    /// millisecond interval would always see zero elapsed and never progress.
    [[nodiscard]] u64 microsecond_count() const;

    /// Strictly increasing host monotonic time for RetailOS system-clock APIs.
    [[nodiscard]] u64 system_clock_microsecond_count();


    /// Microsoft CRT rand() state.
    ///
    /// Lives here rather than in the module, and is a per-guest LCG rather than
    /// std::rand(), for two reasons. It is the sequence real hardware produces
    /// -- MS's CRT is a specific generator and glibc's is a different one, so
    /// srand(N) then rand() diverges. And std::rand() is process-global, so any
    /// host library touching it would perturb the guest's stream and make a run
    /// non-reproducible across machines. Determinism is what the screenshot
    /// tests and save states are built on, so it cannot depend on the host libc.
    [[nodiscard]] u32 next_random() noexcept {
        random_seed_ = random_seed_ * 214013u + 2531011u;
        return (random_seed_ >> 16) & 0x7FFFu;
    }
    void seed_random(u32 seed) noexcept { random_seed_ = seed; }

    [[nodiscard]] u32 last_error() const noexcept { return last_error_; }
    void set_last_error(u32 value) noexcept { last_error_ = value; }

    /// Total HLE calls dispatched, and calls that hit an unimplemented import.
    [[nodiscard]] u64 hle_call_count() const noexcept { return hle_calls_; }

    [[nodiscard]] const std::deque<VideoCoreEvent>& videocore_events() const noexcept {
        return videocore_events_;
    }
    void clear_videocore_events() { videocore_events_.clear(); }
    void set_videocore_trace_enabled(bool enabled) noexcept {
        videocore_trace_enabled_ = enabled;
    }
    void set_videocore_pause_on_call(bool enabled) noexcept {
        videocore_pause_on_call_ = enabled;
    }
    [[nodiscard]] std::optional<VideoCoreEvent> take_videocore_break() {
        auto event = std::move(videocore_break_event_);
        videocore_break_event_.reset();
        return event;
    }

    /// What framework calls hand back, classified.
    ///
    /// Three subsystems here -- audio, files and matrices -- assume a title
    /// never dereferences a value a framework returned; games are supposed to
    /// hold opaque handles. The LLE track checked that statically and found no
    /// declared pointer return anywhere in the 160 used slots and no real
    /// dereference in 2,877 direct call sites, but their scan cannot see a slot
    /// reached through an address taken as a literal.
    ///
    /// This closes that: the dispatcher sees every call however it was reached.
    /// If nothing we implement ever returns a *mapped* address, then any title
    /// dereferencing a return value must touch a low address, and low addresses
    /// are unmapped in the game address space -- so it would fault and be
    /// reported rather than pass silently.
    struct ReturnAudit {
        u64 calls = 0;
        u64 zero = 0;         ///< the commonest return by far
        u64 small = 0;        ///< plausible handles and counts
        u64 mapped = 0;       ///< a real guest address -- the case that matters
        std::map<std::string, u64> mapped_by_import;
    };
    [[nodiscard]] const ReturnAudit& return_audit() const noexcept {
        return return_audit_;
    }

    /// How many times each import was called, keyed by import key.
    ///
    /// A title that is stuck is nearly always stuck *calling something*, and
    /// the shape of that loop -- clock, clock, clock, poll, clock -- says what
    /// it is waiting for. Cheap enough to keep always on: one map bump per HLE
    /// call, against several thousand guest instructions.
    [[nodiscard]] const std::map<std::string, u64>& call_counts() const noexcept {
        return call_counts_;
    }
    [[nodiscard]] u64 hle_missing_call_count() const noexcept {
        return hle_missing_calls_;
    }

    /// The most recent HLE calls, oldest first, as "thread: function".
    ///
    /// Dumped when a run faults. A bad pointer is nearly always produced by an
    /// earlier call that returned the wrong thing, and the register dump alone
    /// does not say which one -- especially with several threads interleaved.
    [[nodiscard]] std::vector<std::string> recent_hle_calls() const;

    /// Import key of the unimplemented function that stopped the run, empty if
    /// the run stopped for another reason. Almost always the next thing to
    /// implement, so it is worth surfacing prominently.
    [[nodiscard]] const std::string& last_missing_import() const noexcept {
        return last_missing_import_;
    }

private:
    DeviceSettings device_settings_;
    [[nodiscard]] bool bind_imports(std::string* error);
    void dump_call_history() const;
    /// Returns false to ask the core to stop.
    bool on_svc(u32 immediate);

    /// ARM semihosting: the guest's own debug channel, left in by armcc.
    /// Carries assertion text and library diagnostics, so it is worth reading
    /// rather than swallowing.
    bool on_semihost();
    void flush_semihost_line(bool force);
    /// Advances the emulated clock over an interval in which nothing can run.
    void skip_idle_to(u32 due, u32 tick);

    Config config_;
    std::filesystem::path content_root_;
    std::optional<EappImage> image_;
    AddressSpace memory_;
    std::unique_ptr<ICpuCore> cpu_;
    HleRegistry registry_;
    ThunkTable thunks_;
    GuestHeap heap_;
    std::unique_ptr<classic::RegionHeap> classic_heap_;
    std::unique_ptr<classic::Arena> classic_arena_;
    MetadataState metadata_state_;
    AudioState audio_state_;
    InputState input_state_;
    MiscState misc_state_;
    FilesystemState filesystem_state_;
    Scheduler scheduler_;
    Vfs vfs_;
    /// Address of the SVC thunk being serviced, so block_thread() can send the
    /// thread back to it. Zero outside an HLE call.
    GuestAddr current_thunk_ = 0;
    unsigned call_depth_ = 0;
    std::unique_ptr<VideoCore> videocore_;
    std::unique_ptr<classic::Compositor> classic_compositor_;
    std::unique_ptr<classic::GraphicsContext> classic_graphics_;
    std::unique_ptr<classic::GpuRenderer> classic_gpu_;
    classic::AudioService classic_audio_;
    std::unique_ptr<Display> display_ = Display::create();
    std::unique_ptr<AudioDevice> audio_;
    std::string audio_error_;
    u32 audio_rate_ = 0;
    u16 audio_channels_ = 0;
    u16 audio_bits_ = 0;
    std::filesystem::path screenshot_path_;
    u64 frame_limit_ = 0;

    // Persistent firmware frame-driver state. Keeping these between calls
    // lets a desktop frontend advance a frame, service native UI events, then
    // advance the next frame without rerunning static constructors.
    bool game_driver_ready_ = false;
    bool single_frame_mode_ = false;
    GuestAddr game_main_vector_ = 0;
    GuestAddr game_app_block_ = 0;
    GuestAddr game_event_pool_ = 0;
    GuestAddr game_io_pool_ = 0;
    u64 game_frame_index_ = 0;

    /// Partial semihosting output, flushed a line at a time.
    std::string semihost_line_;

    u32 last_error_ = 0;
    std::string last_missing_import_;
    u64 hle_calls_ = 0;
    u64 videocore_sequence_ = 0;
    std::deque<VideoCoreEvent> videocore_events_;
    std::optional<VideoCoreEvent> videocore_break_event_;
    bool videocore_trace_enabled_ = true;
    bool videocore_pause_on_call_ = false;
    std::vector<GuestAddr> pending_completions_;

public:
    /// One entry in the per-frame event list handed to vec4.
    ///
    /// The firmware delivers input this way: nothing registers and nothing
    /// polls for buttons -- the frame driver builds a list, points app+0x30 at
    /// it, calls vec4, then frees it. Type values are not yet mapped to
    /// physical controls.
    /// type is 1..5; gate is 1 for source kind 7 and 2 for kind 4, and
    /// InputEvents#1 only accepts 1. The payload is filled in at publish time
    /// with a millisecond timestamp.
    struct InputEvent {
        u8 type = 0;
        u32 payload = 0;
        u8 gate = 1;
    };
    void queue_input_event(u8 type, u32 payload, u8 gate = 1) {
        pending_input_.push_back(InputEvent{type, payload, gate});
    }

    /// A completed async file operation, published on app+0x2c for one frame.
    ///
    /// The second of the two per-frame lists. AsyncFileIO#0 enqueues work to a
    /// worker; the worker puts the finished node on the context's completed
    /// list; the frame driver hands that to the guest as app[0x2c] and frees it
    /// afterwards. A title that streaming-opens a file is waiting for this, not
    /// polling and not reading -- which is why four titles stall at their first
    /// sound.
    void queue_io_completion(GuestAddr request, std::string path) {
        pending_io_.push_back(IoCompletion{request, std::move(path)});
    }

private:
    struct IoCompletion {
        GuestAddr request = 0;
        std::string path;
    };
    std::vector<IoCompletion> pending_io_;

public:

private:
    std::vector<InputEvent> pending_input_;
    std::vector<ReplayEntry> replay_entries_;
    std::size_t replay_next_entry_ = 0;
    u64 replay_polls_left_ = 0;
    u8 hardware_wheel_position_ = 0;
    bool completions_enabled_ = true;
    std::map<std::string, u64> call_counts_;
    ReturnAudit return_audit_;
    u64 hle_missing_calls_ = 0;

    /// Ring buffer of recent calls; sized to cover a frame or so of activity
    /// without costing anything noticeable.
    static constexpr std::size_t kCallHistory = 24;
    std::array<std::string, kCallHistory> call_history_;
    std::size_t call_history_next_ = 0;
    std::string last_guest_call_failure_;
    bool return_sentinel_hit_ = false;
    /// Set when a thread exited with nothing runnable, so run() knows the stop
    /// is resumable rather than terminal.
    bool thread_parked_ = false;
    /// Set by block_thread(): pc already points at the call to re-issue.
    bool call_reissued_ = false;
    /// Emulated cycles credited for time the guest spent entirely asleep. See
    /// the idle handling in Emulator::run.
    u64 idle_cycles_ = 0;
    std::chrono::steady_clock::time_point system_clock_start_ =
        std::chrono::steady_clock::now();
    u64 last_system_clock_us_ = 0;
    /// Seeded to 1, as the MS CRT does before any srand().
    u32 random_seed_ = 1;
    /// Tick at which every thread became blocked; 0 when something is runnable.
    u32 blocked_since_ = 0;
};

}  // namespace clickhle

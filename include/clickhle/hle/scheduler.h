#pragma once

#include <string>
#include <vector>

#include "clickhle/common/types.h"
#include "clickhle/core/cpu.h"

namespace clickhle {

class AddressSpace;
class Emulator;

/// Cooperative round-robin scheduler for guest threads. Current game imports
/// use the single-thread path; thread creation support is reserved for future
/// HLE modules.
///
class Scheduler {
public:
    /// SVC immediate a thread returns to when its entry point exits.
    static constexpr u32 kThreadExitSvc = 0xFFFFFEu;

    struct Thread {
        u32 handle = 0;
        std::string name;
        CpuContext context;
        GuestAddr stack_base = 0;
        u32 stack_size = 0;
        /// Per-thread TLS slot array. Windows CE gives every thread its own,
        /// reached through the pointer in the KData page -- so it has to be
        /// swapped on every context switch, not shared.
        GuestAddr tls_slots = 0;
        bool started = false;
        bool finished = false;
        /// Tick count before which the thread must not be resumed.
        u32 wake_at = 0;
        /// Suspend count; CreateThread with CREATE_SUSPENDED starts at 1.
        i32 suspended = 0;
        /// Set by ExitThread, read by GetExitCodeThread.
        u32 exit_code = 0;
    };

    /// `tls_pointer` is the KData slot holding the running thread's TLS array
    /// (lpvTls), and `thread_pointer` the slot holding its handle
    /// (ahSys[SH_CURTHREAD]). Both are republished on every switch, because a
    /// guest reads them directly to find out which thread it is.
    void initialise(GuestAddr stack_arena, u32 arena_size, u32 stack_size,
                    GuestAddr tls_pointer, GuestAddr thread_pointer,
                    u32 tls_bytes);

    /// Registers the already-running main thread, whose stack the loader set up.
    void adopt_main_thread(u32 handle, GuestAddr tls_slots, ICpuCore& cpu);

    /// Returns 0 if no stack could be carved out.
    [[nodiscard]] u32 create(GuestAddr entry, GuestAddr parameter, bool suspended,
                             ICpuCore& cpu, AddressSpace& memory);

    /// Switches to the next runnable thread. Returns false if the current
    /// thread is the only candidate, in which case nothing changed.
    bool yield_now(ICpuCore& cpu, u32 tick, AddressSpace& memory);

    /// Marks the running thread finished and switches away. Returns false if it
    /// could not switch -- either everything has exited or the remaining
    /// threads are all asleep. The caller must stop the current run slice in
    /// that case; the CPU still holds the dead thread's registers.
    bool exit_current(ICpuCore& cpu, u32 tick, AddressSpace& memory);

    void sleep_current(u32 milliseconds, u32 tick);

    /// Cancels a sleep. Used to back out of a block that could not switch away.
    void wake_current();

    /// When the earliest sleeping thread is next due, or `tick` if one is
    /// already runnable. Lets the run loop skip idle time instead of spinning
    /// through it; see Emulator::run.
    [[nodiscard]] u32 earliest_wake(u32 tick) const;

    [[nodiscard]] Thread* find(u32 handle);
    [[nodiscard]] std::size_t count() const noexcept { return threads_.size(); }
    [[nodiscard]] std::size_t runnable_count(u32 tick) const;

    /// True if some live thread is merely *sleeping* -- blocked now, but with a
    /// wake time that will arrive on its own.
    ///
    /// This is the difference between an idle guest and a stuck one, and the
    /// deadlock watchdog turns on it. A menu loop is Sleep(n) then poll-input,
    /// so with nobody at the controls it has no runnable thread almost all of
    /// the time; "nothing is runnable right now" is what every game looks like
    /// between frames, not evidence of a deadlock. A guest is only genuinely
    /// stuck when every live thread is waiting on something that nothing is
    /// scheduled to deliver.
    [[nodiscard]] bool any_sleeping(u32 tick) const;
    [[nodiscard]] u32 current_handle() const;

    /// True while any thread has yet to finish, asleep or not.
    [[nodiscard]] bool any_alive() const;

    /// True once at least one extra thread exists, so the quantum loop only
    /// pays for scheduling when a game is actually threaded.
    [[nodiscard]] bool active() const noexcept { return threads_.size() > 1; }

    /// Thread table access, for save states. The contexts inside are plain
    /// values, so this round-trips without any host references.
    [[nodiscard]] const std::vector<Thread>& threads() const noexcept {
        return threads_;
    }
    void restore_threads(std::vector<Thread> threads, std::size_t current) {
        threads_ = std::move(threads);
        current_ = current < threads_.size() ? current : 0;
    }
    [[nodiscard]] std::size_t current_index() const noexcept { return current_; }

private:
    [[nodiscard]] bool is_runnable(const Thread& thread, u32 tick) const;
    void install_tls(AddressSpace& memory);

    std::vector<Thread> threads_;
    std::size_t current_ = 0;
    GuestAddr arena_ = 0;
    u32 arena_size_ = 0;
    u32 stack_size_ = 0;
    u32 next_stack_offset_ = 0;
    u32 next_handle_ = 0x0005'0100;
    GuestAddr tls_pointer_ = 0;
    GuestAddr thread_pointer_ = 0;
    u32 tls_bytes_ = 0;
};

}  // namespace clickhle

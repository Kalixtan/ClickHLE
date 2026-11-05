#include "clickhle/hle/scheduler.h"

#include "clickhle/common/log.h"
#include "clickhle/core/address_space.h"
#include "clickhle/hle/registry.h"

namespace clickhle {
namespace {

constexpr std::string_view kCategory = "sched";

/// Matches the loader's allowance below the top of the main stack.
constexpr u32 kStackHeadroom = 64;

}  // namespace

void Scheduler::initialise(GuestAddr stack_arena, u32 arena_size,
                           u32 stack_size, GuestAddr tls_pointer,
                           GuestAddr thread_pointer, u32 tls_bytes) {
    tls_pointer_ = tls_pointer;
    thread_pointer_ = thread_pointer;
    tls_bytes_ = tls_bytes;
    arena_ = stack_arena;
    arena_size_ = arena_size;
    stack_size_ = stack_size;
    next_stack_offset_ = 0;
    threads_.clear();
    current_ = 0;
}

void Scheduler::adopt_main_thread(u32 handle, GuestAddr tls_slots,
                                  ICpuCore& cpu) {
    Thread main;
    main.handle = handle;
    main.name = "main";
    main.tls_slots = tls_slots;
    main.context = cpu.save_context();
    main.started = true;
    threads_.push_back(main);
    current_ = 0;
}

u32 Scheduler::create(GuestAddr entry, GuestAddr parameter, bool suspended,
                      ICpuCore& cpu, AddressSpace& memory) {
    if (next_stack_offset_ + stack_size_ > arena_size_) {
        CLICKHLE_ERROR(kCategory) << "thread stack arena exhausted";
        return 0;
    }

    Thread thread;
    thread.handle = next_handle_;
    next_handle_ += 4;
    thread.name = "thread-" + std::to_string(threads_.size());
    // The TLS array lives at the bottom of the thread's own stack allocation,
    // which is the one region guaranteed to exist for exactly this thread's
    // lifetime. The stack grows down from the top, so they never meet.
    thread.tls_slots = arena_ + next_stack_offset_;
    thread.stack_base = arena_ + next_stack_offset_ + tls_bytes_;
    thread.stack_size = stack_size_ - tls_bytes_;
    next_stack_offset_ += stack_size_;
    thread.suspended = suspended ? 1 : 0;

    // Build the context the thread will start with. It has never run, so this
    // is synthesised rather than saved: entry point in pc, argument in r0, and
    // lr pointing at the exit sentinel so that returning from the entry
    // function is a clean thread exit rather than a branch into nowhere.
    CpuContext context = cpu.save_context();  // inherit cpsr (user mode, ARM)
    context.r.fill(0);
    context.r[reg::kR0] = parameter;
    context.r[reg::kPc] = entry;
    context.r[reg::kSp] = thread.stack_base + thread.stack_size - kStackHeadroom;
    thread.context = context;

    // The exit sentinel lives just below the return sentinel in the thunk page.
    const GuestAddr exit_address = ThunkTable::kReturnSentinel + 4;
    if (!memory.write<u32>(exit_address, encode_svc(kThreadExitSvc))) {
        CLICKHLE_ERROR(kCategory) << "failed to install the thread exit sentinel";
        return 0;
    }
    thread.context.r[reg::kLr] = exit_address;

    threads_.push_back(thread);
    CLICKHLE_DEBUG(kCategory)
        << "created " << thread.name << " entry=0x" << std::hex << entry
        << " stack=0x" << thread.stack_base << std::dec;
    return thread.handle;
}

bool Scheduler::is_runnable(const Thread& thread, u32 tick) const {
    if (thread.finished || thread.suspended > 0) {
        return false;
    }
    // Sleep is honoured against the guest clock rather than by blocking, so a
    // sleeping thread simply is not a candidate until its deadline passes.
    return static_cast<i32>(tick - thread.wake_at) >= 0;
}

std::size_t Scheduler::runnable_count(u32 tick) const {
    std::size_t count = 0;
    for (const auto& thread : threads_) {
        if (is_runnable(thread, tick)) {
            ++count;
        }
    }
    return count;
}

bool Scheduler::any_sleeping(u32 tick) const {
    for (const auto& thread : threads_) {
        if (thread.finished || thread.suspended > 0) {
            continue;
        }
        // Signed, because tick wraps. A wake_at "in the past" means the thread
        // is already due and runnable_count would have counted it.
        if (thread.wake_at != 0 &&
            static_cast<i32>(thread.wake_at - tick) > 0) {
            return true;
        }
    }
    return false;
}

bool Scheduler::any_alive() const {
    for (const auto& thread : threads_) {
        if (!thread.finished) {
            return true;
        }
    }
    return false;
}

u32 Scheduler::current_handle() const {
    return current_ < threads_.size() ? threads_[current_].handle : 0;
}

Scheduler::Thread* Scheduler::find(u32 handle) {
    for (auto& thread : threads_) {
        if (thread.handle == handle) {
            return &thread;
        }
    }
    return nullptr;
}

void Scheduler::install_tls(AddressSpace& memory) {
    if (current_ >= threads_.size()) {
        return;
    }
    if (tls_pointer_ != 0) {
        static_cast<void>(
            memory.write<u32>(tls_pointer_, threads_[current_].tls_slots));
    }
    // The running thread's handle, which its CRT uses to find its own
    // per-thread data. Must match what CreateThread reported.
    if (thread_pointer_ != 0) {
        static_cast<void>(
            memory.write<u32>(thread_pointer_, threads_[current_].handle));
    }
}

bool Scheduler::yield_now(ICpuCore& cpu, u32 tick, AddressSpace& memory) {
    if (threads_.size() < 2) {
        return false;
    }
    // Round-robin from the one after the current, so no thread is starved.
    std::size_t candidate = current_;
    for (std::size_t i = 1; i <= threads_.size(); ++i) {
        const std::size_t index = (current_ + i) % threads_.size();
        if (index != current_ && is_runnable(threads_[index], tick)) {
            candidate = index;
            break;
        }
    }
    if (candidate == current_) {
        return false;
    }

    threads_[current_].context = cpu.save_context();
    current_ = candidate;
    threads_[current_].started = true;
    cpu.restore_context(threads_[current_].context);
    install_tls(memory);
    CLICKHLE_TRACE(kCategory) << "switch -> " << threads_[current_].name
                            << " pc=0x" << std::hex
                            << threads_[current_].context.r[reg::kPc]
                            << std::dec;
    return true;
}

void Scheduler::sleep_current(u32 milliseconds, u32 tick) {
    if (current_ < threads_.size()) {
        threads_[current_].wake_at = tick + milliseconds;
    }
}

u32 Scheduler::earliest_wake(u32 tick) const {
    bool found = false;
    u32 best = 0;
    for (const auto& thread : threads_) {
        if (thread.finished || thread.suspended > 0) {
            continue;
        }
        // Signed, like is_runnable: the tick wraps and a naive comparison puts
        // a thread that is already due on the far side of four billion.
        const i32 delta = static_cast<i32>(thread.wake_at - tick);
        if (delta <= 0) {
            return tick;  // something is runnable now; nothing to skip
        }
        if (!found || delta < static_cast<i32>(best - tick)) {
            best = thread.wake_at;
            found = true;
        }
    }
    return found ? best : tick;
}

void Scheduler::wake_current() {
    if (current_ < threads_.size()) {
        threads_[current_].wake_at = 0;
    }
}

bool Scheduler::exit_current(ICpuCore& cpu, u32 tick, AddressSpace& memory) {
    if (current_ >= threads_.size()) {
        return false;
    }
    threads_[current_].finished = true;
    CLICKHLE_DEBUG(kCategory) << threads_[current_].name << " exited";

    for (std::size_t i = 1; i <= threads_.size(); ++i) {
        const std::size_t index = (current_ + i) % threads_.size();
        if (is_runnable(threads_[index], tick)) {
            current_ = index;
            threads_[current_].started = true;
            cpu.restore_context(threads_[current_].context);
            install_tls(memory);
            return true;
        }
    }
    // Nothing runnable right now. Crucially this must NOT report success: no
    // context was restored, so the CPU is still holding the dead thread's
    // registers and would carry on executing past its exit sentinel with
    // whatever happens to be in them. The caller stops this run slice, idles,
    // and re-enters once a sleeping thread is due.
    return false;
}

}  // namespace clickhle

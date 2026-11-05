#pragma once

#include <array>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "clickhle/common/types.h"

namespace clickhle {

class AddressSpace;

/// ARM register indices, named for readability at call sites.
namespace reg {
inline constexpr unsigned kR0 = 0;
inline constexpr unsigned kR1 = 1;
inline constexpr unsigned kR2 = 2;
inline constexpr unsigned kR3 = 3;
inline constexpr unsigned kSp = 13;
inline constexpr unsigned kLr = 14;
inline constexpr unsigned kPc = 15;
inline constexpr unsigned kCount = 16;
}  // namespace reg

/// A complete user-mode ARM context.
///
/// Saved and restored wholesale for two things that both matter a lot later:
/// calling from host back into guest code (WndProc, thread entry points, qsort
/// comparators), and per-thread scheduling. Keep it trivially copyable and
/// free of pointers so it can be serialised into a save state as-is.
struct CpuContext {
    std::array<u32, reg::kCount> r{};
    u32 cpsr = 0;

    [[nodiscard]] bool thumb() const noexcept { return (cpsr & (1u << 5)) != 0; }
    void set_thumb(bool on) noexcept {
        cpsr = on ? (cpsr | (1u << 5)) : (cpsr & ~(1u << 5));
    }
};

enum class RunStatus {
    /// Hit an SVC. The handler has already run; `pc` is wherever it left it.
    Svc,
    /// request_stop() was called, or the guest reached the return sentinel.
    Stopped,
    /// Ran out of the instruction budget passed to run().
    BudgetExhausted,
    /// Unmapped access, undefined instruction, or a core-level error.
    Fault,
    /// No CPU backend is compiled in.
    Unsupported,
};

[[nodiscard]] std::string_view to_string(RunStatus status) noexcept;

inline std::ostream& operator<<(std::ostream& stream, RunStatus status) {
    return stream << to_string(status);
}

struct RunResult {
    RunStatus status = RunStatus::Unsupported;
    u64 instructions_executed = 0;
    /// Set when status == Svc: the SVC immediate the guest executed.
    u32 svc_immediate = 0;
    /// Set when status == Fault: a human-readable description.
    std::string fault;
};

/// ARM CPU interface. This build provides the Unicorn backend only.
class ICpuCore {
public:
    /// Invoked when the guest executes SVC. The handler may read and write
    /// registers and memory; whatever it leaves in `pc` is where execution
    /// resumes. Returning false asks the core to stop rather than continue.
    using SvcHandler = std::function<bool(u32 immediate)>;

    virtual ~ICpuCore() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;

    virtual void reset() = 0;

    [[nodiscard]] virtual u32 reg(unsigned index) const = 0;
    virtual void set_reg(unsigned index, u32 value) = 0;

    [[nodiscard]] virtual CpuContext save_context() const = 0;
    virtual void restore_context(const CpuContext& context) = 0;

    /// Runs until an SVC, a fault, a stop request, or `max_instructions`.
    virtual RunResult run(u64 max_instructions) = 0;

    /// Safe to call from an SVC handler to unwind out of run().
    virtual void request_stop() = 0;

    /// Stop just before executing this address, and report it as a fault so
    /// the run dumps registers and call history.
    ///
    /// Costs nothing when no breakpoint is set -- the check is skipped
    /// entirely, so the instruction loop is unchanged for ordinary runs.
    /// `reg_index` < 16 makes it conditional: stop only when that register
    /// holds `reg_value`. Without a condition the first hit wins, which is no
    /// use when a function is called thousands of times and only the last call
    /// is wrong.
    /// Counts executions of an address without stopping the run.
    ///
    /// A breakpoint answers "is this reached"; it cannot answer "how often",
    /// because it stops on the first hit. Distinguishing "entered once" from
    /// "entered every frame" is exactly the question a stalled title poses, and
    /// it is not answerable with a stop.
    virtual void add_tracepoint(u32 address) { (void)address; }
    [[nodiscard]] virtual u64 tracepoint_hits(u32 address) const {
        (void)address;
        return 0;
    }

    /// Sample the executing pc every `period` instructions, or 0 to disable.
    ///
    /// A tracepoint answers "is this address reached", which requires already
    /// knowing the address. A hang does not offer one: the whole question is
    /// where the guest went. This builds a histogram instead, so the answer
    /// falls out of the top entry without a hypothesis.
    virtual void set_profile_period(u64 period) { (void)period; }

    /// The `count` hottest sampled addresses, descending.
    [[nodiscard]] virtual std::vector<std::pair<u32, u64>> profile_top(
        std::size_t count) const {
        (void)count;
        return {};
    }

    virtual void add_breakpoint(u32 address, unsigned reg_index = 32,
                                u32 reg_value = 0) = 0;

    virtual void set_svc_handler(SvcHandler handler) = 0;

    /// Instructions retired since the last reset, live during a run.
    ///
    /// The emulated clock is derived from this, so it has to be readable from
    /// inside an SVC handler rather than only at the end of a run slice.
    [[nodiscard]] virtual u64 instruction_count() const = 0;

    // Convenience wrappers shared by every backend.
    [[nodiscard]] u32 pc() const { return reg(reg::kPc); }
    void set_pc(u32 value) { set_reg(reg::kPc, value); }
    [[nodiscard]] u32 sp() const { return reg(reg::kSp); }
    void set_sp(u32 value) { set_reg(reg::kSp, value); }
    [[nodiscard]] u32 lr() const { return reg(reg::kLr); }
    void set_lr(u32 value) { set_reg(reg::kLr, value); }
};

/// Creates a core by backend name. Returns nullptr if the name is unknown or
/// the backend was not compiled in. Pass an empty name for the build default.
[[nodiscard]] std::unique_ptr<ICpuCore> make_cpu_core(std::string_view backend,
                                                      AddressSpace& memory);

/// Names of the backends compiled into this build, for diagnostics and --help.
[[nodiscard]] std::vector<std::string_view> available_cpu_backends();

}  // namespace clickhle

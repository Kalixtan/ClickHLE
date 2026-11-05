// Unicorn adapter for the ARM guest CPU. Guest memory is shared with the
// emulator core, and CPU hooks route SVCs and memory faults to that core.
// Unicorn is loaded at runtime; see unicorn_dynload.h.

#include <sstream>

#include "clickhle/common/log.h"
#include "clickhle/core/address_space.h"
#include "clickhle/core/cpu.h"
#include "unicorn_dynload.h"
#include "unicorn_shim.h"

namespace clickhle {
namespace {

namespace us = unicorn_shim;

constexpr std::string_view kCategory = "cpu";

// The interface exposes the current instruction address at stop points.
// Unicorn reports that address directly, without ARM7's pipeline lookahead.

// r0..r15, in ICpuCore's index order. NOT assumed contiguous with
// UC_ARM_REG_R0 -- see unicorn_shim.h's own comment on exactly why.
constexpr int kRegMap[reg::kCount] = {
    us::kRegR0,  us::kRegR1,  us::kRegR2,  us::kRegR3, us::kRegR4,
    us::kRegR5,  us::kRegR6,  us::kRegR7,  us::kRegR8, us::kRegR9,
    us::kRegR10, us::kRegR11, us::kRegR12, us::kRegSp, us::kRegLr,
    us::kRegPc,
};

class UnicornCpu final : public ICpuCore {
public:
    explicit UnicornCpu(AddressSpace& memory) : memory_(memory), lib_(UnicornDynload::instance()) {
        if (!lib_.available()) {
            CLICKHLE_ERROR(kCategory) << lib_.error();
            return;
        }
        if (lib_.open(us::kArchArm, us::kModeArm, &uc_) != us::kErrOk) {
            CLICKHLE_ERROR(kCategory) << "uc_open failed";
            uc_ = nullptr;
            return;
        }
        // Select the closest available match to the real iPod's ARM7TDMI
        // core. uc_open() alone does not pick a specific CPU model -- see
        // unicorn_shim.h's own comment on why UC_CPU_ARM_926 is the best
        // Unicorn offers here (not an exact match, unlike DangerVM's
        // oracle). Must happen before any other Unicorn API call per
        // unicorn.h's own doc comment on UC_CTL_CPU_MODEL, hence here.
        const us::uc_err ctl_err = lib_.ctl_set_cpu_model(
            uc_, static_cast<int>(us::uc_ctl_write(us::kCtlCpuModel, 1)), us::kCpuArm926);
        if (ctl_err != us::kErrOk) {
            CLICKHLE_ERROR(kCategory) << "uc_ctl(CPU_MODEL) failed: " << lib_.strerror(ctl_err);
        }
        map_existing_regions();

        lib_.hook_add(uc_, &intr_hook_, us::kHookIntr,
                      reinterpret_cast<void*>(&UnicornCpu::onIntrThunk), this, 1, 0);
        lib_.hook_add(uc_, &code_hook_, us::kHookCode,
                      reinterpret_cast<void*>(&UnicornCpu::onCodeThunk), this, 1, 0);
        lib_.hook_add(uc_, &mem_hook_, us::kHookMemUnmapped,
                      reinterpret_cast<void*>(&UnicornCpu::onUnmappedThunk), this, 1, 0);
    }

    ~UnicornCpu() override {
        if (uc_ != nullptr) {
            lib_.close(uc_);
        }
    }

    [[nodiscard]] std::string_view name() const override { return "unicorn"; }

    [[nodiscard]] bool ready() const { return uc_ != nullptr; }

    void reset() override {
        if (uc_ == nullptr) {
            return;
        }
        // CPSR MUST be written first: SP/LR are ARM banked registers (a
        // separate physical copy per processor mode), so zeroing them
        // BEFORE switching to USR mode would zero the wrong mode's copy,
        // leaving the USR-mode SP/LR at whatever Unicorn's own post-open
        // reset value happens to be instead of the explicit 0 intended
        // here. Caught (in the sibling DangerVM oracle's Machine::call(),
        // same underlying bug) when SP/LR read back as 0 after a real
        // value was written, immediately explained by this exact ordering.
        //
        // USR mode, ARM (not Thumb) state, interrupts unmasked -- the same
        // post-reset state AresCpu::reset() leaves the guest in (see its
        // own comment: ares' power() resets to supervisor mode at the
        // vector, which is real-hardware-correct and wrong for a userland
        // HLE guest, so both backends force USR/i=0/f=0 here instead).
        constexpr u32 kCpsrUsrArm = 0x10;
        u32 cpsr = kCpsrUsrArm;
        lib_.reg_write(uc_, us::kRegCpsr, &cpsr);
        for (unsigned i = 0; i < reg::kCount; ++i) {
            u32 zero = 0;
            lib_.reg_write(uc_, kRegMap[i], &zero);
        }
        instructions_ = 0;
        stop_requested_ = false;
        fault_.clear();
        real_pc_ = 0;
    }

    [[nodiscard]] u32 reg(unsigned index) const override {
        if (uc_ == nullptr || index >= reg::kCount) {
            return 0;
        }
        u32 value = 0;
        lib_.reg_read(uc_, kRegMap[index], &value);
        return value;
    }

    void set_reg(unsigned index, u32 value) override {
        if (uc_ == nullptr || index >= reg::kCount) {
            return;
        }
        if (index == reg::kPc) {
            pc_written_ = true;
            // set_pc() writes the real target address directly -- no
            // lookahead applied on writes, matching AresCpu::set() /
            // set_pc()'s own documented convention exactly.
        }
        lib_.reg_write(uc_, kRegMap[index], &value);
    }

    [[nodiscard]] CpuContext save_context() const override {
        CpuContext context;
        for (unsigned i = 0; i < reg::kCount; ++i) {
            context.r[i] = reg(i);
        }
        if (uc_ != nullptr) {
            u32 cpsr = 0;
            lib_.reg_read(uc_, us::kRegCpsr, &cpsr);
            context.cpsr = cpsr;
        }
        return context;
    }

    void restore_context(const CpuContext& context) override {
        if (uc_ == nullptr) {
            return;
        }
        u32 cpsr = context.cpsr;
        lib_.reg_write(uc_, us::kRegCpsr, &cpsr);
        for (unsigned i = 0; i < reg::kCount; ++i) {
            if (i == reg::kPc) {
                continue;  // handled below, real (non-lookahead) address
            }
            u32 v = context.r[i];
            lib_.reg_write(uc_, kRegMap[i], &v);
        }
        u32 real_pc = context.r[reg::kPc];
        lib_.reg_write(uc_, us::kRegPc, &real_pc);
        pc_written_ = false;
    }

    RunResult run(u64 max_instructions) override {
        stop_requested_ = false;
        fault_.clear();
        if (uc_ == nullptr) {
            RunResult result;
            result.status = RunStatus::Unsupported;
            result.fault = lib_.available()
                                ? "unicorn backend failed to initialize"
                                : lib_.error();
            return result;
        }

        const u64 start = instructions_;

        u32 start_pc = 0;
        lib_.reg_read(uc_, us::kRegPc, &start_pc);
        // uc_emu_start's `count` parameter caps executed instructions
        // directly -- no manual loop needed, matching run()'s own budget
        // semantics (0 = unbounded, handled by uc_emu_start already
        // treating 0 as "no instruction limit").
        const us::uc_err err = lib_.emu_start(uc_, start_pc, 0, 0,
                                              max_instructions == 0
                                                  ? 0
                                                  : static_cast<std::size_t>(max_instructions));

        RunStatus status;
        if (!fault_.empty()) {
            status = RunStatus::Fault;
        } else if (stop_requested_) {
            status = RunStatus::Stopped;
        } else if (err == us::kErrOk &&
                  max_instructions != 0 &&
                  (instructions_ - start) >= max_instructions) {
            status = RunStatus::BudgetExhausted;
        } else if (err != us::kErrOk) {
            // Something Unicorn itself stopped on that our hooks didn't
            // already classify (e.g. an invalid instruction) -- report it
            // rather than silently calling it "Stopped".
            std::ostringstream message;
            message << "unicorn stopped: " << lib_.strerror(err);
            fault_ = message.str();
            status = RunStatus::Fault;
        } else {
            status = RunStatus::Stopped;
        }

        RunResult result;
        result.status = status;
        result.instructions_executed = instructions_ - start;
        result.svc_immediate = last_svc_;
        result.fault = fault_;
        return result;
    }

    void request_stop() override {
        stop_requested_ = true;
        if (uc_ != nullptr) {
            lib_.emu_stop(uc_);
        }
    }

    void add_breakpoint(u32 address, unsigned reg_index, u32 reg_value) override {
        breakpoints_.push_back({address, reg_index, reg_value});
    }

    void set_svc_handler(SvcHandler handler) override {
        svc_handler_ = std::move(handler);
    }

    [[nodiscard]] u64 instruction_count() const override { return instructions_; }

private:
    struct Breakpoint {
        u32 address;
        unsigned reg_index;
        u32 reg_value;
    };

    [[nodiscard]] bool thumb() const {
        if (uc_ == nullptr) {
            return false;
        }
        u32 cpsr = 0;
        lib_.reg_read(uc_, us::kRegCpsr, &cpsr);
        return (cpsr & (1u << 5)) != 0;
    }

    void map_existing_regions() {
        for (const auto& region : memory_.regions()) {
            u32 perms = us::kProtNone;
            if (region->prot & AddressSpace::kRead) perms |= us::kProtRead;
            if (region->prot & AddressSpace::kWrite) perms |= us::kProtWrite;
            if (region->prot & AddressSpace::kExec) perms |= us::kProtExec;
            void* ptr = memory_.host_ptr(region->base, region->size);
            if (ptr == nullptr) {
                continue;  // a mirror region: no storage of its own, see below
            }
            const us::uc_err err =
                lib_.mem_map_ptr(uc_, region->base, region->size, perms, ptr);
            if (err != us::kErrOk) {
                CLICKHLE_ERROR(kCategory)
                    << "uc_mem_map_ptr failed for region '" << region->name
                    << "': " << lib_.strerror(err);
            }
        }
        // Mirrors (map_mirror()) have no storage of their own in
        // AddressSpace -- host_ptr() resolves them transparently for
        // memory_.read()/write(), but Unicorn needs its own real mapping.
        // Map each mirror region onto the SAME host bytes as its source so
        // a guest access through either alias lands on one shared buffer,
        // exactly like AddressSpace's own aliasing.
        for (const auto& region : memory_.regions()) {
            if (!region->is_mirror) {
                continue;
            }
            void* ptr = memory_.host_ptr(region->mirror_source, region->size);
            if (ptr == nullptr) {
                continue;
            }
            u32 perms = us::kProtNone;
            if (region->prot & AddressSpace::kRead) perms |= us::kProtRead;
            if (region->prot & AddressSpace::kWrite) perms |= us::kProtWrite;
            if (region->prot & AddressSpace::kExec) perms |= us::kProtExec;
            lib_.mem_map_ptr(uc_, region->base, region->size, perms, ptr);
        }
    }

    // --- Unicorn hook trampolines (C linkage callbacks -> member calls) ---

    static void onIntrThunk(us::uc_engine*, u32 intno, void* user_data) {
        static_cast<UnicornCpu*>(user_data)->onIntr(intno);
    }
    static void onCodeThunk(us::uc_engine*, u64 address, u32 size, void* user_data) {
        static_cast<UnicornCpu*>(user_data)->onCode(static_cast<u32>(address), size);
    }
    static bool onUnmappedThunk(us::uc_engine*, us::uc_mem_type type, u64 address,
                                int size, i64 value, void* user_data) {
        return static_cast<UnicornCpu*>(user_data)->onUnmapped(type, static_cast<u32>(address), size, value);
    }

    void onCode(u32 address, u32 /*size*/) {
        real_pc_ = address;
        ++instructions_;
        if (!breakpoints_.empty()) {
            for (const auto& bp : breakpoints_) {
                if (bp.address != address) {
                    continue;
                }
                if (bp.reg_index < reg::kCount && reg(bp.reg_index) != bp.reg_value) {
                    continue;
                }
                std::ostringstream message;
                message << "breakpoint at 0x" << std::hex << address;
                fault_ = message.str();
                CLICKHLE_INFO(kCategory) << fault_;
                stop_requested_ = true;
                lib_.emu_stop(uc_);
                return;
            }
        }
    }

    bool onUnmapped(us::uc_mem_type type, u32 address, int /*size*/, i64 /*value*/) {
        std::ostringstream message;
        const char* what = (type == us::kMemWriteUnmapped || type == us::kMemWriteProt)
                              ? "write"
                              : "read";
        message << "unmapped " << what << " at 0x" << std::hex << address
               << " (pc 0x" << pc() << ")";
        fault_ = message.str();
        CLICKHLE_ERROR(kCategory) << fault_;
        std::ostringstream regs;
        for (unsigned i = 0; i < reg::kCount; ++i) {
            regs << " r" << i << "=0x" << std::hex << reg(i);
        }
        CLICKHLE_ERROR(kCategory) << "regs:" << regs.str();
        return false;  // false = do not resume; this is a real fault
    }

    /// Same host-call boundary and pc/lr fallback rules as
    /// AresCpu::softwareInterrupt() -- see that function's own long
    /// comment for why both signals (pc changed, or pc_written_ set) are
    /// needed, and why ARM semihosting's `svc #0x123456` gets a different
    /// fallback (before-4, the trap's own next instruction) than an
    /// ordinal SVC (lr(), the calling function's return address).
    void onIntr(u32 intno) {
        // ARM SWI/SVC is intno 2 in Unicorn's ARM model. Anything else
        // (undefined instruction, data abort, ...) is a real fault this
        // adapter doesn't model -- report and stop rather than guess.
        constexpr u32 kArmSwiIntNo = 2;
        if (intno != kArmSwiIntNo) {
            std::ostringstream message;
            message << "unicorn interrupt " << intno << " at pc 0x" << std::hex << pc();
            fault_ = message.str();
            CLICKHLE_ERROR(kCategory) << fault_;
            lib_.emu_stop(uc_);
            return;
        }

        // At interrupt time Unicorn's PC has already advanced past the
        // SVC instruction, so the instruction itself sits at pc-4 (ARM) or
        // pc-2 (Thumb). Decode the real immediate from its own encoding --
        // ARM: cond(4) 1111(4) imm24(24); Thumb: 11011111 imm8.
        u32 raw_pc = 0;
        lib_.reg_read(uc_, us::kRegPc, &raw_pc);
        u32 immediate = 0;
        if (thumb()) {
            const auto instr = memory_.read<u16>(raw_pc - 2);
            immediate = instr ? (*instr & 0xffu) : 0;
        } else {
            const auto instr = memory_.read<u32>(raw_pc - 4);
            immediate = instr ? (*instr & 0xffffffu) : 0;
        }
        last_svc_ = immediate;

        if (!svc_handler_) {
            std::ostringstream message;
            message << "unmapped svc 0x" << std::hex << immediate;
            fault_ = message.str();
            CLICKHLE_ERROR(kCategory) << fault_;
            lib_.emu_stop(uc_);
            return;
        }

        const u32 before = raw_pc;
        pc_written_ = false;
        const bool keep_running = svc_handler_(immediate);

        u32 after = 0;
        lib_.reg_read(uc_, us::kRegPc, &after);
        if (after == before && !pc_written_) {
            constexpr u32 kSemihostSvc = 0x123456;
            u32 target = 0;
            if (immediate == kSemihostSvc) {
                target = before - (thumb() ? 2u : 4u);
            } else {
                lib_.reg_read(uc_, us::kRegLr, &target);
            }
            lib_.reg_write(uc_, us::kRegPc, &target);
        }

        if (!keep_running) {
            stop_requested_ = true;
            lib_.emu_stop(uc_);
        }
    }

    AddressSpace& memory_;
    UnicornDynload& lib_;
    us::uc_engine* uc_ = nullptr;
    us::uc_hook intr_hook_{};
    us::uc_hook code_hook_{};
    us::uc_hook mem_hook_{};

    u64 instructions_ = 0;
    u32 real_pc_ = 0;
    u32 last_svc_ = 0;
    bool pc_written_ = false;
    bool stop_requested_ = false;
    std::string fault_;
    SvcHandler svc_handler_;
    std::vector<Breakpoint> breakpoints_;
};

}  // namespace

bool unicorn_cpu_available() { return UnicornDynload::instance().available(); }

std::unique_ptr<ICpuCore> make_unicorn_cpu_core(AddressSpace& memory) {
    auto core = std::make_unique<UnicornCpu>(memory);
    if (!core->ready()) {
        return nullptr;
    }
    return core;
}

}  // namespace clickhle

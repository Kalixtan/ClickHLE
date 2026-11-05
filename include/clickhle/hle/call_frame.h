#pragma once

#include "clickhle/common/endian.h"
#include "clickhle/common/types.h"
#include "clickhle/core/address_space.h"
#include "clickhle/core/cpu.h"

namespace clickhle {

class Emulator;

/// Reads guest function arguments according to the ARM procedure call standard.
///
/// Words 0-3 come from r0-r3, the rest from the stack at sp. 64-bit values are
/// 8-byte aligned, which in register terms means they must start on an even
/// register. The ARM7TDMI has no FPU, so floats and doubles are passed in core
/// registers exactly like integers -- there is no separate floating-point path
/// to model.
class ArgReader {
public:
    ArgReader(const ICpuCore& cpu, const AddressSpace& memory)
        : cpu_(cpu), memory_(memory) {}

    [[nodiscard]] u32 next_u32();
    [[nodiscard]] i32 next_i32() { return static_cast<i32>(next_u32()); }
    [[nodiscard]] u64 next_u64();
    [[nodiscard]] i64 next_i64() { return static_cast<i64>(next_u64()); }
    [[nodiscard]] GuestAddr next_ptr() { return next_u32(); }
    [[nodiscard]] float next_float() {
        return endian::bits_to_float(next_u32());
    }
    [[nodiscard]] double next_double() {
        return endian::bits_to_double(next_u64());
    }

    /// False once any read has fallen off the end of the mapped stack, which
    /// almost always means the arity declared for the function is wrong.
    [[nodiscard]] bool ok() const noexcept { return ok_; }

    void reset() noexcept {
        word_index_ = 0;
        ok_ = true;
    }

private:
    [[nodiscard]] u32 next_word();
    void align_to_even();

    const ICpuCore& cpu_;
    const AddressSpace& memory_;
    unsigned word_index_ = 0;
    bool ok_ = true;
};

/// Everything an HLE function implementation is handed.
///
/// Note what is absent: no host pointers to guest data outlive the call, and
/// no state lives in the handler itself. Handlers must be re-entrant, because
/// several of them (qsort, EnumWindows, DispatchMessage) call back into guest
/// code, which can re-enter the HLE layer.
struct HleCallFrame {
    ICpuCore& cpu;
    AddressSpace& memory;
    Emulator& emulator;
    ArgReader args;

    HleCallFrame(ICpuCore& cpu_ref, AddressSpace& memory_ref,
                 Emulator& emulator_ref)
        : cpu(cpu_ref),
          memory(memory_ref),
          emulator(emulator_ref),
          args(cpu_ref, memory_ref) {}

    /// Writes one field of a guest output structure.
    ///
    /// Unlike AddressSpace::write this is not [[nodiscard]]. Filling in a MSG
    /// or PAINTSTRUCT field by field, the useful reaction to a bad pointer is
    /// one diagnostic naming it, not seven identical error branches -- and a
    /// caller that passed a bad struct pointer has already lost.
    template <typename T>
    bool store(GuestAddr address, T value) {
        if (memory.write<T>(address, value)) {
            return true;
        }
        warn_bad_store(address);
        return false;
    }

    void ret(u32 value) { cpu.set_reg(reg::kR0, value); }

    void ret64(u64 value) {
        cpu.set_reg(reg::kR0, static_cast<u32>(value & 0xFFFFFFFFu));
        cpu.set_reg(reg::kR1, static_cast<u32>(value >> 32));
    }

    void ret_float(float value) { ret(endian::float_to_bits(value)); }
    void ret_double(double value) { ret64(endian::double_to_bits(value)); }
    void ret_void() {}

private:
    static void warn_bad_store(GuestAddr address);
};

}  // namespace clickhle

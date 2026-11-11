#pragma once

#include "clickhle/common/types.h"
#include <array>
#include <cstdint>

namespace clickhle::vc2 {
enum class VectorKernel { General, Move, Add, Subtract, ReverseSubtract, And, Or, Complement, Multiply };
class Core;
struct VectorOperand {
    u32 base = 0;
    bool vertical = false;
    unsigned width = 1;
    int reg = -1;
    bool step = false, discard = false;
    char role = 0;
    bool packed_columns = false;
    u32 offset(const Core &c, unsigned k) const;
};


// Shared by the interpreter and native lowering. These fields describe the
// existing model, including provisional forms guarded by the decoder.
struct VectorModes {
    bool nibble = false;
    bool blend = false;
    bool byte_mask = false;
    bool texture_nibble_mask = false;
    bool alpha_bit = false;
    bool half_complement = false;
    bool complement = false;
    bool byte_or = false;
    bool halfword_or = false;
    bool quant_sign = false;
    bool edge_sign = false;
    bool nibble_or = false;
    bool split48 = false;
    bool split80 = false;
    bool lowcarry = false;
    bool lowborrow = false;
    bool vertical_carry = false;
    bool highcarry = false;
    bool highborrow = false;
    bool carry_operands = false;
    bool filtered_byte = false;
    VectorKernel kernel = VectorKernel::General;
};
struct VectorPlan {
    std::array<u32, 5> words{};
    unsigned length = 0;
    VectorOperand d, a, b;
    unsigned count = 1, pred = 0, sru = 0, sreg = 7, scalar = 7;
    bool setf = false, clear = false, accumulate = false, x15 = false;
    int bkind = 0;
    std::int64_t immediate = 0;
    VectorModes modes;
};
} // namespace clickhle::vc2

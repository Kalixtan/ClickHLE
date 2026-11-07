#pragma once
#include "clickhle/common/types.h"

namespace clickhle {
// RetailOS truncates floats to signed words, scaling by 2^16 for fixed-point
// arguments. Its soft-float conversion saturates overflow and returns zero
// for NaNs. Work from the IEEE bits to avoid undefined host float-to-int casts.
inline u32 float_to_scaled_integer(u32 bits, unsigned fractional_bits) {
    const u32 exponent = (bits >> 23) & 255, fraction = bits & 0x7fffff;
    if (exponent == 255 && fraction)
        return 0;
    if (exponent >= 158 - fractional_bits)
        return bits & 0x80000000 ? 0x80000000 : 0x7fffffff;
    if (exponent < 127 - fractional_bits)
        return 0;
    const u32 mantissa = fraction | 0x800000;
    const unsigned shift = 150 - fractional_bits;
    const u32 magnitude = exponent >= shift ? mantissa << (exponent - shift)
                                            : mantissa >> (shift - exponent);
    return bits & 0x80000000 ? 0u - magnitude : magnitude;
}
inline u32 float_to_fixed16(u32 bits) { return float_to_scaled_integer(bits, 16); }
inline u32 float_to_integer(u32 bits) { return float_to_scaled_integer(bits, 0); }
} // namespace clickhle

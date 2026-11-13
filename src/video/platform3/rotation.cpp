#include "clickhle/video/classic_rotation.h"
#include "sine_table.h"

#include <bit>
#include <cstdint>
#include <limits>

namespace clickhle::classic {
namespace {

i32 word(u32 value) { return std::bit_cast<i32>(value); }
u32 bits(i32 value) { return std::bit_cast<u32>(value); }
i32 add(i32 a, i32 b) { return word(bits(a) + bits(b)); }
i32 sub(i32 a, i32 b) { return word(bits(a) - bits(b)); }
i32 multiply(i32 a, i32 b) {
    return word(static_cast<u32>((static_cast<i64>(a) * b) >> 16));
}
i32 shift_right(i32 value, unsigned amount) {
    return amount >= 32 ? (value < 0 ? -1 : 0) : value >> amount;
}
i32 shift_left(i32 value, unsigned amount) {
    return amount >= 32 ? 0 : word(bits(value) << amount);
}

i32 inverse_sqrt(i32 value) {
    if (value == 0) return std::numeric_limits<i32>::max();
    if (value == 0x10000) return value;
    constexpr std::array<u16, 8> seeds{46340, 43690, 41448, 39519,
                                       37837, 36352, 35030, 33842};
    const i32 leading = std::countl_zero(bits(value));
    const unsigned shift = static_cast<u32>(28 - leading) & 0xffu;
    const u32 index = bits(shift_right(value, shift)) & 7u;
    const i32 exponent = leading - 16;
    i32 result = static_cast<i32>(seeds[index]) * 2;
    if (exponent > 0)
        result = shift_left(result, static_cast<unsigned>((exponent & 1) + (exponent >> 1)));
    else result = shift_right(result, static_cast<unsigned>((-exponent) >> 1));
    if (exponent & 1) result = multiply(0xb504, result);
    for (unsigned i = 0; i < 3; ++i) {
        const i32 square = multiply(result, multiply(result, value));
        result = multiply(shift_right(result, 1), sub(0x30000, square));
    }
    return result;
}

i32 trig(i32 angle, bool cosine) {
    // 0x080de8d4 / 0x080de9b4: input is signed 16.16 radians.
    while (angle < 0) angle = add(angle, 411774);
    i32 phase = word(bits(angle) * 10430u);
    if (cosine) phase = shift_right(add(shift_right(phase, 16), 0x4000), 4);
    else phase = shift_right(phase, 20);
    u32 index = bits(phase) & 0x3ffu;
    if (bits(phase) & 0x400u) index = 0x3ffu - index;
    const i32 value = kSineTable[index];
    return (bits(phase) & 0x800u) ? -value : value;
}

} // namespace

std::array<i32, 16> rotation_matrix(i32 angle, i32 x, i32 y, i32 z) {
    const i32 norm = inverse_sqrt(add(add(multiply(x, x), multiply(y, y)), multiply(z, z)));
    x = multiply(norm, x); y = multiply(norm, y); z = multiply(norm, z);
    const i32 radians = multiply(angle, 1143);
    const i32 sine = trig(radians, false);
    const i32 cosine = trig(radians, true);
    const i32 one_minus_cosine = sub(0x10000, cosine);
    std::array<i32, 16> m{};
    m[0] = add(multiply(one_minus_cosine, multiply(x, x)), cosine);
    m[4] = sub(multiply(one_minus_cosine, multiply(y, x)), multiply(sine, z));
    m[8] = add(multiply(z, multiply(x, one_minus_cosine)), multiply(sine, y));
    m[1] = add(multiply(y, multiply(x, one_minus_cosine)), multiply(sine, z));
    m[5] = add(multiply(y, multiply(y, one_minus_cosine)), cosine);
    m[9] = sub(multiply(z, multiply(y, one_minus_cosine)), multiply(sine, x));
    m[2] = sub(multiply(z, multiply(x, one_minus_cosine)), multiply(sine, y));
    m[6] = add(multiply(z, multiply(y, one_minus_cosine)), multiply(sine, x));
    m[10] = add(multiply(z, multiply(z, one_minus_cosine)), cosine);
    m[15] = 0x10000;
    return m;
}

std::array<i32, 16> multiply_matrices(const std::array<i32, 16>& left,
                                      const std::array<i32, 16>& right) {
    std::array<i32, 16> result{};
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            u64 sum = 0;
            for (u32 i = 0; i < 4; ++i) {
                const i64 product = static_cast<i64>(left[row + i * 4]) *
                                    right[i + column * 4];
                sum += static_cast<u64>(product);
            }
            result[row + column * 4] = word(static_cast<u32>((sum + 0x8000u) >> 16));
        }
    }
    return result;
}

} // namespace clickhle::classic

#include "api.h"
#include "clickhle/common/endian.h"
#include "clickhle/hle/call_frame.h"
#include <array>
#include <stdexcept>

namespace clickhle {
namespace {
using Matrix = std::array<u32, 16>;

Matrix read_matrix(HleCallFrame &frame, u32 address) {
    Matrix result{};
    for (u32 i = 0; i < result.size(); ++i) {
        const auto value = frame.memory.read<u32>(address + 4 * i);
        if (!value) throw std::runtime_error("Invalid matrix source");
        result[i] = *value;
    }
    return result;
}

void write_matrix(HleCallFrame &frame, u32 address, const Matrix &matrix) {
    for (u32 i = 0; i < matrix.size(); ++i)
        if (!frame.store<u32>(address + 4 * i, matrix[i]))
            throw std::runtime_error("Invalid matrix destination");
}

float as_float(u32 bits) { return endian::bits_to_float(bits); }
u32 as_bits(float value) { return endian::float_to_bits(value); }
} // namespace

std::optional<u32> g5_graphics_call(u32 ordinal, HleCallFrame &frame) {
    if (ordinal != 165 && ordinal != 166 && (ordinal < 169 || ordinal > 172) && ordinal != 175 && ordinal != 176) return std::nullopt;
    const u32 destination = frame.cpu.reg(0);
    Matrix result{};
    const bool fixed = ordinal == 166 || ordinal == 170 || ordinal == 172 || ordinal == 176;
    const auto multiply_fixed = [](u32 a, u32 b) {
        return static_cast<u32>((i64(static_cast<i32>(a)) * static_cast<i32>(b)) >> 16);
    };
    if (ordinal == 165 || ordinal == 166) {
        for (unsigned i = 0; i < result.size(); i += 5) result[i] = fixed ? 0x10000 : 0x3f800000;
    } else if (ordinal >= 169 && ordinal <= 172) {
        result = read_matrix(frame, destination);
        const std::array<u32, 3> scale{frame.cpu.reg(1), frame.cpu.reg(2), frame.cpu.reg(3)};
        if (ordinal == 169 || ordinal == 170) {
            for (unsigned row = 0; row < 4; ++row) {
                if (fixed) {
                    result[row + 12] += multiply_fixed(result[row], scale[0]) +
                        multiply_fixed(result[row + 4], scale[1]) +
                        multiply_fixed(result[row + 8], scale[2]);
                } else {
                    float value = as_float(result[row]) * as_float(scale[0]);
                    value = value + as_float(result[row + 4]) * as_float(scale[1]);
                    value = value + as_float(result[row + 8]) * as_float(scale[2]);
                    result[row + 12] = as_bits(value + as_float(result[row + 12]));
                }
            }
        } else {
            for (unsigned column = 0; column < 3; ++column)
                for (unsigned row = 0; row < 4; ++row) {
                    const auto i = column * 4 + row;
                    result[i] = fixed ? multiply_fixed(result[i], scale[column]) :
                        as_bits(as_float(result[i]) * as_float(scale[column]));
                }
        }
    } else {
        // Read both operands before writing: guest matrices may overlap.
        const Matrix left = read_matrix(frame, frame.cpu.reg(1));
        const Matrix right = read_matrix(frame, frame.cpu.reg(2));
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned row = 0; row < 4; ++row) {
                if (fixed) {
                    u32 sum = 0;
                    for (unsigned k = 0; k < 4; ++k)
                        sum += multiply_fixed(left[k * 4 + row], right[column * 4 + k]);
                    result[column * 4 + row] = sum;
                    continue;
                }
                float sum = as_float(left[row]) * as_float(right[column * 4]);
                for (unsigned k = 1; k < 4; ++k)
                    sum = sum + as_float(left[k * 4 + row]) * as_float(right[column * 4 + k]);
                result[column * 4 + row] = as_bits(sum);
            }
    }
    write_matrix(frame, destination, result);
    return ordinal >= 175 ? result[15] : destination;
}
} // namespace clickhle

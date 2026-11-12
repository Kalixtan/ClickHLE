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
    if (ordinal != 165 && ordinal != 169 && ordinal != 175) return std::nullopt;
    const u32 destination = frame.cpu.reg(0);
    Matrix result{};
    if (ordinal == 165) {
        for (unsigned i = 0; i < result.size(); i += 5) result[i] = 0x3f800000;
    } else if (ordinal == 169) {
        result = read_matrix(frame, destination);
        const float x = as_float(frame.cpu.reg(1));
        const float y = as_float(frame.cpu.reg(2));
        const float z = as_float(frame.cpu.reg(3));
        for (unsigned row = 0; row < 4; ++row) {
            float value = as_float(result[row]) * x;
            value = value + as_float(result[row + 4]) * y;
            value = value + as_float(result[row + 8]) * z;
            result[row + 12] = as_bits(value + as_float(result[row + 12]));
        }
    } else {
        // Read both operands before writing: guest matrices may overlap.
        const Matrix left = read_matrix(frame, frame.cpu.reg(1));
        const Matrix right = read_matrix(frame, frame.cpu.reg(2));
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned row = 0; row < 4; ++row) {
                float sum = as_float(left[row]) * as_float(right[column * 4]);
                for (unsigned k = 1; k < 4; ++k)
                    sum = sum + as_float(left[k * 4 + row]) * as_float(right[column * 4 + k]);
                result[column * 4 + row] = as_bits(sum);
            }
    }
    write_matrix(frame, destination, result);
    return ordinal == 175 ? result[15] : destination;
}
} // namespace clickhle

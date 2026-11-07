#include "graphics_api.h"
#include "clickhle/common/endian.h"
#include "clickhle/hle/call_frame.h"
#include <cmath>
#include <stdexcept>

namespace clickhle {
u32 GraphicsApi::matrix(u32 ordinal, HleCallFrame &frame, const std::array<u32, 16> &a) {
    using Matrix = std::array<u32, 16>;
    const auto read = [&](u32 address) {
        Matrix out{};
        for (u32 i = 0; i < 16; ++i) {
            const auto value = frame.memory.read<u32>(address + 4 * i);
            if (!value) throw std::runtime_error("Invalid matrix source");
            out[i] = *value;
        }
        return out;
    };
    const auto write = [&](const Matrix &value) {
        for (u32 i = 0; i < 16; ++i)
            if (!frame.store<u32>(a[0] + 4 * i, value[i]))
                throw std::runtime_error("Invalid matrix destination");
    };
    const auto f = [](u32 bits) { return endian::bits_to_float(bits); };
    const auto bits = [](float value) { return endian::float_to_bits(value); };
    const auto fixed_mul = [](u32 left, u32 right) {
        return static_cast<u32>((static_cast<i64>(static_cast<i32>(left)) * static_cast<i32>(right)) >> 16);
    };
    Matrix out{};
    if (ordinal == 165 || ordinal == 166) {
        for (unsigned i = 0; i < 16; i += 5) out[i] = ordinal == 165 ? 0x3f800000 : 0x10000;
        write(out);
        return a[0];
    }
    if (ordinal == 167) {
        const float x = 1.0f / (f(a[2]) - f(a[1]));
        const float y = 1.0f / (f(a[4]) - f(a[3]));
        const float z = 1.0f / (f(a[5]) - f(a[6]));
        out[0] = bits(x * 2.0f); out[5] = bits(y * 2.0f); out[10] = bits(z * 2.0f);
        out[12] = bits(-(f(a[2]) + f(a[1])) * x);
        out[13] = bits(-(f(a[4]) + f(a[3])) * y);
        out[14] = bits((f(a[6]) + f(a[5])) * z);
        out[15] = 0x3f800000;
        write(out);
        return 0x3f800000;
    }
    if (ordinal == 169 || ordinal == 170 || ordinal == 171 || ordinal == 172) {
        out = read(a[0]);
        const bool fixed = ordinal == 170 || ordinal == 172;
        if (ordinal == 169 || ordinal == 170) {
            for (unsigned row = 0; row < 4; ++row) {
                if (fixed)
                    out[row + 12] += fixed_mul(out[row], a[1]) + fixed_mul(out[row + 4], a[2]) + fixed_mul(out[row + 8], a[3]);
                else {
                    float value = f(out[row]) * f(a[1]);
                    value = value + f(out[row + 4]) * f(a[2]);
                    value = value + f(out[row + 8]) * f(a[3]);
                    out[row + 12] = bits(value + f(out[row + 12]));
                }
            }
        } else {
            for (unsigned column = 0; column < 3; ++column)
                for (unsigned row = 0; row < 4; ++row) {
                    const unsigned i = column * 4 + row;
                    out[i] = fixed ? fixed_mul(out[i], a[column + 1]) : bits(f(out[i]) * f(a[column + 1]));
                }
        }
        write(out);
        return a[0];
    }
    if (ordinal == 173) {
        const float angle = f(a[1]) * f(0x3c8efa35);
        // Provisional numerical approximation: RetailOS uses a custom CORDIC
        // routine (10082c2c). Host trig differs by a few low bits for some angles.
        // Axis normalization and matrix operation order follow the ARM export.
        const float sine = std::sin(angle), cosine = std::cos(angle);
        const float norm = 1.0f / std::sqrt((f(a[2]) * f(a[2]) + f(a[3]) * f(a[3])) + f(a[4]) * f(a[4]));
        const float x = f(a[2]) * norm, y = f(a[3]) * norm, z = f(a[4]) * norm;
        const float xx = x*x, yy = y*y, zz = z*z, t = 1.0f - cosine;
        const float xy = (x*y)*t, yz = (y*z)*t, zx = (z*x)*t;
        const float xs = x*sine, ys = y*sine, zs = z*sine;
        const std::array<float, 9> rotation{
            (1.0f-xx)*cosine+xx, xy+zs, zx-ys,
            xy-zs, (1.0f-yy)*cosine+yy, yz+xs,
            zx+ys, yz-xs, (1.0f-zz)*cosine+zz};
        out = read(a[0]);
        for (unsigned row = 0; row < 4; ++row) {
            const float first = f(out[row]), second = f(out[row+4]), third = f(out[row+8]);
            for (unsigned column = 0; column < 3; ++column)
                out[row+column*4] = bits((first*rotation[column*3] + second*rotation[column*3+1]) + third*rotation[column*3+2]);
        }
        write(out);
        return 0;
    }
    if (ordinal == 175 || ordinal == 176) {
        // Snapshot both inputs so overlapping guest matrices remain safe.
        const auto left = read(a[1]), right = read(a[2]);
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned row = 0; row < 4; ++row) {
                if (ordinal == 176) {
                    u32 sum = 0;
                    for (unsigned k = 0; k < 4; ++k) sum += fixed_mul(left[k * 4 + row], right[column * 4 + k]);
                    out[column * 4 + row] = sum;
                } else {
                    float sum = f(left[row]) * f(right[column * 4]);
                    for (unsigned k = 1; k < 4; ++k)
                        sum = sum + f(left[k * 4 + row]) * f(right[column * 4 + k]);
                    out[column * 4 + row] = bits(sum);
                }
            }
        write(out);
        return out[15];
    }
    throw std::runtime_error("Native matrix API #" + std::to_string(ordinal) + " is not implemented");
}
} // namespace clickhle

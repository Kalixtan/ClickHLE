#include "graphics_api.h"
#include "clickhle/common/endian.h"
#include "clickhle/hle/call_frame.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace clickhle {
namespace {
i32 fixed_sine(u32 angle) {
    // The 257 retail samples equal floor(4096*sin(i*pi/512)), with the
    // endpoint clamped to 4095. Linear interpolation uses eight fraction bits.
    static const auto table = [] {
        std::array<i32, 257> values{};
        for (unsigned i = 0; i < values.size(); ++i)
            values[i] = std::min(4095, static_cast<int>(4096 * std::sin(i * 0x1.921fb54442d18p+1 / 512)));
        return values;
    }();
    const u32 index = (angle >> 16) & 1023;
    u32 fraction = (angle >> 8) & 255;
    i32 first, second;
    if (index < 256) { first = table[index]; second = table[index + 1]; }
    else if (index < 512) {
        first = table[511 - index]; second = table[512 - index]; fraction = 256 - fraction;
    } else if (index < 768) { first = -table[index - 512]; second = -table[index - 511]; }
    else { first = -table[1023 - index]; second = -table[1024 - index]; fraction = 256 - fraction; }
    return (first * static_cast<i32>(256 - fraction) + second * static_cast<i32>(fraction)) >> 4;
}
u32 fixed_root(u32 value) {
    // 0x10120e78 starts from a sign-extended, 32-bit (value << 8),
    // rounds the integer root, then scales the result by sixteen.
    const i32 scaled = static_cast<i32>(value << 8);
    if (scaled <= 0) return 0;
    u32 remainder = static_cast<u32>(scaled), root = 0;
    for (u32 bit = 1u << 30; bit; bit >>= 2) {
        if (remainder >= root + bit) { remainder -= root + bit; root = (root >> 1) + bit; }
        else root >>= 1;
    }
    if (remainder > root) ++root;
    return root << 4;
}
} // namespace

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
    if (ordinal == 168) {
        // Perspective uses double precision for tan(fov/2), then rounds the
        // cotangent to float. Host tan remains a numerical approximation to
        // the retail polynomial; this is not a bit-exact transcendental port.
        const double half_angle = static_cast<double>(f(a[1])) * 0.5 * 0x1.1df46a0000000p-6;
        const float cotangent = static_cast<float>(1.0 / std::tan(half_angle));
        const float reciprocal_depth = 1.0f / (f(a[3]) - f(a[4]));
        out[0] = bits(cotangent / f(a[2]));
        out[5] = bits(cotangent);
        out[10] = bits((f(a[4]) + f(a[3])) * reciprocal_depth);
        out[11] = 0xbf800000;
        out[14] = bits(((f(a[4]) * 2.0f) * f(a[3])) * reciprocal_depth);
        write(out);
        return out[14];
    }
    if (ordinal == 174) {
        const u32 angle = fixed_mul(a[1], 0x2d82d);
        const u32 sine = static_cast<u32>(fixed_sine(angle));
        const u32 cosine = static_cast<u32>(fixed_sine(angle + 0x1000000));
        const u32 length = fixed_root(fixed_mul(a[2], a[2]) + fixed_mul(a[3], a[3]) + fixed_mul(a[4], a[4]));
        if (!length) throw std::runtime_error("Invalid fixed rotation axis");
        // Preserve the retail scaling bug: its 64-bit numerator is 0x10000,
        // not 0x100000000, and the quotient is then shifted right by sixteen.
        const u32 reciprocal = static_cast<u32>((uint64_t(0x10000) / length) >> 16);
        const u32 x = fixed_mul(a[2], reciprocal), y = fixed_mul(a[3], reciprocal), z = fixed_mul(a[4], reciprocal);
        const u32 xx = fixed_mul(x, x), yy = fixed_mul(y, y), zz = fixed_mul(z, z), t = 0x10000 - cosine;
        const u32 xy = fixed_mul(fixed_mul(y, t), x), yz = fixed_mul(fixed_mul(z, t), y), zx = fixed_mul(fixed_mul(x, t), z);
        const u32 xs = fixed_mul(x, sine), ys = fixed_mul(y, sine), zs = fixed_mul(z, sine);
        const std::array<u32, 9> rotation{
            fixed_mul(0x10000-xx, cosine)+xx, xy+zs, zx-ys,
            xy-zs, fixed_mul(0x10000-yy, cosine)+yy, yz+xs,
            zx+ys, yz-xs, fixed_mul(0x10000-zz, cosine)+zz};
        out = read(a[0]);
        for (unsigned row = 0; row < 4; ++row) {
            const u32 first = out[row], second = out[row+4], third = out[row+8];
            for (unsigned column = 0; column < 3; ++column)
                out[row+column*4] = fixed_mul(first, rotation[column*3]) +
                    fixed_mul(second, rotation[column*3+1]) + fixed_mul(third, rotation[column*3+2]);
        }
        write(out);
        return out[11];
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

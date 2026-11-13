#include "graphics_api.h"
#include "graphics_numeric.h"
#include "clickhle/hle/call_frame.h"
#include <stdexcept>

namespace clickhle {
u32 GraphicsApi::attribute_value(u32 ordinal, HleCallFrame &frame, const std::array<u32, 16> &a) {
    if (a[0] >= 16) {
        error_ = 0x501;
        throw std::runtime_error("Invalid vertex attribute index");
    }
    if (ordinal == 135 || ordinal == 136 || ordinal == 150) {
        // The retail scalar routines discard z; the float-array routine
        // overwrites z with w. All three upload an unwritten fourth word.
        // Read that word at the original SP-relative location rather than
        // inventing a homogeneous coordinate. Earlier firmware stack history
        // is not reproduced by HLE, so this component remains indeterminate.
        const u32 offset = ordinal == 136 ? 16 : 20;
        const auto stale = frame.memory.read<u32>(frame.cpu.sp() - offset);
        if (!stale) throw std::runtime_error("Unmapped RetailOS attribute scratch word");
        std::array<u32, 4> values{};
        for (unsigned i = 0; i < 3; ++i) {
            const unsigned component = i == 2 ? 3 : i;
            u32 value = a[component + 1];
            if (ordinal == 136) {
                const auto source = frame.memory.read<u32>(a[1] + component * 4);
                if (!source) throw std::runtime_error("Invalid vertex attribute source");
                value = *source;
            }
            values[i] = ordinal == 150 ? value : float_to_fixed16(value);
        }
        values[3] = *stale;
        uniform(0x231 + a[0], values);
        return 0;
    }
    const bool fixed = ordinal == 151;
    const bool array = fixed || !(ordinal & 1);
    const unsigned count = fixed ? 4 : (ordinal - 129) / 2 + 1;
    std::array<u32, 4> values{};
    for (unsigned i = 0; i < count; ++i) {
        u32 value = a[i + 1];
        if (array) {
            const auto source = frame.memory.read<u32>(a[1] + 4 * i);
            if (!source) throw std::runtime_error("Invalid vertex attribute source");
            value = *source;
        }
        values[i] = fixed ? value : float_to_fixed16(value);
    }
    uniform(0x231 + a[0], std::span(values).first(count));
    return 0;
}

u32 GraphicsApi::uniforms(u32 ordinal, HleCallFrame &frame, const std::array<u32, 16> &a) {
    if (a[0] == 0xffffffff)
        return 0;
    const bool matrix = ordinal >= 123;
    const unsigned kind = matrix ? 1 : (ordinal - 107) % 4;
    const bool array = matrix || (kind & 1);
    const bool floating = matrix || kind < 2;
    const unsigned width = matrix ? ordinal - 121 : (ordinal - 107) / 4 + 1;
    const u32 count = array ? a[1] : 1;
    if (static_cast<i32>(count) < 0) {
        error_ = 0x501;
        throw std::runtime_error("Invalid uniform array count");
    }
    if (count > 65536)
        throw std::runtime_error("Uniform upload too large");
    const u32 first_slot = a[0] < 4 ? a[0] + 1 : a[0] - 4 + 0x101;
    const u32 pointer = a[matrix ? 3 : 2];
    const auto read = [&](u32 index) {
        const auto value = frame.memory.read<u32>(pointer + index * 4);
        if (!value) throw std::runtime_error("Invalid uniform source");
        return *value;
    };
    for (u32 element = 0; element < count; ++element) {
        const unsigned rows = matrix ? width : 1;
        for (unsigned row = 0; row < rows; ++row) {
            std::array<u32, 4> values{};
            for (unsigned column = 0; column < width; ++column) {
                const u32 index = matrix
                    ? element * width * width + (a[2] ? column * width + row : row * width + column)
                    : element * width + column;
                const u32 value = array ? read(index) : a[column + 1];
                values[column] = floating ? float_to_fixed16(value) : value << 16;
            }
            uniform(first_slot + element * rows + row, std::span(values).first(width));
        }
    }
    return 0;
}
} // namespace clickhle

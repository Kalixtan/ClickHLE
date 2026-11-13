#include "graphics_api.h"
#include "graphics_numeric.h"
#include "clickhle/hle/call_frame.h"
#include "clickhle/core/emulator.h"
#include <string_view>
#include <stdexcept>

namespace clickhle {
namespace {
u32 clamped_fixed16(u32 bits) {
    if (bits >= 0x80800000) return 0;
    if (static_cast<i32>(bits) > 0x3f800000) bits = 0x3f800000;
    return float_to_fixed16(bits);
}
bool stencil_face(u32 value) { return value == 0x404 || value == 0x405 || value == 0x408; }
bool stencil_operation(u32 value) {
    return value == 0 || value == 0x150a || (value >= 0x1e00 && value <= 0x1e03) ||
           value == 0x8507 || value == 0x8508;
}
} // namespace

u32 GraphicsApi::string_query(HleCallFrame &frame, u32 name) {
    static constexpr std::array<std::string_view, 5> strings{
        "Apple Computer, Inc.", "Apple VCO2 OpenGL Engine", "OpenGL ES 2.0",
        "GL_APPLE_fence GL_APPLE_ES_fixed_point GL_APPLE_ES_quads GL_ARB_texture_rectangle "
        "GL_OES_compressed_paletted_texture GL_OES_mapbuffer ", "OpenGL ES GLSL 1.10"};
    const u32 index = name == 0x8b8c ? 4 : name - 0x1f00;
    if (index >= strings.size()) {
        error_ = 0x500;
        throw std::runtime_error("Invalid graphics string query");
    }
    auto &address = string_addresses_[index];
    if (!address) {
        const auto text = strings[index];
        address = frame.emulator.heap().allocate(static_cast<u32>(text.size() + 1));
        if (!address || !frame.memory.write_bytes(address, text.data(), static_cast<u32>(text.size() + 1)))
            throw std::runtime_error("Cannot allocate graphics query string");
    }
    return address;
}

u32 GraphicsApi::state_query(u32 ordinal, HleCallFrame &frame, const std::array<u32, 16> &a) {
    std::array<u32, 10> values{};
    unsigned count = 1;
    switch (a[0]) {
    case 0xb44: case 0xb71: case 0xb90: case 0xbd0: case 0xbe2:
    case 0xc11: case 0x8037: case 0x809e: case 0x80a0:
        values[0] = enabled_[a[0]]; break;
    // RetailOS returns unit indices here, not GL_TEXTURE0 + index.
    case 0x84e0: values[0] = active_texture_; break;
    case 0x84e1: values[0] = client_active_texture_; break;
    case 0x84e2: values[0] = 2; break;
    case 0x86a2: values[0] = 10; break;
    case 0x86a3:
        count = 10;
        for (unsigned i = 0; i < count; ++i) values[i] = 0x8b90 + i;
        break;
    default:
        throw std::runtime_error("RetailOS graphics state query enters its fatal diagnostic handler");
    }
    for (unsigned i = 0; i < count; ++i) {
        const u32 value = values[i];
        bool stored;
        if (ordinal == 50) stored = frame.store<u8>(a[1] + i, value != 0);
        else {
            const u32 result = ordinal == 54 ? value << 16
                : ordinal == 55 ? endian::float_to_bits(static_cast<float>(value)) : value;
            stored = frame.store<u32>(a[1] + i * 4, result);
        }
        if (!stored) throw std::runtime_error("Invalid graphics state query destination");
    }
    return 0;
}

u32 GraphicsApi::raster(u32 ordinal, const std::array<u32, 16> &a) {
    const auto invalid = [&](u32 code) {
        error_ = code;
        throw std::runtime_error("Invalid raster state for graphics API #" + std::to_string(ordinal));
    };
    u32 slot = 0;
    unsigned count = 0;
    std::array<u32, 4> values{a[0], a[1], a[2], a[3]};
    switch (ordinal) {
    case 5:
        slot = 0x2a9; count = 4;
        for (auto &value : values) value = clamped_fixed16(value);
        break;
    case 6: slot = 0x2a7; count = 1; break;
    case 7: slot = 0x2a8; count = 2; break;
    case 8: slot = 0x2a5; count = 2; break;
    case 9: slot = 0x2a6; count = 4; break;
    case 16:
        if (a[0] < 0x84c0 || a[0] > 0x84c2) invalid(0x500);
        client_active_texture_ = a[0] - 0x84c0;
        return client_active_texture_;
    case 31: slot = 0x2a1; count = 1; break;
    case 32:
        if (depth_write_mask_ != a[0]) {
            depth_write_mask_ = static_cast<u8>(a[0]);
            queue({0, (0x2a2u << 16) | ((a[0] & 1) << 2), 0, 0});
        }
        return 0;
    case 33:
        slot = 0x2a3; count = 2;
        values[0] = clamped_fixed16(a[0]); values[1] = clamped_fixed16(a[1]);
        break;
    case 43: slot = 0x28d; count = 1; break;
    case 76:
        switch (a[0]) {
        case 0xb44: case 0xb71: case 0xb90: case 0xbd0: case 0xc11:
        case 0xbe2: case 0x8037: case 0x809e: case 0x80a0:
            return enabled_[a[0]];
        default: invalid(0x500);
        }
        return 0;
    case 85:
        slot = 0x28e; count = 1; values[0] = float_to_fixed16(a[0]); break;
    case 86:
        slot = 0x293; count = 2;
        values[0] = float_to_fixed16(a[0]); values[1] = float_to_fixed16(a[1]);
        break;
    case 89:
        slot = 0x297; count = 2; values[0] = clamped_fixed16(a[0]); break;
    case 90: case 138:
        if (static_cast<i32>(a[2]) < 0 || static_cast<i32>(a[3]) < 0) invalid(0x501);
        slot = ordinal == 90 ? 0x295 : 0x28c; count = 4;
        break;
    case 93:
        if (a[0] < 0x200 || a[0] > 0x207) invalid(0x500);
        slot = 0x29a; count = 3; break;
    case 94:
        if (!stencil_face(a[0]) || a[1] < 0x200 || a[1] > 0x207) invalid(0x500);
        slot = 0x29b; count = 4; break;
    case 95: slot = 0x29c; count = 1; break;
    case 96:
        if (!stencil_face(a[0])) invalid(0x500);
        slot = 0x29d; count = 2; break;
    case 97: case 98: {
        const unsigned first = ordinal == 98 ? 1 : 0;
        if (first && !stencil_face(a[0])) invalid(0x500);
        for (unsigned i = first; i < first + 3; ++i)
            if (!stencil_operation(a[i])) invalid(0x500);
        slot = first ? 0x29f : 0x29e; count = first + 3;
        break;
    }
    default: throw std::logic_error("Unknown raster API dispatch");
    }
    uniform(slot, std::span(values).first(count));
    return 0;
}
} // namespace clickhle

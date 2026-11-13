#include "graphics_api.h"
#include "graphics_numeric.h"
#include "clickhle/common/endian.h"
#include "clickhle/hle/call_frame.h"
#include "clickhle/core/emulator.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace clickhle {
namespace {
[[noreturn]] void retail_graphics_trap(u32 ordinal) {
    throw std::runtime_error("RetailOS graphics API #" + std::to_string(ordinal) +
                             " enters its fatal diagnostic handler");
}
constexpr std::size_t kPacketCapacity = 0x2000;
std::vector<u8> words(std::span<const u32> values) {
    std::vector<u8> bytes(values.size() * 4);
    for (std::size_t i = 0; i < values.size(); ++i)
        endian::store_le<u32>(reinterpret_cast<std::byte *>(bytes.data() + 4 * i), values[i]);
    return bytes;
}
std::vector<u8> guest_bytes(HleCallFrame &frame, u32 address, u32 size) {
    if (!size) return {};
    const auto *data = frame.memory.host_ptr(address, size);
    if (!data) throw std::runtime_error("Graphics API reads unmapped guest memory");
    const auto *bytes = reinterpret_cast<const u8 *>(data);
    return {bytes, bytes + size};
}
u32 read_word(std::span<const u8> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("Truncated VC2 program record");
    return endian::load_le<u32>(reinterpret_cast<const std::byte *>(bytes.data() + offset));
}
u32 texture_pixel_size(u32 format, u32 type) {
    switch (format) {
    case 0x1906: case 0x1909: return 1;
    case 0x190a: return 2;
    case 0x1907:
        if (type == 0x1401) return 3;
        if (type == 0x8363) return 2;
        break;
    case 0x1908:
        if (type == 0x1401) return 4;
        if (type == 0x8033 || type == 0x8034) return 2;
        break;
    }
    throw std::runtime_error("Invalid texture pixel type");
}
u32 location_slot(u32 location) { return location < 4 ? location + 1 : location - 4 + 0x101; }
float color_float(u32 bits) {
    // The ARM implementation tests the IEEE representation before converting.
    if (bits >= 0x80800000) return 0;
    if (static_cast<i32>(bits) > 0x3f800000) return 1;
    return endian::bits_to_float(bits);
}
} // namespace

GraphicsApi::GraphicsApi(Submit submit) : submit_(std::move(submit)) {}
GraphicsApi::GraphicsApi(Submit submit, Transport transport)
    : submit_(std::move(submit)), transport_(std::move(transport)) {}
void GraphicsApi::reset(HleCallFrame &frame) {
    // API 152 clears context state and the uploaded-program cache, but leaves
    // the separately owned custom program descriptors intact.
    auto custom = std::move(custom_programs_);
    const auto strings = string_addresses_;
    for (const auto &buffer : buffers_)
        if (buffer.address) frame.emulator.heap().release(buffer.address);
    *this = GraphicsApi(submit_, transport_);
    custom_programs_ = std::move(custom);
    string_addresses_ = strings;
}
void GraphicsApi::queue(std::array<u32, 4> header, std::span<const u8> payload) {
    const std::size_t size = (16 + payload.size() + 3) & ~std::size_t(3);
    if (size > kPacketCapacity) throw std::runtime_error("Graphics packet exceeds staging capacity");
    // A caller may pass a view returned by pending_packets(). Preserve that
    // input before a flush or header write can overwrite the staging buffer.
    const std::less<const u8 *> less;
    if (!payload.empty() && less(payload.data(), staging_.data() + staging_.size()) &&
        less(staging_.data(), payload.data() + payload.size())) {
        const std::vector<u8> copy(payload.begin(), payload.end());
        queue(header, copy);
        return;
    }
    if (pending_size_ + size > kPacketCapacity) flush();
    const auto offset = pending_size_;
    for (unsigned i = 0; i < header.size(); ++i)
        endian::store_le<u32>(reinterpret_cast<std::byte *>(staging_.data() + offset + 4 * i), header[i]);
    std::copy(payload.begin(), payload.end(), staging_.begin() + offset + 16);
    // RetailOS rounds the cursor up without clearing padding. Preserve the
    // previous staging contents, including across submissions.
    pending_size_ += size;
}
void GraphicsApi::flush() {
    if (!pending_size_) return;
    submit_(pending_packets());
    pending_size_ = 0;
}
void GraphicsApi::finish() {
    if (!transport_.wait) throw std::runtime_error("Graphics completion requires a VC2 transport");
    if (completion_ == 0xffffffff) transport_.wait(completion_);
    const u32 marker = ++completion_;
    queue({9, marker, 0, 0});
    flush();
    transport_.wait(marker);
}
void GraphicsApi::uniform(u32 slot, std::span<const u32> values) {
    const auto bytes = words(values);
    queue({0, ((slot & 0xffff) << 16) | 1, static_cast<u32>(bytes.size()), 0}, bytes);
}
void GraphicsApi::fragments(u32 type, u32 object, std::span<const u8> prefix,
                            std::span<const u8> data, u32 flags) {
    const std::size_t total = prefix.size() + data.size();
    if (total > 0x1fffffff) throw std::runtime_error("Graphics upload exceeds packet length field");
    std::size_t offset = 0;
    u32 index = 0;
    while (offset < total) {
        // RetailOS reserves the entire prefix before starting it. Data
        // fragments require at least four payload bytes of staging space.
        // Splitting a prefix into leftover room changes fragment boundaries.
        const auto required = offset < prefix.size() ? prefix.size() + 16 : 20;
        if (pending_size_ + required > kPacketCapacity) flush();
        const auto segment_left = offset < prefix.size() ? prefix.size() - offset : total - offset;
        const auto size = std::min(segment_left, kPacketCapacity - pending_size_ - 16);
        std::vector<u8> part;
        part.reserve(size);
        for (std::size_t i = offset; i < offset + size; ++i)
            part.push_back(i < prefix.size() ? prefix[i] : data[i - prefix.size()]);
        queue({7, (static_cast<u32>(total) << 3) | type,
               flags | ((index & 0x7fff) << 16) | static_cast<u32>(size), object}, part);
        offset += size;
        ++index;
    }
}
void GraphicsApi::capability(u32 key, bool enabled) {
    u32 slot = 0;
    switch (key) {
    case 0xb44: slot = 0x292; break;
    case 0xb71: slot = 0x2a4; break;
    case 0xb90: slot = 0x2a0; break;
    case 0xbd0: slot = 0x2ab; break;
    case 0xc11: slot = 0x296; break;
    case 0xbe2: slot = 0x2aa; break;
    case 0x8037: slot = 0x294; break;
    case 0x809e: slot = 0x299; break;
    case 0x80a0: slot = 0x298; break;
    default: error_ = 0x500; return;
    }
    if (enabled_[key] == enabled) return;
    enabled_[key] = enabled;
    queue({0, (slot << 16) | (enabled ? 4u : 0u), 0, 0});
}
u32 GraphicsApi::call(u32 ordinal, HleCallFrame &frame) {
    std::array<u32, 16> a{};
    for (unsigned i = 0; i < a.size(); ++i)
        a[i] = i < 4 ? frame.cpu.reg(i) : frame.memory.read<u32>(frame.cpu.sp() + 4 * (i - 4)).value_or(0);
    if ((ordinal >= 129 && ordinal <= 136) || ordinal == 150 || ordinal == 151)
        return attribute_value(ordinal, frame, a);
    if (ordinal >= 107 && ordinal <= 125) return uniforms(ordinal, frame, a);
    if (ordinal == 50 || (ordinal >= 54 && ordinal <= 56)) return state_query(ordinal, frame, a);
    if (ordinal == 3 || ordinal == 10 || ordinal == 11 || ordinal == 44 ||
        ordinal == 51 || ordinal == 52 || ordinal == 75 || ordinal == 83 || ordinal == 126)
        return buffers(ordinal, frame, a);
    if (ordinal >= 165 && ordinal <= 176) return matrix(ordinal, frame, a);
    switch (ordinal) {
    case 41: finish(); return 0;
    case 42: flush(); return 0;
    case 145: case 146: {
        if (a[0] != 0x85b3 || a[1] >= 32) {
            if (a[0] != 0x85b3 && a[0] != 0x8a0b) error_ = 0x502;
            retail_graphics_trap(ordinal);
        }
        // These exports index vertex attributes, not buffer object names.
        // RetailOS dereferences an invalid pointer for an unbound attribute.
        if (a[1] >= attributes_.size() || !attributes_[a[1]].buffer)
            throw std::runtime_error("Graphics completion query has no bound attribute buffer");
        const u32 marker = buffers_[attributes_[a[1]].buffer].completion;
        if (ordinal == 145) {
            if (!transport_.completed) throw std::runtime_error("Graphics completion requires a VC2 transport");
            const u32 completed = transport_.completed();
            return completion_ < marker || completed >= marker;
        }
        if (!transport_.wait) throw std::runtime_error("Graphics completion requires a VC2 transport");
        if (completion_ >= marker) transport_.wait(marker);
        return 0;
    }
    case 87: return read_pixels(frame, a);
    case 157: case 161: case 162: case 163: return overlay(ordinal, frame, a);
    case 5: case 6: case 7: case 8: case 9: case 16: case 31: case 32: case 33:
    case 43: case 76: case 85: case 86: case 89: case 90: case 93: case 94:
    case 95: case 96: case 97: case 98: case 138:
        return raster(ordinal, a);
    // These exports branch directly to RetailOS's fatal handler. They are
    // deliberately unsupported by the firmware, rather than successful no-ops.
    case 1: case 2: case 18: case 24: case 25: case 28: case 29: case 34:
    case 46: case 47: case 49: case 57: case 59: case 61: case 63:
    case 65: case 66: case 67: case 68: case 69: case 77: case 78:
    case 81: case 82: case 88: case 91: case 127: case 128:
    case 139: case 140: case 141: case 142: case 143: case 144:
        retail_graphics_trap(ordinal);
    case 27: case 30:
        if (static_cast<i32>(a[0]) < 0) error_ = 0x501;
        retail_graphics_trap(ordinal);
    case 48: case 58: case 60: case 62: case 92:
        if (static_cast<i32>(a[1]) < 0) error_ = 0x501;
        retail_graphics_trap(ordinal);
    case 20:
        if (static_cast<i32>(a[4]) < 0 || static_cast<i32>(a[5]) < 0 || static_cast<i32>(a[7]) < 0)
            error_ = 0x501;
        retail_graphics_trap(ordinal);
    case 22: case 23: {
        const unsigned width = ordinal == 22 ? 6 : 7;
        if (static_cast<i32>(a[width]) < 0 || static_cast<i32>(a[width + 1]) < 0)
            error_ = 0x501;
        retail_graphics_trap(ordinal);
    }
    case 100:
        error_ = 0x502;
        retail_graphics_trap(ordinal);
    case 106:
        if (static_cast<i32>(a[5]) < 0 || static_cast<i32>(a[6]) < 0 || static_cast<i32>(a[7]) < 0)
            error_ = 0x501;
        retail_graphics_trap(ordinal);
    case 0:
        if (a[0] < 0x84c0 || a[0] > 0x84c2) error_ = 0x500;
        else active_texture_ = a[0] - 0x84c0;
        return 0;
    case 4:
        if (a[0] != 0xde1 && a[0] != 0x84f5 && a[0] != 0x8513) error_ = 0x500;
        else {
            textures_[active_texture_] = a[1];
            // RetailOS accepts unit 2 even though its binding overlaps the
            // first attribute's component count at context offset 0x9c.
            if (active_texture_ == 2) attributes_[0].size = a[1];
            textures_dirty_ |= static_cast<u16>(1u << active_texture_);
        }
        return 1;
    case 12:
        if (a[0] & ~0x4500u) { error_ = 0x501; return 0; }
        queue({3, ((a[0] & 0x4000) ? 1u : 0u) | ((a[0] & 0x100) ? 2u : 0u) |
                      ((a[0] & 0x400) ? 4u : 0u), 0, 0});
        return 0;
    case 13: {
        std::array<u32, 4> color{};
        for (unsigned i = 0; i < 4; ++i)
            color[i] = static_cast<u32>(static_cast<i32>(color_float(a[i]) * 65535.0f));
        uniform(0x289, color);
        return 0;
    }
    case 14: {
        const std::array<u32, 1> depth{static_cast<u32>(static_cast<i32>(color_float(a[0]) * 65536.0f))};
        uniform(0x28a, depth);
        return 0;
    }
    case 15: uniform(0x28b, std::span(a).first(1)); return 0;
    case 17: {
        const std::array<u32, 1> mask{(a[0] & 255) | ((a[1] & 255) << 8) |
                                      ((a[2] & 255) << 16) | ((a[3] & 255) << 24)};
        uniform(0x2ac, mask);
        return 0;
    }
    case 19: {
        if (static_cast<i32>(a[3]) < 0 || static_cast<i32>(a[4]) < 0 || static_cast<i32>(a[6]) < 0) {
            error_ = 0x501; retail_graphics_trap(ordinal);
        }
        if (a[2] < 0x8b90 || a[2] > 0x8b99) { error_ = 0x500; retail_graphics_trap(ordinal); }
        if (static_cast<i32>(a[1]) > 0) { error_ = 0x501; retail_graphics_trap(ordinal); }
        if (a[5]) { error_ = 0x502; retail_graphics_trap(ordinal); }
        if (a[0] != 0xde1 && a[0] != 0x84f5) { error_ = 0x500; retail_graphics_trap(ordinal); }
        const auto scratch = frame.memory.read<u32>(frame.cpu.sp() - 32);
        if (!scratch) throw std::runtime_error("Unmapped RetailOS compressed texture scratch word");
        const std::array<u32, 9> descriptor{a[0], a[1], 0, a[3], a[4], 1, a[2], 0x1401, *scratch};
        // Descriptor layout is target, level, zero, width, height, depth,
        // compressed format, byte type, and an unwritten stack word.
        auto prefix = words(descriptor);
        const auto data = guest_bytes(frame, a[7], a[6]);
        fragments(2, textures_[active_texture_], prefix, data);
        return 0;
    }
    case 21: {
        if (static_cast<i32>(a[5]) < 0 || static_cast<i32>(a[6]) < 0 || a[7]) {
            error_ = 0x501;
            throw std::runtime_error("Invalid framebuffer texture copy dimensions or border");
        }
        if ((a[0] != 0xde1 && a[0] != 0x84f5) || a[2] != 0x1908) {
            error_ = 0x500;
            throw std::runtime_error("Invalid framebuffer texture copy target or format");
        }
        const std::array<u32, 10> descriptor{
            a[0], a[1], 0, a[3], a[4], a[5], a[6], a[2], 0, textures_[active_texture_]};
        queue({1, 0, 0, 0}, words(descriptor));
        return 0;
    }
    case 37: case 38: return draw(ordinal, frame, a);
    case 35: capability(a[0], false); return 0;
    case 39: capability(a[0], true); return 0;
    case 36:
    case 40: {
        if (a[0] >= 16) { error_ = 0x501; retail_graphics_trap(ordinal); }
        const auto mask = static_cast<u16>(1u << a[0]);
        if (ordinal == 40) attributes_enabled_ |= mask;
        else attributes_enabled_ &= ~mask;
        attributes_dirty_ |= mask;
        return 0;
    }
    case 105: {
        if (static_cast<i32>(a[4]) < 0 || static_cast<i32>(a[5]) < 0) {
            error_ = 0x501;
            throw std::runtime_error("Invalid texture update dimensions");
        }
        if ((a[0] != 0xde1 && a[0] != 0x84f5) || a[6] < 0x1906 || a[6] > 0x190a) {
            error_ = 0x500;
            throw std::runtime_error("Invalid texture update target or format");
        }
        const u32 bytes_per_pixel = texture_pixel_size(a[6], a[7]);
        const uint64_t size = uint64_t(a[4]) * a[5] * bytes_per_pixel;
        if (size > 0x1fffffff - 40) throw std::runtime_error("Texture update too large");
        // RetailOS uses the texture-upload command with the subimage bit set
        // on every fragment. Offsets replace the full image's dimensions.
        const std::array<u32, 10> descriptor{a[0], a[1], 0, a[2], a[3], a[4], a[5], a[6], a[7], 0};
        fragments(2, textures_[active_texture_], words(descriptor),
                  guest_bytes(frame, a[8], static_cast<u32>(size)), 0x80000000);
        return 0;
    }
    case 99: {
        if (a[5] || static_cast<i32>(a[3]) < 0 || static_cast<i32>(a[4]) < 0) {
            error_ = 0x501; retail_graphics_trap(ordinal);
        }
        if (a[2] != a[6]) { error_ = 0x502; retail_graphics_trap(ordinal); }
        if ((a[0] != 0xde1 && a[0] != 0x84f5) || a[6] < 0x1906 || a[6] > 0x190a) {
            error_ = 0x500; retail_graphics_trap(ordinal);
        }
        // Allocation-only uploads skip RetailOS's pixel-type helper.
        const u32 bytes_per_pixel = a[8] ? texture_pixel_size(a[6], a[7]) : 0;
        const uint64_t size = uint64_t(a[3]) * a[4] * bytes_per_pixel;
        if (size > 0x1fffffff - 36) throw std::runtime_error("Texture upload too large");
        // The ARM routine does not initialize this stack word. As with the
        // overlay wrapper, prior firmware stack history remains unspecified.
        const auto scratch = frame.memory.read<u32>(frame.cpu.sp() - 56);
        if (!scratch) throw std::runtime_error("Unmapped RetailOS texture scratch word");
        const std::array<u32, 9> descriptor{a[0], a[1], 0, a[3], a[4], 1, a[6], a[7], *scratch};
        fragments(2, textures_[active_texture_], words(descriptor),
                  guest_bytes(frame, a[8], static_cast<u32>(size)));
        return 0;
    }
    case 84:
        if (a[1] != 1 && a[1] != 2 && a[1] != 4 && a[1] != 8) {
            error_ = 0x501; retail_graphics_trap(ordinal);
        }
        if (a[0] == 0xcf5) unpack_alignment_ = a[1];
        else if (a[0] == 0xd05) pack_alignment_ = a[1];
        else { error_ = 0x500; retail_graphics_trap(ordinal); }
        return 0;
    case 101: case 102: case 103: case 104: {
        u32 value = a[2];
        if (ordinal == 102 || ordinal == 104) {
            const auto source = frame.memory.read<u32>(a[2]);
            if (!source) throw std::runtime_error("Invalid texture parameter source");
            value = *source;
        }
        const u32 integer = ordinal <= 102 ? float_to_integer(value) : value;
        bool valid = a[0] == 0xde1 || a[0] == 0x84f5 || a[0] == 0x8513;
        switch (a[1]) {
        case 0x2800: case 0x2801:
            valid &= integer == 0x2600 || integer == 0x2601 || (integer >= 0x2700 && integer <= 0x2703);
            break;
        case 0x2802: case 0x2803:
            valid &= integer == 0x2901 || integer == 0x812f || integer == 0x8370;
            break;
        case 0x8066: break;
        default: valid = false; break;
        }
        if (!valid) {
            error_ = 0x500;
            retail_graphics_trap(ordinal);
        }
        // Only the scalar-float wrapper returns normally after validation.
        // Filters/wrap modes are validated but emit no command in RetailOS.
        if (ordinal != 101) retail_graphics_trap(ordinal);
        if (a[1] != 0x8066) return 1;
        const std::array<u32, 1> priority{float_to_fixed16(endian::float_to_bits(color_float(value)))};
        uniform(static_cast<u16>(textures_[active_texture_]) + 0x249, priority);
        return 0;
    }
    case 71: case 72: case 73: {
        if (ordinal == 73 && a[1] != 0x8645) {
            error_ = 0x500;
            throw std::runtime_error("Invalid vertex pointer query");
        }
        if (a[0] >= attributes_.size()) {
            error_ = 0x501;
            throw std::runtime_error("Invalid vertex attribute index");
        }
        const auto &attribute = attributes_[a[0]];
        u32 value = 0;
        if (ordinal == 73) value = attribute.address;
        else switch (a[1]) {
        case 0x8622: value = (attributes_enabled_ >> a[0]) & 1; break;
        case 0x8623: value = attribute.size; break;
        case 0x8624: value = attribute.stride; break;
        case 0x8625: value = attribute.type; break;
        case 0x886a: value = attribute.normalized; break;
        default:
            error_ = 0x500;
            throw std::runtime_error("Invalid vertex attribute query");
        }
        if (ordinal == 71) value = endian::float_to_bits(static_cast<float>(value));
        if (!frame.store<u32>(a[2], value))
            throw std::runtime_error("Invalid vertex query destination");
        return value;
    }
    case 74:
        if ((a[0] != 0x8192 && a[0] != 0x8b8b) || a[1] < 0x1100 || a[1] > 0x1102) {
            error_ = 0x500;
            throw std::runtime_error("Invalid graphics hint");
        }
        return a[0];
    case 79: return a[0] < next_texture_;
    case 26: {
        const std::array<u32, 1> value{a[0]};
        uniform(0x291, value);
        return 0;
    }
    case 80: {
        const std::array<u32, 1> value{float_to_fixed16(a[0])};
        uniform(0x28f, value);
        return 0;
    }
    case 137: {
        if (a[0] >= attributes_.size() || a[1] < 1 || a[1] > 4 ||
            static_cast<i32>(a[4]) < 0) {
            error_ = 0x501;
            retail_graphics_trap(ordinal);
        }
        if (a[2] != 0x140c || a[3] != 0) {
            if (!((a[2] >= 0x1400 && a[2] <= 0x1406) || a[2] == 0x140c || a[2] == 0x20000))
                error_ = 0x500;
            retail_graphics_trap(ordinal);
        }
        attributes_[a[0]] = {a[1], a[2], a[4] ? a[4] : 4 * a[1], a[5], false, array_buffer_};
        if (!a[0]) textures_[2] = a[1];
        const std::array<u32, 3> value{a[1], 0, 0};
        uniform(0x211 + a[0], value);
        return 0;
    }
    case 70: {
        const auto name = frame.memory.read_cstring(a[1]);
        if (!name) throw std::runtime_error("Invalid uniform name");
        if (*name == "mvp") return 0;
        if (*name == "fadeout") return 4;
        return 0xffffffff;
    }
    case 64: return string_query(frame, a[0]);
    case 177: {
        const auto text = frame.memory.read_cstring(a[0], 0x100000);
        if (!text) throw std::runtime_error("Invalid VC2 text string");
        const auto size = static_cast<u16>(text->size());
        queue({10, (a[1] & 65535) | (a[2] << 16), a[3], size},
              std::span(reinterpret_cast<const u8 *>(text->data()), size));
        return 0;
    }
    case 178: {
        u32 value = 0;
        // The original routine converts and stores each word in order, even
        // when source and destination overlap.
        for (unsigned i = 0; i < 16; ++i) {
            const auto source = frame.memory.read<u32>(a[1] + 4 * i);
            if (!source) throw std::runtime_error("Invalid matrix conversion source");
            value = float_to_fixed16(*source);
            if (!frame.store<u32>(a[0] + 4 * i, value))
                throw std::runtime_error("Invalid matrix conversion destination");
        }
        return value;
    }
    case 159: return select_program(a[0]);
    case 160: return load_program(frame, a);
    case 45:
        if (static_cast<i32>(a[0]) < 0) { error_ = 0x501; return 0; }
        for (u32 i = 0; i < a[0]; ++i)
            if (!frame.store<u32>(a[1] + 4 * i, next_texture_++))
                throw std::runtime_error("Invalid texture-name output");
        return a[0];
    case 53: { const u32 error = error_; error_ = 0; return error; }
    case 147:
        if (a[0] != 0xffffffff) uniform(location_slot(a[0]), std::span(a).subspan(1, 4));
        return 0;
    case 148:
    case 149: {
        if (a[0] == 0xffffffff) return 0;
        if (static_cast<i32>(a[1]) < 0 || a[1] > 65536) { error_ = 0x501; return 0; }
        const bool matrices = ordinal == 149;
        auto data = guest_bytes(frame, a[matrices ? 3 : 2], a[1] * (matrices ? 64 : 16));
        for (u32 i = 0; i < a[1] * (matrices ? 4 : 1); ++i) {
            std::array<u32, 4> row{};
            for (u32 j = 0; j < 4; ++j) {
                const u32 index = matrices && a[2] ? (i / 4) * 16 + j * 4 + i % 4 : i * 4 + j;
                row[j] = read_word(data, index * 4);
            }
            uniform(location_slot(a[0]) + i, row);
        }
        return 0;
    }
    case 154: queue({11, 0, static_cast<u32>(std::clamp(static_cast<i32>(a[1]), 0, 6)), 0}); return 1;
    case 155: return 0;
    case 156: return 1;
    case 158:
        if (a[0] < 0x3f000 || a[0] > 0x3f005) return 0x300c;
        queue({11, a[0] - 0x3f000 + 1, static_cast<i32>(a[1]) > 0 ? 1u : 0u, 0});
        return 0x3000;
    default:
        throw std::runtime_error("Native graphics API #" + std::to_string(ordinal) + " has no verified native implementation (r0=" + std::to_string(a[0]) + ", r1=" + std::to_string(a[1]) + ", r2=" + std::to_string(a[2]) + ", r3=" + std::to_string(a[3]) + ", a4=" + std::to_string(a[4]) + ", a5=" + std::to_string(a[5]) + ")");
    }
}
} // namespace clickhle

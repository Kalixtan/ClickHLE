#include "graphics_api.h"
#include "clickhle/common/endian.h"
#include "clickhle/hle/call_frame.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace clickhle {
namespace {
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
u32 location_slot(u32 location) { return location < 4 ? location + 1 : location - 4 + 0x101; }
float color_float(u32 bits) {
    // The ARM implementation tests the IEEE representation before converting.
    if (bits >= 0x80800000) return 0;
    if (static_cast<i32>(bits) > 0x3f800000) return 1;
    return endian::bits_to_float(bits);
}
} // namespace

GraphicsApi::GraphicsApi(Submit submit) : submit_(std::move(submit)) {}
void GraphicsApi::queue(std::array<u32, 4> header, std::span<const u8> payload) {
    const std::size_t size = (16 + payload.size() + 3) & ~std::size_t(3);
    if (size > kPacketCapacity) throw std::runtime_error("Graphics packet exceeds staging capacity");
    if (pending_.size() + size > kPacketCapacity) flush();
    const auto encoded = words(header);
    const auto offset = pending_.size();
    std::copy(encoded.begin(), encoded.end(), staging_.begin() + offset);
    std::copy(payload.begin(), payload.end(), staging_.begin() + offset + 16);
    // RetailOS rounds the cursor up without clearing padding. Preserve the
    // previous staging contents, including across submissions.
    pending_.insert(pending_.end(), staging_.begin() + offset, staging_.begin() + offset + size);
}
void GraphicsApi::flush() {
    if (pending_.empty()) return;
    submit_(pending_);
    pending_.clear();
}
void GraphicsApi::uniform(u32 slot, std::span<const u32> values) {
    const auto bytes = words(values);
    queue({0, ((slot & 0xffff) << 16) | 1, static_cast<u32>(bytes.size()), 0}, bytes);
}
void GraphicsApi::fragments(u32 type, u32 object, std::span<const u8> prefix,
                            std::span<const u8> data) {
    const std::size_t total = prefix.size() + data.size();
    if (total > 0x1fffffff) throw std::runtime_error("Graphics upload exceeds packet length field");
    std::size_t offset = 0;
    u32 index = 0;
    while (offset < total) {
        if (pending_.size() + 16 >= kPacketCapacity) flush();
        const auto segment_left = offset < prefix.size() ? prefix.size() - offset : total - offset;
        const auto size = std::min(segment_left, kPacketCapacity - pending_.size() - 16);
        std::vector<u8> part;
        part.reserve(size);
        for (std::size_t i = offset; i < offset + size; ++i)
            part.push_back(i < prefix.size() ? prefix[i] : data[i - prefix.size()]);
        queue({7, (static_cast<u32>(total) << 3) | type,
               ((index & 0x7fff) << 16) | static_cast<u32>(size), object}, part);
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
    if (ordinal >= 165 && ordinal <= 176) return matrix(ordinal, frame, a);
    switch (ordinal) {
    case 0:
        if (a[0] < 0x84c0 || a[0] > 0x84c2) error_ = 0x500;
        else active_texture_ = a[0] - 0x84c0;
        return 0;
    case 4:
        if (a[0] != 0xde1 && a[0] != 0x84f5 && a[0] != 0x8513) error_ = 0x500;
        else { textures_[active_texture_] = a[1]; textures_dirty_ |= static_cast<u16>(1u << active_texture_); }
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
        if ((a[0] != 0xde1 && a[0] != 0x84f5) || a[1] || a[2] < 0x8b90 || a[2] > 0x8b99 ||
            static_cast<i32>(a[3]) < 0 || static_cast<i32>(a[4]) < 0 || a[5] || static_cast<i32>(a[6]) < 0) {
            error_ = 0x501; return 0;
        }
        const std::array<u32, 9> descriptor{a[0], a[1], 0, a[3], a[4], 1, a[2], 0x1401, 0};
        // Descriptor layout is target, level, zero, width, height, depth,
        // compressed format, byte type, and a reserved zero word.
        auto prefix = words(descriptor);
        const auto data = guest_bytes(frame, a[7], a[6]);
        fragments(2, textures_[active_texture_], prefix, data);
        return 0;
    }
    case 37: case 38: return draw(ordinal, frame, a);
    case 35: capability(a[0], false); return 0;
    case 39: capability(a[0], true); return 0;
    case 36:
    case 40: {
        if (a[0] >= 16) throw std::runtime_error("Invalid graphics attribute index");
        const auto mask = static_cast<u16>(1u << a[0]);
        if (ordinal == 40) attributes_enabled_ |= mask;
        else attributes_enabled_ &= ~mask;
        attributes_dirty_ |= mask;
        return 0;
    }
    case 99: {
        if ((a[0] != 0xde1 && a[0] != 0x84f5) || a[2] != a[6] ||
            a[6] < 0x1906 || a[6] > 0x190a || a[5] ||
            static_cast<i32>(a[3]) < 0 || static_cast<i32>(a[4]) < 0)
            throw std::runtime_error("Invalid texture upload arguments");
        u32 bytes_per_pixel = 0;
        switch (a[6]) {
        case 0x1906: case 0x1909: bytes_per_pixel = 1; break;
        case 0x190a: bytes_per_pixel = 2; break;
        case 0x1907:
            if (a[7] == 0x1401) bytes_per_pixel = 3;
            else if (a[7] == 0x8363) bytes_per_pixel = 2;
            break;
        case 0x1908:
            if (a[7] == 0x1401) bytes_per_pixel = 4;
            else if (a[7] == 0x8033 || a[7] == 0x8034) bytes_per_pixel = 2;
            break;
        }
        if (!bytes_per_pixel) throw std::runtime_error("Invalid texture pixel type");
        const uint64_t size = a[8] ? uint64_t(a[3]) * a[4] * bytes_per_pixel : 0;
        if (size > 0x1fffffff - 36) throw std::runtime_error("Texture upload too large");
        const std::array<u32, 9> descriptor{a[0], a[1], 0, a[3], a[4], 1, a[6], a[7], 0};
        fragments(2, textures_[active_texture_], words(descriptor),
                  guest_bytes(frame, a[8], static_cast<u32>(size)));
        return 0;
    }
    case 84:
        if (a[1] != 1 && a[1] != 2 && a[1] != 4 && a[1] != 8)
            throw std::runtime_error("Unverified pixel storage alignment");
        if (a[0] == 0xcf5) unpack_alignment_ = a[1];
        else if (a[0] == 0xd05) pack_alignment_ = a[1];
        else throw std::runtime_error("Unverified pixel storage parameter");
        return 0;
    case 101: {
        if (a[1] != 0x8066 || (a[0] != 0xde1 && a[0] != 0x84f5 && a[0] != 0x8513))
            throw std::runtime_error("Unverified texture parameter: target=" + std::to_string(a[0]) + " name=" + std::to_string(a[1]) + " value=" + std::to_string(a[2]));
        const std::array<u32, 1> value{static_cast<u32>(static_cast<i32>(color_float(a[2]) * 65536.0f))};
        uniform(static_cast<u16>(textures_[active_texture_]) + 0x249, value);
        return 0;
    }
    case 137: {
        if (a[0] >= attributes_.size() || a[1] < 1 || a[1] > 4 ||
            a[2] != 0x140c || a[3] != 0 || static_cast<i32>(a[4]) < 0)
            throw std::runtime_error("Unverified vertex attribute format");
        attributes_[a[0]] = {a[1], a[2], a[4], a[5], false};
        const std::array<u32, 3> value{a[1], 0, 0};
        uniform(0x211 + a[0], value);
        return 0;
    }
    case 125: {
        if (a[0] == 0xffffffff) return 0;
        if (a[0] >= 4 || static_cast<i32>(a[1]) < 0 || a[1] > 1024)
            throw std::runtime_error("Unverified matrix upload range");
        const auto data = guest_bytes(frame, a[3], a[1] * 64);
        for (u32 matrix_index = 0; matrix_index < a[1]; ++matrix_index) {
            for (u32 row = 0; row < 4; ++row) {
                std::array<u32, 4> value{};
                for (u32 column = 0; column < 4; ++column) {
                    const u32 index = matrix_index * 16 + (a[2] ? column * 4 + row : row * 4 + column);
                    value[column] = static_cast<u32>(static_cast<i32>(
                        endian::bits_to_float(read_word(data, index * 4)) * 65536.0f));
                }
                uniform(location_slot(a[0]) + matrix_index * 4 + row, value);
            }
        }
        return 0;
    }
    case 159: return select_program(a[0]);
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

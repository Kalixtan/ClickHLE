#include "graphics_api.h"
#include "clickhle/core/emulator.h"
#include "clickhle/hle/call_frame.h"
#include <stdexcept>

namespace clickhle {
namespace {
std::vector<u8> encode_words(std::span<const u32> values) {
    std::vector<u8> bytes(values.size() * 4);
    for (size_t i = 0; i < values.size(); ++i)
        endian::store_le<u32>(reinterpret_cast<std::byte *>(bytes.data() + i * 4), values[i]);
    return bytes;
}
} // namespace

u32 GraphicsApi::overlay(u32 ordinal, HleCallFrame &frame, const std::array<u32, 16> &a) {
    switch (ordinal) {
    case 161: {
        if (static_cast<i32>(a[0]) > 64 || static_cast<i32>(a[1]) > 64) return 0x300c;
        if (static_cast<i32>(a[0]) < 0 || static_cast<i32>(a[1]) < 0) {
            error_ = 0x501;
            throw std::runtime_error("Invalid overlay texture dimensions");
        }
        const u32 size = a[2] ? a[0] * a[1] * 2 : 0;
        const auto *source = size ? frame.memory.host_ptr(a[2], size) : nullptr;
        if (size && !source) throw std::runtime_error("Unmapped overlay texture");
        // TexImage2D leaves the descriptor's last word unwritten. Its nested
        // stack slot is 104 bytes below this wrapper's incoming SP. HLE does
        // not reproduce earlier firmware stack history; preserve the guest
        // word here instead of substituting a firmware address or constant.
        const auto scratch = frame.memory.read<u32>(frame.cpu.sp() - 104);
        if (!scratch) throw std::runtime_error("Unmapped RetailOS overlay scratch word");
        const std::array<u32, 9> descriptor{0x84f5, 0, 0, a[0], a[1], 1, 0x1908, 0x8033, *scratch};
        fragments(2, 64, encode_words(descriptor), std::span(reinterpret_cast<const u8 *>(source), size));
        // The wrapper binds then restores unit zero, leaving it dirty even
        // though its final binding and active unit have not changed.
        textures_dirty_ |= 1;
        overlay_width_ = a[0];
        overlay_height_ = a[1];
        const u32 error = error_;
        error_ = 0;
        return error;
    }
    case 162: {
        overlay_enabled_ = true;
        const u32 x = a[0] << 16, y = a[1] << 16;
        const u32 right = (a[0] + overlay_width_) << 16, bottom = (a[1] + overlay_height_) << 16;
        overlay_vertices_ = {x, y, 0, 0x10000, right, y, 0, 0x10000,
                             right, bottom, 0, 0x10000, x, bottom, 0, 0x10000};
        const u32 width = overlay_width_ << 16, height = overlay_height_ << 16;
        overlay_coordinates_ = {0, 0, width, 0, width, height, 0, height};
        return 0;
    }
    case 163:
        overlay_enabled_ = false;
        return 0;
    }
    if (overlay_enabled_) {
        const auto old_attributes = attributes_;
        const auto old_enabled = attributes_enabled_;
        const auto old_texture = textures_[0], old_unit = active_texture_, old_program = selected_program_;
        active_texture_ = 0;
        textures_[0] = 64;
        textures_dirty_ |= 1;
        select_program(49);
        // API 162 constructs the fixed 320x240 orthographic projection using
        // the ARM float helper; these are its values after API 125 conversion.
        constexpr std::array<u32, 16> projection{
            409, 0, 0, 0, 0, 546, 0, 0, 0, 0, 0xffff0000, 0,
            0xffff0000, 0xffff0000, 0, 0x10000};
        for (unsigned row = 0; row < 4; ++row) uniform(1 + row, std::span(projection).subspan(row * 4, 4));
        struct Scratch {
            GuestHeap &heap;
            u32 address;
            ~Scratch() { if (address) heap.release(address); }
        } scratch{frame.emulator.heap(), frame.emulator.heap().allocate(96)};
        if (!scratch.address) throw std::runtime_error("Cannot allocate overlay vertices");
        const auto vertices = encode_words(overlay_vertices_), coordinates = encode_words(overlay_coordinates_);
        if (!frame.memory.write_bytes(scratch.address, vertices.data(), 64) ||
            !frame.memory.write_bytes(scratch.address + 64, coordinates.data(), 32))
            throw std::runtime_error("Cannot store overlay vertices");
        attributes_enabled_ |= 3;
        attributes_dirty_ |= 3;
        for (unsigned i = 0; i < 2; ++i) {
            const u32 width = i ? 2 : 4;
            attributes_[i] = {width, 0x140c, width * 4, scratch.address + i * 64, false, array_buffer_};
            if (!i) textures_[2] = width;
            const std::array<u32, 3> value{width, 0, 0};
            uniform(0x211 + i, value);
        }
        std::array<u32, 16> draw_args{7, 0, 4};
        draw(37, frame, draw_args);
        attributes_enabled_ = old_enabled;
        attributes_dirty_ = 3;
        select_program(old_program);
        textures_[0] = old_texture;
        textures_dirty_ |= 1;
        active_texture_ = old_unit;
        attributes_ = old_attributes;
        textures_[2] = attributes_[0].size;
    }
    queue({6, 0, 0, 0});
    flush();
    return 1;
}
} // namespace clickhle

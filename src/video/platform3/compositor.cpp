#include "clickhle/video/classic_compositor.h"

#include <algorithm>

namespace clickhle::classic {
namespace {

constexpr std::array<std::array<u8, 3>, 6> blend_order{{
    {1, 2, 0}, {2, 1, 0}, {0, 2, 1},
    {2, 0, 1}, {0, 1, 2}, {1, 0, 2},
}};

unsigned blend_register(unsigned slot, unsigned ordering) {
    const unsigned group = slot == 0 ? 0 : (slot == 5 ? 2 : 1);
    return (0xd8u + blend_order[ordering][group] * 8u) / 4u;
}

} // namespace

bool Compositor::set_layer(unsigned slot, const LayerDescriptor& layer) {
    if (slot >= 5 || layer.format >= 8 || layer.x > 320 || layer.y > 240 ||
        (layer.width | layer.height | layer.source_width | layer.source_height |
         layer.x | layer.y | layer.crop_x | layer.crop_y) & 0x80000000u)
        return false;
    constexpr std::array<u32, 8> half_bytes{1, 2, 4, 4, 4, 4, 8, 8};
    const u32 multiplier = half_bytes[layer.format];
    const u32 width = std::min(layer.width, 320u - layer.x);
    const u32 height = std::min(layer.height, 240u - layer.y);
    const u32 start = multiplier * layer.crop_x;
    const u32 end = multiplier * (layer.crop_x + width);
    const u32 first = start >> 3;
    const u32 last = (end >> 3) + ((end & 7u) != 0 ? 1u : 0u);
    const u32 stride = (multiplier * layer.source_width) >> 1;
    const u32 words = (end >> 3) - first;
    const u32 tail = (words & 3u) ? 4u - (words & 3u) : ((end & 7u) ? 4u : 0u);
    u32 control = (layer.format << 8) | (tail << 24) | (start & 7u);
    if (multiplier < 8) control |= 0x10000;
    const unsigned index = (0x58 + slot * 0x18) / 4;
    registers_[0x3ac / 4] = 0;
    registers_[index] = stride;
    registers_[index + 1] = control;
    registers_[index + 2] = layer.source + layer.crop_y * stride + first * 4;
    registers_[index + 3] = (width << 16) | height;
    registers_[index + 4] = (last - first) & 0xffff;
    registers_[index + 5] = (layer.x << 16) | layer.y;
    return true;
}

bool Compositor::set_enabled(unsigned slot, bool enabled) {
    constexpr std::array<u32, 6> bits{0x40, 0x20, 0x10, 8, 4, 0x80};
    if (slot >= bits.size()) return false;
    auto& control = registers_[8 / 4];
    control = (control & ~bits[slot]) | (enabled ? bits[slot] : 0u);
    return true;
}

bool Compositor::set_sources(unsigned slot, u8 format,
                             const std::array<u32, 4>& planes, u32 secondary_offset) {
    if (slot < 5) {
        registers_[(0x60 + slot * 0x18) / 4] = planes[0];
        return true;
    }
    if (slot != 5) return false;

    std::array<u32, 4> sources{};
    if (format == 8 && planes[0] && planes[1] && planes[2]) {
        sources = {planes[0], planes[2], 0, planes[1]};
    } else if (format == 9 && planes[0] && planes[1]) {
        sources = {planes[0], planes[1], planes[0] + secondary_offset,
                   planes[1] + secondary_offset};
    } else if (format == 10 && planes[0] && planes[1] && planes[2] && planes[3]) {
        sources = planes;
    } else {
        return true;
    }
    for (unsigned i = 0; i < sources.size(); ++i) registers_[0x38 / 4 + i] = sources[i];
    return true;
}

void Compositor::write_lookup_tables(const std::array<u32, 256>& first,
                                     const std::array<u32, 256>& second,
                                     const std::array<u32, 256>& third) {
    for (std::size_t i = 0; i < first.size(); ++i) {
        registers_[0x400 / 4 + i] = first[i];
        registers_[0x800 / 4 + i] = second[i];
        registers_[0xc00 / 4 + i] = third[i];
    }
}

void Compositor::initialize_lookup_tables() {
    std::array<u32, 256> values{};
    for (u32 i = 0; i < values.size(); ++i) values[i] = i * 4;
    write_lookup_tables(values, values, values);
}

void Compositor::set_output_control(u32 first, u32 second,
                                    const std::array<u8, 3>& fields, u32 last) {
    u32& control = registers_[8 / 4];
    constexpr u32 mask = 0x30000000u | 0x03000000u | 0x00300000u | 0x00030000u | 1u;
    u32 value = first != 0 ? 0x20000000u : 0u;
    if (second == 1) value |= 0x10000000u;
    if (fields[0] == 1 || fields[0] == 2) value |= static_cast<u32>(fields[0]) << 24;
    if (fields[1] == 1) value |= 0x00100000u;
    if (fields[2] == 1 || fields[2] == 2) value |= static_cast<u32>(fields[2]) << 16;
    if (last == 1) value |= 1u;
    control = (control & ~mask) | value;
}

bool Compositor::set_blend_mode(unsigned slot, unsigned mode, unsigned ordering) {
    if (slot > 5) return false;
    if (slot == 4) return mode == 4;
    if (mode > 4 || ordering >= 6) return false;
    constexpr std::array<u32, 5> high{1, 0, 5, 9, 11};
    constexpr std::array<u32, 5> low{0, 1, 4, 8, 10};
    u32& control = registers_[blend_register(slot, ordering)];
    control = (control & ~0xf000f000u) | (high[mode] << 28) | (low[mode] << 12);
    return true;
}

bool Compositor::set_blend_parameter(unsigned slot, u32 value, unsigned ordering) {
    if (slot > 5 || slot == 4 || ordering >= 6) return false;
    u32& control = registers_[blend_register(slot, ordering)];
    // Firmware clears the low byte but ORs the entire argument without masking.
    control = (control & ~0xffu) | value;
    return true;
}

std::optional<u32> Compositor::read(u32 address) const {
    if (address < base || address - base >= registers_.size() * 4 || (address & 3))
        return std::nullopt;
    return registers_[(address - base) / 4];
}

bool Compositor::write(u32 address, u32 value) {
    if (address < base || address - base >= registers_.size() * 4 || (address & 3))
        return false;
    registers_[(address - base) / 4] = value;
    return true;
}

} // namespace clickhle::classic

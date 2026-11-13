#include "clickhle/video/classic_pixel.h"
#include <bit>
#include <algorithm>

namespace clickhle::classic {

u32 float_to_fixed(u32 bits) {
    const bool negative = (bits & 0x80000000u) != 0;
    const u32 exponent = (bits >> 23) & 0xffu;
    if (exponent >= 142) return negative ? 0x80000000u : 0x7fffffffu;
    if (exponent < 111) return 0;
    const u32 mantissa = (bits & 0x7fffffu) | 0x800000u;
    const int shift = static_cast<int>(exponent) - 134;
    const u32 magnitude = shift >= 0 ? mantissa << shift : mantissa >> -shift;
    return negative ? 0u - magnitude : magnitude;
}

std::array<u8, 4> fixed_color_to_bytes(const std::array<u32, 4>& color) {
    std::array<u8, 4> result{};
    for (std::size_t i = 0; i < color.size(); ++i) {
        const i32 component = std::clamp(std::bit_cast<i32>(color[i]), 0, 65536);
        result[i] = static_cast<u8>((static_cast<u32>(component) * 511u) >> 17);
    }
    return result;
}

std::optional<u32> pack_clear_color(u8 format, const std::array<u8, 4>& c) {
    switch (format) {
    case 4:
        return (u32(c[0]) << 24) | (u32(c[1]) << 16) | (u32(c[2]) << 8) | c[3];
    case 5:
        return (u32(c[0] & 0xf8u) << 8) | (u32(c[1] & 0xfcu) << 3) | (c[2] >> 3);
    case 6:
        return (u32(c[0] & 0xf0u) << 8) | (u32(c[1] & 0xf0u) << 4) |
               (c[2] & 0xf0u) | (c[3] >> 4);
    case 7:
        return (u32(c[0] & 0xf8u) << 8) | (u32(c[1] & 0xf8u) << 3) |
               (u32(c[2] & 0xf8u) >> 2) | (c[3] >> 7);
    default:
        return std::nullopt;
    }
}

bool rgb565_to_rgb(std::span<const u8> source, u32 stride_pixels,
                   u32 width, u32 height, std::span<u8> output) {
    if (width > stride_pixels) return false;
    const u64 pixels = u64(width) * height;
    if (pixels > output.size() / 3) return false;
    if (height && (u64(height - 1) * stride_pixels + width) > source.size() / 2)
        return false;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const auto input = (u64(y) * stride_pixels + x) * 2;
            const u16 pixel = u16(source[input]) | (u16(source[input + 1]) << 8);
            const u8 red = static_cast<u8>((pixel >> 11) & 31u);
            const u8 green = static_cast<u8>((pixel >> 5) & 63u);
            const u8 blue = static_cast<u8>(pixel & 31u);
            const auto out = (u64(y) * width + x) * 3;
            output[out] = static_cast<u8>((red << 3) | (red >> 2));
            output[out + 1] = static_cast<u8>((green << 2) | (green >> 4));
            output[out + 2] = static_cast<u8>((blue << 3) | (blue >> 2));
        }
    }
    return true;
}

bool fill_masked_rectangle(std::span<u8> pixels, u32 stride, ClearRectangle rect,
                           unsigned bytes_per_pixel, u32 color, u32 mask) {
    if (bytes_per_pixel != 2 && bytes_per_pixel != 4) return false;
    const u32 full_mask = bytes_per_pixel == 2 ? 0xffffu : 0xffffffffu;
    mask &= full_mask;
    if (mask == full_mask) return false;
    if (rect.width == 0 || rect.height == 0) return true;
    const u64 first = u64(rect.y) * stride + rect.x;
    const u64 capacity = pixels.size() / bytes_per_pixel;
    const u64 rows = u64(rect.height - 1) * stride;
    if (first > capacity || rows > capacity - first ||
        rect.width > capacity - first - rows) return false;

    for (u32 y = 0; y < rect.height; ++y) {
        for (u32 x = 0; x < rect.width; ++x) {
            const auto offset = static_cast<std::size_t>((first + u64(y) * stride + x) * bytes_per_pixel);
            u32 previous = 0;
            for (unsigned byte = 0; byte < bytes_per_pixel; ++byte)
                previous |= u32(pixels[offset + byte]) << (byte * 8);
            const u32 value = (previous & ~mask) | (color & mask);
            for (unsigned byte = 0; byte < bytes_per_pixel; ++byte)
                pixels[offset + byte] = static_cast<u8>(value >> (byte * 8));
        }
    }
    return true;
}

bool fill_full_surface_band(std::span<u8> pixels, u32 stride, u32 first_row,
                            u32 row_count, unsigned bytes_per_pixel, u32 color) {
    if (bytes_per_pixel != 2 && bytes_per_pixel != 4) return false;
    const u64 first = u64(first_row) * stride;
    u64 count = u64(row_count) * stride;
    // The 16-bit helper writes pairs of pixels and discards an odd halfword.
    if (bytes_per_pixel == 2) count &= ~u64{1};
    const u64 capacity = pixels.size() / bytes_per_pixel;
    if (first > capacity || count > capacity - first) return false;
    for (u64 i = 0; i < count; ++i) {
        const auto offset = static_cast<std::size_t>((first + i) * bytes_per_pixel);
        for (unsigned byte = 0; byte < bytes_per_pixel; ++byte)
            pixels[offset + byte] = static_cast<u8>(color >> (byte * 8));
    }
    return true;
}

bool clear_surface_color(std::span<u8> pixels, u8 format, u32 stride,
                         ClearRectangle surface, ClearRectangle rectangle,
                         const std::array<u32, 4>& color,
                         const std::array<u8, 4>& write_mask) {
    const auto packed = pack_clear_color(format, fixed_color_to_bytes(color));
    std::array<u8, 4> mask_bytes{};
    for (std::size_t i = 0; i < mask_bytes.size(); ++i)
        mask_bytes[i] = write_mask[i] ? 0xff : 0;
    const auto mask = pack_clear_color(format, mask_bytes);
    if (!packed || !mask) return false;
    const unsigned bytes_per_pixel = format == 4 ? 4u : 2u;
    const u32 full_mask = bytes_per_pixel == 4 ? 0xffffffffu : 0xffffu;
    if (*mask == full_mask)
        return fill_full_surface_band(pixels, stride, surface.y, surface.height,
                                      bytes_per_pixel, *packed);
    return fill_masked_rectangle(pixels, stride, rectangle, bytes_per_pixel, *packed, *mask);
}

} // namespace clickhle::classic

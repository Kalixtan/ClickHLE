#pragma once

#include "clickhle/common/types.h"
#include <array>
#include <optional>
#include <span>

namespace clickhle::classic {

// 7G OSOS 0x0809b904: IEEE-754 single bits to saturating signed 16.16 bits.
[[nodiscard]] u32 float_to_fixed(u32 bits);

// 7G OSOS 0x082948f0 clamps 16.16 components and converts them to bytes.
[[nodiscard]] std::array<u8, 4> fixed_color_to_bytes(const std::array<u32, 4>& color);

// Convert the four component bytes consumed by the 7G clear routines into
// the selected surface format. The byte order here is the firmware's input
// order; its relationship to guest GL colors is established separately.
[[nodiscard]] std::optional<u32> pack_clear_color(u8 format,
                                                  const std::array<u8, 4>& components);

// Expand little-endian RGB565 rows for the host display. The compositor's
// color-matrix and panel transfer behavior are not modeled by this helper.
bool rgb565_to_rgb(std::span<const u8> source, u32 stride_pixels,
                   u32 width, u32 height, std::span<u8> output);

struct ClearRectangle {
    u32 x, y, width, height;
};

// Partial-mask path of the original surface fill. Stride is in pixels.
// Full-mask clears use a different firmware path and are not modeled here.
bool fill_masked_rectangle(std::span<u8> pixels, u32 stride, ClearRectangle rect,
                           unsigned bytes_per_pixel, u32 color, u32 mask);

// Full-mask path uses a contiguous band from the surface, not the clear rectangle.
bool fill_full_surface_band(std::span<u8> pixels, u32 stride, u32 first_row,
                            u32 row_count, unsigned bytes_per_pixel, u32 color);

// The color portion of OSOS 0x08248d98, for a surface whose backing memory and
// rectangle have already been resolved. Unsupported formats return false.
bool clear_surface_color(std::span<u8> pixels, u8 format, u32 stride,
                         ClearRectangle surface, ClearRectangle rectangle,
                         const std::array<u32, 4>& color,
                         const std::array<u8, 4>& write_mask);

} // namespace clickhle::classic

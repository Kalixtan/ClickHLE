#include "clickhle/video/classic_pixel.h"

int main() {
    using clickhle::classic::pack_clear_color;
    using clickhle::classic::float_to_fixed;
    if (float_to_fixed(0x3dcccccd) != 6553u || float_to_fixed(0xbdcccccd) != 0xffffe667u ||
        float_to_fixed(0x3f800000) != 65536u || float_to_fixed(0x7fc00000) != 0x7fffffffu ||
        float_to_fixed(0xff800000) != 0x80000000u || float_to_fixed(1) != 0) return 1;
    if (clickhle::classic::fixed_color_to_bytes({0xffff0000u, 0u, 0x8000u, 0x10000u}) !=
        std::array<clickhle::u8, 4>{0, 0, 127, 255}) return 1;
    constexpr std::array<clickhle::u8, 4> color{0x91, 0x53, 0x2d, 0xc7};
    if (pack_clear_color(4, color) != 0x91532dc7u ||
        pack_clear_color(5, color) != 0x9285u ||
        pack_clear_color(6, color) != 0x952cu ||
        pack_clear_color(7, color) != 0x928bu ||
        pack_clear_color(3, color) || pack_clear_color(8, color)) return 1;
    std::array<clickhle::u8, 16> pixels{};
    pixels.fill(0xa5);
    if (!clickhle::classic::fill_masked_rectangle(pixels, 4, {1, 0, 2, 2}, 2, 0x1234, 0x0ff0)) return 1;
    constexpr std::array<clickhle::u8, 16> expected{
        0xa5, 0xa5, 0x35, 0xa2, 0x35, 0xa2, 0xa5, 0xa5,
        0xa5, 0xa5, 0x35, 0xa2, 0x35, 0xa2, 0xa5, 0xa5,
    };
    if (pixels != expected ||
        clickhle::classic::fill_masked_rectangle(pixels, 4, {0, 1, 4, 2}, 2, 0, 0xff) ||
        clickhle::classic::fill_masked_rectangle(pixels, 4, {0, 0, 4, 1}, 2, 0, 0xffff)) return 1;
    if (pixels != expected) return 1;
    pixels.fill(0xa5);
    if (!clickhle::classic::fill_full_surface_band(pixels, 3, 1, 1, 2, 0x1234)) return 1;
    constexpr std::array<clickhle::u8, 16> band{
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0x34, 0x12,
        0x34, 0x12, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
    };
    if (pixels != band ||
        clickhle::classic::fill_full_surface_band(pixels, 4, 1, 2, 2, 0) || pixels != band) return 1;
    pixels.fill(0);
    if (!clickhle::classic::clear_surface_color(pixels, 5, 4, {0, 0, 4, 2}, {1, 0, 1, 1},
                                               {0x10000, 0, 0, 0x10000}, {1, 1, 1, 1})) return 1;
    for (std::size_t i = 0; i < pixels.size(); i += 2)
        if (pixels[i] != 0 || pixels[i + 1] != 0xf8) return 1;
    pixels.fill(0xff);
    if (!clickhle::classic::clear_surface_color(pixels, 5, 4, {0, 0, 4, 2}, {1, 0, 1, 1},
                                               {0, 0, 0, 0}, {1, 0, 0, 0})) return 1;
    if (pixels[2] != 0xff || pixels[3] != 0x07 || pixels[0] != 0xff || pixels[1] != 0xff) return 1;
    constexpr std::array<clickhle::u8, 12> source{0x00, 0xf8, 0xe0, 0x07, 0xaa, 0xaa,
                                                 0x1f, 0x00, 0xff, 0xff, 0xaa, 0xaa};
    std::array<clickhle::u8, 12> rgb{};
    constexpr std::array<clickhle::u8, 12> expected_rgb{255, 0, 0, 0, 255, 0,
                                                       0, 0, 255, 255, 255, 255};
    if (!clickhle::classic::rgb565_to_rgb(source, 3, 2, 2, rgb) || rgb != expected_rgb ||
        clickhle::classic::rgb565_to_rgb(source, 1, 2, 2, rgb) ||
        clickhle::classic::rgb565_to_rgb(source, 3, 2, 3, rgb)) return 1;
}

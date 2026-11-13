#include "clickhle/video/classic_compositor.h"

#include <array>
#include <iostream>

using namespace clickhle;

int main() {
    // Expected records come from the firmware-checked descriptor encoder.
    constexpr std::array<std::array<u32, 6>, 8> expected{{
        {0xa0, 0x4010003, 0x8123140, 0x640032, 0xd, 0xa0014},
        {0x141, 0x3010106, 0x8123282, 0x640032, 0x1a, 0xa0014},
        {0x282, 0x2010204, 0x8123508, 0x640032, 0x33, 0xa0014},
        {0x282, 0x2010304, 0x8123508, 0x640032, 0x33, 0xa0014},
        {0x282, 0x2010404, 0x8123508, 0x640032, 0x33, 0xa0014},
        {0x282, 0x2010504, 0x8123508, 0x640032, 0x33, 0xa0014},
        {0x504, 0x600, 0x8123a14, 0x640032, 0x64, 0xa0014},
        {0x504, 0x700, 0x8123a14, 0x640032, 0x64, 0xa0014},
    }};
    classic::Compositor compositor;
    for (unsigned format = 0; format < expected.size(); ++format) {
        classic::LayerDescriptor layer{format, 0x8123000, 321, 240, 100, 50, 10, 20, 3, 2};
        for (unsigned slot = 0; slot < 5; ++slot) {
            if (!compositor.set_layer(slot, layer)) return 1;
            for (unsigned i = 0; i < 6; ++i) {
                const auto value = compositor.read(classic::Compositor::base + 0x58 + slot * 0x18 + i * 4);
                if (!value || *value != expected[format][i]) {
                    std::cerr << "Incorrect layer record for format " << format << '\n';
                    return 1;
                }
            }
        }
    }
    if (!compositor.write(classic::Compositor::base + 8, 0x103)) return 1;
    for (unsigned slot = 0; slot < 6; ++slot)
        if (!compositor.set_enabled(slot, true)) return 1;
    if (compositor.read(classic::Compositor::base + 8) != 0x1ffu) return 1;
    if (!compositor.set_enabled(2, false) ||
        compositor.read(classic::Compositor::base + 8) != 0x1efu) return 1;
    if (!compositor.write(classic::Compositor::base + 8, 0xffffffff)) return 1;
    compositor.set_output_control(0, 0, {1, 1, 1}, 1);
    if (compositor.read(classic::Compositor::base + 8) != 0xcdddffffu) return 1;
    for (u32 reg : {0xd8u, 0xe0u, 0xe8u})
        if (!compositor.write(classic::Compositor::base + reg, 0x12345678)) return 1;
    if (!compositor.set_blend_mode(0, 2, 0) ||
        compositor.read(classic::Compositor::base + 0xe0) != 0x52344678u ||
        compositor.read(classic::Compositor::base + 0xd8) != 0x12345678u ||
        !compositor.set_blend_mode(4, 4, 99) ||
        compositor.set_blend_mode(4, 0, 0) || compositor.set_blend_mode(6, 0, 0) ||
        compositor.set_blend_mode(0, 5, 0) || compositor.set_blend_mode(0, 0, 6)) return 1;
    if (!compositor.set_blend_parameter(0, 0x800003a5, 0) ||
        compositor.read(classic::Compositor::base + 0xe0) != 0xd23447a5u ||
        compositor.set_blend_parameter(4, 0, 0) ||
        compositor.set_blend_parameter(5, 0, 6)) return 1;
    for (u32 offset : {0x400u, 0x800u, 0xc00u, 0xffcu}) {
        if (!compositor.write(classic::Compositor::base + offset, offset) ||
            compositor.read(classic::Compositor::base + offset) != offset) return 1;
    }
    std::array<u32, 256> first{}, second{}, third{};
    for (u32 i = 0; i < first.size(); ++i) {
        first[i] = 0x100000 + i * 7;
        second[i] = 0x200000 + i * 7;
        third[i] = 0x300000 + i * 7;
    }
    compositor.write_lookup_tables(first, second, third);
    for (u32 i = 0; i < first.size(); ++i) {
        if (compositor.read(classic::Compositor::base + 0x400 + i * 4) != first[i] ||
            compositor.read(classic::Compositor::base + 0x800 + i * 4) != second[i] ||
            compositor.read(classic::Compositor::base + 0xc00 + i * 4) != third[i]) return 1;
    }
    compositor.initialize_lookup_tables();
    for (u32 i = 0; i < 256; ++i)
        for (u32 table : {0x400u, 0x800u, 0xc00u})
            if (compositor.read(classic::Compositor::base + table + i * 4) != i * 4) return 1;
    constexpr std::array<u32, 4> planes{0x1000, 0x2000, 0x3000, 0x4000};
    constexpr std::array<std::array<u32, 4>, 3> expected_sources{{
        {0x1000, 0x3000, 0, 0x2000},
        {0x1000, 0x2000, 0x1100, 0x2100},
        {0x1000, 0x2000, 0x3000, 0x4000},
    }};
    for (u8 format = 8; format <= 10; ++format) {
        if (!compositor.set_sources(5, format, planes, 0x100)) return 1;
        for (u32 i = 0; i < 4; ++i)
            if (compositor.read(classic::Compositor::base + 0x38 + i * 4) != expected_sources[format - 8][i]) return 1;
    }
    if (!compositor.set_sources(5, 8, {0, 2, 3, 4}, 0) ||
        compositor.read(classic::Compositor::base + 0x38) != 0x1000 ||
        compositor.set_sources(6, 8, planes, 0)) return 1;
    for (u8 format : std::array<u8, 5>{0, 8, 9, 10, 11}) {
        for (u32 mask = 0; mask < 16; ++mask) {
            std::array<u32, 4> input{};
            for (u32 i = 0; i < 4; ++i) {
                input[i] = (mask & (1u << i)) ? 0x9000000 + i * 0x10000 : 0;
                if (!compositor.write(classic::Compositor::base + 0x38 + i * 4, 0xa5a5a5a5)) return 1;
            }
            std::array<u32, 4> result{0xa5a5a5a5, 0xa5a5a5a5, 0xa5a5a5a5, 0xa5a5a5a5};
            if (format == 8 && (mask & 7) == 7) result = {input[0], input[2], 0, input[1]};
            if (format == 9 && (mask & 3) == 3) result = {input[0], input[1], input[0] + 0x1234, input[1] + 0x1234};
            if (format == 10 && mask == 15) result = input;
            if (!compositor.set_sources(5, format, input, 0x1234)) return 1;
            for (u32 i = 0; i < 4; ++i)
                if (compositor.read(classic::Compositor::base + 0x38 + i * 4) != result[i]) return 1;
        }
    }
    if (compositor.set_enabled(6, true) || compositor.read(classic::Compositor::base + 1) ||
        compositor.write(classic::Compositor::base - 4, 0) ||
        compositor.write(classic::Compositor::base + 0x1000, 0)) return 1;
    return 0;
}

#include "clickhle/core/address_space.h"
#include "clickhle/video/classic_graphics.h"

#include <array>

int main() {
    using namespace clickhle;
    using namespace clickhle::classic;
    AddressSpace memory;
    if (!memory.map(0x1000, 0x1000, AddressSpace::kRW, "test surface")) return 1;
    GraphicsContext context;
    if (clear_bound_surface(context, memory, 0x4000) != ClearResult::missing_surface) return 1;
    context.bind_draw_surface({0x1000, 128, 8, 5, {0, 0, 8, 8}});
    context.set_clear_color({0x10000, 0, 0, 0x10000});
    if (clear_bound_surface(context, memory, 0x4000) != ClearResult::complete) return 1;
    for (u32 offset = 0; offset < 128; offset += 2)
        if (memory.read<u16>(0x1000 + offset) != 0xf800u) return 1;
    const auto rgb = read_surface_rgb(*context.draw_surface(), memory);
    if (!rgb || rgb->size() != 8 * 8 * 3) return 1;
    for (std::size_t i = 0; i < rgb->size(); i += 3)
        if ((*rgb)[i] != 255 || (*rgb)[i + 1] != 0 || (*rgb)[i + 2] != 0) return 1;
    context.set_clear_color({0, 0, 0, 0});
    context.set_color_mask({1, 0, 0, 0});
    context.set_scissor({1, 2, 3, 2}, true);
    if (clear_bound_surface(context, memory, 0x4000) != ClearResult::complete ||
        memory.read<u16>(0x1000 + (2 * 8 + 1) * 2) != 0 ||
        memory.read<u16>(0x1000) != 0xf800u) return 1;
    const auto before = memory.read<u16>(0x1000);
    if (clear_bound_surface(context, memory, 0x4100) != ClearResult::unsupported_buffers ||
        memory.read<u16>(0x1000) != before) return 1;
    if (clear_bound_surface(context, memory, 1) != ClearResult::complete || context.error() != 0x501) return 1;
    context.bind_draw_surface({0x80000000, 128, 8, 5, {0, 0, 8, 8}});
    if (clear_bound_surface(context, memory, 0x4000) != ClearResult::invalid_memory) return 1;
    if (read_surface_rgb(*context.draw_surface(), memory)) return 1;
}

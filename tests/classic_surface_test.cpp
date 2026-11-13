#include "clickhle/video/classic_surface.h"
#include "clickhle/core/address_space.h"

int main() {
    clickhle::AddressSpace memory;
    if (!memory.map(0x1000, 0x1000, clickhle::AddressSpace::kRW, "surface")) return 1;
    for (clickhle::u32 i = 0; i < 0x68; ++i)
        if (!memory.write<clickhle::u8>(0x1100 + i, static_cast<clickhle::u8>(i)) ||
            !memory.write<clickhle::u8>(0x1200 + i, 0xa5)) return 1;
    if (!clickhle::classic::initialize_surface_config(memory, 0x1200, 0x1100)) return 1;
    for (clickhle::u32 i = 0; i < 0x66; ++i)
        if (memory.read<clickhle::u8>(0x1200 + i) != i) return 1;
    if (memory.read<clickhle::u8>(0x1266) != 0xa5 ||
        clickhle::classic::initialize_surface_config(memory, 0x1200, 0x2000)) return 1;
    if (!clickhle::classic::initialize_game_config(memory, 0x1300) ||
        memory.read<clickhle::u32>(0x1300) != 0x10 ||
        memory.read<clickhle::u32>(0x135c) != 320 ||
        memory.read<clickhle::u32>(0x1360) != 240 ||
        memory.read<clickhle::u8>(0x1364) != 5 ||
        memory.read<clickhle::u8>(0x1365) != 0) return 1;
    clickhle::classic::SurfaceBuffers surface{{1, 0x1111, 2}, {2, 0x2222, 2}, 0x2222};
    auto swap = clickhle::classic::begin_surface_swap(surface);
    if (swap.presented_buffer != 2 || swap.wait_buffer != 1 ||
        surface.front.handle != 2 || surface.back.handle != 1 || surface.draw_pixels != 0x2222) return 1;
    clickhle::classic::finish_surface_swap(surface);
    if (surface.draw_pixels != 0x1111) return 1;
    surface.front.type = 3;
    swap = clickhle::classic::begin_surface_swap(surface);
    if (swap.presented_buffer != 1 || swap.wait_buffer != 2) return 1;
    clickhle::classic::finish_surface_swap(surface);
    if (surface.draw_pixels != 0) return 1;
    for (clickhle::u8 front_type = 0; front_type < 5; ++front_type) {
        for (clickhle::u8 back_type = 0; back_type < 5; ++back_type) {
            clickhle::classic::SurfaceBuffers pair{{1, 0x1111, front_type},
                                                   {2, 0x2222, back_type}, 0xdeadbeef};
            const auto event = clickhle::classic::begin_surface_swap(pair);
            if (event.presented_buffer != 2 || event.wait_buffer != 1 ||
                pair.front.handle != 2 || pair.back.handle != 1 || pair.draw_pixels != 0xdeadbeef) return 1;
            clickhle::classic::finish_surface_swap(pair);
            if (pair.draw_pixels != (front_type == 2 ? 0x1111u : 0u)) return 1;
        }
    }
}

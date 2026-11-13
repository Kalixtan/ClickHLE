#include "clickhle/video/classic_surface.h"
#include "clickhle/core/address_space.h"

#include <array>

#include <utility>

namespace clickhle::classic {

bool initialize_game_config(AddressSpace& memory, GuestAddr config) {
    constexpr std::array<u32, 25> words{
        0x10, 5, 6, 5, 0, 0x3038, 5, 0x10, 0, 0x400, 0x400, 0x100000,
        0, 0, 0x3038, 0, 0, 0, 5, 0x3038, 0, 0, 0, 0x140, 0xf0,
    };
    for (u32 i = 0; i < words.size(); ++i)
        if (!memory.write<u32>(config + i * 4, words[i])) return false;
    return memory.write<u8>(config + 0x64, 5) && memory.write<u8>(config + 0x65, 0);
}

bool initialize_surface_config(AddressSpace& memory, GuestAddr surface, GuestAddr config) {
    std::array<u8, 0x66> bytes{};
    return memory.read_bytes(config, bytes.data(), static_cast<u32>(bytes.size())) &&
           memory.write_bytes(surface, bytes.data(), static_cast<u32>(bytes.size()));
}

SurfaceSwap begin_surface_swap(SurfaceBuffers& surface) {
    const SurfaceSwap swap{surface.back.handle, surface.front.handle};
    std::swap(surface.front, surface.back);
    return swap;
}

void finish_surface_swap(SurfaceBuffers& surface) {
    surface.draw_pixels = surface.back.type == 2 ? surface.back.pixels : 0;
}

} // namespace clickhle::classic

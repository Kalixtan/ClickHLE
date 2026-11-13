#include "clickhle/hle/classic_memory.h"

using namespace clickhle;

int main() {
    using classic::game_heap_budget;
    if (game_heap_budget(0) != 8192 || game_heap_budget(1) != 3073 ||
        game_heap_budget(5120) != 8192 || game_heap_budget(5121) != 8192 ||
        game_heap_budget(0xffffffffu) != 8192) return 1;

    constexpr u32 page_size = 0x80000;
    const std::array<GuestAddr, 4> pages{0x20000000, 0x20080000, 0x20100000, 0x20180000};
    auto layout = classic::plan_arena(pages, page_size);
    if (!layout || layout->reserve_count != 2 ||
        layout->reserves != std::array<GuestAddr, 2>{0x20000000, 0x20080000} ||
        layout->regions.size() != 1 || layout->regions[0].base != 0x20100000 ||
        layout->regions[0].size != 0x100000) return 1;

    // The isolated page is reserved first, before the larger contiguous pair.
    const std::array<GuestAddr, 3> fragmented{0x20100000, 0x20180000, 0x20000000};
    layout = classic::plan_arena(fragmented, page_size);
    if (!layout || layout->reserve_count != 1 || layout->reserves[0] != 0x20000000 ||
        layout->regions.size() != 1 || layout->regions[0].base != 0x20100000 ||
        layout->regions[0].size != 0x100000) return 1;

    const std::array<GuestAddr, 2> duplicate{0x20000000, 0x20000000};
    const std::array<GuestAddr, 1> overflow{0xfffffff0};
    if (classic::plan_arena(duplicate, page_size) ||
        classic::plan_arena(overflow, page_size) ||
        classic::plan_arena({}, page_size) || classic::plan_arena(pages, 0)) return 1;
}

#include "clickhle/hle/classic_memory.h"

#include <algorithm>
#include <limits>

namespace clickhle::classic {

std::optional<ArenaLayout> plan_arena(std::span<const GuestAddr> pages, u32 page_size) {
    if (page_size < 32 || (page_size & 7u) != 0 || pages.empty()) return std::nullopt;
    ArenaLayout result;
    std::vector<HeapRegion> regions;
    regions.reserve(pages.size());
    for (GuestAddr base : pages) {
        if (base == 0 || (base & 7u) != 0 ||
            base > std::numeric_limits<u32>::max() - page_size) return std::nullopt;
        for (const auto& region : regions) {
            if (base < region.base + region.size && region.base < base + page_size)
                return std::nullopt;
        }
        regions.push_back({base, page_size});
    }
    result.reserve_count = static_cast<u32>(std::min<std::size_t>(2, pages.size() / 2));

    // Preserve the firmware's traversal order, including equal-sized regions.
    // Sorting by address first would change which pages become reserves.
    for (std::size_t i = 0; i < regions.size(); ++i) {
        for (std::size_t j = i + 1; j < regions.size(); ++j) {
            auto& left = regions[i];
            auto& right = regions[j];
            if (left.base + left.size == right.base) {
                left.size += right.size;
                right = {0xffffffffu, 0};
            } else if (right.base + right.size == left.base) {
                left.base = right.base;
                left.size += right.size;
                right = {0xffffffffu, 0};
            }
        }
    }
    // The firmware's smallest-region search starts with INT_MAX. Larger
    // regions are outside that routine's valid input domain.
    for (const auto& region : regions) {
        if (region.size >= 0x7fffffffu) return std::nullopt;
    }
    u32 reserved = 0;
    while (reserved < result.reserve_count) {
        auto smallest = regions.end();
        for (auto it = regions.begin(); it != regions.end(); ++it) {
            if (it->size != 0 && (smallest == regions.end() || it->size < smallest->size))
                smallest = it;
        }
        if (smallest == regions.end()) return std::nullopt;
        while (smallest->size != 0 && reserved < result.reserve_count) {
            result.reserves[reserved++] = smallest->base;
            smallest->base += page_size;
            smallest->size -= page_size;
        }
    }
    for (const auto& region : regions) {
        if (region.size != 0) result.regions.push_back(region);
    }
    return result;
}

} // namespace clickhle::classic

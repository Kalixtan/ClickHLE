#pragma once

#include <array>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "clickhle/common/types.h"

namespace clickhle { class AddressSpace; }

namespace clickhle::classic {

struct HeapRegion {
    GuestAddr base;
    u32 size;
};

struct ArenaLayout {
    std::array<GuestAddr, 2> reserves{};
    u32 reserve_count = 0;
    std::vector<HeapRegion> regions;
};

// 7G launcher 0x0828b1ac. Zero and out-of-range requests select 5 MiB,
// then the launcher adds 3 MiB. The argument and return value are KiB.
[[nodiscard]] constexpr u32 game_heap_budget(u32 requested_kib) {
    return ((requested_kib - 1u < 0x1400u) ? requested_kib : 0x1400u) + 0xc00u;
}

// 7G arena initialization 0x081b1358..0x081b1490. Input is the ordered
// sequence supplied by the OS page provider. This does not choose physical
// addresses, run the provider, or initialize the heap's guest metadata.
[[nodiscard]] std::optional<ArenaLayout> plan_arena(
    std::span<const GuestAddr> pages, u32 page_size);

// Original 7G heap metadata lives in guest RAM. This models its unlocked,
// single-thread path; OS locks, provider callbacks and resize are separate.
class RegionHeap {
public:
    static constexpr u32 control_size = 0x398;
    RegionHeap(AddressSpace& memory, GuestAddr control) : memory_(memory), control_(control) {}
    void initialize();
    void add_region(GuestAddr base, u32 size);
    [[nodiscard]] GuestAddr allocate(u32 size, u8 tag, u16 thread);
    void release(GuestAddr pointer);

private:
    [[nodiscard]] u32 read(GuestAddr address) const;
    void write(GuestAddr address, u32 value);
    void adjust(u32 offset, u32 amount);
    void unlink(GuestAddr block);
    void insert_free(GuestAddr block);
    AddressSpace& memory_;
    GuestAddr control_;
};

// Reserved pages and fallback rules in 0x081b1528 and 0x081b1224.
// Page addresses come from the caller's arena plan.
class Arena {
public:
    Arena(RegionHeap& heap, ArenaLayout layout, u32 page_size)
        : heap_(heap), layout_(std::move(layout)), page_size_(page_size) {}
    void initialize();
    [[nodiscard]] GuestAddr allocate(u32 size, u8 tag, u16 thread);
    void release(GuestAddr pointer);

private:
    RegionHeap& heap_;
    ArenaLayout layout_;
    u32 page_size_;
    u32 unavailable_ = 0;
};

} // namespace clickhle::classic

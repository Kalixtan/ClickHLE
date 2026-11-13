#include "clickhle/hle/classic_memory.h"
#include "clickhle/core/address_space.h"

#include <stdexcept>

int main() {
    using namespace clickhle;
    AddressSpace memory;
    if (!memory.map(0x10000000, 0x100000, AddressSpace::kRW, "heap fixture")) return 1;
    std::array<std::byte, classic::RegionHeap::control_size> dirty;
    dirty.fill(std::byte{0xa5});
    if (!memory.write_bytes(0x10001000, dirty.data(), static_cast<u32>(dirty.size()))) return 1;
    classic::RegionHeap heap(memory, 0x10001000);
    heap.initialize();
    heap.add_region(0x10010000, 0x10000);
    const auto read = [&](GuestAddr address) { return memory.read<u32>(address).value(); };
    if (read(0x100010b4) != 0xa5a50000 || read(0x100010b8) != 0xa5a5a5a5 ||
        read(0x100010c0) != 0xa5a5a500 || read(0x100010cc) != 0xa5a5a500) return 1;
    if (read(0x10001000) != 65520 || read(0x10001010) != 0x10010008 ||
        read(0x10001014) != 65520 || read(0x1001fff8) != 2) return 1;

    // Captured from 7G ARM: zero requests still allocate a sixteen-byte block.
    const auto zero = heap.allocate(0, 1, 1);
    if (zero != 0x1001fff0 || read(zero - 8) != 0x12 ||
        read(zero - 4) != 0x10001) return 1;
    heap.release(zero);

    const auto a = heap.allocate(64, 1, 1);
    const auto b = heap.allocate(128, 2, 1);
    const auto c = heap.allocate(64, 3, 1);
    const auto d = heap.allocate(256, 4, 1);
    heap.release(b);
    heap.release(d);
    const auto selected = heap.allocate(100, 5, 1);
    if (selected != 0x1001ff48 || read(selected - 8) != 0x72 ||
        read(selected - 4) != 0x10005) return 1;
    if (!memory.write<u32>(a, 0x12345678)) return 1;
    if (heap.allocate(0x10000, 1, 1) != 0 || read(a) != 0x12345678) return 1;
    heap.release(a);
    heap.release(c);
    heap.release(selected);
    heap.release(0);
    if (read(0x10001000) != 65520 || read(0x10001008) != 0 ||
        read(0x10001390) != 0 || read(0x10010008) != 65521) return 1;

    // Region additions must participate in the same best-fit list.
    heap.add_region(0x10030000, 0x10000);
    if (heap.allocate(16, 1, 1) != 0x1003ffe8 || read(0x100010b0) != 2) return 1;
    try {
        (void)heap.allocate(8, 58, 1);
        return 1;
    } catch (const std::runtime_error&) {}
    AddressSpace m;
    if (!m.map(0x40000000, 0x50000, AddressSpace::kRW, "arena fixture")) return 1;
    classic::RegionHeap h(m, 0x40000000);
    classic::ArenaLayout layout{{0x40020000, 0x40030000}, 2, {{0x40010000, 0x10000}}};
    classic::Arena arena(h, layout, 0x10000);
    arena.initialize();
    auto first = arena.allocate(0xfff0, 1, 1);
    auto second = arena.allocate(0x10000, 1, 1);
    if (first != 0x40020000 || second != 0x40030000) return 1;
    arena.release(first);
    if (arena.allocate(0xfff0, 1, 1) != first || arena.allocate(0x10001, 1, 1) != 0) return 1;
    arena.release(first);
    if (arena.allocate(0xff00, 1, 1) != 0x400100f8 || arena.allocate(0xff00, 1, 1) != 0x400200f8) return 1;

    for (const auto request : {0xffe7u, 0xffe8u, 0xffefu, 0xfff0u}) {
        AddressSpace boundary_memory;
        if (!boundary_memory.map(0x50000000, 0x50000, AddressSpace::kRW, "boundary")) return 1;
        classic::RegionHeap boundary_heap(boundary_memory, 0x50000000);
        boundary_heap.initialize(); boundary_heap.add_region(0x50010000, 0x10000);
        classic::ArenaLayout boundary_layout{{0x50030000, 0x50040000}, 2, {}};
        classic::Arena boundary(boundary_heap, boundary_layout, 0x10000);
        const auto actual = boundary.allocate(request, 1, 1);
        const auto expected = request == 0xffef ? 0u : request == 0xfff0 ? 0x50030000u : 0x50010010u;
        if (actual != expected) return 1;
    }

    AddressSpace page_memory;
    if (!page_memory.map(0x60000000, 0x200000, AddressSpace::kRW, "512 KiB pages")) return 1;
    classic::RegionHeap page_heap(page_memory, 0x60000000);
    classic::ArenaLayout page_layout{{0x60100000, 0x60180000}, 2, {{0x60080000, 0x80000}}};
    classic::Arena page_arena(page_heap, page_layout, 0x80000);
    page_arena.initialize();
    auto near_page = page_arena.allocate(0x7ff80, 1, 1);
    if (near_page != 0x60080078) return 1;
    page_arena.release(near_page);

}

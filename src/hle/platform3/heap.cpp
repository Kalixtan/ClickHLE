#include "clickhle/hle/classic_memory.h"
#include "clickhle/core/address_space.h"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace clickhle::classic {
namespace {
constexpr u32 size_mask = 0x3ffffffc;
constexpr u32 free_flag = 1;
constexpr u32 previous_free = 2;
u32 bucket(u32 size) { return static_cast<u32>(std::bit_width(size)) - 1u; }
}

u32 RegionHeap::read(GuestAddr address) const {
    auto value = memory_.read<u32>(address);
    if (!value) throw std::runtime_error("Classic heap: unmapped metadata read");
    return *value;
}

void RegionHeap::write(GuestAddr address, u32 value) {
    if (!memory_.write<u32>(address, value))
        throw std::runtime_error("Classic heap: unmapped metadata write");
}

void RegionHeap::adjust(u32 offset, u32 amount) {
    write(control_ + offset, read(control_ + offset) + amount);
}

void RegionHeap::initialize() {
    if (!memory_.host_ptr(control_, control_size))
        throw std::runtime_error("Classic heap: unmapped control block");
    // 0x081926cc leaves padding and dormant lock storage untouched.
    for (u32 offset = 0; offset <= 0xb0; offset += 4) write(control_ + offset, 0);
    for (u32 offset : {0xb4u, 0xb5u, 0xc0u, 0xccu}) {
        if (!memory_.write<u8>(control_ + offset, 0))
            throw std::runtime_error("Classic heap: unmapped control flag");
    }
    write(control_ + 0xc4, 0);
    write(control_ + 0xc8, 0);
    for (u32 offset = 0xd0; offset < control_size; offset += 4) write(control_ + offset, 0);
}

void RegionHeap::unlink(GuestAddr block) {
    const auto next = read(block + 4);
    const auto previous = read(block + 8);
    if (previous) write(previous + 4, next);
    if (next) write(next + 8, previous);
}

void RegionHeap::insert_free(GuestAddr block) {
    // 0x0819243c: merge next, then previous, preserving the old lower flags.
    auto size = read(block) & size_mask;
    auto next = block + size;
    if (read(next) & free_flag) {
        const auto next_size = read(next) & size_mask;
        adjust(0, 0u - next_size);
        write(block, read(block) + next_size);
        unlink(next);
    }
    if (read(block) & previous_free) {
        const auto previous_size = read(block - 4);
        const auto previous = block - previous_size;
        if (!(read(previous) & free_flag) || (read(previous) & size_mask) != previous_size)
            throw std::runtime_error("Classic heap: corrupt previous free block");
        adjust(0, 0u - previous_size);
        write(previous, read(previous) + (read(block) & size_mask));
        unlink(previous);
        block = previous;
    }
    write(block, read(block) | free_flag);
    size = read(block) & size_mask;
    next = block + size;
    write(next, read(next) | previous_free);
    write(next - 4, size);

    // Equal sizes insert before existing blocks, as in 0x08192568.
    auto previous = control_ + 0xd0;
    auto cursor = read(previous + 4);
    while (cursor && (read(cursor) & size_mask) < size) {
        previous = cursor;
        cursor = read(cursor + 4);
    }
    write(block + 4, cursor);
    write(block + 8, previous);
    write(previous + 4, block);
    if (cursor) write(cursor + 8, block);
    adjust(0, size);
}

void RegionHeap::add_region(GuestAddr base, u32 size) {
    const auto count = read(control_ + 0xb0);
    if (count >= 20) return; // Firmware region table has twenty entries.
    if (size < 32 || base > 0xffffffffu - size || !memory_.host_ptr(base, size))
        throw std::runtime_error("Classic heap: invalid region");
    const auto header = (base + 15u) & ~7u;
    const auto usable = (size - (header + 4u - base) - 4u) & ~7u;
    write(header - 4, 0);
    write(header + usable, 0);
    write(control_ + 0x10 + count * 8, header);
    write(control_ + 0x14 + count * 8, usable);
    write(header, usable);
    write(header + 4, 0);
    write(header + 8, 0);
    adjust(4, usable);
    insert_free(header);
    write(control_ + 0xb0, count + 1);
}

GuestAddr RegionHeap::allocate(u32 requested, u8 tag, u16 thread) {
    if (tag >= 58 || thread >= 80)
        throw std::runtime_error("Classic heap: accounting index outside verified range");
    const u32 needed = (std::max(16u, ((requested + 3u) & size_mask) + 8u) + 7u) & ~7u;
    auto block = read(control_ + 0xd4);
    while (block && (read(block) & size_mask) < needed) block = read(block + 4);
    if (!block) return 0;
    auto size = read(block) & size_mask;
    write(block + size, read(block + size) & ~previous_free);
    unlink(block);
    write(block, read(block) & ~free_flag);
    adjust(0, 0u - size);
    if (size > needed + 16u) {
        const auto allocated = block + size - needed;
        write(block, read(block) - needed);
        write(allocated, needed);
        insert_free(block);
        block = allocated;
        size = needed;
    }
    write(block + 4, static_cast<u32>(tag) | (static_cast<u32>(thread) << 16));
    adjust(0x224 + static_cast<u32>(tag) * 4, size);
    adjust(0x30c, size);
    adjust(0xe0 + static_cast<u32>(thread) * 4, size);
    adjust(0x220, size);
    adjust(0x310 + bucket(size) * 4, 1);
    write(control_ + 0x394, std::max(read(control_ + 0x394), read(control_ + 0x30c)));
    adjust(0x390, 1);
    adjust(8, size);
    adjust(0xc, 1);
    return block + 8;
}

void RegionHeap::release(GuestAddr pointer) {
    if (!pointer) return;
    const auto block = pointer - 8;
    const auto flags = read(block);
    if (!flags || (flags & free_flag))
        throw std::runtime_error("Classic heap: invalid free");
    const auto size = flags & size_mask;
    const auto owner = read(block + 4);
    const u32 tag = owner & 0xffffu;
    const u32 thread = owner >> 16;
    if (tag >= 58 || thread >= 80)
        throw std::runtime_error("Classic heap: corrupt allocation owner");
    adjust(0x224 + tag * 4, 0u - size);
    adjust(0x30c, 0u - size);
    adjust(0xe0 + thread * 4, 0u - size);
    adjust(0x220, 0u - size);
    adjust(0x310 + bucket(size) * 4, 0u - 1u);
    adjust(0x390, 0u - 1u);
    adjust(8, 0u - size);
    insert_free(block);
}

void Arena::initialize() {
    heap_.initialize();
    unavailable_ = 0;
    for (const auto& region : layout_.regions) heap_.add_region(region.base, region.size);
}

GuestAddr Arena::allocate(u32 size, u8 tag, u16 thread) {
    if (size > page_size_) return 0;
    if (size >= page_size_ - 16u) {
        for (u32 i = 0; i < layout_.reserve_count; ++i) {
            if (!(unavailable_ & (1u << i))) {
                unavailable_ |= 1u << i;
                return layout_.reserves[i];
            }
        }
    }
    auto pointer = heap_.allocate(size, tag, thread);
    for (u32 i = 0; !pointer && i < layout_.reserve_count; ++i) {
        if (!(unavailable_ & (1u << i))) {
            unavailable_ |= 1u << i;
            heap_.add_region(layout_.reserves[i], page_size_);
            pointer = heap_.allocate(size, tag, thread);
        }
    }
    return pointer;
}

void Arena::release(GuestAddr pointer) {
    for (u32 i = 0; i < layout_.reserve_count; ++i) {
        if (pointer == layout_.reserves[i]) {
            unavailable_ &= ~(1u << i);
            return;
        }
    }
    heap_.release(pointer);
}

} // namespace clickhle::classic

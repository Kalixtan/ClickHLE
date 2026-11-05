#include "clickhle/hle/guest_heap.h"

#include "clickhle/common/log.h"

namespace clickhle {
namespace {

constexpr std::string_view kCategory = "heap";

u32 round_up(u32 value, u32 multiple) {
    const u32 remainder = value % multiple;
    return remainder == 0 ? value : value + (multiple - remainder);
}

}  // namespace

void GuestHeap::initialise(GuestAddr base, u32 size) {
    base_ = base;
    size_ = size;
    in_use_ = 0;
    blocks_.clear();
    if (size > 0) {
        blocks_[base] = Block{size, false};
    }
    CLICKHLE_DEBUG(kCategory) << "arena at 0x" << std::hex << base << std::dec
                            << ", " << size << " bytes";
}

GuestAddr GuestHeap::allocate(u32 size) {
    if (size == 0) {
        // malloc(0) may return either null or a unique pointer. Returning a
        // real one-byte block is friendlier: guest code that frees the result
        // unconditionally then works either way.
        size = 1;
    }
    const u32 wanted = round_up(size, kAlignment);

    for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
        Block& block = it->second;
        if (block.used || block.size < wanted) {
            continue;
        }
        const GuestAddr address = it->first;
        const u32 remainder = block.size - wanted;
        // Only split when the tail can hold a useful allocation; otherwise the
        // few spare bytes go to the caller and the arena stays unfragmented.
        if (remainder >= kAlignment * 2) {
            block.size = wanted;
            blocks_[address + wanted] = Block{remainder, false};
        }
        block.used = true;
        in_use_ += block.size;
        return address;
    }

    CLICKHLE_WARN(kCategory) << "out of memory allocating " << size << " bytes ("
                           << in_use_ << " of " << size_ << " in use)";
    return kNullGuest;
}

bool GuestHeap::release(GuestAddr address) {
    if (address == kNullGuest) {
        return true;
    }
    const auto it = blocks_.find(address);
    if (it == blocks_.end() || !it->second.used) {
        CLICKHLE_WARN(kCategory)
            << "free of a pointer that is not a live allocation: 0x" << std::hex
            << address;
        return false;
    }
    it->second.used = false;
    in_use_ -= it->second.size;
    coalesce_from(address);
    return true;
}

void GuestHeap::coalesce_from(GuestAddr address) {
    auto it = blocks_.find(address);
    if (it == blocks_.end()) {
        return;
    }
    // Merge backwards first so the surviving block is the earliest one, then
    // forwards from there.
    if (it != blocks_.begin()) {
        auto previous = std::prev(it);
        if (!previous->second.used &&
            previous->first + previous->second.size == it->first) {
            previous->second.size += it->second.size;
            blocks_.erase(it);
            it = previous;
        }
    }
    auto next = std::next(it);
    while (next != blocks_.end() && !next->second.used &&
           it->first + it->second.size == next->first) {
        it->second.size += next->second.size;
        next = blocks_.erase(next);
    }
}

std::optional<u32> GuestHeap::size_of(GuestAddr address) const {
    const auto it = blocks_.find(address);
    if (it == blocks_.end() || !it->second.used) {
        return std::nullopt;
    }
    return it->second.size;
}

std::size_t GuestHeap::live_allocations() const {
    std::size_t count = 0;
    for (const auto& [address, block] : blocks_) {
        (void)address;
        if (block.used) {
            ++count;
        }
    }
    return count;
}

}  // namespace clickhle

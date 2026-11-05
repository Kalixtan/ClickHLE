#pragma once

#include <map>
#include <utility>
#include <vector>
#include <optional>

#include "clickhle/common/types.h"

namespace clickhle {

/// The guest's malloc arena.
///
/// This cannot forward to the host allocator. Guest code stores the pointers it
/// gets back inside guest structures, does arithmetic on them, and passes them
/// to functions we implement -- so they must be real addresses inside the guest
/// address space, not host pointers narrowed to 32 bits.
///
/// First-fit with coalescing. Bookkeeping lives host-side rather than in guest
/// memory: a game that scribbles past the end of an allocation then corrupts our
/// metadata would produce a bug that looks like anything but the buffer overrun
/// it is. Keeping it out of reach makes that failure mode diagnosable.
class GuestHeap {
public:
    /// Alignment of every allocation. Eight bytes matches what the ARM ABI
    /// expects of malloc, which matters because guest code will happily store a
    /// double at offset zero.
    static constexpr u32 kAlignment = 8;

    void initialise(GuestAddr base, u32 size);

    /// Returns 0 when out of memory, which is what malloc does; guest code is
    /// entitled to check for null and carry on.
    [[nodiscard]] GuestAddr allocate(u32 size);

    /// False if `address` was not a live allocation. Freeing 0 is a no-op and
    /// succeeds, per the C standard.
    bool release(GuestAddr address);

    /// Usable size of a live allocation, or nullopt if it is not one.
    [[nodiscard]] std::optional<u32> size_of(GuestAddr address) const;

    [[nodiscard]] u32 bytes_in_use() const noexcept { return in_use_; }
    [[nodiscard]] u32 capacity() const noexcept { return size_; }
    [[nodiscard]] GuestAddr base() const noexcept { return base_; }

    /// Live allocation count; a leak check for tests and diagnostics.
    [[nodiscard]] std::size_t live_allocations() const;

    /// The block list, for save states. Ordered by address.
    [[nodiscard]] std::vector<std::pair<GuestAddr, std::pair<u32, bool>>>
    snapshot() const {
        std::vector<std::pair<GuestAddr, std::pair<u32, bool>>> out;
        out.reserve(blocks_.size());
        for (const auto& [address, block] : blocks_) {
            out.emplace_back(address, std::make_pair(block.size, block.used));
        }
        return out;
    }

    /// Replaces the block list wholesale. The arena base and size come from
    /// initialise(); only the partitioning is restored.
    void restore(
        const std::vector<std::pair<GuestAddr, std::pair<u32, bool>>>& blocks) {
        blocks_.clear();
        in_use_ = 0;
        for (const auto& [address, block] : blocks) {
            blocks_[address] = Block{block.first, block.second};
            if (block.second) {
                in_use_ += block.first;
            }
        }
    }

private:
    struct Block {
        u32 size = 0;
        bool used = false;
    };

    void coalesce_from(GuestAddr address);

    std::map<GuestAddr, Block> blocks_;
    GuestAddr base_ = 0;
    u32 size_ = 0;
    u32 in_use_ = 0;
};

}  // namespace clickhle

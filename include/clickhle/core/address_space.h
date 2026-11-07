#include <cstdio>
#include <cstdlib>
#pragma once

#include <memory>
#include <optional>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "clickhle/common/endian.h"
#include "clickhle/common/types.h"

namespace clickhle {

/// The guest's flat 32-bit address space.
///
/// There is no MMU and no address translation. The PP502x's ARM7TDMI has
/// neither, and we run games as userland clients of a firmware library under
/// HLE, so guest addresses are used directly. The image loads flat at
/// 0x18000000, as the firmware loads it.
class AddressSpace {
public:
    enum Prot : u32 {
        kNone  = 0,
        kRead  = 1u << 0,
        kWrite = 1u << 1,
        kExec  = 1u << 2,
        kRW    = kRead | kWrite,
        kRX    = kRead | kExec,
    };

    struct Region {
        GuestAddr base = 0;
        u32 size = 0;
        u32 prot = kNone;
        std::string name;
        std::vector<std::byte> data;

        /// When set, this region has no storage of its own: every access is
        /// redirected to `mirror_source + (addr - base)`. See map_mirror().
        bool is_mirror = false;
        GuestAddr mirror_source = 0;

        [[nodiscard]] bool contains(GuestAddr addr) const noexcept {
            return addr >= base && (addr - base) < size;
        }
    };

    AddressSpace() = default;
    AddressSpace(const AddressSpace&) = delete;
    AddressSpace& operator=(const AddressSpace&) = delete;
    AddressSpace(AddressSpace&&) = default;
    AddressSpace& operator=(AddressSpace&&) = default;

    /// Maps a zero-filled region. Fails on overlap, on zero size, or if the
    /// region would wrap past the end of the 32-bit space.
    bool map(GuestAddr base, u32 size, u32 prot, std::string name);

    /// Maps `size` bytes at `base` that alias memory already mapped at
    /// `source`, rather than having storage of their own.
    ///
    /// PP502x needs this: SDRAM answers at both 0x00000000 and 0x10000000, and
    /// the firmware uses both. Its game-library table is reached through
    /// low-address pointers while its code runs from the high alias, so
    /// modelling them as two independent regions makes a write through one
    /// invisible to the other -- which reads as data corruption a long way from
    /// the cause.
    ///
    /// The source range must already be mapped, and mirrors do not nest.
    bool map_mirror(GuestAddr base, u32 size, GuestAddr source,
                    std::string name);

    [[nodiscard]] const Region* find(GuestAddr addr) const noexcept;
    [[nodiscard]] const std::vector<std::unique_ptr<Region>>& regions()
        const noexcept {
        return regions_;
    }

    /// Returns a host pointer to `length` bytes at `addr`, or nullptr if that
    /// range is not entirely inside one mapped region. Valid until the next
    /// map() call. Use for bulk copies; prefer the typed helpers otherwise.
    [[nodiscard]] std::byte* host_ptr(GuestAddr addr, u32 length) noexcept;
    [[nodiscard]] const std::byte* host_ptr(GuestAddr addr,
                                            u32 length) const noexcept;

    [[nodiscard]] bool read_bytes(GuestAddr addr, void* dst, u32 length) const;
    [[nodiscard]] bool write_bytes(GuestAddr addr, const void* src, u32 length);

    template <typename T>
    [[nodiscard]] std::optional<T> read(GuestAddr addr) const {
        static_assert(std::is_integral_v<T>);
        const std::byte* p = host_ptr(addr, sizeof(T));
        if (p == nullptr) {
            return std::nullopt;
        }
        const T value = endian::load_le<T>(p);
        note_read(addr, sizeof(T), static_cast<u64>(value));
        return value;
    }

    template <typename T>
    [[nodiscard]] bool write(GuestAddr addr, T value) {
        static_assert(std::is_integral_v<T>);
        std::byte* p = host_ptr(addr, sizeof(T));
        if (p == nullptr) {
            return false;
        }
        endian::store_le<T>(p, value);
        note_write(addr, sizeof(T), static_cast<u64>(value));
        return true;
    }

    /// Watch an address: every write covering it is reported to `on_watch`.
    ///
    /// The cheap half of a debugger, and the half that earns its keep most
    /// often. A guest structure going null is nearly always some earlier write
    /// putting the wrong thing there, and the register dump at the eventual
    /// fault is a long way from whoever did it.
    void watch(GuestAddr address) { watches_.push_back(address); }
    void set_watch_handler(std::function<void(GuestAddr, u32, u64)> handler) {
        on_watch_ = std::move(handler);
    }
    [[nodiscard]] bool watching() const noexcept { return !watches_.empty(); }
    void note_write(GuestAddr addr, u32 size, u64 value) const {
        if (watches_.empty() || !on_watch_) {
            return;
        }
        for (const GuestAddr w : watches_) {
            if (w >= addr && w < addr + size) {
                on_watch_(addr, size, value);
                return;
            }
        }
    }

    /// Watch an address for *reads*, the mirror of watch().
    ///
    /// Writes answer "who put this here?". Reads answer "what is this code
    /// looking at?", which is the question when a guest is polling a structure
    /// whose layout is unknown -- the offsets it reads while waiting are the
    /// fields that matter, and nothing else reveals them.
    void watch_read(GuestAddr address) { read_watches_.push_back(address); }
    void set_read_watch_handler(
        std::function<void(GuestAddr, u32, u64)> handler) {
        on_read_watch_ = std::move(handler);
    }
    [[nodiscard]] bool watching_reads() const noexcept {
        return !read_watches_.empty() && static_cast<bool>(on_read_watch_);
    }
    [[nodiscard]] bool watched_read_overlaps(GuestAddr addr, u32 size) const noexcept {
        for (const GuestAddr watched : read_watches_) {
            if (watched >= addr && watched - addr < size) {
                return true;
            }
        }
        return false;
    }
    void note_read(GuestAddr addr, u32 size, u64 value) const {
        if (read_watches_.empty() || !on_read_watch_) {
            return;
        }
        for (const GuestAddr w : read_watches_) {
            if (w >= addr && w < addr + size) {
                on_read_watch_(addr, size, value);
                return;
            }
        }
    }

    /// Reads a NUL-terminated single-byte string. Stops at `max_length` so a
    /// corrupt pointer cannot walk the whole address space.
    [[nodiscard]] std::optional<std::string> read_cstring(
        GuestAddr addr, u32 max_length = 4096) const;

    /// Reads a NUL-terminated UTF-16LE string and transcodes to UTF-8.
    /// Windows CE is a wide-character OS: every API the games use is the `W`
    /// variant, so this is the common case, not the exotic one.
    [[nodiscard]] std::optional<std::string> read_wstring(
        GuestAddr addr, u32 max_length = 4096) const;

    /// Total bytes currently mapped, for diagnostics.
    [[nodiscard]] u64 mapped_bytes() const noexcept;

private:
    std::vector<GuestAddr> watches_;
    std::function<void(GuestAddr, u32, u64)> on_watch_;
    std::vector<GuestAddr> read_watches_;
    std::function<void(GuestAddr, u32, u64)> on_read_watch_;

    Region* find_mutable(GuestAddr addr) noexcept;

    // unique_ptr so that adding a region never invalidates outstanding
    // host_ptr() results into the regions already mapped.
    std::vector<std::unique_ptr<Region>> regions_;
};

}  // namespace clickhle

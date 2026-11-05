#include "clickhle/core/address_space.h"

#include <algorithm>
#include <cstring>

#include "clickhle/common/log.h"

namespace clickhle {
namespace {

constexpr std::string_view kCategory = "mem";

/// Appends one UTF-16 code point to a UTF-8 string. Surrogate pairs are joined
/// by the caller; unpaired surrogates are emitted as U+FFFD rather than
/// producing invalid UTF-8.
void append_utf8(std::string& out, u32 code_point) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    }
}

}  // namespace

bool AddressSpace::map(GuestAddr base, u32 size, u32 prot, std::string name) {
    if (size == 0) {
        CLICKHLE_ERROR(kCategory) << "refusing zero-sized mapping '" << name << "'";
        return false;
    }
    // Guard against wrapping past 0xFFFFFFFF.
    if (base > 0xFFFFFFFFu - (size - 1)) {
        CLICKHLE_ERROR(kCategory)
            << "mapping '" << name << "' would wrap the address space";
        return false;
    }
    const u64 new_begin = base;
    const u64 new_end = static_cast<u64>(base) + size;
    for (const auto& region : regions_) {
        const u64 begin = region->base;
        const u64 end = static_cast<u64>(region->base) + region->size;
        if (new_begin < end && begin < new_end) {
            CLICKHLE_ERROR(kCategory)
                << "mapping '" << name << "' overlaps existing '"
                << region->name << "'";
            return false;
        }
    }

    auto region = std::make_unique<Region>();
    region->base = base;
    region->size = size;
    region->prot = prot;
    region->name = std::move(name);
    region->data.assign(size, std::byte{0});

    CLICKHLE_DEBUG(kCategory)
        << "mapped '" << region->name << "' " << size << " bytes";
    regions_.push_back(std::move(region));

    std::sort(regions_.begin(), regions_.end(),
              [](const auto& a, const auto& b) { return a->base < b->base; });
    return true;
}

bool AddressSpace::map_mirror(GuestAddr base, u32 size, GuestAddr source,
                              std::string name) {
    if (size == 0) {
        return false;
    }
    const Region* target = find(source);
    if (target == nullptr || target->is_mirror) {
        CLICKHLE_ERROR(kCategory)
            << "mirror source 0x" << std::hex << source
            << " is unmapped or is itself a mirror" << std::dec;
        return false;
    }
    if (!map(base, size, target->prot, std::move(name))) {
        return false;
    }
    Region* region = find_mutable(base);
    region->is_mirror = true;
    region->mirror_source = source;
    // A mirror holds no storage; releasing it is the whole point.
    region->data.clear();
    region->data.shrink_to_fit();
    return true;
}

const AddressSpace::Region* AddressSpace::find(GuestAddr addr) const noexcept {
    for (const auto& region : regions_) {
        if (region->contains(addr)) {
            return region.get();
        }
    }
    return nullptr;
}

AddressSpace::Region* AddressSpace::find_mutable(GuestAddr addr) noexcept {
    for (auto& region : regions_) {
        if (region->contains(addr)) {
            return region.get();
        }
    }
    return nullptr;
}

std::byte* AddressSpace::host_ptr(GuestAddr addr, u32 length) noexcept {
    if (length == 0) {
        return nullptr;
    }
    Region* region = find_mutable(addr);
    if (region == nullptr) {
        return nullptr;
    }
    if (region->is_mirror) {
        // One hop only -- map_mirror() refuses to alias an alias, so this
        // cannot loop.
        const u32 offset = addr - region->base;
        if (length > region->size - offset) {
            return nullptr;
        }
        return host_ptr(region->mirror_source + offset, length);
    }
    const u32 offset = addr - region->base;
    if (length > region->size - offset) {
        return nullptr;  // straddles the end of the region
    }
    return region->data.data() + offset;
}

const std::byte* AddressSpace::host_ptr(GuestAddr addr,
                                        u32 length) const noexcept {
    return const_cast<AddressSpace*>(this)->host_ptr(addr, length);
}

bool AddressSpace::read_bytes(GuestAddr addr, void* dst, u32 length) const {
    if (length == 0) {
        return true;
    }
    const std::byte* src = host_ptr(addr, length);
    if (src == nullptr) {
        return false;
    }
    std::memcpy(dst, src, length);
    return true;
}

bool AddressSpace::write_bytes(GuestAddr addr, const void* src, u32 length) {
    if (length == 0) {
        return true;
    }
    std::byte* dst = host_ptr(addr, length);
    if (dst == nullptr) {
        return false;
    }
    std::memcpy(dst, src, length);
    note_write(addr, length, 0);
    return true;
}

std::optional<std::string> AddressSpace::read_cstring(GuestAddr addr,
                                                      u32 max_length) const {
    std::string out;
    for (u32 i = 0; i < max_length; ++i) {
        const auto ch = read<u8>(addr + i);
        if (!ch.has_value()) {
            return std::nullopt;
        }
        if (*ch == 0) {
            return out;
        }
        out.push_back(static_cast<char>(*ch));
    }
    return out;  // truncated at max_length rather than failing
}

std::optional<std::string> AddressSpace::read_wstring(GuestAddr addr,
                                                      u32 max_length) const {
    std::string out;
    for (u32 i = 0; i < max_length; ++i) {
        const auto unit = read<u16>(addr + i * 2);
        if (!unit.has_value()) {
            return std::nullopt;
        }
        if (*unit == 0) {
            return out;
        }
        u32 code_point = *unit;
        if (code_point >= 0xD800 && code_point <= 0xDBFF) {
            const auto low = read<u16>(addr + (i + 1) * 2);
            if (low.has_value() && *low >= 0xDC00 && *low <= 0xDFFF) {
                code_point = 0x10000u + ((code_point - 0xD800u) << 10) +
                             (*low - 0xDC00u);
                ++i;
            } else {
                code_point = 0xFFFD;
            }
        } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
            code_point = 0xFFFD;
        }
        append_utf8(out, code_point);
    }
    return out;
}

u64 AddressSpace::mapped_bytes() const noexcept {
    u64 total = 0;
    for (const auto& region : regions_) {
        total += region->size;
    }
    return total;
}

}  // namespace clickhle

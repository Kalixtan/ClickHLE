#pragma once

#include <cstring>
#include <span>
#include <type_traits>

#include "clickhle/common/types.h"

/// Explicit little-endian accessors.
///
/// The guest (ARM920T, and every PE file we touch) is little-endian. The host
/// might not be. Everything that crosses that boundary goes through these, and
/// nothing anywhere casts a packed struct over a byte buffer -- that would be
/// both endian-dependent and undefined behaviour on alignment grounds.
///
/// These compile down to a single load on any sane little-endian target.
namespace clickhle::endian {

template <typename T>
[[nodiscard]] T load_le(const std::byte* p) noexcept {
    static_assert(std::is_integral_v<T>, "load_le is for integral types");
    using U = std::make_unsigned_t<T>;
    U v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        v |= static_cast<U>(static_cast<U>(std::to_integer<u8>(p[i]))
                            << (8u * i));
    }
    return static_cast<T>(v);
}

template <typename T>
void store_le(std::byte* p, T value) noexcept {
    static_assert(std::is_integral_v<T>, "store_le is for integral types");
    using U = std::make_unsigned_t<T>;
    const auto v = static_cast<U>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        p[i] = static_cast<std::byte>((v >> (8u * i)) & 0xFFu);
    }
}

/// Bounds-checked read from a byte span. Returns false if `offset` + sizeof(T)
/// would run past the end, which is the common case when parsing a truncated
/// or hostile PE file.
template <typename T>
[[nodiscard]] bool read_le(std::span<const std::byte> data, std::size_t offset,
                           T& out) noexcept {
    if (offset > data.size() || data.size() - offset < sizeof(T)) {
        return false;
    }
    out = load_le<T>(data.data() + offset);
    return true;
}

/// Reinterpret the bit pattern of a 32-bit word as a float, and back.
/// Uses memcpy, which is the only well-defined way to do this in C++.
[[nodiscard]] inline float bits_to_float(u32 bits) noexcept {
    static_assert(sizeof(float) == sizeof(u32));
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

[[nodiscard]] inline u32 float_to_bits(float f) noexcept {
    u32 bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

[[nodiscard]] inline double bits_to_double(u64 bits) noexcept {
    static_assert(sizeof(double) == sizeof(u64));
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}

[[nodiscard]] inline u64 double_to_bits(double d) noexcept {
    u64 bits;
    std::memcpy(&bits, &d, sizeof(bits));
    return bits;
}

}  // namespace clickhle::endian

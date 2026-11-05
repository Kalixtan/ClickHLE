#pragma once

#include <cstddef>
#include <cstdint>

namespace clickhle {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

/// An address in the guest's 32-bit ARM address space.
///
/// Deliberately a distinct 32-bit integer rather than a host pointer: guest
/// pointers are 32-bit regardless of what the host is, and guest code performs
/// arithmetic on them. Never store a host pointer where a GuestAddr belongs.
using GuestAddr = u32;

inline constexpr GuestAddr kNullGuest = 0;

}  // namespace clickhle

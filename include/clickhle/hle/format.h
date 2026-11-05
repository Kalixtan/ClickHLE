#pragma once

#include <string>

#include "clickhle/common/types.h"

namespace clickhle {

class AddressSpace;

/// Where a printf-family implementation gets its variadic arguments.
///
/// Two shapes exist and they are not interchangeable: `sprintf` reads a genuine
/// argument list (r0-r3 then the stack), while `wvsprintfW` receives a `va_list`
/// -- a guest pointer to walk. Same formatter, different source.
class FormatArgs {
public:
    virtual ~FormatArgs() = default;
    virtual u32 next_word() = 0;
    virtual u64 next_double_word() = 0;
};

/// Reads arguments from a guest `va_list` pointer.
class VaListArgs final : public FormatArgs {
public:
    VaListArgs(const AddressSpace& memory, GuestAddr list)
        : memory_(memory), cursor_(list) {}

    u32 next_word() override;
    u64 next_double_word() override;

private:
    const AddressSpace& memory_;
    GuestAddr cursor_;
};

/// Formats a printf-style string, resolving `%s` and `%p` through guest memory.
///
/// `wide` selects what `%s` and `%c` mean: the `W` entry points take UTF-16
/// arguments, the narrow ones take bytes. Windows CE is a wide-character OS, so
/// the wide path is the common case.
///
/// Numeric conversions are delegated to the host's `snprintf` rather than
/// reimplemented, because matching C's rounding, padding and `%g` behaviour by
/// hand is a well-known way to be subtly wrong for years.
[[nodiscard]] std::string format_guest(const AddressSpace& memory,
                                       const std::string& format,
                                       FormatArgs& args, bool wide);

/// Writes `text` into guest memory as a NUL-terminated UTF-16LE string.
/// Returns the number of characters written, excluding the terminator.
u32 write_guest_wstring(AddressSpace& memory, GuestAddr destination,
                        const std::string& text);

/// Writes `text` into guest memory as a NUL-terminated byte string.
u32 write_guest_cstring(AddressSpace& memory, GuestAddr destination,
                        const std::string& text);

}  // namespace clickhle

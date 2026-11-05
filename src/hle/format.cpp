#include "clickhle/hle/format.h"

#include <cctype>
#include <cstdio>
#include <vector>

#include "clickhle/common/endian.h"
#include "clickhle/core/address_space.h"

namespace clickhle {
namespace {

/// Appends one code point to a UTF-8 string.
void append_utf8(std::string& out, u32 code_point) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    }
}

/// Decodes one UTF-8 sequence, appending its code point as UTF-16 code units.
void append_utf16(std::vector<u16>& out, const std::string& text,
                  std::size_t& index) {
    const auto lead = static_cast<unsigned char>(text[index]);
    u32 code_point = lead;
    std::size_t extra = 0;
    if (lead >= 0xF0) {
        code_point = lead & 0x07u;
        extra = 3;
    } else if (lead >= 0xE0) {
        code_point = lead & 0x0Fu;
        extra = 2;
    } else if (lead >= 0xC0) {
        code_point = lead & 0x1Fu;
        extra = 1;
    }
    for (std::size_t i = 0; i < extra && index + 1 < text.size(); ++i) {
        ++index;
        code_point = (code_point << 6) |
                     (static_cast<unsigned char>(text[index]) & 0x3Fu);
    }
    if (code_point >= 0x10000) {
        code_point -= 0x10000;
        out.push_back(static_cast<u16>(0xD800 + (code_point >> 10)));
        out.push_back(static_cast<u16>(0xDC00 + (code_point & 0x3FF)));
    } else {
        out.push_back(static_cast<u16>(code_point));
    }
}

std::vector<u16> to_utf16(const std::string& text) {
    std::vector<u16> out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        append_utf16(out, text, i);
    }
    return out;
}

bool is_flag(char c) {
    return c == '-' || c == '+' || c == ' ' || c == '#' || c == '0';
}

/// Renders one numeric conversion through the host's snprintf.
template <typename T>
std::string render(const std::string& spec, T value) {
    char buffer[256];
    const int written = std::snprintf(buffer, sizeof(buffer), spec.c_str(), value);
    if (written < 0) {
        return {};
    }
    if (static_cast<std::size_t>(written) < sizeof(buffer)) {
        return std::string(buffer, static_cast<std::size_t>(written));
    }
    std::vector<char> large(static_cast<std::size_t>(written) + 1);
    std::snprintf(large.data(), large.size(), spec.c_str(), value);
    return std::string(large.data(), static_cast<std::size_t>(written));
}

}  // namespace

u32 VaListArgs::next_word() {
    const auto value = memory_.read<u32>(cursor_);
    cursor_ += 4;
    return value.value_or(0);
}

u64 VaListArgs::next_double_word() {
    // va_list on ARM aligns 64-bit values to an 8-byte boundary.
    if ((cursor_ & 7u) != 0) {
        cursor_ += 4;
    }
    const u64 low = next_word();
    const u64 high = next_word();
    return low | (high << 32);
}

std::string format_guest(const AddressSpace& memory, const std::string& format,
                         FormatArgs& args, bool wide) {
    std::string out;
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%') {
            out.push_back(format[i]);
            continue;
        }
        if (i + 1 >= format.size()) {
            break;
        }
        if (format[i + 1] == '%') {
            out.push_back('%');
            ++i;
            continue;
        }

        // Rebuild the conversion spec as we scan it, so it can be handed to
        // snprintf verbatim for the numeric cases.
        std::string spec = "%";
        std::size_t j = i + 1;
        while (j < format.size() && is_flag(format[j])) {
            spec.push_back(format[j++]);
        }
        // Width, possibly taken from an argument.
        if (j < format.size() && format[j] == '*') {
            spec += std::to_string(static_cast<i32>(args.next_word()));
            ++j;
        } else {
            while (j < format.size() && std::isdigit(static_cast<unsigned char>(format[j]))) {
                spec.push_back(format[j++]);
            }
        }
        // Precision, likewise.
        if (j < format.size() && format[j] == '.') {
            spec.push_back(format[j++]);
            if (j < format.size() && format[j] == '*') {
                spec += std::to_string(static_cast<i32>(args.next_word()));
                ++j;
            } else {
                while (j < format.size() &&
                       std::isdigit(static_cast<unsigned char>(format[j]))) {
                    spec.push_back(format[j++]);
                }
            }
        }
        // Length modifiers. These affect argument width but never reach
        // snprintf, whose expectations about `l` differ from the guest's.
        bool is_long_long = false;
        bool force_wide = wide;
        bool force_narrow = false;
        while (j < format.size()) {
            const char c = format[j];
            if (c == 'h') {
                ++j;
                force_narrow = true;
            } else if (c == 'l') {
                ++j;
                if (j < format.size() && format[j] == 'l') {
                    ++j;
                    is_long_long = true;
                } else {
                    force_wide = true;
                }
            } else if (c == 'w') {  // Windows: %ws is always wide
                ++j;
                force_wide = true;
            } else if (c == 'L') {
                ++j;
            } else if (c == 'I' && j + 2 < format.size() &&
                       format[j + 1] == '6' && format[j + 2] == '4') {
                j += 3;
                is_long_long = true;
            } else {
                break;
            }
        }
        if (j >= format.size()) {
            break;
        }

        const char conversion = format[j];
        i = j;

        switch (conversion) {
            case 'd':
            case 'i': {
                if (is_long_long) {
                    out += render(spec + "lld",
                                  static_cast<long long>(args.next_double_word()));
                } else {
                    out += render(spec + "d", static_cast<int>(args.next_word()));
                }
                break;
            }
            case 'u':
            case 'o':
            case 'x':
            case 'X': {
                if (is_long_long) {
                    spec += "ll";
                    spec.push_back(conversion);
                    out += render(spec, static_cast<unsigned long long>(
                                            args.next_double_word()));
                } else {
                    spec.push_back(conversion);
                    out += render(spec, static_cast<unsigned int>(args.next_word()));
                }
                break;
            }
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G': {
                spec.push_back(conversion);
                out += render(spec,
                              endian::bits_to_double(args.next_double_word()));
                break;
            }
            case 'c': {
                const u32 value = args.next_word();
                if (force_wide && !force_narrow) {
                    append_utf8(out, value & 0xFFFFu);
                } else {
                    out.push_back(static_cast<char>(value & 0xFFu));
                }
                break;
            }
            case 's': {
                const auto address = static_cast<GuestAddr>(args.next_word());
                std::string text;
                if (address == kNullGuest) {
                    text = "(null)";
                } else if (force_wide && !force_narrow) {
                    text = memory.read_wstring(address).value_or("(bad)");
                } else {
                    text = memory.read_cstring(address).value_or("(bad)");
                }
                // Width and precision still apply, and snprintf can do them.
                spec.push_back('s');
                out += render(spec, text.c_str());
                break;
            }
            case 'p': {
                out += render("%08X", static_cast<unsigned int>(args.next_word()));
                break;
            }
            case 'n': {
                // Deliberately ignored: %n writes through a guest pointer and is
                // a classic exploit primitive. No game needs it.
                (void)args.next_word();
                break;
            }
            default: {
                out.push_back('%');
                out.push_back(conversion);
                break;
            }
        }
    }
    return out;
}

u32 write_guest_wstring(AddressSpace& memory, GuestAddr destination,
                        const std::string& text) {
    const auto units = to_utf16(text);
    for (std::size_t i = 0; i < units.size(); ++i) {
        if (!memory.write<u16>(destination + static_cast<u32>(i) * 2, units[i])) {
            return static_cast<u32>(i);
        }
    }
    static_cast<void>(
        memory.write<u16>(destination + static_cast<u32>(units.size()) * 2, 0));
    return static_cast<u32>(units.size());
}

u32 write_guest_cstring(AddressSpace& memory, GuestAddr destination,
                        const std::string& text) {
    if (!memory.write_bytes(destination, text.c_str(),
                            static_cast<u32>(text.size() + 1))) {
        return 0;
    }
    return static_cast<u32>(text.size());
}

}  // namespace clickhle

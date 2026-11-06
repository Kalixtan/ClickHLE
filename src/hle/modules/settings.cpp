// Handlers for the device's Settings library.

#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include "clickhle/hle/modules.h"

#include <optional>
#include <string>
#include <utility>

#include "clickhle/common/log.h"
#include "clickhle/core/emulator.h"
#include "clickhle/hle/call_frame.h"
#include "clickhle/hle/registry.h"

namespace clickhle {
namespace {

constexpr std::string_view kCategory = "settings";
constexpr const char* kModule = "Settings";

/// Longest setting name we will read out of guest memory. A bound, not a
/// limit that means anything: it stops a bad pointer walking the address space.
constexpr u32 kMaxNameLength = 64;

std::string read_name(HleCallFrame& frame, GuestAddr address) {
    std::string out;
    for (u32 i = 0; i < kMaxNameLength; ++i) {
        const auto c = frame.memory.read<u8>(address + i);
        if (!c.has_value() || *c == 0) {
            break;
        }
        out.push_back(static_cast<char>(*c));
    }
    return out;
}

std::optional<std::string> read_name_strict(HleCallFrame& frame,
                                             GuestAddr address) {
    std::string out;
    for (u32 i = 0; i < kMaxNameLength; ++i) {
        const auto c = frame.memory.read<u8>(address + i);
        if (!c.has_value()) return std::nullopt;
        if (*c == 0) return out;
        out.push_back(static_cast<char>(*c));
    }
    return std::nullopt;
}

}  // namespace

void register_settings(HleRegistry& registry) {
    // Settings#0 dispatches by name. The firmware returns -49 for a null
    // output buffer, -50 when no entry matches, and otherwise returns the
    // selected setting handler's result. The size argument is an in/out
    // pointer: input capacity, then the number of bytes written.
    registry.add(kModule, 0, "get_setting", 3, [](HleCallFrame& f) {
        const GuestAddr name = f.args.next_ptr();
        const GuestAddr out = f.args.next_ptr();
        const GuestAddr size = f.args.next_ptr();
        // CLICKHLE_DUMP_SETTINGS=1 shows the two trailing arguments and what
        // they point at. They are undocumented, and identifying them is the
        // prerequisite for implementing additional preferences.
        if (std::getenv("CLICKHLE_DUMP_SETTINGS") != nullptr) {
            const auto peek = [&f](GuestAddr p) {
                std::string s;
                if (p == kNullGuest) return std::string("<null>");
                char buf[32];
                std::snprintf(buf, sizeof(buf), "0x%x[", p);
                s = buf;
                for (u32 i = 0; i < 8; ++i) {
                    const auto w = f.memory.read<u32>(p + i * 4);
                    std::snprintf(buf, sizeof(buf), "%s0x%x", i ? "," : "",
                                  w.value_or(0xdeadbeef));
                    s += buf;
                }
                return s + "]";
            };
            CLICKHLE_INFO(kCategory)
                << "get_setting(\"" << read_name(f, name)
                << "\") out=" << peek(out) << " size=" << peek(size);
        }
        constexpr u32 kBadOutput = 0xFFFFFFCFu;  // -49
        constexpr u32 kUnknownSetting = 0xFFFFFFCEu;  // -50
        if (out == kNullGuest) {
            f.ret(kBadOutput);
            return;
        }

        const auto key_value = read_name_strict(f, name);
        if (!key_value.has_value()) {
            f.cpu.request_stop();
            return;
        }
        const std::string& key = *key_value;
        if (key != "Language" && key != "TimeFormat") {
            CLICKHLE_DEBUG(kCategory)
                << "get_setting(\"" << key << "\") -> absent";
            f.ret(kUnknownSetting);
            return;
        }
        const auto capacity_value = size != kNullGuest
                                        ? f.memory.read<u32>(size)
                                        : std::optional<u32>(4u);
        if (!capacity_value.has_value()) {
            f.cpu.request_stop();
            return;
        }
        const u32 capacity = *capacity_value;
        if (key == "Language") {
            // The caller treats this as a four-byte language index. Index 0
            // selects English, the default resource set used by the corpus.
            // The comparison emulator reports the handler's required size
            // even when capacity is short, while leaving the output untouched.
            // Do not turn a matched key into the dispatcher's unknown-name
            // error just because the handler cannot write its value.
            if (capacity >= sizeof(u32)) {
                const char* want = std::getenv("CLICKHLE_LANGUAGE");
                const u32 value = want != nullptr
                                      ? static_cast<u32>(std::strtoul(want, nullptr, 0))
                                      : 0u;
                if (!f.store<u32>(out, value)) {
                    f.cpu.request_stop();
                    return;
                }
                CLICKHLE_DEBUG(kCategory)
                    << "get_setting(\"Language\") -> " << value
                    << (value == 0 ? " (English)" : "");
            }
            if (size != kNullGuest) {
                if (!f.store<u32>(size, sizeof(u32))) {
                    f.cpu.request_stop();
                    return;
                }
            }
            f.ret(0);
            return;
        }
        if (key == "TimeFormat") {
            const char* want = std::getenv("CLICKHLE_TIME_FORMAT");
            const char* value = (want != nullptr && std::string(want) == "24")
                                    ? "24"
                                    : "12";
            const u32 length = 3;  // includes the terminating NUL
            const u32 written = std::min(capacity, length);
            for (u32 i = 0; i < written; ++i) {
                if (!f.store<u8>(out + i, static_cast<u8>(value[i]))) {
                    f.cpu.request_stop();
                    return;
                }
            }
            if (written == 0) {
                f.ret(kUnknownSetting);
                return;
            }
            if (size != kNullGuest) {
                if (!f.store<u32>(size, written)) {
                    f.cpu.request_stop();
                    return;
                }
            }
            f.ret(0);
            return;
        }
    });

    // Both exports resolve to the same `bx lr` stub in RetailOS.
    // They have no side effects and preserve the caller's r0 value.
    registry.add(kModule, 1, "setting_noop_1", 0,
                 [](HleCallFrame& f) { f.ret_void(); });
    registry.add(kModule, 2, "setting_noop_2", 0,
                 [](HleCallFrame& f) { f.ret_void(); });
}

}  // namespace clickhle

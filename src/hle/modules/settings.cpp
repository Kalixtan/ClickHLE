// Handlers for the device's Settings library.

#include <cstdlib>
#include <cstdio>
#include "clickhle/hle/modules.h"

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

}  // namespace

void register_settings(HleRegistry& registry) {
    // get(name, ...) -- the two trailing arguments are not yet understood, so
    // they are read and discarded rather than guessed at.
    //
    // Unknown settings are treated as absent. The current implementation
    // supplies the Language preference used by several games.
    registry.add(kModule, 0, "get_setting", 3, [](HleCallFrame& f) {
        const GuestAddr name = f.args.next_ptr();
        const GuestAddr arg2 = f.args.next_ptr();
        const GuestAddr arg3 = f.args.next_ptr();
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
                << "\") arg2=" << peek(arg2) << " arg3=" << peek(arg3);
        }
        // Language is a four-byte index written through arg2. Index 0 selects
        // English, the default resource set used by the included game corpus.
        const std::string key = read_name(f, name);
        if (key == "Language") {
            // Set CLICKHLE_LANGUAGE to select a different language index.
            const char* want = std::getenv("CLICKHLE_LANGUAGE");
            const u32 value = want != nullptr
                                  ? static_cast<u32>(std::strtoul(want, nullptr, 0))
                                  : 0u;
            f.store<u32>(arg2, value);
            if (arg3 != kNullGuest) {
                f.store<u32>(arg3, 4);
            }
            CLICKHLE_DEBUG(kCategory)
                << "get_setting(\"Language\") -> " << value
                << (value == 0 ? " (English)" : "");
            f.ret(1);
            return;
        }
        CLICKHLE_DEBUG(kCategory)
            << "get_setting(\"" << key << "\") -> absent";
        f.ret(1);
    });

    // Settings#1 and #2 are unidentified and deliberately unregistered, so
    // calling one stops the run and names it.
}

}  // namespace clickhle

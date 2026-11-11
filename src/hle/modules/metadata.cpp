// The "Metadata" library: 152 ordinals, the largest after OpenGLES.
//
// The library includes music and artwork services. Most ordinal names and
// object contracts are still being recovered from the firmware.
//
// The emulator has no device music database, so data queries must report an
// empty result while honoring the firmware's object and handle rules.

#include "clickhle/hle/modules.h"

#include <string>
#include <utility>
#include <vector>

#include "clickhle/common/log.h"
#include "clickhle/core/emulator.h"
#include "clickhle/hle/call_frame.h"
#include "clickhle/hle/registry.h"

namespace clickhle {
namespace {

constexpr std::string_view kCategory = "meta";
constexpr const char* kModule = "Metadata";

/// No playlist is selected. Distinct from playlist zero, which would be a real
/// playlist that happens to be first.
constexpr u32 kNoPlaylist = 0xFFFFFFFFu;

u32 allocate_slot(std::vector<bool>& slots) {
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (!slots[i]) {
            slots[i] = true;
            return static_cast<u32>(i);
        }
    }
    if (slots.size() >= kNoPlaylist) {
        return kNoPlaylist;
    }
    slots.push_back(true);
    return static_cast<u32>(slots.size() - 1);
}

void release_slot(std::vector<bool>& slots, u32 handle) {
    if (handle < slots.size()) {
        slots[handle] = false;
    }
}

bool valid_music_handle(HleCallFrame& f, u32 handle) {
    const auto& slots = f.emulator.metadata_state().music_library_slots;
    if (handle < slots.size() && slots[handle]) return true;
    CLICKHLE_ERROR(kCategory) << "invalid Metadata handle " << handle
                              << " (RetailOS lookup returns null)";
    // The wrappers dereference the lookup result without a null check.
    f.cpu.request_stop();
    return false;
}

}  // namespace

void register_metadata(HleRegistry& registry) {
    const auto fn = [&registry](u32 ordinal, std::string name, u8 arity,
                                HleHandler handler) {
        registry.add(kModule, ordinal, std::move(name), arity,
                     std::move(handler));
    };

    // RetailOS inserts a new library object and returns its table index. Index
    // zero is valid even though the represented music library is empty.
    fn(0, "music_library_create", 0, [](HleCallFrame& f) {
        const u32 handle =
            allocate_slot(f.emulator.metadata_state().music_library_slots);
        CLICKHLE_DEBUG(kCategory) << "music library handle " << handle;
        f.ret(handle);
    });

    fn(1, "music_library_release", 1, [](HleCallFrame& f) {
        release_slot(f.emulator.metadata_state().music_library_slots,
                     f.args.next_u32());
        f.ret_void();
    });

    // The second pair manages artwork-library objects in a separate table.
    fn(2, "artwork_library_create", 0, [](HleCallFrame& f) {
        const u32 handle =
            allocate_slot(f.emulator.metadata_state().artwork_library_slots);
        CLICKHLE_DEBUG(kCategory) << "artwork library handle " << handle;
        f.ret(handle);
    });
    fn(3, "artwork_library_release", 1, [](HleCallFrame& f) {
        release_slot(f.emulator.metadata_state().artwork_library_slots,
                     f.args.next_u32());
        f.ret_void();
    });

    // These two exports are literal zero-return stubs in RetailOS.
    fn(121, "metadata_zero_121", 0,
       [](HleCallFrame& f) { f.ret(0); });
    fn(122, "metadata_zero_122", 0,
       [](HleCallFrame& f) { f.ret(0); });

    // These wrappers all resolve argument 0 through the music-library object
    // table before dereferencing the returned object. Their service calls are
    // still incomplete, but invalid and released handles must not look valid.
    fn(29, "metadata_29", 4, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        for (u32 i = 1; i < 4; ++i) (void)f.args.next_u32();
        if (!valid_music_handle(f, handle)) return;
        f.ret_void();
    });
    fn(31, "metadata_31", 4, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        for (u32 i = 1; i < 4; ++i) (void)f.args.next_u32();
        if (!valid_music_handle(f, handle)) return;
        f.ret_void();
    });
    fn(45, "metadata_45", 4, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        for (u32 i = 1; i < 4; ++i) (void)f.args.next_u32();
        if (!valid_music_handle(f, handle)) return;
        f.ret(0);
    });
    fn(47, "metadata_47", 4, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        for (u32 i = 1; i < 4; ++i) (void)f.args.next_u32();
        if (!valid_music_handle(f, handle)) return;
        f.ret_void();
    });

    fn(62, "audio_service", 0, [](HleCallFrame& f) {
        auto& state = f.emulator.metadata_state();
        if (state.service_object == kNullGuest) {
            state.service_object = f.emulator.heap().allocate(0x90);
            if (state.service_object == kNullGuest) {
                CLICKHLE_ERROR(kCategory)
                    << "Metadata#62 could not allocate its 0x90-byte service object";
                // RetailOS immediately runs the constructor with the result;
                // a failed allocation would fault there instead of returning
                // a usable null singleton.
                f.cpu.request_stop();
                return;
            }
            for (u32 offset = 0; offset < 0x90; offset += 4) {
                f.store<u32>(state.service_object + offset, 0);
            }
            // FUN_1019102c installs the pointer loaded from the firmware's
            // vtable literal at 0x10191088.
            f.store<u32>(state.service_object, 0x00668cb4);
            // FUN_1019102c initializes these fields after constructing
            // the service object. The configuration-derived +0x08 value
            // and embedded service at +0x1c depend on runtime globals we
            // do not have, but these zero/default fields are deterministic.
            f.store<u32>(state.service_object + 0x04, 0);
            f.store<u32>(state.service_object + 0x10, 0);
            f.store<u32>(state.service_object + 0x14, kNoPlaylist);
            f.store<u32>(state.service_object + 0x18, 0);
            f.store<u32>(state.service_object + 0x20, 0);
            // FUN_1019102c constructs the nested object at +0x24 through
            // 0x101a3830. Its base constructor installs the second vtable at
            // +0x28, and five empty 16-byte members follow from +0x3c.
            f.store<u32>(state.service_object + 0x24, 0x006698c0);
            f.store<u32>(state.service_object + 0x28, 0x0067315c);
            f.store<u32>(state.service_object + 0x2c, 0);
            for (const u32 offset : {0x3cu, 0x4cu, 0x5cu, 0x6cu, 0x7cu}) {
                f.store<u32>(state.service_object + offset, 0x00664cd0);
                f.store<u32>(state.service_object + offset + 4, 0x00672aa0);
                f.store<u32>(state.service_object + offset + 8, 0);
                f.store<u32>(state.service_object + offset + 0xc, kNoPlaylist);
            }
            f.store<u32>(state.service_object + 0x8c, 0);
        }
        f.ret(state.service_object);
    });
    // Ghidra shows this getter reading the current index at object + 0x14.
    fn(125, "current_index", 1, [](HleCallFrame& f) {
        const GuestAddr object = f.args.next_ptr();
        const auto index = f.memory.read<u32>(object + 0x14);
        if (!index.has_value()) {
            f.cpu.request_stop();
            return;
        }
        f.ret(*index);
    });
    // The firmware getter returns the raw byte at object + 0x18.
    fn(126, "current_state", 1, [](HleCallFrame& f) {
        const GuestAddr object = f.args.next_ptr();
        const auto state = f.memory.read<u8>(object + 0x18);
        if (!state.has_value()) {
            f.cpu.request_stop();
            return;
        }
        f.ret(*state);
    });
    // RetailOS checks the audio context's +0x118 word, then invokes vtable
    // slot +0x0c on the #62 singleton when it is -1. That method cleans the
    // embedded service and calls vtable slot +0x08. The slot target at
    // 0x10190afc loads singleton+4 and returns it normalized to 0/1. The
    // constructor initializes +4 to zero; Audio#40 registrations are the
    // only modeled transition that makes the stream service available.
    fn(134, "has_registered_stream", 0, [](HleCallFrame& f) {
        f.ret(f.emulator.audio_state().registered_stream_count != 0 ? 1u : 0u);
    });

    // The #62 singleton's vtable slot +8 points to 0x10190afc in the 5.5G
    // firmware. That method returns whether object+4 is nonzero. #135 calls
    // the slot indirectly; reading the field also covers the empty service
    // state created above without embedding firmware code or a guest vtable.
    fn(135, "audio_service_active", 1, [](HleCallFrame& f) {
        const GuestAddr object = f.args.next_ptr();
        const auto active = f.memory.read<u32>(object + 4);
        if (!active.has_value()) {
            f.cpu.request_stop();
            return;
        }
        f.ret(*active != 0 ? 1u : 0u);
    });

    // These setters are direct byte stores in the track-state object.
    fn(131, "set_state_flag_1a", 2, [](HleCallFrame& f) {
        const GuestAddr object = f.args.next_ptr();
        const u8 value = static_cast<u8>(f.args.next_u32());
        if (!f.store<u8>(object + 0x1a, value)) {
            f.cpu.request_stop();
            return;
        }
        f.ret_void();
    });
    fn(132, "set_state_flag_19", 2, [](HleCallFrame& f) {
        const GuestAddr object = f.args.next_ptr();
        const u8 value = static_cast<u8>(f.args.next_u32());
        if (!f.store<u8>(object + 0x19, value)) {
            f.cpu.request_stop();
            return;
        }
        f.ret_void();
    });

    // This initializer also performs firmware-specific stream/configuration
    // cleanup. Reproduce the confirmed empty-index state; the media settings
    // and stream object are not present in this emulator.
    fn(119, "track_state_init", 1, [](HleCallFrame& f) {
        const GuestAddr object = f.args.next_ptr();
        // RetailOS immediately passes object+0x1c to its cleanup helper and
        // then accesses fields in the object. It has no null or unmapped
        // pointer guard; silently skipping initialization turns a guest data
        // fault into an apparent success.
        if (object == kNullGuest ||
            !f.memory.read<u32>(object + 0x1c).has_value()) {
            f.cpu.request_stop();
            return;
        }
        // RetailOS releases the old object through its resource-aware
        // destructor, then clears this member. The destructor's nested
        // resource graph is not modeled, but retaining a stale pointer here
        // would still diverge on every subsequent initialization.
        const auto old_object = f.memory.read<u32>(object + 4);
        if (!old_object.has_value()) {
            f.cpu.request_stop();
            return;
        }
        if (*old_object != 0 && !f.store<u32>(object + 4, 0)) {
            f.cpu.request_stop();
            return;
        }
        // RetailOS initializes the current index and state flags before
        // copying device configuration. This emulator has no media settings
        // to copy, so preserve the confirmed empty state.
        if (!f.store<u32>(object + 0x14, kNoPlaylist) ||
            !f.store<u8>(object + 0x18, 0) ||
            !f.store<u8>(object + 0x19, 0) ||
            !f.store<u8>(object + 0x1a, 0)) {
            f.cpu.request_stop();
            return;
        }
        f.ret(0);
    });

    // FUN_10190708 enters its playlist update only when object+4 is set and
    // object+0x1a is clear. The empty #62 service takes the early return.
    fn(123, "playlist_update", 2, [](HleCallFrame& f) {
        const GuestAddr object = f.args.next_ptr();
        (void)f.args.next_u32();
        const auto service = f.memory.read<u32>(object + 4);
        const auto disabled = f.memory.read<u8>(object + 0x1a);
        if (!service.has_value() || !disabled.has_value()) {
            f.cpu.request_stop();
            return;
        }
        if (*service != 0 && *disabled == 0) {
            CLICKHLE_ERROR(kCategory) << "Metadata#123 active playlist update is not implemented";
            f.cpu.request_stop();
            return;
        }
        f.ret_void();
    });

    CLICKHLE_DEBUG(kCategory) << kModule << ": empty library reported";
}

}  // namespace clickhle

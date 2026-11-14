#include "clickhle/hle/modules.h"

#include <algorithm>
#include <array>
#include <vector>

#include "clickhle/core/emulator.h"
#include "clickhle/hle/call_frame.h"
#include "clickhle/hle/registry.h"

namespace clickhle {
namespace {

// These contracts are from the 7G OSOS 2.0.4 export bodies. In
// particular, Metadata#45 is unrelated to the same ordinal on Platform 1.

u32 create_object(HleCallFrame& frame, std::vector<GuestAddr>& objects,
                  GuestAddr vtable) {
    // Both firmware table insertions reject slot zero. It is the null handle,
    // so seed it before searching for the first usable entry.
    if (objects.empty()) objects.push_back(1);
    const GuestAddr object = frame.emulator.heap().allocate(0x48);
    if (object == 0) return 0xffffffffu;

    // The 7G constructors create a 0x48-byte object. Their service pointer
    // comes from OSOS globals that are absent in this game-only HLE; it is
    // therefore null. Container internals remain opaque to the caller.
    std::array<u8, 0x48> bytes{};
    if (!frame.memory.write_bytes(object, bytes.data(), bytes.size()) ||
        !frame.store<u32>(object, vtable) ||
        !frame.store<u32>(object + 0x40, 0)) {
        (void)frame.emulator.heap().release(object);
        frame.cpu.request_stop();
        return 0xffffffffu;
    }

    auto slot = std::find(objects.begin(), objects.end(), GuestAddr{0});
    if (slot == objects.end()) {
        objects.push_back(object);
        return static_cast<u32>(objects.size() - 1);
    }
    *slot = object;
    return static_cast<u32>(slot - objects.begin());
}

void release_object(HleCallFrame& frame, std::vector<GuestAddr>& objects) {
    const u32 handle = frame.args.next_u32();
    // The OSOS table routines reserve handle zero for null.
    if (handle != 0 && handle < objects.size() && objects[handle] != 0) {
        (void)frame.emulator.heap().release(objects[handle]);
        objects[handle] = 0;
    }
    frame.ret_void();
}

} // namespace

void register_classic_metadata(HleRegistry& registry) {
    registry.add("Metadata", 45, "classic_metadata_create", 0,
                 [](HleCallFrame& frame) {
        auto& objects = frame.emulator.metadata_state().classic_objects;
        // Export body: FUN_082bd368 -> FUN_0813359c(alloc(0x48), 0, 0).
        frame.ret(create_object(frame, objects, 0x08984e18));
    });

    registry.add("Metadata", 46, "classic_metadata_release", 1,
                 [](HleCallFrame& frame) {
        release_object(frame, frame.emulator.metadata_state().classic_objects);
    });

    registry.add("Metadata", 19, "classic_metadata_aux_create", 0,
                 [](HleCallFrame& frame) {
        // Export body: FUN_082bc7c8 -> FUN_0815dbb4(alloc(0x48), 0).
        frame.ret(create_object(
            frame, frame.emulator.metadata_state().classic_auxiliary_objects,
            0x089ca674));
    });

    registry.add("Metadata", 20, "classic_metadata_aux_release", 1,
                 [](HleCallFrame& frame) {
        release_object(frame,
                       frame.emulator.metadata_state().classic_auxiliary_objects);
    });

    registry.add("Metadata", 47, "classic_metadata_notify", 0,
                 [](HleCallFrame& frame) {
        // FUN_082bc820 calls a method at vtable +0x38 only when the OSOS
        // Metadata service getter (FUN_080e6160) returns an object. That
        // firmware-owned service is not instantiated by the game-only HLE.
        // Model the observed null-service branch; do not invent a callback.
        frame.ret_void();
    });
}

} // namespace clickhle

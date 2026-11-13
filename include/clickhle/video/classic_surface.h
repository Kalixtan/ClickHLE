#pragma once

#include "clickhle/common/types.h"

namespace clickhle { class AddressSpace; }

namespace clickhle::classic {

// Configuration selected by the 7G game launcher for its 320x240 surface.
// Values come from OSOS configuration initialization and selection.
[[nodiscard]] bool initialize_game_config(AddressSpace& memory, GuestAddr config);

// Base constructor path with no optional attributes: copy configuration
// words 0..0x60 and bytes 0x64/0x65 into the guest surface.
[[nodiscard]] bool initialize_surface_config(AddressSpace& memory, GuestAddr surface,
                                             GuestAddr config);

// The fields used by the 7G surface's present routine. Handles identify
// buffer objects; pixels is that object's plane pointer for type 2.
struct SurfaceBuffer {
    u32 handle = 0;
    u32 pixels = 0;
    u8 type = 0;
};

struct SurfaceBuffers {
    SurfaceBuffer front;
    SurfaceBuffer back;
    u32 draw_pixels = 0;
};

struct SurfaceSwap {
    u32 presented_buffer;
    u32 wait_buffer;
};

// Call after handing the old back buffer to the display and before waiting.
[[nodiscard]] SurfaceSwap begin_surface_swap(SurfaceBuffers& surface);
// Call only after the wait on SurfaceSwap::wait_buffer has completed.
void finish_surface_swap(SurfaceBuffers& surface);

} // namespace clickhle::classic

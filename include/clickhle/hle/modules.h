#pragma once

#include <map>
#include <string>
#include <vector>

#include "clickhle/common/types.h"

namespace clickhle {

class Emulator;

class HleRegistry;
struct HleCallFrame;

/// HLE modules register themselves through explicit calls rather than static
/// constructors. Static self-registration across translation units depends on
/// the linker not discarding objects nobody references, which behaves
/// differently on every toolchain -- an explicit list is boring and portable.
///
/// Adding a module: write the file, declare it here, call it in register_all().

/// The largest library a click-wheel game imports: 179 ordinals, every game.
void register_opengles(HleRegistry& registry);

/// Apple's "miscTBD", typo and all: 15 ordinals, every game. Holds the heap.
void register_misc(HleRegistry& registry);

/// The OpenGL ES 1.x implementation, registered by name under module "gl".
/// OpenGLES ordinal handlers forward calls to it.
void register_libgles(HleRegistry& registry);

/// "InputEvents": the click wheel. Two ordinals for the whole input API.
void register_input(HleRegistry& registry);

/// Forces a button mask into every input poll, pulsed with the given period in
/// polls. Investigation only: which bit is which control is not established,
/// and this is how the mapping gets found. Zero disables.
void set_forced_buttons(u32 mask, u32 period);

/// Forces bits into the wheel word, on the same pulse as set_forced_buttons.
/// Which word a title actually reads is worth being able to test, not assume.
void set_forced_wheel(u32 mask);

/// Base for the event type byte posted on the vec4 input list. Investigation:
/// the id->type translation is not yet known, so it is swept.
void set_event_type_base(u32 base);

/// "Settings": three ordinals, the device's preference store.
void register_settings(HleRegistry& registry);

/// "AsyncFileIO": asset and save-file access, over the VFS.
void register_fileio(HleRegistry& registry);

/// "Audio": sound effects and the music stream. Bookkeeping only; silent.
void register_audio(HleRegistry& registry);

/// "Metadata": the iPod's music library. Reports an empty one.
void register_metadata(HleRegistry& registry);

void register_all_modules(HleRegistry& registry);

/// Frames presented and draw calls issued so far. Zero until the renderer is
/// wired up.
void gles_statistics(Emulator& emulator, u64& frames, u64& draws);

/// Present one emulated frame through the configured display backend.
void gles_present_frame(HleCallFrame& frame);

/// Draw calls attributed to each bound texture name.
///
/// A busy frame is not a correct frame: a title can issue hundreds of quads and
/// show a static screen. This says which textures those quads actually used, so
/// "drawn but invisible" and "never drawn" can be told apart -- which a
/// screenshot cannot do.
[[nodiscard]] const std::map<u32, u64>& gles_draws_by_texture(Emulator& emulator);

}  // namespace clickhle

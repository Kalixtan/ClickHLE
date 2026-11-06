// libGLES_CM: OpenGL ES 1.0 and EGL.
//
// Roadmap tasks #19 and #20.
//
// ES 1.0 is a fixed-function API -- matrix stack, glVertexPointer, alpha test,
// texenv -- so it maps almost one-for-one onto desktop GL up to 2.1. That is why
// this file is a few hundred lines rather than a few thousand: no ubershader, no
// state tracking, just unit conversion and pointer translation.
//
// Two things genuinely need translating:
//
//   1. Fixed point. Every `x`-suffixed entry point takes 16.16 fixed-point
//      arguments, because the ARM920T has no FPU. Divide by 65536.
//   2. Vertex array pointers. glVertexPointer is handed a *guest* address, and
//      the data is only read later, at draw time. So the pointers are recorded
//      and resolved during glDrawElements/glDrawArrays, when the index range
//      says how much of the array is actually live.

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "clickhle/common/endian.h"
#include "clickhle/common/log.h"
#include "clickhle/core/emulator.h"
#include "clickhle/hle/format.h"
#include "clickhle/hle/modules.h"
#include "clickhle/hle/registry.h"
#include "clickhle/video/display.h"

namespace clickhle {
namespace {

constexpr std::string_view kCategory = "gles";
/// Registered by name, not by ordinal. Click-wheel games import by ordinal
/// only, so nothing here is reachable directly -- src/hle/modules/opengles.cpp
/// forwards an ordinal to one of these once there is evidence for what that
/// ordinal is.
constexpr const char* kModule = "gl";

constexpr i32 kScreenWidth = 320;
constexpr i32 kScreenHeight = 240;

/// GL_FIXED exists in ES but not in desktop GL, so arrays declared with it are
/// converted on the way through.
constexpr u32 kGlFixed = 0x140C;

/// EGL handles. Opaque to the guest, so any non-zero value will do; distinctive
/// ones make traces readable.
constexpr u32 kEglDisplay = 0x4001;
constexpr u32 kEglConfig = 0x4002;
constexpr u32 kEglSurface = 0x4003;
constexpr u32 kEglContext = 0x4004;
constexpr u32 kEglTrue = 1;
constexpr i32 kEglSuccess = 0x3000;
constexpr i32 kEglNotInitialized = 0x3001;
constexpr i32 kEglBadAlloc = 0x3003;
constexpr i32 kEglBadAttribute = 0x3004;
constexpr i32 kEglBadConfig = 0x3005;
constexpr i32 kEglBadContext = 0x3006;
constexpr i32 kEglBadDisplay = 0x3008;
constexpr i32 kEglBadMatch = 0x3009;
constexpr i32 kEglBadSurface = 0x300d;
constexpr i32 kEglBadParameter = 0x300c;

// eglQuerySurface attributes.
constexpr i32 kEglWidth = 0x3057;
constexpr i32 kEglHeight = 0x3056;

std::optional<i32> egl_config_attribute(i32 attribute) {
    switch (attribute) {
        case 0x3020: return 16;  // EGL_BUFFER_SIZE
        case 0x3021: return 0;   // EGL_ALPHA_SIZE
        case 0x3022: return 5;   // EGL_BLUE_SIZE
        case 0x3023: return 6;   // EGL_GREEN_SIZE
        case 0x3024: return 5;   // EGL_RED_SIZE
        case 0x3025: return 16;  // EGL_DEPTH_SIZE
        case 0x3026: return 0;   // EGL_STENCIL_SIZE
        case 0x3027: return 0x3038;  // EGL_CONFIG_CAVEAT = EGL_NONE
        case 0x3028: return 1;   // EGL_CONFIG_ID (distinct from opaque handle)
        case 0x3029: return 0;   // EGL_LEVEL
        case 0x302a: return kScreenHeight;  // EGL_MAX_PBUFFER_HEIGHT
        case 0x302b: return kScreenWidth * kScreenHeight;  // MAX_PBUFFER_PIXELS
        case 0x302c: return kScreenWidth;   // EGL_MAX_PBUFFER_WIDTH
        case 0x302d: return 0;   // EGL_NATIVE_RENDERABLE
        case 0x302e: return 0;   // EGL_NATIVE_VISUAL_ID
        case 0x302f: return 0;   // EGL_NATIVE_VISUAL_TYPE
        case 0x3031: return 0;   // EGL_SAMPLES
        case 0x3032: return 0;   // EGL_SAMPLE_BUFFERS
        case 0x3033: return 0x0007;  // WINDOW | PIXMAP | PBUFFER
        case 0x3034: return 0x3038;  // EGL_TRANSPARENT_TYPE = EGL_NONE
        case 0x3035: return 0;   // EGL_TRANSPARENT_RED_VALUE
        case 0x3036: return 0;   // EGL_TRANSPARENT_GREEN_VALUE
        case 0x3037: return 0;   // EGL_TRANSPARENT_BLUE_VALUE
        case 0x3039: return 0;   // EGL_BIND_TO_TEXTURE_RGB
        case 0x303a: return 0;   // EGL_BIND_TO_TEXTURE_RGBA
        case 0x303b: return 0;   // EGL_MIN_SWAP_INTERVAL
        case 0x303c: return 1;   // EGL_MAX_SWAP_INTERVAL
        case 0x303d: return 0;   // EGL_LUMINANCE_SIZE
        case 0x303e: return 0;   // EGL_ALPHA_MASK_SIZE
        case 0x303f: return 0x308e;  // EGL_COLOR_BUFFER_TYPE = EGL_RGB_BUFFER
        case 0x3040: return 0x0001;  // EGL_RENDERABLE_TYPE = EGL_OPENGL_ES_BIT
        case 0x3042: return 0x0001;  // EGL_CONFORMANT = EGL_OPENGL_ES_BIT
        default: return std::nullopt;
    }
}

bool egl_config_attribute_is_minimum(i32 attribute) {
    switch (attribute) {
        case 0x3020:  // EGL_BUFFER_SIZE
        case 0x3021:  // EGL_ALPHA_SIZE
        case 0x3022:  // EGL_BLUE_SIZE
        case 0x3023:  // EGL_GREEN_SIZE
        case 0x3024:  // EGL_RED_SIZE
        case 0x3025:  // EGL_DEPTH_SIZE
        case 0x3026:  // EGL_STENCIL_SIZE
        case 0x3031:  // EGL_SAMPLES
        case 0x3032:  // EGL_SAMPLE_BUFFERS
        case 0x303d:  // EGL_LUMINANCE_SIZE
        case 0x303e:  // EGL_ALPHA_MASK_SIZE
            return true;
        default:
            return false;
    }
}

bool egl_config_attribute_is_mask(i32 attribute) {
    return attribute == 0x3033 || attribute == 0x3040 || attribute == 0x3042;
}

/// EXPLICIT DEVIATION FROM HARDWARE FIDELITY, scoped to one title -- same
/// spirit as emulator.cpp's kVortexDivisorInstructionPatched, see that
/// comment for the full standing precedent (deliberate workaround, real
/// firmware mechanism not identifiable this session).
///
/// Three of Vortex's UI icon textures (32x32, from the packed "tex" atlas)
/// dispatch through the SAME format code as plenty of other, correctly-
/// rendering textures (RGB565/GL_RGB, confirmed by LLE's disassembly) --
/// but visually render as top-half static, bottom-half a recognisable icon.
/// Confirmed directly, offline, against the exact captured source bytes:
/// interpreted as 8bpp (1 byte/pixel, GL_LUMINANCE) at the SAME declared
/// 32x32 size, all three decode into a perfectly clean icon (arrows,
/// checkmarks, a shift-key glyph) with no reinterpretation of width/height
/// needed at all -- only the bytes-per-pixel assumption is wrong. Confirmed
/// live in this exact build: `caller=0x18015770` (the RGB565 dispatch
/// path) never produces a 32x32 texture anywhere else in a full run, so
/// "32x32 through this path" is a safe, narrow signal for exactly these
/// three broken uploads and nothing else.
///
/// Root cause not identified -- LLE flagged a real, unresolved candidate
/// (a race between the format dispatcher's header-copy loop and whatever
/// fills its source buffer, which could explain the SAME format bytes
/// belonging to two different logical assets) but confirming it needs
/// firmware-level timing knowledge neither static analysis nor live
/// execution settled this session. Scoped as narrowly as the divisor patch:
/// exact entry point, exact image size, exact declared dimensions.
constexpr GuestAddr kVortexEntryPoint = 0x18021798;
constexpr u32 kVortexImageSize = 414600;

[[nodiscard]] float fixed_to_float(u32 value) {
    return static_cast<float>(static_cast<i32>(value)) / 65536.0f;
}

/// A client-side vertex array as the guest declared it.
struct ArrayPointer {
    i32 size = 0;
    u32 type = 0;
    i32 stride = 0;
    GuestAddr pointer = kNullGuest;
    bool enabled = false;
};

/// ES 1.0 permits several texture units, and games use them: Sticky Balls
/// binds unit 1, disables it, then configures unit 0 for the draw. Collapsing
/// that onto a single unit means one unit's disable or texcoord array silently
/// applies to the other.
constexpr u32 kMaxTextureUnits = 4;

struct GlState {
    /// Ratio between the host window and the guest's 320x240 panel.
    ///
    /// The guest calls glViewport(0, 0, 320, 240) because that is its whole
    /// screen. Presenting at 2x without scaling that rectangle draws the game
    /// into one quarter of the window.
    i32 scale = 1;
    ArrayPointer vertex;
    ArrayPointer colour;
    /// One texcoord array per unit, selected by glClientActiveTexture.
    ArrayPointer texcoord[kMaxTextureUnits];
    ArrayPointer normal;
    /// Unit selected for glTexCoordPointer / glEnableClientState.
    u32 client_active_unit = 0;
    bool surface_ready = false;
    bool egl_initialized = false;
    bool egl_context_created = false;
    bool egl_surface_created = false;
    bool egl_context_current = false;
    u32 egl_draw_surface = 0;
    u32 egl_read_surface = 0;
    GLenum pending_gl_error = GL_NO_ERROR;
    /// EGL status of the most recent EGL call in this emulated instance.
    i32 egl_error = kEglSuccess;
    std::map<i32, GuestAddr> egl_strings;
    std::map<GLenum, GuestAddr> gl_strings;
    u64 frames = 0;
    u64 draws = 0;
    /// The texture bound by the most recent glBindTexture, and how many draws
    /// each one accounted for.
    u32 bound_texture = 0;
    u32 bound_texture_target = 0x0DE1;
    std::map<u32, u64> draws_by_texture;
    std::set<u32> dumped;
    std::map<u32, std::pair<i32, i32>> texture_dimensions;
    std::map<u32, i32> texture_max_level;
    std::vector<float> scratch_vertex;
    std::vector<float> scratch_colour;
    std::vector<float> scratch_texcoord[kMaxTextureUnits];
};

thread_local GlState* active_gl_state = nullptr;

GlState& gl_state() { return *active_gl_state; }

void set_egl_error(i32 error) {
    gl_state().egl_error = error;
}

void set_gl_error(GLenum error) {
    if (gl_state().pending_gl_error == GL_NO_ERROR) {
        gl_state().pending_gl_error = error;
    }
}

bool require_egl_initialized(HleCallFrame& frame) {
    if (gl_state().egl_initialized) return true;
    set_egl_error(kEglNotInitialized);
    frame.ret(0);
    return false;
}

/// Byte stride actually used to walk an array. Zero means tightly packed, and
/// the element size depends on the type -- getting this wrong reads the wrong
/// vertices without any error being reported anywhere.
[[nodiscard]] u32 element_size(const ArrayPointer& array) {
    u32 component = 4;
    switch (array.type) {
        case GL_BYTE:
        case GL_UNSIGNED_BYTE:  component = 1; break;
        case GL_SHORT:
        case GL_UNSIGNED_SHORT: component = 2; break;
        default:                component = 4; break;  // GL_FLOAT, GL_FIXED
    }
    return component * static_cast<u32>(array.size);
}

[[nodiscard]] u32 array_stride(const ArrayPointer& array) {
    return array.stride != 0 ? static_cast<u32>(array.stride)
                             : element_size(array);
}

/// Scratch buffers for GL_FIXED arrays converted to float. Kept alive until the
/// next draw because glVertexPointer only records the pointer -- GL reads it
/// during glDrawElements, by which time a local buffer would be long gone.
struct Scratch {
    std::vector<float>& vertex = gl_state().scratch_vertex;
    std::vector<float>& colour = gl_state().scratch_colour;
    std::vector<float>* texcoord = gl_state().scratch_texcoord;
};

Scratch scratch() { return {}; }

/// Resolves one guest array into something host GL can read, converting
/// GL_FIXED to float on the way. Returns false if the range is not mapped.
bool bind_array(AddressSpace& memory, const ArrayPointer& array, u32 vertex_count,
                std::vector<float>& conversion, const void*& host_pointer,
                u32& host_type, i32& host_stride) {
    if (!array.enabled || array.pointer == kNullGuest || vertex_count == 0) {
        return false;
    }
    const u32 stride = array_stride(array);
    const u64 span = static_cast<u64>(stride) * (vertex_count - 1) +
                     element_size(array);
    const std::byte* base = memory.host_ptr(array.pointer, static_cast<u32>(span));
    if (base == nullptr) {
        CLICKHLE_ERROR(kCategory)
            << "vertex array at 0x" << std::hex << array.pointer << std::dec
            << " spans " << span << " bytes of unmapped memory";
        return false;
    }

    if (array.type != kGlFixed) {
        host_pointer = base;
        host_type = array.type;
        host_stride = array.stride;
        return true;
    }

    // GL_FIXED -> GL_FLOAT, repacked tightly.
    const auto components = static_cast<u32>(array.size);
    conversion.resize(static_cast<std::size_t>(vertex_count) * components);
    for (u32 v = 0; v < vertex_count; ++v) {
        const std::byte* source = base + static_cast<std::size_t>(v) * stride;
        for (u32 c = 0; c < components; ++c) {
            u32 raw = 0;
            std::memcpy(&raw, source + c * 4, sizeof(raw));
            conversion[static_cast<std::size_t>(v) * components + c] =
                fixed_to_float(raw);
        }
    }
    host_pointer = conversion.data();
    host_type = GL_FLOAT;
    host_stride = 0;
    return true;
}

/// Draws whatever GDI has put on screen over the GL scene, just before present.
///
/// Over rather than under, and only when something has been drawn. Retail games
/// use GDI for front-end and loading screens and GL for gameplay, so the two
/// rarely overlap; the case that matters is the GDI-only program, where there
/// is no GL scene to hide. Compositing an untouched surface would paint black
/// over everything, which is why coredll_gdi tracks whether it is dirty rather
/// than just whether it exists.
///
/// Uploaded as a texture and drawn as a quad rather than glDrawPixels: the
/// pipeline is already set up for textured quads, and glDrawPixels interacts
/// badly with the viewport scaling.
void composite_gdi_overlay_impl() {
    // Was a Windows CE concern: that platform's games drew front-end screens
    // through GDI and gameplay through GL, so the two had to be composited.
    // Click-wheel games have one graphics API and no such split.
}

/// Points host GL at the guest's arrays for one draw call.
/// Uploaded dimensions per texture name, recorded where GL actually binds.
///
/// The ordinal layer keeps its own copy, and the two DISAGREE in practice --
/// measured on Zuma, a draw whose GL-bound texture was 2 ran with the ordinal
/// layer still believing 4 or 7 was current. Anything that must match the
/// texture a draw samples has to key off this one.
std::map<u32, std::pair<i32, i32>>& texture_dimensions() {
    return gl_state().texture_dimensions;
}

/// Texel -> normalised, for the texture this draw will actually sample.
///
/// These games give texture coordinates in TEXELS. ES 1.x reaches the same
/// place with a texture matrix of 1/width, 1/height -- but it has to be the
/// width and height of the texture being sampled NOW, not of whatever was bound
/// most recently.
///
/// Measured before the fix, on Zuma's level-select frame: of 2598 draws of
/// texture 2 (322x242), only 186 ran with 1/322, 1/242. 1172 ran with texture
/// 4's 1/512, 1/45 and 1240 with texture 7's 1/510, 1/212. A full-screen
/// background quad -- correct vertices, identity modelview, correct ortho,
/// correct texel coords, correct texture contents -- came out as a squashed
/// 44-row band, because its coordinates were divided by another texture's size.
void install_texel_scale() {
    // The selected texture target is enabled at DRAW time whenever a real
    // texture is bound. Only GL_TEXTURE_2D needs texel-to-normalised scaling;
    // rectangle textures use texel coordinates directly and cube maps use
    // direction vectors.
    //
    // The ordinal layer enabled GL_TEXTURE_2D once, from a `static bool`, on
    // the first glBindTexture of the process. Measured on Ms. PAC-MAN by
    // reading GL back at every draw: `tex2d_enabled=0` on all 9269 draws of
    // texture 6. An untextured quad renders solid white, and the screen was
    // (255,255,255) in all 76,800 pixels.
    //
    // This is the SAME shape as the texture-matrix bug: a correct value
    // installed at the wrong *time*, invisible to any check of the value.
    // Doing it per draw is independent of bind ordering and of which unit was
    // active when the one-shot fired.
    //
    // Guarded on a non-zero binding so an untextured draw stays untextured --
    // texture name 0 is GL's reserved default and Ms. PAC-MAN does bind it for
    // 2066 draws.
    if (gl_state().bound_texture != 0) {
        glEnable(gl_state().bound_texture_target);
    } else {
        glDisable(gl_state().bound_texture_target);
    }
    const auto it = texture_dimensions().find(gl_state().bound_texture);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    if (gl_state().bound_texture_target == GL_TEXTURE_2D &&
        it != texture_dimensions().end() && it->second.first > 0 &&
        it->second.second > 0) {
        glScalef(1.0f / static_cast<float>(it->second.first),
                 1.0f / static_cast<float>(it->second.second), 1.0f);
    }
    glMatrixMode(GL_MODELVIEW);
}

void apply_arrays(HleCallFrame& frame, u32 vertex_count) {
    install_texel_scale();
    GlState& state = gl_state();
    const void* pointer = nullptr;
    u32 type = 0;
    i32 stride = 0;

    if (bind_array(frame.memory, state.vertex, vertex_count, scratch().vertex,
                   pointer, type, stride)) {
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(state.vertex.size, static_cast<GLenum>(type), stride,
                        pointer);
    } else {
        glDisableClientState(GL_VERTEX_ARRAY);
    }

    if (bind_array(frame.memory, state.colour, vertex_count, scratch().colour,
                   pointer, type, stride)) {
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(state.colour.size, static_cast<GLenum>(type), stride,
                       pointer);
    } else {
        glDisableClientState(GL_COLOR_ARRAY);
    }

    // Each unit's texcoord array has to be bound with that unit selected on the
    // *client* side, which is a separate selector from glActiveTexture.
    for (u32 unit = 0; unit < kMaxTextureUnits; ++unit) {
        glClientActiveTexture(GL_TEXTURE0 + unit);
        if (bind_array(frame.memory, state.texcoord[unit], vertex_count,
                       scratch().texcoord[unit], pointer, type, stride)) {
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(state.texcoord[unit].size,
                              static_cast<GLenum>(type), stride, pointer);
        } else {
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        }
    }
    glClientActiveTexture(GL_TEXTURE0 + state.client_active_unit);
}

/// OES_compressed_paletted_texture.
///
/// This is what the Gizmondo's "compressed textures" actually are -- not an
/// undocumented nVidia scheme, but the standard Khronos paletted-texture
/// extension, which is fully specified. A palette of 16 (PALETTE4) or 256
/// (PALETTE8) entries is followed by 4- or 8-bit indices.
///
/// Desktop GL dropped paletted textures, so the decode happens here and the
/// result is uploaded as plain RGBA8.
struct PalettedFormat {
    u32 entries;        ///< 16 or 256
    u32 entry_bytes;    ///< bytes per palette entry
    bool four_bit;      ///< 4-bit indices, two pixels per byte
    const char* name;
};

[[nodiscard]] bool paletted_format(i32 internal_format, PalettedFormat& out) {
    switch (internal_format) {
        case 0x8B90: out = {16,  3, true,  "PALETTE4_RGB8"};     return true;
        case 0x8B91: out = {16,  4, true,  "PALETTE4_RGBA8"};    return true;
        case 0x8B92: out = {16,  2, true,  "PALETTE4_R5_G6_B5"}; return true;
        case 0x8B93: out = {16,  2, true,  "PALETTE4_RGBA4"};    return true;
        case 0x8B94: out = {16,  2, true,  "PALETTE4_RGB5_A1"};  return true;
        case 0x8B95: out = {256, 3, false, "PALETTE8_RGB8"};     return true;
        case 0x8B96: out = {256, 4, false, "PALETTE8_RGBA8"};    return true;
        case 0x8B97: out = {256, 2, false, "PALETTE8_R5_G6_B5"}; return true;
        case 0x8B98: out = {256, 2, false, "PALETTE8_RGBA4"};    return true;
        case 0x8B99: out = {256, 2, false, "PALETTE8_RGB5_A1"};  return true;
        default: return false;
    }
}

/// Expands one palette entry to RGBA8.
void decode_entry(i32 internal_format, const std::byte* source, u8* rgba) {
    const auto byte_at = [source](std::size_t i) {
        return std::to_integer<u8>(source[i]);
    };
    // 5- and 4-bit channels are replicated into the high bits rather than
    // shifted, so full-scale input maps to 255 rather than 248.
    const auto expand5 = [](u32 v) { return static_cast<u8>((v << 3) | (v >> 2)); };
    const auto expand6 = [](u32 v) { return static_cast<u8>((v << 2) | (v >> 4)); };
    const auto expand4 = [](u32 v) { return static_cast<u8>((v << 4) | v); };
    // Multi-byte palette entries are stored in the guest's byte order, and the
    // ARM920T is little-endian. Reading these big-endian swaps red and blue and
    // scrambles green -- which looks like "the colours are wrong" rather than
    // like an endianness bug, so it is worth being explicit about.
    const auto pack16 = [](u32 low, u32 high) { return (high << 8) | low; };

    switch (internal_format) {
        case 0x8B90:
        case 0x8B95:  // RGB8
            rgba[0] = byte_at(0); rgba[1] = byte_at(1);
            rgba[2] = byte_at(2); rgba[3] = 255;
            break;
        case 0x8B91:
        case 0x8B96:  // RGBA8
            rgba[0] = byte_at(0); rgba[1] = byte_at(1);
            rgba[2] = byte_at(2); rgba[3] = byte_at(3);
            break;
        case 0x8B92:
        case 0x8B97: {  // R5_G6_B5
            const u32 v = pack16(byte_at(0), byte_at(1));
            rgba[0] = expand5((v >> 11) & 0x1F);
            rgba[1] = expand6((v >> 5) & 0x3F);
            rgba[2] = expand5(v & 0x1F);
            rgba[3] = 255;
            break;
        }
        case 0x8B93:
        case 0x8B98: {  // RGBA4
            const u32 v = pack16(byte_at(0), byte_at(1));
            rgba[0] = expand4((v >> 12) & 0xF);
            rgba[1] = expand4((v >> 8) & 0xF);
            rgba[2] = expand4((v >> 4) & 0xF);
            rgba[3] = expand4(v & 0xF);
            break;
        }
        case 0x8B94:
        case 0x8B99: {  // RGB5_A1
            const u32 v = pack16(byte_at(0), byte_at(1));
            rgba[0] = expand5((v >> 11) & 0x1F);
            rgba[1] = expand5((v >> 6) & 0x1F);
            rgba[2] = expand5((v >> 1) & 0x1F);
            rgba[3] = (v & 1) != 0 ? 255 : 0;
            break;
        }
        default:
            rgba[0] = rgba[1] = rgba[2] = 0;
            rgba[3] = 255;
            break;
    }
}

/// glCompressedTexImage2D is GL 1.3, so unlike everything else here it has to
/// be resolved at run time rather than linked against.
using CompressedTexImage2D = void (*)(GLenum, GLint, GLenum, GLsizei, GLsizei,
                                      GLint, GLsizei, const void*);
CompressedTexImage2D compressed_tex_image_2d = nullptr;

/// Reports any pending GL error against the call that caused it.
///
/// Texture uploads fail silently otherwise: GL records the error, leaves the
/// texture undefined, and the game draws untextured geometry that looks like a
/// missing asset rather than a rejected upload.
void check_gl_error(const char* what) {
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        CLICKHLE_WARN(kCategory)
            << what << " raised GL error 0x" << std::hex << error << std::dec;
    }
}

/// Highest mip level uploaded for each texture name.
///
/// A texture whose min filter asks for mipmaps but which only has level 0 is
/// *incomplete*, and GL samples white from it -- which looks like missing
/// geometry rather than a texture bug. ES 1.0 games routinely set a mipmapping
/// filter and upload a single level, so the max level is pinned to whatever was
/// actually supplied.
std::map<u32, i32>& texture_max_level() {
    return gl_state().texture_max_level;
}

/// Records a level upload and keeps GL_TEXTURE_MAX_LEVEL consistent with it.
void note_texture_level(GLenum target, i32 level) {
    if (target != GL_TEXTURE_2D) {
        return;
    }
    GLint name = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &name);
    i32& highest = texture_max_level()[static_cast<u32>(name)];
    highest = std::max(highest, level < 0 ? 0 : level);
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, highest);
}

ArrayPointer* array_for(u32 cap) {
    switch (cap) {
        case GL_VERTEX_ARRAY:        return &gl_state().vertex;
        case GL_COLOR_ARRAY:         return &gl_state().colour;
        case GL_TEXTURE_COORD_ARRAY:
            return &gl_state().texcoord[gl_state().client_active_unit];
        case GL_NORMAL_ARRAY:        return &gl_state().normal;
        default:                     return nullptr;
    }
}

/// Opens the display and does the one-time viewport/projection/modelview
/// setup, exactly once, no matter which of several call sites is first to
/// need a real GL context to exist.
///
/// Previously this setup lived duplicated (and once, un-guarded) at each of
/// eglCreateWindowSurface's and gles_present_frame's own call sites. Found
/// missing from a third: glGenTextures. A title that calls glGenTextures
/// before its first swap or eglCreateWindowSurface -- Texas Hold'em does,
/// seven times, all before the display ever opens -- gets host GL calls with
/// no context, which silently discard the write and leave the ids vector at
/// its zero-initialised default. Every one of those textures landed on GL's
/// reserved id 0, each upload overwriting the last, because "the id was 0"
/// looked like ordinary texture-0 traffic instead of seven different assets
/// fighting over one slot. Same lazy-context-creation shape as the screen
/// projection and blend-enable bugs found earlier this session, a third
/// instance -- but unlike those two (one-shot latches that could simply wait
/// for the next call), a texture id has to be handed back synchronously and
/// used forever after, so the fix here is to guarantee the context exists
/// before glGenTextures asks the host for one, not to retry later.
bool ensure_display_open(Emulator& emulator) {
    Display& display = emulator.display();
    if (display.is_open()) {
        return true;
    }
    if (!display.open(kScreenWidth, kScreenHeight, "ClickHLE")) {
        return false;
    }
    gl_state().scale = display.scale();
    const i32 s = gl_state().scale;
    glViewport(0, 0, kScreenWidth * s, kScreenHeight * s);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    return true;
}

}  // namespace

void gles_composite_gdi(void) { composite_gdi_overlay_impl(); }

void gles_present_frame(HleCallFrame& f) {
    auto& storage = f.emulator.gles_state_storage();
    if (!storage.has_value()) storage.emplace<GlState>();
    GlState& state = std::any_cast<GlState&>(storage);
    GlState* previous_state = active_gl_state;
    active_gl_state = &state;
    struct RestoreState {
        GlState*& slot;
        GlState* previous;
        ~RestoreState() { slot = previous; }
    } restore{active_gl_state, previous_state};
    // One definition of "a frame happened", shared by all three kinds of
    // program. It used to live inside eglSwapBuffers, which meant a game that
    // never calls EGL -- a GDI-only program, or a DirectDraw one -- presented
    // pixels that were never counted, never screenshotted, and could not
    // satisfy --frames. Those runs had no way to stop except the timeout.
    Display& display = f.emulator.display();
    // The window is normally opened by eglCreateWindowSurface, which a game
    // that renders through DirectDraw or GDI never calls. Without this it
    // would present, screenshot and count frames into a display that was
    // never opened, and produce nothing at all.
    if (!ensure_display_open(f.emulator)) {
        return;
    }
    composite_gdi_overlay_impl();
    if (getenv("CLICKHLE_DUMP_PRESENT_READBACK") != nullptr) {
        // Same timing the REAL screenshot mechanism uses -- right before
        // present(), after every draw this frame has already happened.
        // Removes any ambiguity about whether a specific mid-frame draw
        // happened to cover this pixel yet.
        GLubyte px[3] = {99, 99, 99};
        glReadPixels(160, 120, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);
        std::size_t nonblack = 0;
        std::vector<GLubyte> row(kScreenWidth * 3);
        glReadPixels(0, 120, kScreenWidth, 1, GL_RGB, GL_UNSIGNED_BYTE,
                     row.data());
        for (std::size_t i = 0; i < row.size(); i += 3) {
            if (row[i] != 0 || row[i + 1] != 0 || row[i + 2] != 0) {
                ++nonblack;
            }
        }
        GLfloat clear_color[4] = {-1, -1, -1, -1};
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color);
        GLint viewport[4] = {-1, -1, -1, -1};
        glGetIntegerv(GL_VIEWPORT, viewport);
        CLICKHLE_INFO(kCategory)
            << "pre-present readback frame=" << gl_state().frames
            << " draws=" << gl_state().draws << " center=("
            << static_cast<int>(px[0]) << "," << static_cast<int>(px[1])
            << "," << static_cast<int>(px[2]) << ") row120_nonblack="
            << nonblack << "/" << kScreenWidth << " clearColor=("
            << clear_color[0] << "," << clear_color[1] << ","
            << clear_color[2] << "," << clear_color[3] << ") viewport=("
            << viewport[0] << "," << viewport[1] << "," << viewport[2] << ","
            << viewport[3] << ")";
    }
    display.present();

    ++gl_state().frames;
    if (!f.emulator.screenshot_path().empty()) {
        static_cast<void>(display.screenshot(f.emulator.screenshot_path()));
    }
    // Diagnostic: start the next frame blank. AFTER the screenshot, or the
    // capture is of the cleared buffer rather than the frame just drawn.
    //
    // Zuma calls glClear ONCE in 2600 frames -- it draws over the previous
    // frame and relies on its background covering every pixel, so anything
    // this frame does not cover keeps the PREVIOUS screen's pixels. That is
    // why menu artwork survives onto the level-select screen.
    //
    // Legitimate on hardware and NOT to become the default: a title that
    // deliberately accumulates across frames would break. This answers one
    // question -- is the residue purely uncovered pixels, or is a quad also
    // being drawn wrong?
    if (getenv("CLICKHLE_FORCE_CLEAR") != nullptr) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    const u64 limit = f.emulator.frame_limit();
    if (limit != 0 && gl_state().frames >= limit) {
        CLICKHLE_INFO(kCategory) << "reached the " << limit << "-frame limit";
        f.cpu.request_stop();
        return;
    }
    if (!display.pump()) {
        // The user closed the window. Stopping the CPU is the honest response
        // until WM_CLOSE is plumbed through to the guest.
        CLICKHLE_INFO(kCategory)
            << "window closed after " << gl_state().frames << " frames";
        f.cpu.request_stop();
    }
}

namespace {
}  // namespace

void register_libgles(HleRegistry& registry) {
    const auto fn = [&registry](const char* name, u8 arity, HleHandler handler) {
        // libGLES_CM exports by name, unlike COREDLL.
        const std::string_view export_name(name);
        const bool is_egl = export_name.starts_with("egl");
        const bool is_get_error = export_name == "eglGetError";
        registry.add_named(kModule, name, arity,
            [handler = std::move(handler), is_egl,
             is_get_error](HleCallFrame& frame) {
                auto& storage = frame.emulator.gles_state_storage();
                if (!storage.has_value()) storage.emplace<GlState>();
                GlState* previous = active_gl_state;
                active_gl_state = &std::any_cast<GlState&>(storage);
                // EGL reports the status of the most recent EGL call. A new
                // EGL entry clears the prior status; eglGetError itself reads
                // and clears that status in its handler.
                if (is_egl && !is_get_error) {
                    active_gl_state->egl_error = kEglSuccess;
                }
                struct RestoreState {
                    GlState*& slot;
                    GlState* previous;
                    ~RestoreState() { slot = previous; }
                } restore{active_gl_state, previous};
                handler(frame);
            });
    };

    // --- EGL -----------------------------------------------------------------

    fn("eglGetDisplay", 1, [](HleCallFrame& f) {
        (void)f.args.next_u32();  // native display; meaningless here
        f.ret(kEglDisplay);
    });

    fn("eglInitialize", 3, [](HleCallFrame& f) {
        const u32 display = f.args.next_u32();
        const GuestAddr major = f.args.next_ptr();
        const GuestAddr minor = f.args.next_ptr();
        if (display != kEglDisplay) {
            set_egl_error(kEglBadDisplay);
            f.ret(0);
            return;
        }
        if (major != kNullGuest && !f.store<i32>(major, 1)) {
            f.cpu.request_stop();
            return;
        }
        if (minor != kNullGuest && !f.store<i32>(minor, 0)) {
            f.cpu.request_stop();
            return;
        }
        gl_state().egl_initialized = true;
        f.ret(kEglTrue);
    });

    fn("eglTerminate", 1, [](HleCallFrame& f) {
        if (f.args.next_u32() != kEglDisplay) {
            set_egl_error(kEglBadDisplay);
            f.ret(0);
            return;
        }
        f.emulator.display().close();
        gl_state().egl_initialized = false;
        gl_state().egl_context_created = false;
        gl_state().egl_surface_created = false;
        gl_state().egl_context_current = false;
        gl_state().egl_draw_surface = 0;
        gl_state().egl_read_surface = 0;
        gl_state().surface_ready = false;
        f.ret(kEglTrue);
    });

    fn("eglChooseConfig", 5, [](HleCallFrame& f) {
        const u32 display = f.args.next_u32();
        const GuestAddr attributes = f.args.next_ptr();
        const GuestAddr configs = f.args.next_ptr();
        const i32 capacity = f.args.next_i32();
        const GuestAddr count = f.args.next_ptr();
        if (display != kEglDisplay) {
            set_egl_error(kEglBadDisplay);
            f.ret(0);
            return;
        }
        if (!require_egl_initialized(f)) return;
        if (count == kNullGuest || capacity < 0) {
            set_egl_error(kEglBadParameter);
            f.ret(0);
            return;
        }

        bool matches = true;
        if (attributes != kNullGuest) {
            bool terminated = false;
            std::vector<std::pair<i32, i32>> requested_attributes;
            for (u32 i = 0; i < 64; ++i) {
                const u64 key_address64 = static_cast<u64>(attributes) + i * 8u;
                if (key_address64 > std::numeric_limits<GuestAddr>::max() - 4u) {
                    f.cpu.request_stop();
                    return;
                }
                const GuestAddr key_address = static_cast<GuestAddr>(key_address64);
                const auto key = f.memory.read<i32>(key_address);
                if (!key.has_value()) {
                    f.cpu.request_stop();
                    return;
                }
                if (*key == 0x3038) {  // EGL_NONE
                    terminated = true;
                    break;
                }
                const auto requested = f.memory.read<i32>(key_address + 4);
                if (!requested.has_value()) {
                    f.cpu.request_stop();
                    return;
                }
                requested_attributes.emplace_back(*key, *requested);
            }
            if (!terminated) {
                set_egl_error(kEglBadAttribute);
                f.ret(0);
                return;
            }

            const auto config_id = std::find_if(
                requested_attributes.begin(), requested_attributes.end(),
                [](const auto& item) { return item.first == 0x3028; });
            if (config_id != requested_attributes.end() &&
                config_id->second < 0 && config_id->second != -1) {
                set_egl_error(kEglBadAttribute);
                f.ret(0);
                return;
            }
            if (config_id != requested_attributes.end() &&
                config_id->second != -1) {
                // EGL_CONFIG_ID overrides every other attribute in the list.
                matches = config_id->second == 1;
            } else {
                for (const auto& [key, requested] : requested_attributes) {
                    if ((key == 0x3041 && requested < 0) ||
                        (key != 0x3029 && key != 0x3041 && requested < 0 &&
                         requested != -1)) {
                        set_egl_error(kEglBadAttribute);
                        f.ret(0);
                        return;
                    }
                    if (key == 0x3041) {  // EGL_MATCH_NATIVE_PIXMAP
                        if (requested != -1) matches = false;
                        continue;
                    }
                    const auto actual = egl_config_attribute(key);
                    if (!actual.has_value()) {
                        set_egl_error(kEglBadAttribute);
                        f.ret(0);
                        return;
                    }
                    if (requested == -1 && key != 0x3029) {
                        continue;  // EGL_DONT_CARE (not valid for EGL_LEVEL)
                    }
                    if (egl_config_attribute_is_mask(key)) {
                        matches = matches && ((*actual & requested) == requested);
                    } else if (egl_config_attribute_is_minimum(key)) {
                        matches = matches && *actual >= requested;
                    } else {
                        matches = matches && *actual == requested;
                    }
                }
            }
        }
        const i32 available = matches ? 1 : 0;
        const i32 returned = configs == kNullGuest
                                 ? available
                                 : std::min(available, capacity);
        if (returned != 0 && configs != kNullGuest &&
            !f.store<u32>(configs, kEglConfig)) {
            f.cpu.request_stop();
            return;
        }
        if (!f.store<i32>(count, returned)) {
            f.cpu.request_stop();
            return;
        }
        f.ret(kEglTrue);
    });

    fn("eglGetConfigs", 4, [](HleCallFrame& f) {
        const u32 display = f.args.next_u32();
        const GuestAddr configs = f.args.next_ptr();
        const i32 capacity = f.args.next_i32();
        const GuestAddr count = f.args.next_ptr();
        if (display != kEglDisplay) {
            set_egl_error(kEglBadDisplay);
            f.ret(0);
            return;
        }
        if (!require_egl_initialized(f)) return;
        if (count == kNullGuest || capacity < 0) {
            set_egl_error(kEglBadParameter);
            f.ret(0);
            return;
        }
        const i32 returned = configs == kNullGuest ? 1 : std::min(capacity, 1);
        if (returned != 0 && !f.store<u32>(configs, kEglConfig)) {
            f.cpu.request_stop();
            return;
        }
        if (!f.store<i32>(count, returned)) {
            f.cpu.request_stop();
            return;
        }
        f.ret(kEglTrue);
    });

    fn("eglCreateWindowSurface", 4, [](HleCallFrame& f) {
        const u32 display = f.args.next_u32();
        const u32 config = f.args.next_u32();
        (void)f.args.next_u32();  // native window (the HWND we handed out)
        (void)f.args.next_ptr();  // attribute list
        if (display != kEglDisplay) {
            set_egl_error(kEglBadDisplay);
            f.ret(0);
            return;
        }
        if (!require_egl_initialized(f)) return;
        if (config != kEglConfig) {
            set_egl_error(kEglBadConfig);
            f.ret(0);
            return;
        }

        // This is where the host window normally first appears: the guest has
        // now committed to rendering, rather than merely having a window.
        // ensure_display_open is idempotent, so this is a no-op if something
        // else (glGenTextures, a draw) already needed a real context first.
        if (!ensure_display_open(f.emulator)) {
            set_egl_error(kEglBadAlloc);
            f.ret(0);  // EGL_NO_SURFACE
            return;
        }
        gl_state().surface_ready = true;
        gl_state().egl_surface_created = true;
        f.ret(kEglSurface);
    });

    fn("eglCreateContext", 4, [](HleCallFrame& f) {
        const u32 display = f.args.next_u32();
        const u32 config = f.args.next_u32();
        (void)f.args.next_u32();
        (void)f.args.next_ptr();
        if (display != kEglDisplay) {
            set_egl_error(kEglBadDisplay);
            f.ret(0);
            return;
        }
        if (!require_egl_initialized(f)) return;
        if (config != kEglConfig) {
            set_egl_error(kEglBadConfig);
            f.ret(0);
            return;
        }
        gl_state().egl_context_created = true;
        f.ret(kEglContext);
    });

    fn("eglGetProcAddress", 1, [](HleCallFrame& f) {
        // Null means "no such entry point", which is a legitimate answer and
        // the one every caller already has to handle. Returning anything else
        // would mean minting a thunk per name; worth doing only once a game
        // actually depends on an extension rather than merely probing for one.
        const auto name = f.memory.read_cstring(f.args.next_ptr());
        CLICKHLE_DEBUG(kCategory) << "eglGetProcAddress('"
                                << name.value_or(std::string{}) << "') -> null";
        f.ret(kNullGuest);
    });

    // The EGL query surface the audit found missing. Conflict - Vietnam stops
    // dead on eglGetError, which is a reasonable thing for a program to call
    // after every setup step.
    fn("eglGetError", 0, [](HleCallFrame& f) {
        const i32 error = gl_state().egl_error;
        gl_state().egl_error = kEglSuccess;
        f.ret(static_cast<u32>(error));
    });

    fn("eglQueryString", 2, [](HleCallFrame& f) {
        const u32 display = f.args.next_u32();
        const i32 name = f.args.next_i32();
        if (display != kEglDisplay) {
            set_egl_error(kEglBadDisplay);
            f.ret(kNullGuest);
            return;
        }
        if (!require_egl_initialized(f)) return;
        auto& cached = gl_state().egl_strings;
        const auto it = cached.find(name);
        if (it != cached.end()) { f.ret(it->second); return; }
        std::string value;
        switch (name) {
            case 0x3053: value = "1.0"; break;               // EGL_VERSION
            case 0x3054: value = "NVIDIA Corporation"; break; // EGL_VENDOR
            case 0x3055: value = ""; break;                   // EGL_EXTENSIONS
            default:
                set_egl_error(kEglBadParameter);
                f.ret(kNullGuest);
                return;
        }
        const GuestAddr block =
            f.emulator.heap().allocate(static_cast<u32>(value.size()) + 1);
        if (block == kNullGuest) {
            set_egl_error(kEglBadAlloc);
            f.ret(kNullGuest);
            return;
        }
        write_guest_cstring(f.memory, block, value);
        cached[name] = block;
        f.ret(block);
    });

    fn("eglGetConfigAttrib", 4, [](HleCallFrame& f) {
        const u32 display = f.args.next_u32();
        const u32 config = f.args.next_u32();
        const i32 attribute = f.args.next_i32();
        const GuestAddr out = f.args.next_ptr();
        if (display != kEglDisplay || config != kEglConfig ||
            out == kNullGuest) {
            set_egl_error(display != kEglDisplay ? kEglBadDisplay :
                          (config != kEglConfig ? kEglBadConfig :
                           kEglBadParameter));
            f.ret(0);
            return;
        }
        if (!require_egl_initialized(f)) return;
        const auto value = egl_config_attribute(attribute);
        if (!value.has_value()) {
            set_egl_error(kEglBadAttribute);
            f.ret(0);
            return;
        }
        if (!f.store<i32>(out, *value)) {
            f.cpu.request_stop();
            return;
        }
        f.ret(kEglTrue);
    });

    fn("eglGetCurrentDisplay", 0, [](HleCallFrame& f) {
        f.ret(gl_state().egl_context_current ? kEglDisplay : 0u);
    });
    fn("eglGetCurrentContext", 0, [](HleCallFrame& f) {
        f.ret(gl_state().egl_context_current ? kEglContext : 0u);
    });
    fn("eglGetCurrentSurface", 1, [](HleCallFrame& f) {
        const i32 which = f.args.next_i32();
        if (which == 0x3059) {  // EGL_DRAW
            f.ret(gl_state().egl_context_current
                      ? gl_state().egl_draw_surface : 0u);
        } else if (which == 0x305a) {  // EGL_READ
            f.ret(gl_state().egl_context_current
                      ? gl_state().egl_read_surface : 0u);
        } else {
            set_egl_error(kEglBadParameter);
            f.ret(0);
        }
    });
    fn("eglWaitGL", 0, [](HleCallFrame& f) { glFinish(); f.ret(kEglTrue); });
    fn("eglWaitNative", 1, [](HleCallFrame& f) {
        (void)f.args.next_i32();
        f.ret(kEglTrue);
    });
    // Offscreen surfaces. Hit & Myth asks for both. Backed by the same single
    // surface as the window: there is one framebuffer here, and a game that
    // renders offscreen then blits is still rendering to the thing we present.
    fn("eglCreatePbufferSurface", 3, [](HleCallFrame& f) {
        const u32 display = f.args.next_u32();
        const u32 config = f.args.next_u32();
        (void)f.args.next_ptr();  // attribute list
        if (display != kEglDisplay) {
            set_egl_error(kEglBadDisplay);
            f.ret(0);
            return;
        }
        if (!require_egl_initialized(f)) return;
        if (config != kEglConfig) {
            set_egl_error(kEglBadConfig);
            f.ret(0);
            return;
        }
        gl_state().egl_surface_created = true;
        f.ret(kEglSurface);
    });
    fn("eglCreatePixmapSurface", 4, [](HleCallFrame& f) {
        const u32 display = f.args.next_u32();
        const u32 config = f.args.next_u32();
        (void)f.args.next_u32();  // native pixmap
        (void)f.args.next_ptr();
        if (display != kEglDisplay) {
            set_egl_error(kEglBadDisplay);
            f.ret(0);
            return;
        }
        if (!require_egl_initialized(f)) return;
        if (config != kEglConfig) {
            set_egl_error(kEglBadConfig);
            f.ret(0);
            return;
        }
        gl_state().egl_surface_created = true;
        f.ret(kEglSurface);
    });

    fn("eglQueryContext", 4, [](HleCallFrame& f) {
        const u32 display = f.args.next_u32();
        const u32 context = f.args.next_u32();
        const i32 attribute = f.args.next_i32();
        const GuestAddr out = f.args.next_ptr();
        if (display != kEglDisplay) {
            set_egl_error(kEglBadDisplay);
            f.ret(0);
            return;
        }
        if (!require_egl_initialized(f)) return;
        if (context != kEglContext || !gl_state().egl_context_created) {
            set_egl_error(kEglBadContext);
            f.ret(0);
            return;
        }
        if (out == kNullGuest) {
            set_egl_error(kEglBadParameter);
            f.ret(0);
            return;
        }
        i32 value = 0;
        if (attribute == 0x3028) {  // EGL_CONFIG_ID
            value = 1;
        } else if (attribute == 0x3097) {  // EGL_CONTEXT_CLIENT_TYPE
            value = 0x30a0;  // EGL_OPENGL_ES_API
        } else if (attribute == 0x3098) {  // EGL_CONTEXT_CLIENT_VERSION
            value = 1;
        } else {
            set_egl_error(kEglBadAttribute);
            f.ret(0);
            return;
        }
        if (!f.store<i32>(out, value)) {
            f.cpu.request_stop();
            return;
        }
        f.ret(kEglTrue);
    });

    fn("glGetError", 0, [](HleCallFrame& f) {
        // Usually GL_NO_ERROR, because the layer's own checking drains the
        // host error queue after each call. Games poll this to decide whether
        // a context is healthy, and "fine" is the honest answer when it is.
        if (gl_state().pending_gl_error != GL_NO_ERROR) {
            const GLenum error = gl_state().pending_gl_error;
            gl_state().pending_gl_error = GL_NO_ERROR;
            f.ret(static_cast<u32>(error));
            return;
        }
        f.ret(static_cast<u32>(glGetError()));
    });

    fn("eglMakeCurrent", 4, [](HleCallFrame& f) {
        const u32 display = f.args.next_u32();
        const u32 draw = f.args.next_u32();
        const u32 read = f.args.next_u32();
        const u32 context = f.args.next_u32();
        if (display != kEglDisplay) {
            set_egl_error(kEglBadDisplay);
            f.ret(0);
            return;
        }
        if (!require_egl_initialized(f)) return;
        if (context != 0 &&
            (context != kEglContext || !gl_state().egl_context_created)) {
            set_egl_error(kEglBadContext);
            f.ret(0);
            return;
        }
        if (draw != 0 &&
            (draw != kEglSurface || !gl_state().egl_surface_created)) {
            set_egl_error(kEglBadSurface);
            f.ret(0);
            return;
        }
        if (read != 0 &&
            (read != kEglSurface || !gl_state().egl_surface_created)) {
            set_egl_error(kEglBadSurface);
            f.ret(0);
            return;
        }
        if ((context == 0 && (draw != 0 || read != 0)) ||
            (context != 0 && (draw == 0 || read == 0))) {
            set_egl_error(kEglBadMatch);
            f.ret(0);
            return;
        }
        gl_state().egl_context_current = context != 0;
        gl_state().egl_draw_surface = draw;
        gl_state().egl_read_surface = read;
        f.ret(kEglTrue);
    });

    fn("eglQuerySurface", 4, [](HleCallFrame& f) {
        const u32 display = f.args.next_u32();
        const u32 surface = f.args.next_u32();
        const i32 attribute = f.args.next_i32();
        const GuestAddr value = f.args.next_ptr();
        if (display != kEglDisplay) {
            set_egl_error(kEglBadDisplay);
            f.ret(0);
            return;
        }
        if (!require_egl_initialized(f)) return;
        if (surface != kEglSurface || !gl_state().egl_surface_created) {
            set_egl_error(kEglBadSurface);
            f.ret(0);
            return;
        }
        if (value == kNullGuest) {
            set_egl_error(kEglBadParameter);
            f.ret(0);
            return;
        }
        i32 result = 0;
        if (attribute == kEglWidth) {
            result = kScreenWidth;
        } else if (attribute == kEglHeight) {
            result = kScreenHeight;
        } else {
            set_egl_error(kEglBadAttribute);
            f.ret(0);
            return;
        }
        if (!f.store<i32>(value, result)) {
            f.cpu.request_stop();
            return;
        }
        f.ret(kEglTrue);
    });

    fn("eglSwapBuffers", 2, [](HleCallFrame& f) {
        const u32 display = f.args.next_u32();
        const u32 surface = f.args.next_u32();
        if (display != kEglDisplay) {
            set_egl_error(kEglBadDisplay);
            f.ret(0);
            return;
        }
        if (!require_egl_initialized(f)) return;
        if (surface != kEglSurface || !gl_state().egl_surface_created) {
            set_egl_error(kEglBadSurface);
            f.ret(0);
            return;
        }
        gles_present_frame(f);
        f.ret(kEglTrue);
    });

    fn("eglSwapIntervalNV", 2, [](HleCallFrame& f) {
        (void)f.args.next_u32();
        (void)f.args.next_i32();
        f.ret(kEglTrue);
    });

    fn("eglDestroySurface", 2, [](HleCallFrame& f) {
        const u32 display = f.args.next_u32();
        const u32 surface = f.args.next_u32();
        if (display != kEglDisplay) {
            set_egl_error(kEglBadDisplay);
            f.ret(0);
            return;
        }
        if (!require_egl_initialized(f)) return;
        if (surface != kEglSurface || !gl_state().egl_surface_created) {
            set_egl_error(kEglBadSurface);
            f.ret(0);
            return;
        }
        gl_state().egl_surface_created = false;
        if (gl_state().egl_draw_surface == surface) gl_state().egl_draw_surface = 0;
        if (gl_state().egl_read_surface == surface) gl_state().egl_read_surface = 0;
        gl_state().surface_ready = false;
        f.ret(kEglTrue);
    });

    fn("eglDestroyContext", 2, [](HleCallFrame& f) {
        const u32 display = f.args.next_u32();
        const u32 context = f.args.next_u32();
        if (display != kEglDisplay) {
            set_egl_error(kEglBadDisplay);
            f.ret(0);
            return;
        }
        if (!require_egl_initialized(f)) return;
        if (context != kEglContext || !gl_state().egl_context_created) {
            set_egl_error(kEglBadContext);
            f.ret(0);
            return;
        }
        gl_state().egl_context_created = false;
        gl_state().egl_context_current = false;
        gl_state().egl_draw_surface = 0;
        gl_state().egl_read_surface = 0;
        f.ret(kEglTrue);
    });

    // --- Frame and global state ---------------------------------------------

    fn("glClear", 1, [](HleCallFrame& f) {
        glClear(static_cast<GLbitfield>(f.args.next_u32()));
        f.ret_void();
    });

    fn("glClearColorx", 4, [](HleCallFrame& f) {
        const float r = fixed_to_float(f.args.next_u32());
        const float g = fixed_to_float(f.args.next_u32());
        const float b = fixed_to_float(f.args.next_u32());
        const float a = fixed_to_float(f.args.next_u32());
        glClearColor(r, g, b, a);
        f.ret_void();
    });

    fn("glViewport", 4, [](HleCallFrame& f) {
        const i32 x = f.args.next_i32();
        const i32 y = f.args.next_i32();
        const i32 w = f.args.next_i32();
        const i32 h = f.args.next_i32();
        const i32 s = gl_state().scale;
        glViewport(x * s, y * s, w * s, h * s);
        f.ret_void();
    });

    fn("glScissor", 4, [](HleCallFrame& f) {
        const i32 x = f.args.next_i32();
        const i32 y = f.args.next_i32();
        const i32 w = f.args.next_i32();
        const i32 h = f.args.next_i32();
        const i32 s = gl_state().scale;
        if (std::getenv("CLICKHLE_DUMP_SCISSOR") != nullptr) {
            CLICKHLE_INFO(kCategory) << "glScissor(" << x << ", " << y << ", "
                                     << w << ", " << h << ")";
        }
        glScissor(x * s, y * s, w * s, h * s);
        f.ret_void();
    });

    fn("glEnable", 1, [](HleCallFrame& f) {
        const GLenum cap = static_cast<GLenum>(f.args.next_u32());
        if (std::getenv("CLICKHLE_DUMP_SCISSOR") != nullptr) {
            CLICKHLE_INFO(kCategory) << "glEnable(0x" << std::hex << cap
                                     << std::dec << ")";
        }
        glEnable(cap);
        f.ret_void();
    });

    fn("glDisable", 1, [](HleCallFrame& f) {
        const GLenum cap = static_cast<GLenum>(f.args.next_u32());
        if (std::getenv("CLICKHLE_DUMP_SCISSOR") != nullptr) {
            CLICKHLE_INFO(kCategory) << "glDisable(0x" << std::hex << cap
                                     << std::dec << ")";
        }
        glDisable(cap);
        f.ret_void();
    });

    fn("glFlush", 0, [](HleCallFrame& f) { glFlush(); f.ret_void(); });
    fn("glFinish", 0, [](HleCallFrame& f) { glFinish(); f.ret_void(); });

    fn("glAlphaFuncx", 2, [](HleCallFrame& f) {
        const auto func = static_cast<GLenum>(f.args.next_u32());
        glAlphaFunc(func, fixed_to_float(f.args.next_u32()));
        f.ret_void();
    });

    fn("glAlphaFunc", 2, [](HleCallFrame& f) {
        const auto func = static_cast<GLenum>(f.args.next_u32());
        glAlphaFunc(func, f.args.next_float());
        f.ret_void();
    });

    fn("glBlendFunc", 2, [](HleCallFrame& f) {
        const auto source = static_cast<GLenum>(f.args.next_u32());
        glBlendFunc(source, static_cast<GLenum>(f.args.next_u32()));
        f.ret_void();
    });

    fn("glDepthFunc", 1, [](HleCallFrame& f) {
        glDepthFunc(static_cast<GLenum>(f.args.next_u32()));
        f.ret_void();
    });

    fn("glDepthMask", 1, [](HleCallFrame& f) {
        glDepthMask(f.args.next_u32() != 0 ? GL_TRUE : GL_FALSE);
        f.ret_void();
    });

    fn("glCullFace", 1, [](HleCallFrame& f) {
        glCullFace(static_cast<GLenum>(f.args.next_u32()));
        f.ret_void();
    });

    fn("glColor4x", 4, [](HleCallFrame& f) {
        const float r = fixed_to_float(f.args.next_u32());
        const float g = fixed_to_float(f.args.next_u32());
        const float b = fixed_to_float(f.args.next_u32());
        const float a = fixed_to_float(f.args.next_u32());
        glColor4f(r, g, b, a);
        f.ret_void();
    });

    // --- Matrices ------------------------------------------------------------

    fn("glMatrixMode", 1, [](HleCallFrame& f) {
        glMatrixMode(static_cast<GLenum>(f.args.next_u32()));
        f.ret_void();
    });

    fn("glLoadIdentity", 0, [](HleCallFrame& f) {
        glLoadIdentity();
        f.ret_void();
    });

    fn("glOrthox", 6, [](HleCallFrame& f) {
        const double l = fixed_to_float(f.args.next_u32());
        const double r = fixed_to_float(f.args.next_u32());
        const double b = fixed_to_float(f.args.next_u32());
        const double t = fixed_to_float(f.args.next_u32());
        const double n = fixed_to_float(f.args.next_u32());
        const double fa = fixed_to_float(f.args.next_u32());
        glOrtho(l, r, b, t, n, fa);
        f.ret_void();
    });

    // --- The rest of the ES 1.0 common profile --------------------------------
    //
    // Filled in against the export table of the real nVidia libGLES_CM.dll that
    // shipped in the SDK, rather than against guesswork about what games might
    // call: it exports 168 entry points, and diffing that list against this
    // file is an exact statement of what is left. Catapult was blocked on
    // glClearDepthx, which is how the audit started.
    //
    // The f/x pairs are the same function twice over: ES has no FPU-dependent
    // API, so every float entry point has a 16.16 fixed-point twin. Only the
    // conversion differs, so they share an implementation wherever possible.

    const auto unary_float = [&fn](const char* fname, const char* xname,
                                   void (*apply)(float)) {
        fn(fname, 1, [apply](HleCallFrame& f) {
            apply(f.args.next_float());
            f.ret_void();
        });
        fn(xname, 1, [apply](HleCallFrame& f) {
            apply(fixed_to_float(f.args.next_u32()));
            f.ret_void();
        });
    };
    unary_float("glClearDepthf", "glClearDepthx",
                [](float d) { glClearDepth(static_cast<double>(d)); });
    // NOT via unary_float: ES 1.0 gives these no "f" form. The float entry
    // point keeps the base name and only the fixed-point variant is suffixed,
    // so pairing them mechanically minted glPointSizef and glLineWidthf --
    // names the real driver does not export and no binary can ever import.
    // Dead registrations inflate the coverage denominator, which matters now
    // that coverage is a dashboard. The x forms are registered further down.
    fn("glPointSize", 1, [](HleCallFrame& f) {
        glPointSize(f.args.next_float());
        f.ret_void();
    });
    fn("glLineWidth", 1, [](HleCallFrame& f) {
        glLineWidth(f.args.next_float());
        f.ret_void();
    });
    unary_float("glSampleCoverage", "glSampleCoveragex", [](float value) {
        glSampleCoverage(value, GL_FALSE);
    });

    fn("glClearStencil", 1, [](HleCallFrame& f) {
        glClearStencil(f.args.next_i32());
        f.ret_void();
    });

    fn("glColorMask", 4, [](HleCallFrame& f) {
        const auto r = static_cast<GLboolean>(f.args.next_u32() != 0);
        const auto g = static_cast<GLboolean>(f.args.next_u32() != 0);
        const auto b = static_cast<GLboolean>(f.args.next_u32() != 0);
        glColorMask(r, g, b, static_cast<GLboolean>(f.args.next_u32() != 0));
        f.ret_void();
    });

    fn("glColor4f", 4, [](HleCallFrame& f) {
        const float r = f.args.next_float();
        const float g = f.args.next_float();
        const float b = f.args.next_float();
        glColor4f(r, g, b, f.args.next_float());
        f.ret_void();
    });

    fn("glDepthRangex", 2, [](HleCallFrame& f) {
        const float near_plane = fixed_to_float(f.args.next_u32());
        glDepthRange(near_plane, fixed_to_float(f.args.next_u32()));
        f.ret_void();
    });

    fn("glFrustumx", 6, [](HleCallFrame& f) {
        const float l = fixed_to_float(f.args.next_u32());
        const float r = fixed_to_float(f.args.next_u32());
        const float b = fixed_to_float(f.args.next_u32());
        const float t = fixed_to_float(f.args.next_u32());
        const float n = fixed_to_float(f.args.next_u32());
        glFrustum(l, r, b, t, n, fixed_to_float(f.args.next_u32()));
        f.ret_void();
    });

    fn("glOrthof", 6, [](HleCallFrame& f) {
        const float l = f.args.next_float();
        const float r = f.args.next_float();
        const float b = f.args.next_float();
        const float t = f.args.next_float();
        const float n = f.args.next_float();
        glOrtho(l, r, b, t, n, f.args.next_float());
        f.ret_void();
    });

    fn("glHint", 2, [](HleCallFrame& f) {
        const auto target = static_cast<GLenum>(f.args.next_u32());
        glHint(target, static_cast<GLenum>(f.args.next_u32()));
        f.ret_void();
    });

    fn("glShadeModel", 1, [](HleCallFrame& f) {
        glShadeModel(static_cast<GLenum>(f.args.next_u32()));
        f.ret_void();
    });

    fn("glLogicOp", 1, [](HleCallFrame& f) {
        glLogicOp(static_cast<GLenum>(f.args.next_u32()));
        f.ret_void();
    });

    fn("glPixelStorei", 2, [](HleCallFrame& f) {
        const auto pname = static_cast<GLenum>(f.args.next_u32());
        glPixelStorei(pname, f.args.next_i32());
        f.ret_void();
    });

    fn("glPolygonOffset", 2, [](HleCallFrame& f) {
        const float factor = f.args.next_float();
        glPolygonOffset(factor, f.args.next_float());
        f.ret_void();
    });

    // Stencil.
    fn("glStencilFunc", 3, [](HleCallFrame& f) {
        const auto func = static_cast<GLenum>(f.args.next_u32());
        const i32 ref = f.args.next_i32();
        glStencilFunc(func, ref, f.args.next_u32());
        f.ret_void();
    });
    fn("glStencilMask", 1, [](HleCallFrame& f) {
        glStencilMask(f.args.next_u32());
        f.ret_void();
    });
    fn("glStencilOp", 3, [](HleCallFrame& f) {
        const auto sfail = static_cast<GLenum>(f.args.next_u32());
        const auto zfail = static_cast<GLenum>(f.args.next_u32());
        glStencilOp(sfail, zfail, static_cast<GLenum>(f.args.next_u32()));
        f.ret_void();
    });

    // Transforms in float form; the fixed-point twins already exist below.
    fn("glTranslatef", 3, [](HleCallFrame& f) {
        const float x = f.args.next_float();
        const float y = f.args.next_float();
        glTranslatef(x, y, f.args.next_float());
        f.ret_void();
    });
    fn("glScalef", 3, [](HleCallFrame& f) {
        const float x = f.args.next_float();
        const float y = f.args.next_float();
        glScalef(x, y, f.args.next_float());
        f.ret_void();
    });
    fn("glRotatef", 4, [](HleCallFrame& f) {
        const float angle = f.args.next_float();
        const float x = f.args.next_float();
        const float y = f.args.next_float();
        glRotatef(angle, x, y, f.args.next_float());
        f.ret_void();
    });
    fn("glNormal3f", 3, [](HleCallFrame& f) {
        const float x = f.args.next_float();
        const float y = f.args.next_float();
        glNormal3f(x, y, f.args.next_float());
        f.ret_void();
    });
    fn("glNormal3x", 3, [](HleCallFrame& f) {
        const float x = fixed_to_float(f.args.next_u32());
        const float y = fixed_to_float(f.args.next_u32());
        glNormal3f(x, y, fixed_to_float(f.args.next_u32()));
        f.ret_void();
    });

    // Matrices supplied as 16 floats, straight through.
    const auto matrix_float = [](HleCallFrame& f, void (*apply)(const float*)) {
        const GuestAddr m = f.args.next_ptr();
        float host[16];
        for (u32 i = 0; i < 16; ++i) {
            host[i] = endian::bits_to_float(
                f.memory.read<u32>(m + i * 4).value_or(0));
        }
        apply(host);
        f.ret_void();
    };
    fn("glLoadMatrixf", 1, [matrix_float](HleCallFrame& f) {
        matrix_float(f, [](const float* m) { glLoadMatrixf(m); });
    });
    fn("glMultMatrixf", 1, [matrix_float](HleCallFrame& f) {
        matrix_float(f, [](const float* m) { glMultMatrixf(m); });
    });

    fn("glMultiTexCoord4f", 5, [](HleCallFrame& f) {
        const auto unit = static_cast<GLenum>(f.args.next_u32());
        const float s = f.args.next_float();
        const float t = f.args.next_float();
        const float r = f.args.next_float();
        glMultiTexCoord4f(unit, s, t, r, f.args.next_float());
        f.ret_void();
    });
    fn("glMultiTexCoord4x", 5, [](HleCallFrame& f) {
        const auto unit = static_cast<GLenum>(f.args.next_u32());
        const float s = fixed_to_float(f.args.next_u32());
        const float t = fixed_to_float(f.args.next_u32());
        const float r = fixed_to_float(f.args.next_u32());
        glMultiTexCoord4f(unit, s, t, r, fixed_to_float(f.args.next_u32()));
        f.ret_void();
    });

    fn("glNormalPointer", 3, [](HleCallFrame& f) {
        GlState& state = gl_state();
        state.normal.size = 3;  // always three components
        state.normal.type = f.args.next_u32();
        state.normal.stride = f.args.next_i32();
        state.normal.pointer = f.args.next_ptr();
        f.ret_void();
    });

    // Lighting and material. Every one takes an enum, then either a scalar or
    // a vector; the enums are unscaled even on the x forms, only values are
    // 16.16 -- the trap that OES_fixed_point sets for you.
    const auto light_scalar = [&fn](const char* fname, const char* xname,
                                    bool per_light) {
        fn(fname, per_light ? 3 : 2, [per_light](HleCallFrame& f) {
            const auto a = static_cast<GLenum>(f.args.next_u32());
            if (per_light) {
                const auto pname = static_cast<GLenum>(f.args.next_u32());
                glLightf(a, pname, f.args.next_float());
            } else {
                glLightModelf(a, f.args.next_float());
            }
            f.ret_void();
        });
        fn(xname, per_light ? 3 : 2, [per_light](HleCallFrame& f) {
            const auto a = static_cast<GLenum>(f.args.next_u32());
            if (per_light) {
                const auto pname = static_cast<GLenum>(f.args.next_u32());
                glLightf(a, pname, fixed_to_float(f.args.next_u32()));
            } else {
                glLightModelf(a, fixed_to_float(f.args.next_u32()));
            }
            f.ret_void();
        });
    };
    light_scalar("glLightf", "glLightx", true);
    light_scalar("glLightModelf", "glLightModelx", false);

    fn("glMaterialf", 3, [](HleCallFrame& f) {
        const auto face = static_cast<GLenum>(f.args.next_u32());
        const auto pname = static_cast<GLenum>(f.args.next_u32());
        glMaterialf(face, pname, f.args.next_float());
        f.ret_void();
    });
    fn("glMaterialx", 3, [](HleCallFrame& f) {
        const auto face = static_cast<GLenum>(f.args.next_u32());
        const auto pname = static_cast<GLenum>(f.args.next_u32());
        glMaterialf(face, pname, fixed_to_float(f.args.next_u32()));
        f.ret_void();
    });

    // How many values a parameter takes. Reading four for a scalar parameter
    // walks off the end of whatever the guest passed.
    const auto param_count = [](GLenum pname) -> u32 {
        switch (pname) {
            case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR:
            case GL_EMISSION: case GL_AMBIENT_AND_DIFFUSE:
            case GL_POSITION: case GL_LIGHT_MODEL_AMBIENT:
                return 4;
            case GL_SPOT_DIRECTION:
                return 3;
            default:
                return 1;
        }
    };
    const auto vector_param = [&fn, param_count](const char* fname,
                                                 const char* xname, int kind) {
        // kind: 0 light, 1 light model, 2 material, 3 texenv
        const auto body = [param_count, kind](HleCallFrame& f, bool fixed) {
            const auto a = static_cast<GLenum>(f.args.next_u32());
            const auto pname = kind == 1
                                   ? a
                                   : static_cast<GLenum>(f.args.next_u32());
            const GuestAddr params = f.args.next_ptr();
            float values[4] = {0, 0, 0, 0};
            const u32 count = param_count(kind == 1 ? a : pname);
            for (u32 i = 0; i < count; ++i) {
                const u32 raw = f.memory.read<u32>(params + i * 4).value_or(0);
                values[i] = fixed ? fixed_to_float(raw)
                                  : endian::bits_to_float(raw);
            }
            switch (kind) {
                case 0: glLightfv(a, pname, values); break;
                case 1: glLightModelfv(a, values); break;
                case 2: glMaterialfv(a, pname, values); break;
                default: glTexEnvfv(a, pname, values); break;
            }
            f.ret_void();
        };
        const u8 arity = kind == 1 ? 2 : 3;
        fn(fname, arity, [body](HleCallFrame& f) { body(f, false); });
        fn(xname, arity, [body](HleCallFrame& f) { body(f, true); });
    };
    vector_param("glLightfv", "glLightxv", 0);
    vector_param("glLightModelfv", "glLightModelxv", 1);
    vector_param("glMaterialfv", "glMaterialxv", 2);
    vector_param("glTexEnvfv", "glTexEnvxv", 3);

    fn("glCopyTexImage2D", 8, [](HleCallFrame& f) {
        const auto target = static_cast<GLenum>(f.args.next_u32());
        const i32 level = f.args.next_i32();
        const auto format = static_cast<GLenum>(f.args.next_u32());
        const i32 x = f.args.next_i32();
        const i32 y = f.args.next_i32();
        const i32 w = f.args.next_i32();
        const i32 h = f.args.next_i32();
        glCopyTexImage2D(target, level, format, x, y, w, h, f.args.next_i32());
        f.ret_void();
    });

    fn("glCopyTexSubImage2D", 8, [](HleCallFrame& f) {
        const auto target = static_cast<GLenum>(f.args.next_u32());
        const i32 level = f.args.next_i32();
        const i32 xoff = f.args.next_i32();
        const i32 yoff = f.args.next_i32();
        const i32 x = f.args.next_i32();
        const i32 y = f.args.next_i32();
        const i32 w = f.args.next_i32();
        glCopyTexSubImage2D(target, level, xoff, yoff, x, y, w,
                            f.args.next_i32());
        f.ret_void();
    });

    // --- Matrix stack --------------------------------------------------------
    //
    // Absent until the collection grew past the first six games, none of which
    // ever pushed a matrix -- they set the modelview outright for each object.
    // Anything with a scene graph pushes and pops instead, so the Johnny
    // Whatever demo stopped here.

    fn("glPushMatrix", 0, [](HleCallFrame& f) {
        glPushMatrix();
        f.ret_void();
    });

    fn("glPopMatrix", 0, [](HleCallFrame& f) {
        glPopMatrix();
        f.ret_void();
    });

    fn("glMultMatrixx", 1, [](HleCallFrame& f) {
        const GuestAddr m = f.args.next_ptr();
        float host[16];
        for (u32 i = 0; i < 16; ++i) {
            host[i] = fixed_to_float(f.memory.read<u32>(m + i * 4).value_or(0));
        }
        glMultMatrixf(host);
        f.ret_void();
    });

    // Frustum and ortho take floats here, not 16.16 -- the f suffix is the
    // giveaway. Passing these through fixed_to_float would flatten the scene.
    fn("glFrustumf", 6, [](HleCallFrame& f) {
        const float l = f.args.next_float();
        const float r = f.args.next_float();
        const float b = f.args.next_float();
        const float t = f.args.next_float();
        const float n = f.args.next_float();
        // Desktop GL spells it glFrustum and takes doubles; ES has no such
        // entry point. Widening is exact, so this is a rename, not a change.
        glFrustum(l, r, b, t, n, f.args.next_float());
        f.ret_void();
    });

    fn("glFrontFace", 1, [](HleCallFrame& f) {
        glFrontFace(static_cast<GLenum>(f.args.next_u32()));
        f.ret_void();
    });

    // Enums are unscaled even on the x entry points; only the value is 16.16.
    fn("glTexEnvx", 3, [](HleCallFrame& f) {
        const GLenum target = static_cast<GLenum>(f.args.next_u32());
        const GLenum pname = static_cast<GLenum>(f.args.next_u32());
        glTexEnvi(target, pname, static_cast<GLint>(f.args.next_i32()));
        f.ret_void();
    });

    fn("glLineWidthx", 1, [](HleCallFrame& f) {
        glLineWidth(fixed_to_float(f.args.next_u32()));
        f.ret_void();
    });

    fn("glPointSizex", 1, [](HleCallFrame& f) {
        glPointSize(fixed_to_float(f.args.next_u32()));
        f.ret_void();
    });

    fn("glPolygonOffsetx", 2, [](HleCallFrame& f) {
        const float factor = fixed_to_float(f.args.next_u32());
        glPolygonOffset(factor, fixed_to_float(f.args.next_u32()));
        f.ret_void();
    });

    fn("glFogxv", 2, [](HleCallFrame& f) {
        const GLenum pname = static_cast<GLenum>(f.args.next_u32());
        const GuestAddr params = f.args.next_ptr();
        // GL_FOG_MODE names an enum; everything else is a 16.16 value.
        if (pname == GL_FOG_MODE) {
            const i32 mode = f.memory.read<i32>(params).value_or(GL_EXP);
            glFogi(pname, mode);
        } else {
            float values[4] = {0, 0, 0, 0};
            const u32 count = (pname == GL_FOG_COLOR) ? 4u : 1u;
            for (u32 i = 0; i < count; ++i) {
                values[i] =
                    fixed_to_float(f.memory.read<u32>(params + i * 4).value_or(0));
            }
            glFogfv(pname, values);
        }
        f.ret_void();
    });

    fn("glTranslatex", 3, [](HleCallFrame& f) {
        const float x = fixed_to_float(f.args.next_u32());
        const float y = fixed_to_float(f.args.next_u32());
        glTranslatef(x, y, fixed_to_float(f.args.next_u32()));
        f.ret_void();
    });

    fn("glRotatex", 4, [](HleCallFrame& f) {
        const float angle = fixed_to_float(f.args.next_u32());
        const float x = fixed_to_float(f.args.next_u32());
        const float y = fixed_to_float(f.args.next_u32());
        glRotatef(angle, x, y, fixed_to_float(f.args.next_u32()));
        f.ret_void();
    });

    fn("glScalex", 3, [](HleCallFrame& f) {
        const u32 rx = f.args.next_u32();
        const u32 ry = f.args.next_u32();
        const u32 rz = f.args.next_u32();
        glScalef(fixed_to_float(rx), fixed_to_float(ry), fixed_to_float(rz));
        f.ret_void();
    });

    fn("glLoadMatrixx", 1, [](HleCallFrame& f) {
        const GuestAddr source = f.args.next_ptr();
        float matrix[16];
        for (u32 i = 0; i < 16; ++i) {
            matrix[i] = fixed_to_float(
                f.memory.read<u32>(source + i * 4).value_or(0));
        }
        glLoadMatrixf(matrix);
        f.ret_void();
    });

    // --- Client arrays -------------------------------------------------------
    //
    // These only record what the guest declared. The data lives in guest memory
    // and is not read until a draw call, so resolving the pointer now would be
    // both wrong and unsafe.

    fn("glEnableClientState", 1, [](HleCallFrame& f) {
        if (ArrayPointer* array = array_for(f.args.next_u32())) {
            array->enabled = true;
        }
        f.ret_void();
    });

    fn("glDisableClientState", 1, [](HleCallFrame& f) {
        if (ArrayPointer* array = array_for(f.args.next_u32())) {
            array->enabled = false;
        }
        f.ret_void();
    });

    const auto record_array = [](HleCallFrame& f, ArrayPointer& array) {
        array.size = f.args.next_i32();
        array.type = f.args.next_u32();
        array.stride = f.args.next_i32();
        array.pointer = f.args.next_ptr();
        f.ret_void();
    };

    fn("glVertexPointer", 4, [record_array](HleCallFrame& f) {
        record_array(f, gl_state().vertex);
    });
    fn("glColorPointer", 4, [record_array](HleCallFrame& f) {
        record_array(f, gl_state().colour);
    });
    fn("glTexCoordPointer", 4, [record_array](HleCallFrame& f) {
        record_array(f, gl_state().texcoord[gl_state().client_active_unit]);
    });

    fn("glClientActiveTexture", 1, [](HleCallFrame& f) {
        const u32 unit = f.args.next_u32() - GL_TEXTURE0;
        if (unit < kMaxTextureUnits) {
            gl_state().client_active_unit = unit;
            glClientActiveTexture(GL_TEXTURE0 + unit);
        }
        f.ret_void();
    });

    fn("glActiveTexture", 1, [](HleCallFrame& f) {
        const u32 unit = f.args.next_u32() - GL_TEXTURE0;
        if (unit < kMaxTextureUnits) {
            glActiveTexture(GL_TEXTURE0 + unit);
        }
        f.ret_void();
    });

    // --- Drawing -------------------------------------------------------------

    fn("glDrawArrays", 3, [](HleCallFrame& f) {
        const auto mode = static_cast<GLenum>(f.args.next_u32());
        const i32 first = f.args.next_i32();
        const i32 count = f.args.next_i32();
        if (count <= 0) {
            f.ret_void();
            return;
        }
        apply_arrays(f, static_cast<u32>(first + count));
        // Isolation switch: render only draws that use one texture. A frame
        // can hide a draw by overdrawing it, and that is indistinguishable
        // from a draw that produces nothing at all.
        static const char* only = getenv("CLICKHLE_ONLY_TEXTURE");
        if (only == nullptr ||
            gl_state().bound_texture == static_cast<u32>(atoi(only))) {
            glDrawArrays(mode, first, count);
        }
        ++gl_state().draws;
        if (getenv("CLICKHLE_DUMP_BOUND") != nullptr) {
            CLICKHLE_INFO(kCategory) << "boundtex " << gl_state().bound_texture;
        }
        // Same sanity readback as glDrawElements -- used to cross-check the
        // methodology itself against a title known to render correctly.
        if (getenv("CLICKHLE_DUMP_READBACK") != nullptr) {
            static u32 seen = 0;
            ++seen;
            if (seen % 200 == 0) {
                GLubyte px[3] = {99, 99, 99};
                glReadPixels(160, 120, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);
                GLfloat cc[4] = {-1, -1, -1, -1};
                glGetFloatv(GL_CURRENT_COLOR, cc);
                GLint tev = 0;
                glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &tev);
                CLICKHLE_INFO(kCategory)
                    << "post-drawArrays readback (160,120): bound="
                    << gl_state().bound_texture << " n=" << count << " rgb=("
                    << static_cast<int>(px[0]) << ","
                    << static_cast<int>(px[1]) << ","
                    << static_cast<int>(px[2]) << ")"
                    << " curColor=" << cc[0] << "," << cc[1] << "," << cc[2]
                    << "," << cc[3] << " texenv=0x" << std::hex << tev
                    << std::dec << " colourArray="
                    << (gl_state().colour.size != 0 ? 1 : 0)
                    << " lighting=" << (glIsEnabled(GL_LIGHTING) ? 1 : 0);
            }
        }
        // Per-draw geometry dump, for working out which quad lands where.
        //
        // The screenshot only shows the composite, and inferring geometry from
        // a composite is how two wrong theories about the banding got proposed.
        // This prints, for one texture, the actual 16.16 vertex and texcoord
        // extents of every draw, so "which region of the atlas does the top
        // band sample" is a measurement rather than a guess.
        if (const char* want = getenv("CLICKHLE_DUMP_DRAWS")) {
            // Accepts a single id, a comma-separated list, or "all". One id at
            // a time meant one full run per texture; comparing where several
            // layers land -- which is how Royal Solitaire's projection
            // mismatch was found -- needs them from the SAME run, in draw
            // order, or the comparison is between different frames.
            const u32 bound = gl_state().bound_texture;
            bool wanted = std::strcmp(want, "all") == 0;
            if (!wanted) {
                for (const char* p = want; *p != '\0';) {
                    const u32 id = static_cast<u32>(std::strtoul(p, nullptr, 10));
                    if (id == bound) {
                        wanted = true;
                        break;
                    }
                    while (*p != '\0' && *p != ',') ++p;
                    if (*p == ',') ++p;
                }
            }
            if (wanted) {
                const float* vp = scratch().vertex.data();
                const float* tp = scratch().texcoord[0].data();
                const u32 vc = static_cast<u32>(
                    std::max<i32>(0, gl_state().vertex.size));
                const u32 tc = static_cast<u32>(
                    std::max<i32>(0, gl_state().texcoord[0].size));
                double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
                double u0 = 1e9, u1 = -1e9, v0 = 1e9, v1 = -1e9;
                for (i32 i = first; i < first + count; ++i) {
                    const auto ui = static_cast<std::size_t>(i);
                    if (vc >= 2 && scratch().vertex.size() >= (ui + 1) * vc) {
                        x0 = std::min<double>(x0, vp[ui * vc + 0]);
                        x1 = std::max<double>(x1, vp[ui * vc + 0]);
                        y0 = std::min<double>(y0, vp[ui * vc + 1]);
                        y1 = std::max<double>(y1, vp[ui * vc + 1]);
                    }
                    if (tc >= 2 && scratch().texcoord[0].size() >= (ui + 1) * tc) {
                        u0 = std::min<double>(u0, tp[ui * tc + 0]);
                        u1 = std::max<double>(u1, tp[ui * tc + 0]);
                        v0 = std::min<double>(v0, tp[ui * tc + 1]);
                        v1 = std::max<double>(v1, tp[ui * tc + 1]);
                    }
                }
                // All FOUR position components. x/y alone hid the thing that
                // mattered: attrib0 is size=4, so z and especially w are live,
                // and a w != 1 divides the whole vertex.
                std::string comps;
                if (vc >= 1 && scratch().vertex.size() >= static_cast<std::size_t>(first + 1) * vc) {
                    char buf[128];
                    for (u32 c = 0; c < vc && c < 4; ++c) {
                        std::snprintf(buf, sizeof(buf), " v%u=%.4f", c,
                                      static_cast<double>(
                                          vp[static_cast<std::size_t>(first) * vc + c]));
                        comps += buf;
                    }
                }
                CLICKHLE_INFO(kCategory)
                    << "draw tex " << gl_state().bound_texture << " n=" << count
                    << " size=" << vc
                    << " xy=[" << x0 << "," << x1 << "]x[" << y0 << "," << y1
                    << "] uv=[" << u0 << "," << u1 << "]x[" << v0 << "," << v1
                    << "] first_vert:" << comps;
                // The matrices ACTUALLY in effect for this draw, read back from
                // GL rather than from our own bookkeeping -- our bookkeeping is
                // what is under suspicion.
                GLfloat mv[16], pr[16], tx[16];
                glGetFloatv(GL_MODELVIEW_MATRIX, mv);
                glGetFloatv(GL_PROJECTION_MATRIX, pr);
                glGetFloatv(GL_TEXTURE_MATRIX, tx);
                char tbuf[256];
                std::snprintf(tbuf, sizeof(tbuf),
                              "  TEX diag %.6f %.6f %.6f %.6f trans %.4f %.4f",
                              static_cast<double>(tx[0]), static_cast<double>(tx[5]),
                              static_cast<double>(tx[10]), static_cast<double>(tx[15]),
                              static_cast<double>(tx[12]), static_cast<double>(tx[13]));
                CLICKHLE_INFO(kCategory) << tbuf;
                GLint mn = 0, mg = 0, ws = 0;
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &mn);
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mg);
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &ws);
                char pbuf[192];
                std::snprintf(pbuf, sizeof(pbuf),
                              "  PARAMS min=0x%x mag=0x%x wrapS=0x%x  (want min/mag 0x2601, wrap 0x812f)",
                              mn, mg, ws);
                CLICKHLE_INFO(kCategory) << pbuf;
                // Is texturing even ON, and is a real texture bound? Read back
                // rather than assume -- the enable is done once on first bind
                // from a static flag, which is precisely the kind of "when"
                // bug the texture matrix turned out to be.
                char ebuf[192];
                std::snprintf(ebuf, sizeof(ebuf),
                              "  STATE tex2d_enabled=%d blend=%d bound=%u alpha_test=%d",
                              glIsEnabled(GL_TEXTURE_2D) ? 1 : 0,
                              glIsEnabled(GL_BLEND) ? 1 : 0,
                              gl_state().bound_texture,
                              glIsEnabled(GL_ALPHA_TEST) ? 1 : 0);
                CLICKHLE_INFO(kCategory) << ebuf;
                // Correlate the mode-selector byte LLE found (written by all
                // 5 of Texas Hold'em's draw functions before their #175/#125
                // pair) with which texture is actually drawing right now.
                // CLICKHLE_DUMP_MODE=1.
                if (getenv("CLICKHLE_DUMP_MODE") != nullptr) {
                    const u32 mode_word =
                        f.memory.read<u32>(0x180597b8).value_or(0xffffffff);
                    char modebuf[96];
                    std::snprintf(modebuf, sizeof(modebuf),
                                  "  MODE 0x%x at draw of texture %u", mode_word,
                                  gl_state().bound_texture);
                    CLICKHLE_INFO(kCategory) << modebuf;
                }
                char mbuf[512];
                std::snprintf(mbuf, sizeof(mbuf),
                              "  MV diag %.4f %.4f %.4f %.4f trans %.2f %.2f %.2f"
                              "  PR diag %.5f %.5f %.5f %.5f trans %.3f %.3f %.3f",
                              static_cast<double>(mv[0]), static_cast<double>(mv[5]),
                              static_cast<double>(mv[10]), static_cast<double>(mv[15]),
                              static_cast<double>(mv[12]), static_cast<double>(mv[13]),
                              static_cast<double>(mv[14]),
                              static_cast<double>(pr[0]), static_cast<double>(pr[5]),
                              static_cast<double>(pr[10]), static_cast<double>(pr[15]),
                              static_cast<double>(pr[12]), static_cast<double>(pr[13]),
                              static_cast<double>(pr[14]));
                CLICKHLE_INFO(kCategory) << mbuf;
                // Full 16 values, not just diag+trans -- a rotation is not
                // diagonal, and diag-only readback can't tell a rotated
                // transform from a garbled one. CLICKHLE_DUMP_DRAWS_FULL=1.
                if (getenv("CLICKHLE_DUMP_DRAWS_FULL") != nullptr) {
                    char full[400];
                    std::snprintf(full, sizeof(full),
                                  "  MV full %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | "
                                  "%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
                                  static_cast<double>(mv[0]), static_cast<double>(mv[1]),
                                  static_cast<double>(mv[2]), static_cast<double>(mv[3]),
                                  static_cast<double>(mv[4]), static_cast<double>(mv[5]),
                                  static_cast<double>(mv[6]), static_cast<double>(mv[7]),
                                  static_cast<double>(mv[8]), static_cast<double>(mv[9]),
                                  static_cast<double>(mv[10]), static_cast<double>(mv[11]),
                                  static_cast<double>(mv[12]), static_cast<double>(mv[13]),
                                  static_cast<double>(mv[14]), static_cast<double>(mv[15]));
                    CLICKHLE_INFO(kCategory) << full;
                }
            }
        }
        // Which texture each draw used. A frame can be busy and still show
        // nothing: Tetris issues 169 quads a frame at a title screen that never
        // changes, with three alpha font atlases uploaded, so the question
        // "is the text being drawn invisibly or not drawn at all?" needs an
        // answer the screenshot cannot give.
        ++gl_state().draws_by_texture[gl_state().bound_texture];
        f.ret_void();
    });

    fn("glDrawElements", 4, [](HleCallFrame& f) {
        const auto mode = static_cast<GLenum>(f.args.next_u32());
        const i32 count = f.args.next_i32();
        const u32 type = f.args.next_u32();
        const GuestAddr indices = f.args.next_ptr();
        if (count < 0) {
            set_gl_error(GL_INVALID_VALUE);
            f.ret_void();
            return;
        }
        if (count == 0 || indices == kNullGuest) {
            f.ret_void();
            return;
        }

        // The iPod firmware's index-copy path handles only GL_UNSIGNED_BYTE
        // and GL_UNSIGNED_SHORT. Treating every other enum as 16-bit both
        // under-checks guest memory and lets desktop GL read the buffer using
        // a different element width (notably for GL_UNSIGNED_INT).
        if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT) {
            set_gl_error(GL_INVALID_ENUM);
            f.ret_void();
            return;
        }

        // Indices have to be read host-side anyway to find how much of each
        // vertex array is live -- GL is about to dereference those arrays, and
        // an under-sized mapping check would be a wild read.
        const u32 index_bytes = type == GL_UNSIGNED_BYTE ? 1u : 2u;
        const std::byte* index_data = f.memory.host_ptr(
            indices, static_cast<u32>(count) * index_bytes);
        if (index_data == nullptr) {
            CLICKHLE_ERROR(kCategory) << "glDrawElements: unmapped index buffer";
            f.ret_void();
            return;
        }
        u32 highest = 0;
        for (i32 i = 0; i < count; ++i) {
            u32 index = 0;
            if (index_bytes == 1) {
                index = std::to_integer<u8>(index_data[i]);
            } else {
                u16 raw = 0;
                std::memcpy(&raw, index_data + i * 2, sizeof(raw));
                index = raw;
            }
            highest = std::max(highest, index);
        }

        apply_arrays(f, highest + 1);
        glDrawElements(mode, count, static_cast<GLenum>(type), index_data);
        ++gl_state().draws;
        // Decisive test: read the color buffer back RIGHT NOW, immediately
        // after this specific draw, before any later clear/present logic in
        // this frame or a subsequent one can touch it. If the screen centre
        // is real colour here but still black in the final screenshot, the
        // bug is downstream (a clear or overwrite between this draw and
        // present). If it is ALREADY black here, rasterisation itself is
        // producing nothing despite every state check passing.
        if (getenv("CLICKHLE_DUMP_READBACK") != nullptr) {
            static u32 seen = 0;
            ++seen;
            if (seen % 200 == 0) {
                GLubyte px[3] = {99, 99, 99};
                glReadPixels(160, 120, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px);
                GLint mn = 0, mg = 0, ws = 0, wt = 0;
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &mn);
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mg);
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &ws);
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wt);
                const auto max_level_it =
                    texture_max_level().find(gl_state().bound_texture);
                GLint logic_mode = 0;
                glGetIntegerv(GL_LOGIC_OP_MODE, &logic_mode);
                GLboolean color_mask[4] = {0, 0, 0, 0};
                glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
                // Full raw vertex attribute (not just xy) for index 0 -- a
                // live w != 1 divides the whole vertex in the fixed
                // pipeline's perspective divide, which a matrix-only check
                // can't see.
                const float* vp = scratch().vertex.data();
                const u32 vc = static_cast<u32>(gl_state().vertex.size);
                std::string comps;
                if (vc >= 1 && scratch().vertex.size() >= vc) {
                    char buf[48];
                    for (u32 c = 0; c < vc && c < 4; ++c) {
                        std::snprintf(buf, sizeof(buf), " v%u=%.4f", c,
                                      static_cast<double>(vp[c]));
                        comps += buf;
                    }
                }
                // The extent actually spanned by every vertex this draw
                // touches. Vertex 0 alone is not enough: a quad whose first
                // corner is legitimately (0,0) looks fine while the other
                // three are degenerate. With PR diag 0.00625 == 2/320, a
                // correct on-screen span must reach ~320x240; a span of a
                // fraction of a unit is a sub-pixel quad that writes nothing.
                double px0 = 1e9, px1 = -1e9, py0 = 1e9, py1 = -1e9;
                if (vc >= 2) {
                    for (u32 i = 0; i <= highest &&
                                    scratch().vertex.size() >= (i + 1) * vc;
                         ++i) {
                        px0 = std::min<double>(px0, vp[i * vc + 0]);
                        px1 = std::max<double>(px1, vp[i * vc + 0]);
                        py0 = std::min<double>(py0, vp[i * vc + 1]);
                        py1 = std::max<double>(py1, vp[i * vc + 1]);
                    }
                }
                CLICKHLE_INFO(kCategory)
                    << "post-draw readback (160,120): bound="
                    << gl_state().bound_texture << " n=" << count
                    << " rgb=(" << static_cast<int>(px[0]) << ","
                    << static_cast<int>(px[1]) << ","
                    << static_cast<int>(px[2]) << ") vsize=" << vc
                    << " vert0:" << comps << " min=0x" << std::hex << mn
                    << " mag=0x" << mg << " wrapS=0x" << ws << " wrapT=0x"
                    << wt << std::dec << " max_level="
                    << (max_level_it != texture_max_level().end()
                            ? max_level_it->second
                            : -999)
                    << " logicOpEnabled=" << (glIsEnabled(GL_COLOR_LOGIC_OP) ? 1 : 0)
                    << " logicMode=0x" << std::hex << logic_mode << std::dec
                    << " colorMask=" << static_cast<int>(color_mask[0])
                    << static_cast<int>(color_mask[1])
                    << static_cast<int>(color_mask[2])
                    << static_cast<int>(color_mask[3]) << " xy=[" << px0 << ","
                    << px1 << "]x[" << py0 << "," << py1 << "]";
            }
        }
        if (getenv("CLICKHLE_DUMP_ELEMENTS") != nullptr) {
            GLfloat mv[16], pr[16], tx[16];
            glGetFloatv(GL_MODELVIEW_MATRIX, mv);
            glGetFloatv(GL_PROJECTION_MATRIX, pr);
            glGetFloatv(GL_TEXTURE_MATRIX, tx);
            GLint src_blend = 0, dst_blend = 0;
            glGetIntegerv(GL_BLEND_SRC, &src_blend);
            glGetIntegerv(GL_BLEND_DST, &dst_blend);
            GLfloat cur_color[4] = {0, 0, 0, 0};
            glGetFloatv(GL_CURRENT_COLOR, cur_color);
            // Real texcoord extent actually touched by this draw's indices --
            // the texture MATRIX can look perfectly ordinary (identity, even)
            // while the raw texcoords it transforms are themselves degenerate
            // (e.g. every index sampling the same single texel), which is
            // exactly how Zuma's very first bug of this session hid: correct
            // matrix, wrong moment/values feeding it.
            const float* tp = scratch().texcoord[0].data();
            const u32 tc = static_cast<u32>(gl_state().texcoord[0].size);
            double u0 = 1e9, u1 = -1e9, v0 = 1e9, v1 = -1e9;
            if (tc >= 2) {
                for (u32 i = 0; i <= highest &&
                                scratch().texcoord[0].size() >= (i + 1) * tc;
                     ++i) {
                    u0 = std::min<double>(u0, tp[i * tc + 0]);
                    u1 = std::max<double>(u1, tp[i * tc + 0]);
                    v0 = std::min<double>(v0, tp[i * tc + 1]);
                    v1 = std::max<double>(v1, tp[i * tc + 1]);
                }
            }
            CLICKHLE_INFO(kCategory)
                << "drawElements n=" << count
                << " bound=" << gl_state().bound_texture
                << " tex2d_enabled=" << (glIsEnabled(GL_TEXTURE_2D) ? 1 : 0)
                << " blend=" << (glIsEnabled(GL_BLEND) ? 1 : 0) << " src=0x"
                << std::hex << src_blend << " dst=0x" << dst_blend << std::dec
                << " color=" << cur_color[0] << "," << cur_color[1] << ","
                << cur_color[2] << "," << cur_color[3] << " texenv=0x"
                << std::hex << [] { GLint m = 0;
                       glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &m);
                       return m; }()
                << std::dec
                << " colourEnabled=" << (gl_state().colour.enabled ? 1 : 0)
                << " colourSize=" << gl_state().colour.size
                // The ACTUAL decoded per-vertex colours this draw will use.
                // With GL_COLOR_ARRAY enabled these override glColor, so the
                // current-colour value above is irrelevant for those draws --
                // and only these numbers say whether the array is the thing
                // darkening the scene.
                << [&] {
                       const u32 cn =
                           static_cast<u32>(gl_state().colour.size);
                       if (cn == 0) return std::string(" carr=none");
                       const float* cp = scratch().colour.data();
                       double lo = 1e9, hi = -1e9;
                       for (u32 i = 0;
                            i <= highest &&
                            scratch().colour.size() >= (i + 1) * cn;
                            ++i) {
                           for (u32 k = 0; k < cn; ++k) {
                               lo = std::min<double>(lo, cp[i * cn + k]);
                               hi = std::max<double>(hi, cp[i * cn + k]);
                           }
                       }
                       char b[96];
                       std::snprintf(b, sizeof(b), " carr=size%u[%.4f..%.4f]",
                                     cn, lo, hi);
                       return std::string(b);
                   }()
                << "  MV diag " << mv[0] << " " << mv[5] << " " << mv[10]
                << " " << mv[15] << " trans " << mv[12] << " " << mv[13]
                << " " << mv[14] << "  PR diag " << pr[0] << " " << pr[5]
                << " " << pr[10] << " " << pr[15] << " trans " << pr[12]
                << " " << pr[13] << " " << pr[14] << "  TEX diag " << tx[0]
                << " " << tx[5] << " " << tx[10] << " " << tx[15] << " trans "
                << tx[12] << " " << tx[13] << "  uv=[" << u0 << "," << u1
                << "]x[" << v0 << "," << v1 << "]";
        }
        f.ret_void();
    });

    // --- Textures ------------------------------------------------------------

    fn("glGenTextures", 2, [](HleCallFrame& f) {
        const i32 count = f.args.next_i32();
        const GuestAddr out = f.args.next_ptr();
        if (count <= 0 || out == kNullGuest) {
            f.ret_void();
            return;
        }
        // A title that calls this before its first swap/eglCreateWindowSurface
        // (Texas Hold'em does, seven times) would otherwise ask host GL for
        // names with no context current -- a silent no-op that leaves every
        // one of these zero-initialised, so seven different textures all
        // "generate" id 0 and fight over the one reserved slot. See
        // ensure_display_open's comment.
        if (!ensure_display_open(f.emulator)) {
            f.ret_void();
            return;
        }
        std::vector<GLuint> names(static_cast<std::size_t>(count));
        glGenTextures(count, names.data());
        for (i32 i = 0; i < count; ++i) {
            f.store<u32>(out + static_cast<u32>(i) * 4, names[static_cast<std::size_t>(i)]);
        }
        f.ret_void();
    });

    fn("glDeleteTextures", 2, [](HleCallFrame& f) {
        const i32 count = f.args.next_i32();
        const GuestAddr in = f.args.next_ptr();
        if (count <= 0 || in == kNullGuest) {
            f.ret_void();
            return;
        }
        std::vector<GLuint> names(static_cast<std::size_t>(count));
        for (i32 i = 0; i < count; ++i) {
            names[static_cast<std::size_t>(i)] =
                f.memory.read<u32>(in + static_cast<u32>(i) * 4).value_or(0);
        }
        glDeleteTextures(count, names.data());
        f.ret_void();
    });

    fn("glBindTexture", 2, [](HleCallFrame& f) {
        const auto target = static_cast<GLenum>(f.args.next_u32());
        const u32 texture = f.args.next_u32();
        gl_state().bound_texture = texture;
        gl_state().bound_texture_target = target;
        // Same lazy-context gap glGenTextures already closed, one call site
        // over: a title that binds a hardcoded texture name (never calls
        // glGenTextures at all) as its very first GL operation uploads real
        // data into a texture object that silently lands nowhere, because no
        // context exists yet. Root-caused for Royal Solitaire (2026-09-12):
        // texture 1's glTexImage2D source bytes in guest memory are real,
        // non-zero, and lost by readback -- with zero GL error reported --
        // specifically because it is the first-ever GL call in the run and
        // the title never calls glGenTextures to trigger the existing guard.
        if (!ensure_display_open(f.emulator)) {
            f.ret_void();
            return;
        }
        glBindTexture(target, texture);
        if (getenv("CLICKHLE_LOG_BINDS") != nullptr) {
            CLICKHLE_INFO(kCategory) << "bind -> texture " << gl_state().bound_texture;
        }
        f.ret_void();
    });

    fn("glTexImage2D", 9, [](HleCallFrame& f) {
        const auto target = static_cast<GLenum>(f.args.next_u32());
        const i32 level = f.args.next_i32();
        auto internal_format = f.args.next_i32();
        const i32 width = f.args.next_i32();
        const i32 height = f.args.next_i32();
        const i32 border = f.args.next_i32();
        auto format = static_cast<GLenum>(f.args.next_u32());
        auto type = static_cast<GLenum>(f.args.next_u32());
        const GuestAddr pixels = f.args.next_ptr();

        // See kVortexEntryPoint above for the full writeup.
        if (width == 32 && height == 32 && format == GL_RGB &&
            type == GL_UNSIGNED_SHORT_5_6_5 &&
            f.emulator.image() != nullptr &&
            f.emulator.image()->entry_point() == kVortexEntryPoint &&
            f.emulator.image()->size_of_image() == kVortexImageSize) {
            CLICKHLE_WARN(kCategory)
                << "Vortex-specific workaround: texture at 0x" << std::hex
                << pixels << std::dec
                << " reinterpreted as 8bpp luminance instead of RGB565 "
                   "(real format mismatch unidentified -- see "
                   "kVortexEntryPoint in this file)";
            format = GL_LUMINANCE;
            type = GL_UNSIGNED_BYTE;
            internal_format = static_cast<i32>(GL_LUMINANCE);
        }

        const void* data = nullptr;
        if (pixels != kNullGuest) {
            // Bytes per pixel for the ES 1.0 formats. Anything else would be a
            // guest bug, but guessing wrong here reads past the allocation.
            u32 bpp = 4;
            switch (type) {
                case GL_UNSIGNED_SHORT_5_6_5:
                case GL_UNSIGNED_SHORT_4_4_4_4:
                case GL_UNSIGNED_SHORT_5_5_5_1: bpp = 2; break;
                case GL_UNSIGNED_BYTE:
                    if (format == GL_RGB) {
                        bpp = 3u;
                    } else if (format == GL_RGBA) {
                        bpp = 4u;
                    } else if (format == GL_LUMINANCE_ALPHA) {
                        bpp = 2u;
                    } else if (format == GL_LUMINANCE ||
                              format == GL_ALPHA) {
                        bpp = 1u;
                    } else {
                        // A genuinely unrecognised (format, GL_UNSIGNED_BYTE)
                        // pair -- the Vortex icon-texture bug (2026-09-12)
                        // was exactly this shape of problem: a title's real
                        // pixel data didn't match what its declared
                        // (format, type) nominally requires, corrupting
                        // whatever renders wherever the mismatch under- or
                        // over-reads. The one Vortex case found so far has
                        // its own narrow, confirmed patch (kVortexEntryPoint
                        // above); this is a general tripwire for the NEXT
                        // one, in this or any other title, so it surfaces as
                        // a loud diagnostic instead of a silent guess.
                        CLICKHLE_WARN(kCategory)
                            << "glTexImage2D: unrecognised format 0x"
                            << std::hex << format
                            << " with type GL_UNSIGNED_BYTE -- guessing "
                               "bpp=1u, verify against the real asset if "
                               "this renders wrong"
                            << std::dec;
                        bpp = 1u;
                    }
                    break;
                default:
                    CLICKHLE_WARN(kCategory)
                        << "glTexImage2D: unrecognised type 0x" << std::hex
                        << type << " (format 0x" << format
                        << ") -- guessing bpp=4, verify against the real "
                           "asset if this renders wrong"
                        << std::dec;
                    break;
            }
            const u64 span =
                static_cast<u64>(width) * static_cast<u32>(height) * bpp;
            data = f.memory.host_ptr(pixels, static_cast<u32>(span));
            if (data == nullptr) {
                CLICKHLE_ERROR(kCategory)
                    << "glTexImage2D: " << span << " bytes at 0x" << std::hex
                    << pixels << " are not mapped";
                f.ret_void();
                return;
            }
            if (getenv("CLICKHLE_DUMP_SRCBYTES") != nullptr) {
                const auto* bytes = static_cast<const u8*>(data);
                u64 nonzero = 0;
                for (u64 i = 0; i < span; ++i) {
                    if (bytes[i] != 0) ++nonzero;
                }
                // Oracle's question (2026-09-12, Vortex icon textures): does
                // real, structured data continue past the span this format
                // assumes -- evidence the header under-declares bpp and we're
                // silently dropping the rest, rather than genuinely reading
                // past the real asset into unrelated memory. One-off: peek
                // an equal-sized window immediately after `span` too, same
                // buffer, no separate allocation.
                u64 trailing_nonzero = 0;
                const void* trailing_data = f.memory.host_ptr(
                    pixels + static_cast<u32>(span), static_cast<u32>(span));
                const auto* trailing = static_cast<const u8*>(trailing_data);
                if (trailing != nullptr) {
                    for (u64 i = 0; i < span; ++i) {
                        if (trailing[i] != 0) ++trailing_nonzero;
                    }
                }
                CLICKHLE_INFO(kCategory)
                    << "src bytes at 0x" << std::hex << pixels << std::dec
                    << ": " << nonzero << "/" << span
                    << " nonzero, trailing " << trailing_nonzero << "/" << span
                    << " nonzero, first 16:"
                    << std::hex << " " << static_cast<int>(bytes[0]) << " "
                    << static_cast<int>(bytes[1]) << " "
                    << static_cast<int>(bytes[2]) << " "
                    << static_cast<int>(bytes[3]) << " "
                    << static_cast<int>(bytes[4]) << " "
                    << static_cast<int>(bytes[5]) << " "
                    << static_cast<int>(bytes[6]) << " "
                    << static_cast<int>(bytes[7]) << std::dec;
            }
            // One-off, Vortex icon-texture investigation: write the raw
            // source bytes (span + the same-sized trailing window) to a
            // file, so they can be re-interpreted offline under a different
            // bpp assumption without needing a live re-run per hypothesis.
            if (const char* dir = getenv("CLICKHLE_DUMP_RAWSRC")) {
                const void* trailing_data = f.memory.host_ptr(
                    pixels + static_cast<u32>(span), static_cast<u32>(span));
                std::vector<u8> raw(static_cast<std::size_t>(span) * 2, 0);
                std::memcpy(raw.data(), data, static_cast<std::size_t>(span));
                if (trailing_data != nullptr) {
                    std::memcpy(raw.data() + span, trailing_data,
                               static_cast<std::size_t>(span));
                }
                std::ostringstream path;
                path << dir << "/rawsrc_0x" << std::hex << pixels << std::dec
                     << "_" << width << "x" << height << ".bin";
                std::ofstream out(path.str(), std::ios::binary);
                out.write(reinterpret_cast<const char*>(raw.data()),
                          static_cast<std::streamsize>(raw.size()));
            }
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(target, level, internal_format, width, height, border,
                     format, type, data);

        check_gl_error("glTexImage2D");
        // Which texture object this landed in, and at what size. A re-upload
        // at a SMALLER size than the previous one leaves the old content in
        // the rows the new image does not cover, and a full-screen quad whose
        // texel coordinates still assume the old height then samples straight
        // into it.
        CLICKHLE_INFO(kCategory)
            << "upload -> texture " << gl_state().bound_texture << " " << width
            << "x" << height << " caller=0x" << std::hex << f.cpu.lr()
            << std::dec;
        texture_dimensions()[gl_state().bound_texture] = {width, height};
        // Read the texture back and write it out, so "what is actually in this
        // sheet" is a picture rather than an inference from the composite.
        // CLICKHLE_DUMP_TEXTURES=<dir>.
        if (const char* dir = getenv("CLICKHLE_DUMP_TEXTURES")) {
            std::vector<u8> rgb(static_cast<std::size_t>(width) *
                                static_cast<std::size_t>(height) * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_BGR, GL_UNSIGNED_BYTE, rgb.data());
            // Alpha separately: the BGR readback discards it, and for the
            // 1-bit-alpha format 0x8034 (RGBA5551) alpha IS the question. A
            // sheet full of real colour that is entirely alpha=0 renders as
            // nothing once blending is on.
            {
                std::vector<u8> alpha_rgba(static_cast<std::size_t>(width) *
                                           static_cast<std::size_t>(height) * 4);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                              alpha_rgba.data());
                if (glGetError() == GL_NO_ERROR) {
                    std::size_t opaque = 0, total = alpha_rgba.size() / 4;
                    for (std::size_t i = 0; i < total; ++i) {
                        if (alpha_rgba[i * 4 + 3] != 0) ++opaque;
                    }
                    CLICKHLE_INFO(kCategory)
                        << "  alpha: texture " << gl_state().bound_texture << " "
                        << opaque << "/" << total << " texels non-zero";
                }
            }
            if (glGetError() == GL_NO_ERROR) {
                // write_bmp wants bottom-up rows, which is what GL hands back.
                char name[512];
                std::snprintf(name, sizeof(name), "%s/tex_%u_%dx%d.bmp", dir,
                              gl_state().bound_texture, width, height);
                static_cast<void>(write_bmp(name, width, height, rgb.data()));
            }
        }
        CLICKHLE_TRACE(kCategory)
            << "texImage2D " << width << "x" << height << " level=" << level
            << " ifmt=0x" << std::hex << internal_format << " fmt=0x" << format
            << " type=0x" << type << std::dec;
        note_texture_level(target, level);
        f.ret_void();
    });

    fn("glCompressedTexImage2D", 8, [](HleCallFrame& f) {
        const auto target = static_cast<GLenum>(f.args.next_u32());
        const i32 level = f.args.next_i32();
        const i32 internal_format = f.args.next_i32();
        const i32 width = f.args.next_i32();
        const i32 height = f.args.next_i32();
        const i32 border = f.args.next_i32();
        const i32 image_size = f.args.next_i32();
        const GuestAddr data = f.args.next_ptr();

        // S3TC/DXT. Desktop GL has these natively via
        // EXT_texture_compression_s3tc, so the data goes through untouched --
        // no decoding, and the driver keeps it compressed in VRAM as intended.
        if (internal_format >= 0x83F0 && internal_format <= 0x83F3) {
            if (data == kNullGuest || image_size <= 0) {
                f.ret_void();
                return;
            }
            const std::byte* source =
                f.memory.host_ptr(data, static_cast<u32>(image_size));
            if (source == nullptr) {
                CLICKHLE_ERROR(kCategory)
                    << "glCompressedTexImage2D: S3TC data at 0x" << std::hex
                    << data << " is not mapped";
                f.ret_void();
                return;
            }
            if (compressed_tex_image_2d == nullptr) {
                compressed_tex_image_2d =
                    reinterpret_cast<CompressedTexImage2D>(
                        SDL_GL_GetProcAddress("glCompressedTexImage2D"));
            }
            if (compressed_tex_image_2d == nullptr) {
                CLICKHLE_WARN(kCategory)
                    << "host GL has no glCompressedTexImage2D; S3TC textures "
                       "will be missing";
                f.ret_void();
                return;
            }
            compressed_tex_image_2d(target, level < 0 ? 0 : level,
                                    static_cast<GLenum>(internal_format), width,
                                    height, border, image_size, source);
            check_gl_error("glCompressedTexImage2D (S3TC)");
            CLICKHLE_TRACE(kCategory)
                << "S3TC fmt=0x" << std::hex << internal_format << std::dec
                << " " << width << "x" << height << " level=" << level
                << " bytes=" << image_size << " expected="
                << (static_cast<i64>((width + 3) / 4) * ((height + 3) / 4) *
                    ((internal_format == 0x83F0 || internal_format == 0x83F1)
                         ? 8 : 16));
            note_texture_level(target, level);
            static bool announced = false;
            if (!announced) {
                announced = true;
                CLICKHLE_INFO(kCategory) << "using host S3TC texture compression";
            }
            f.ret_void();
            return;
        }

        PalettedFormat format{};
        if (!paletted_format(internal_format, format)) {
            static std::vector<i32> reported;
            if (std::find(reported.begin(), reported.end(), internal_format) ==
                reported.end()) {
                reported.push_back(internal_format);
                CLICKHLE_WARN(kCategory)
                    << "glCompressedTexImage2D: unhandled format 0x" << std::hex
                    << internal_format << std::dec << " (" << width << "x"
                    << height << ")";
            }
            f.ret_void();
            return;
        }
        if (width <= 0 || height <= 0 || data == kNullGuest || image_size <= 0) {
            f.ret_void();
            return;
        }

        const std::byte* source =
            f.memory.host_ptr(data, static_cast<u32>(image_size));
        if (source == nullptr) {
            CLICKHLE_ERROR(kCategory)
                << "glCompressedTexImage2D: " << image_size << " bytes at 0x"
                << std::hex << data << " are not mapped";
            f.ret_void();
            return;
        }

        const u32 palette_bytes = format.entries * format.entry_bytes;
        const auto pixels = static_cast<u32>(width) * static_cast<u32>(height);
        const u32 index_bytes = format.four_bit ? (pixels + 1) / 2 : pixels;
        if (static_cast<u32>(image_size) < palette_bytes + index_bytes) {
            CLICKHLE_ERROR(kCategory)
                << "glCompressedTexImage2D: " << format.name << " needs "
                << (palette_bytes + index_bytes) << " bytes, given " << image_size;
            f.ret_void();
            return;
        }

        // Expand the palette once, then index into it per pixel.
        std::vector<u8> palette(static_cast<std::size_t>(format.entries) * 4);
        for (u32 i = 0; i < format.entries; ++i) {
            decode_entry(internal_format, source + i * format.entry_bytes,
                         palette.data() + static_cast<std::size_t>(i) * 4);
        }

        const std::byte* indices = source + palette_bytes;
        std::vector<u8> rgba(static_cast<std::size_t>(pixels) * 4);
        for (u32 p = 0; p < pixels; ++p) {
            u32 index = 0;
            if (format.four_bit) {
                const u8 pair = std::to_integer<u8>(indices[p / 2]);
                // High nibble is the earlier pixel.
                index = (p & 1u) == 0 ? (pair >> 4) : (pair & 0x0Fu);
            } else {
                index = std::to_integer<u8>(indices[p]);
            }
            std::memcpy(rgba.data() + static_cast<std::size_t>(p) * 4,
                        palette.data() + static_cast<std::size_t>(index) * 4, 4);
        }

        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        // A negative level means several mip levels follow; only the base level
        // is decoded here, and GL_TEXTURE_MAX_LEVEL keeps sampling valid.
        glTexImage2D(target, level < 0 ? 0 : level, GL_RGBA, width, height,
                     border, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        check_gl_error("glTexImage2D (paletted)");
        note_texture_level(target, level);
        // The direct glTexImage2D path records this (line ~1717); this path
        // never did. install_texel_scale() looks this up to scale the
        // texture matrix from texel coordinates down to normalized UV --
        // without it, the matrix stays identity and texcoords spanning
        // 0..width/0..height sample far outside [0,1], clamping to a single
        // edge texel. Found live (2026-09-19): Texas Hold'em's title-art
        // background, uploaded exclusively through this paletted path,
        // rendered as a flat single colour instead of the real image for
        // exactly this reason -- not a second overwrite bug, a genuinely
        // separate gap in a different code path from everything else fixed
        // today.
        texture_dimensions()[gl_state().bound_texture] = {width, height};
        CLICKHLE_INFO(kCategory)
            << "upload -> texture " << gl_state().bound_texture << " " << width
            << "x" << height << " (paletted) caller=0x" << std::hex
            << f.cpu.lr() << std::dec;
        // Include alpha information in texture dumps.
        if (const char* dir = getenv("CLICKHLE_DUMP_TEXTURES")) {
            {
                std::vector<u8> alpha_rgba(static_cast<std::size_t>(width) *
                                           static_cast<std::size_t>(height) * 4);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                              alpha_rgba.data());
                if (glGetError() == GL_NO_ERROR) {
                    std::size_t opaque = 0, total = alpha_rgba.size() / 4;
                    for (std::size_t i = 0; i < total; ++i) {
                        if (alpha_rgba[i * 4 + 3] != 0) ++opaque;
                    }
                    CLICKHLE_INFO(kCategory)
                        << "  alpha: texture " << gl_state().bound_texture
                        << " " << opaque << "/" << total
                        << " texels non-zero (paletted)";
                }
            }
            char name[512];
            std::snprintf(name, sizeof(name), "%s/tex_%u_%dx%d_paletted.bmp",
                          dir, gl_state().bound_texture, width, height);
            std::vector<u8> rgb(static_cast<std::size_t>(width) *
                                static_cast<std::size_t>(height) * 3);
            for (u32 p = 0; p < pixels; ++p) {
                rgb[static_cast<std::size_t>(p) * 3 + 0] = rgba[static_cast<std::size_t>(p) * 4 + 0];
                rgb[static_cast<std::size_t>(p) * 3 + 1] = rgba[static_cast<std::size_t>(p) * 4 + 1];
                rgb[static_cast<std::size_t>(p) * 3 + 2] = rgba[static_cast<std::size_t>(p) * 4 + 2];
            }
            // rgba[] is top-down as decoded; write_bmp wants bottom-up, same
            // as the direct path's GL-readback (which comes back bottom-up
            // from glGetTexImage). Flip rows so both dumps agree on
            // orientation.
            const auto row_bytes = static_cast<std::size_t>(width) * 3;
            std::vector<u8> flipped(rgb.size());
            for (i32 row = 0; row < height; ++row) {
                std::memcpy(flipped.data() + static_cast<std::size_t>(row) * row_bytes,
                            rgb.data() +
                                static_cast<std::size_t>(height - 1 - row) * row_bytes,
                            row_bytes);
            }
            static_cast<void>(write_bmp(name, width, height, flipped.data()));
        }

        static std::vector<i32> announced;
        if (std::find(announced.begin(), announced.end(), internal_format) ==
            announced.end()) {
            announced.push_back(internal_format);
            CLICKHLE_INFO(kCategory)
                << "decoding " << format.name << " textures ("
                << width << "x" << height << ")";
        }
        f.ret_void();
    });

    const auto tex_param = [](HleCallFrame& f, bool fixed_point) {
        const auto target = static_cast<GLenum>(f.args.next_u32());
        const auto name = static_cast<GLenum>(f.args.next_u32());
        const u32 raw = f.args.next_u32();
        // Filter and wrap modes are enums even when passed through the `x`
        // entry point, so they must not be scaled by 65536.
        const float value = fixed_point && raw > 0x10000
                                ? fixed_to_float(raw)
                                : static_cast<float>(static_cast<i32>(raw));
        glTexParameterf(target, name, value);
        f.ret_void();
    };

    fn("glTexParameterx", 3, [tex_param](HleCallFrame& f) { tex_param(f, true); });
    fn("glTexParameterf", 3, [](HleCallFrame& f) {
        const auto target = static_cast<GLenum>(f.args.next_u32());
        const auto name = static_cast<GLenum>(f.args.next_u32());
        glTexParameterf(target, name, f.args.next_float());
        f.ret_void();
    });

    fn("glTexEnvf", 3, [](HleCallFrame& f) {
        const auto target = static_cast<GLenum>(f.args.next_u32());
        const auto name = static_cast<GLenum>(f.args.next_u32());
        glTexEnvf(target, name, f.args.next_float());
        f.ret_void();
    });

    // --- Queries -------------------------------------------------------------

    fn("glGetString", 1, [](HleCallFrame& f) {
        const auto name = static_cast<GLenum>(f.args.next_u32());
        // This used to return null, on the reasoning that no game in hand
        // branched on it. Toy Golf does, and so do its siblings: they bring up
        // EGL, call glGetString, and on a null answer tear the whole context
        // down again and null the renderer subsystem -- which the rest of the
        // engine then dereferences. That was the "unmapped read at 0x0" those
        // games died on, about eighteen million instructions later.
        //
        // The strings are allocated once in guest memory and cached, because
        // the caller keeps the pointer and GL promises it stays valid.
        auto& cached = gl_state().gl_strings;
        const auto it = cached.find(name);
        if (it != cached.end()) {
            f.ret(it->second);
            return;
        }

        std::string value;
        switch (name) {
            case GL_VENDOR:   value = "NVIDIA Corporation"; break;
            case GL_RENDERER: value = "GoForce 4500"; break;
            case GL_VERSION:  value = "OpenGL ES-CM 1.0"; break;
            case GL_EXTENSIONS:
                // Only what this layer actually translates. Claiming an
                // extension we do not implement is worse than claiming none: a
                // game takes the fast path and fails somewhere unrelated.
                value =
                    "GL_OES_byte_coordinates "
                    "GL_OES_fixed_point "
                    "GL_OES_single_precision "
                    "GL_OES_read_format "
                    "GL_OES_compressed_paletted_texture "
                    "GL_EXT_texture_compression_s3tc";
                break;
            default:
                f.ret(kNullGuest);
                return;
        }

        const GuestAddr block =
            f.emulator.heap().allocate(static_cast<u32>(value.size()) + 1);
        if (block == kNullGuest) {
            f.ret(kNullGuest);
            return;
        }
        write_guest_cstring(f.memory, block, value);
        cached[name] = block;
        CLICKHLE_DEBUG(kCategory) << "glGetString(0x" << std::hex << name
                                << std::dec << ") -> '" << value << "'";
        f.ret(block);
    });

    fn("glGetIntegerv", 2, [](HleCallFrame& f) {
        const auto name = static_cast<GLenum>(f.args.next_u32());
        const GuestAddr out = f.args.next_ptr();
        if (out == kNullGuest) {
            // glGetIntegerv requires a writable result buffer. A guest null
            // pointer would fault in the native GLES implementation.
            f.cpu.request_stop();
            return;
        }

        // Several GLES queries write vectors. Passing a one-word local to the
        // host GL API lets GL_VIEWPORT and similar queries overwrite the HLE
        // stack, and returning only the first component also breaks callers.
        // The largest fixed-size GLES state vectors are matrices (16 words).
        std::size_t count = 1;
        switch (name) {
            case 0x0BA2:  // GL_VIEWPORT
            case 0x0C10:  // GL_SCISSOR_BOX
            case 0x0B00:  // GL_CURRENT_COLOR
            case 0x0B03:  // GL_CURRENT_TEXTURE_COORDS
            case 0x0C22:  // GL_COLOR_CLEAR_VALUE
            case 0x0C23:  // GL_COLOR_WRITEMASK
                count = 4;
                break;
            case 0x0B02:  // GL_CURRENT_NORMAL
                count = 3;
                break;
            case 0x0D3A:  // GL_MAX_VIEWPORT_DIMS
            case 0x0B70:  // GL_DEPTH_RANGE
            case 0x846D:  // GL_ALIASED_POINT_SIZE_RANGE
            case 0x846E:  // GL_ALIASED_LINE_WIDTH_RANGE
                count = 2;
                break;
            case 0x0BA6:  // GL_MODELVIEW_MATRIX
            case 0x0BA7:  // GL_PROJECTION_MATRIX
            case 0x0BA8:  // GL_TEXTURE_MATRIX
                count = 16;
                break;
            case 0x86A3: {  // GL_COMPRESSED_TEXTURE_FORMATS
                GLint formats = 0;
                glGetIntegerv(0x86A2, &formats);  // GL_NUM_COMPRESSED_TEXTURE_FORMATS
                if (formats > 0) count = static_cast<std::size_t>(formats);
                break;
            }
            default:
                break;
        }
        std::vector<GLint> values(count, 0);
        glGetIntegerv(name, values.data());
        for (std::size_t i = 0; i < count; ++i) {
            const u64 address = static_cast<u64>(out) + i * sizeof(i32);
            if (address > std::numeric_limits<GuestAddr>::max() ||
                !f.store<i32>(static_cast<GuestAddr>(address), values[i])) {
                f.cpu.request_stop();
                return;
            }
        }
        f.ret_void();
    });

    fn("glClearColor", 4, [](HleCallFrame& f) {
        const float r = f.args.next_float();
        const float g = f.args.next_float();
        const float b = f.args.next_float();
        glClearColor(r, g, b, f.args.next_float());
        f.ret_void();
    });

    // Fog. The x variant takes 16.16, the f variant real floats; both reach the
    // same host call.
    fn("glFogf", 2, [](HleCallFrame& f) {
        const auto name = static_cast<GLenum>(f.args.next_u32());
        glFogf(name, f.args.next_float());
        f.ret_void();
    });

    fn("glFogx", 2, [](HleCallFrame& f) {
        const auto name = static_cast<GLenum>(f.args.next_u32());
        const u32 raw = f.args.next_u32();
        // GL_FOG_MODE takes an enum, which per OES_fixed_point is unscaled;
        // every other fog parameter is a real value in 16.16.
        glFogf(name, name == GL_FOG_MODE ? static_cast<float>(raw)
                                         : fixed_to_float(raw));
        f.ret_void();
    });

    fn("glFogfv", 2, [](HleCallFrame& f) {
        const auto name = static_cast<GLenum>(f.args.next_u32());
        const GuestAddr source = f.args.next_ptr();
        // GL_FOG_COLOR is the only vector parameter; the rest are scalars.
        const u32 count = name == GL_FOG_COLOR ? 4u : 1u;
        float values[4] = {0, 0, 0, 0};
        for (u32 i = 0; i < count; ++i) {
            values[i] = endian::bits_to_float(
                f.memory.read<u32>(source + i * 4).value_or(0));
        }
        glFogfv(name, values);
        f.ret_void();
    });

    fn("glTexSubImage2D", 9, [](HleCallFrame& f) {
        const auto target = static_cast<GLenum>(f.args.next_u32());
        const i32 level = f.args.next_i32();
        const i32 x = f.args.next_i32();
        const i32 y = f.args.next_i32();
        const i32 width = f.args.next_i32();
        const i32 height = f.args.next_i32();
        const auto format = static_cast<GLenum>(f.args.next_u32());
        const auto type = static_cast<GLenum>(f.args.next_u32());
        const GuestAddr pixels = f.args.next_ptr();
        if (width <= 0 || height <= 0 || pixels == kNullGuest) {
            f.ret_void();
            return;
        }
        u32 bpp = 4;
        switch (type) {
            case GL_UNSIGNED_SHORT_5_6_5:
            case GL_UNSIGNED_SHORT_4_4_4_4:
            case GL_UNSIGNED_SHORT_5_5_5_1: bpp = 2; break;
            case GL_UNSIGNED_BYTE:
                bpp = format == GL_RGB ? 3u
                    : format == GL_RGBA ? 4u
                    : format == GL_LUMINANCE_ALPHA ? 2u : 1u;
                break;
            default: break;
        }
        const u64 span =
            static_cast<u64>(width) * static_cast<u32>(height) * bpp;
        const std::byte* data =
            f.memory.host_ptr(pixels, static_cast<u32>(span));
        if (data == nullptr) {
            CLICKHLE_ERROR(kCategory)
                << "glTexSubImage2D: " << span << " bytes at 0x" << std::hex
                << pixels << " are not mapped";
            f.ret_void();
            return;
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(target, level, x, y, width, height, format, type, data);
        check_gl_error("glTexSubImage2D");
        CLICKHLE_INFO(kCategory)
            << "SUBupload -> texture " << gl_state().bound_texture << " at ("
            << x << "," << y << ") " << width << "x" << height;
        if (const char* dir = getenv("CLICKHLE_DUMP_TEXTURES")) {
            GLint tw = 0, th = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
            if (tw > 0 && th > 0) {
                std::vector<u8> rgb(static_cast<std::size_t>(tw) *
                                    static_cast<std::size_t>(th) * 3);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_BGR, GL_UNSIGNED_BYTE,
                              rgb.data());
                // Alpha histogram, separately. The BGR readback above discards
                // alpha, and for a 1-bit-alpha format (0x8034, RGBA5551) alpha
                // is the whole question: a sheet full of real colour that is
                // entirely alpha=0 renders as nothing with blending on.
                {
                    std::vector<u8> rgba(static_cast<std::size_t>(tw) *
                                         static_cast<std::size_t>(th) * 4);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                  rgba.data());
                    if (glGetError() == GL_NO_ERROR) {
                        std::size_t opaque = 0, total = rgba.size() / 4;
                        for (std::size_t i = 0; i < total; ++i) {
                            if (rgba[i * 4 + 3] != 0) ++opaque;
                        }
                        CLICKHLE_INFO(kCategory)
                            << "  alpha: texture " << gl_state().bound_texture
                            << " " << opaque << "/" << total << " texels non-zero";
                    }
                }
                if (glGetError() == GL_NO_ERROR) {
                    static int seq = 0;
                    char name[512];
                    std::snprintf(name, sizeof(name), "%s/sub%d_tex_%u_%dx%d.bmp",
                                  dir, seq++, gl_state().bound_texture, tw, th);
                    static_cast<void>(write_bmp(name, tw, th, rgb.data()));
                }
            }
        }
        f.ret_void();
    });

    fn("glReadPixels", 7, [](HleCallFrame& f) {
        const i32 x = f.args.next_i32();
        const i32 y = f.args.next_i32();
        const i32 width = f.args.next_i32();
        const i32 height = f.args.next_i32();
        const auto format = static_cast<GLenum>(f.args.next_u32());
        const auto type = static_cast<GLenum>(f.args.next_u32());
        const GuestAddr destination = f.args.next_ptr();
        if (width <= 0 || height <= 0 || destination == kNullGuest) {
            f.ret_void();
            return;
        }
        const u32 bpp = type == GL_UNSIGNED_BYTE
                            ? (format == GL_RGB ? 3u : 4u)
                            : 2u;
        const u64 span =
            static_cast<u64>(width) * static_cast<u32>(height) * bpp;
        std::byte* target = f.memory.host_ptr(destination,
                                              static_cast<u32>(span));
        if (target == nullptr) {
            CLICKHLE_ERROR(kCategory)
                << "glReadPixels: destination is not mapped";
            f.ret_void();
            return;
        }
        // The guest renders at native resolution, so a readback must come from
        // the same rectangle the guest thinks it drew -- scaled to the surface.
        const i32 s = gl_state().scale;
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(x * s, y * s, width, height, format, type, target);
        check_gl_error("glReadPixels");
        f.ret_void();
    });

    fn("glDepthRangef", 2, [](HleCallFrame& f) {
        const float near_plane = f.args.next_float();
        glDepthRange(static_cast<double>(near_plane),
                     static_cast<double>(f.args.next_float()));
        f.ret_void();
    });

    CLICKHLE_DEBUG(kCategory) << "registered OpenGL ES 1.0 translation";
}

void gles_statistics(Emulator& emulator, u64& frames, u64& draws) {
    if (!emulator.gles_state_storage().has_value()) {
        frames = draws = 0;
        return;
    }
    const auto& state = std::any_cast<const GlState&>(emulator.gles_state_storage());
    frames = state.frames;
    draws = state.draws;
}

const std::map<u32, u64>& gles_draws_by_texture(Emulator& emulator) {
    static const std::map<u32, u64> empty;
    if (!emulator.gles_state_storage().has_value()) return empty;
    return std::any_cast<GlState&>(emulator.gles_state_storage()).draws_by_texture;
}

}  // namespace clickhle

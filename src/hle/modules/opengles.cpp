// Click-wheel games import this firmware library by ordinal. Only identified
// ordinals are registered; unresolved entries stay unbound so the emulator can
// report which import a game needs.

#include <cstdlib>
#include "clickhle/hle/modules.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <utility>

// gles_statistics() lives with the renderer that counts frames, in gles.cpp
// (or gles_stub.cpp when no video backend is compiled in).

#include "clickhle/common/log.h"
#include "clickhle/core/emulator.h"
#include "clickhle/hle/call_frame.h"
#include "clickhle/common/endian.h"
#include "clickhle/hle/registry.h"

namespace clickhle {
namespace {

constexpr std::string_view kCategory = "gles";

/// As the image spells it. Matched against the library name in the eapp
/// header, so it must stay exactly this.
constexpr const char* kModule = "OpenGLES";

/// Every image in hand imports exactly this many, and the firmware exports
/// exactly this many. Kept here so the log line below can say what the
/// denominator is before any image has been loaded.
constexpr u32 kOrdinalCount = 179;

/// Whether the firmware's drawing subsystem has been started. We have no such
/// subsystem -- the inherited GL layer is the renderer -- but the games wait on
/// it, so the flag exists to answer them.
bool g_render_server_running = false;

/// Dimensions of each uploaded texture, by texture name.
///
/// Needed because the games give texture coordinates in **texels**, not in
/// GL's normalised [0,1]. Bejeweled's first sheet is 322x242 and its
/// coordinates run (1,1) to (321,241) -- a 320x240 window inside it, inset by
/// the half-texel border. GL samples that as (1,1) in normalised space, which
/// is one texel in the far corner, so every quad came out a flat colour.
///
/// The firmware's pipeline took texels; ES 1.x reaches the same place with a
/// texture matrix scaled by 1/width, 1/height.
std::map<u32, std::pair<u32, u32>> g_texture_size;

/// The texture bound to the active unit, so an upload knows what it is sizing.
u32 g_bound_texture = 0;

/// The click-wheel panel. 320x240 on the 5G/5.5G, which is the only generation
/// this corpus covers.
constexpr u32 kScreenWidth = 320;
constexpr u32 kScreenHeight = 240;

}  // namespace


namespace {

/// Append one uniform packet to CLICKHLE_DUMP_UNIFORM_RAW, boundaries kept.
///
/// The VideoCore research session asked for the raw bytes of #148/#149 in call
/// order with packet boundaries preserved: per STATUS.md these are the uniform
/// packets queued through 0x1080009c's ring and submitted across 0x3004_0000,
/// which makes them the actual runtime INPUT to rserver.bin. They hold the
/// consumer's code and none of its input, and unlike rserver.bin itself these
/// are generated at runtime, so no static file has them.
///
/// Record layout, little-endian, so boundaries survive concatenation:
///     u32 magic 'U','P','K',0
///     u32 ordinal      (148 or 149)
///     u32 sequence     (call order across BOTH ordinals)
///     u32 byte_count
///     u32 payload[byte_count/4]
void dump_uniform_packet(u32 ordinal, const std::vector<u32>& words) {
    static std::ofstream* out = [] () -> std::ofstream* {
        const char* path = std::getenv("CLICKHLE_DUMP_UNIFORM_RAW");
        if (path == nullptr) return nullptr;
        static std::ofstream f(path, std::ios::binary | std::ios::trunc);
        return f.is_open() ? &f : nullptr;
    }();
    if (out == nullptr) return;
    static u32 sequence = 0;
    const u32 header[4] = {0x004b5055u, ordinal, sequence++,
                           static_cast<u32>(words.size() * 4)};
    out->write(reinterpret_cast<const char*>(header), sizeof(header));
    out->write(reinterpret_cast<const char*>(words.data()),
               static_cast<std::streamsize>(words.size() * 4));
    out->flush();
}

}  // namespace

void register_opengles(HleRegistry& registry) {
    // Add one of these per identified ordinal:
    //
    //     fn(0, "glClear", 1, [](HleCallFrame& f) {
    //         const u32 mask = f.args.next_u32();
    //         ...
    //         f.ret(0);
    //     });
    //
    // The name is ours -- a label for reports and traces -- and is only worth
    // writing once the firmware export has actually been read. An ordinal
    // whose behaviour is still a guess does not belong in this list.
    const auto fn = [&registry](u32 ordinal, std::string name, u8 arity,
                                HleHandler handler) {
        registry.add(kModule, ordinal, std::move(name), arity,
                     std::move(handler));
    };

    // Forward an ordinal to the named GL ES entry point that implements it.
    //
    // The two layers stay separate on purpose: gles.cpp is a general GL ES 1.x
    // implementation that knows nothing about iPod ordinal numbering, and this
    // table is what we have *proved* about this platform's ABI. An ordinal
    // appears here only once its call sites say what it is.
    //
    // Resolved once at registration rather than per call, so a name that does
    // not exist is a loud failure now instead of a null dereference later.
    const auto forward = [&registry](u32 ordinal, const char* gl_name,
                                     u8 arity) {
        const HleFunction* target = registry.find("gl", gl_name);
        if (target == nullptr) {
            // Expected in a build with no video backend, where gles_stub.cpp
            // registers nothing. The ordinal stays unimplemented, which is the
            // honest outcome.
            CLICKHLE_DEBUG(kCategory)
                << "no implementation of " << gl_name << " for ordinal "
                << ordinal;
            return;
        }
        registry.add(kModule, ordinal, gl_name, arity,
                     [target](HleCallFrame& f) { target->handler(f); });
    };

    // Calls a named GL entry point with an explicit argument list.
    //
    // Plain forwarding re-reads r0 onward in the *target's* order, which is
    // only correct when the ordinal and the GL function agree on their
    // arguments. Where they do not -- an attribute-indexed setter over a
    // fixed-function API -- the values have to be reordered first.
    //
    // Done by writing the registers the target will read and resetting the
    // ArgReader, which is why it is limited to four words: a fifth would have
    // to be pushed onto the guest stack, and nothing needs that yet. r1-r3 are
    // restored afterwards; r0 is not, because the target's return value is
    // in it.
    const auto call_with = [](HleCallFrame& f, const HleFunction* target,
                              std::initializer_list<u32> words) {
        u32 saved[4] = {f.cpu.reg(0), f.cpu.reg(1), f.cpu.reg(2), f.cpu.reg(3)};
        u32 index = 0;
        for (const u32 word : words) {
            if (index < 4) {
                f.cpu.set_reg(index, word);
            }
            ++index;
        }
        f.args.reset();
        target->handler(f);
        for (u32 i = 1; i < 4; ++i) {
            f.cpu.set_reg(i, saved[i]);
        }
    };

    // The firmware's texture target.
    //
    // It is 0x84F5, not GL's own GL_TEXTURE_2D (0x0DE1) -- Apple used its own
    // number and the games pass it. An untranslated 0x84F5 reaches the GL layer
    // as an unknown target and every bind and upload is quietly dropped, which
    // is why Bejeweled uploaded four textures, bound them 130 times, and still
    // came out 99% white.
    constexpr u32 kFirmwareTexture2D = 0x84F5;
    constexpr u32 kGlTexture2D = 0x0DE1;
    const HleFunction* gl_matrix_mode_p = registry.find("gl", "glMatrixMode");
    const HleFunction* gl_texture_scale_p = registry.find("gl", "glScalef");
    const HleFunction* gl_texture_identity_p = registry.find("gl", "glLoadIdentity");

    const auto map_target = [](u32 target) {
        // LOG the raw target, because what these titles actually pass decides
        // whether the mapping below is a translation or a silent retarget.
        //
        // LLE corrected this on 2026-09-19: 0x84F5 is NOT a spelling of
        // GL_TEXTURE_2D. The firmware's validator (0x10106460) accepts
        // 0x0DE1 GL_TEXTURE_2D, 0x84F5 GL_TEXTURE_RECTANGLE and 0x8513
        // GL_TEXTURE_CUBE_MAP side by side -- three distinct targets.
        //
        // That matters here: RECTANGLE takes UNNORMALISED texel coordinates
        // natively, which is exactly the convention these games use, and is the
        // natural fit for their NPOT sheets (322x242, 512x45, 220x22).
        if (getenv("CLICKHLE_LOG_TEX_TARGET") != nullptr) {
            CLICKHLE_INFO(kCategory)
                << "texture target 0x" << std::hex << target << std::dec;
        }
        return target == kFirmwareTexture2D ? kGlTexture2D : target;
    };

    // Installs the texel -> normalised texture matrix for whatever texture is
    // bound RIGHT NOW.
    //
    // This used to run only on glBindTexture, and that was wrong. Measured on
    // Zuma's level-select frame by reading GL_TEXTURE_MATRIX back at each draw:
    // of 2598 draws of texture 2 (322x242), only **186** had the matching
    // 1/322, 1/242 scale. 1172 carried 1/512, 1/45 -- texture 4's -- and 1240
    // carried 1/510, 1/212 -- texture 7's. The scale was whatever the last
    // *bind* had installed, which is not the same thing as the texture the draw
    // actually samples.
    //
    // The visible result was a full-screen background quad sampling a sliver of
    // its sheet: correct vertices (0,0,0,1)-(320,240), identity modelview, a
    // correct y-down ortho, correct texel coords 1..321 x 1..241, correct
    // texture contents -- and a squashed 44-row band on screen, because the
    // coordinates were being divided by another texture's dimensions.
    //
    // Installing at DRAW time makes it independent of bind ordering, which is
    // the only thing that can be relied on.
    const auto install_texel_scale = [gl_matrix_mode_p, gl_texture_scale_p,
                                      gl_texture_identity_p,
                                      call_with](HleCallFrame& f) {
        const auto size = g_texture_size.find(g_bound_texture);
        if (size == g_texture_size.end() || size->second.first == 0 ||
            size->second.second == 0 || gl_matrix_mode_p == nullptr ||
            gl_texture_scale_p == nullptr || gl_texture_identity_p == nullptr) {
            return;
        }
        float inset = 0.0f;
        if (const char* v = std::getenv("CLICKHLE_TEXEL_INSET")) {
            inset = static_cast<float>(std::atof(v));
        }
        const float dw = static_cast<float>(size->second.first) - inset;
        const float dh = static_cast<float>(size->second.second) - inset;
        constexpr u32 kTextureMode = 0x1702;
        constexpr u32 kModelviewMode = 0x1700;
        call_with(f, gl_matrix_mode_p, {kTextureMode});
        call_with(f, gl_texture_identity_p, {});
        call_with(f, gl_texture_scale_p,
                  {endian::float_to_bits(dw != 0.0f ? 1.0f / dw : 1.0f),
                   endian::float_to_bits(dh != 0.0f ? 1.0f / dh : 1.0f),
                   endian::float_to_bits(1.0f)});
        call_with(f, gl_matrix_mode_p, {kModelviewMode});
    };


    // --- identified ordinals -------------------------------------------------
    //
    // #12 and #13 were established together, from Tetris and Zuma:
    //
    //     mov r0,#0 ; mov r1,#0 ; mov r2,#0 ; mov r3,#0x3f800000
    //     bl  <#13>
    //     mov r0,#0x4000
    //     bl  <#12>
    //
    // 0x3f800000 is 1.0f and 0x4000 is GL_COLOR_BUFFER_BIT, so that is
    // `glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT)` -- opaque black,
    // the canonical start of a frame. The floats also settle a question the
    // fixed-point pipeline had left open: this API takes IEEE floats, not the
    // 16.16 the geometry uses.
    forward(12, "glClear", 1);
    forward(13, "glClearColor", 4);

    // #157 takes two arguments, both literal zero, and sits in tail position --
    // the last call a function makes before returning. Nine titles stop here
    // immediately after their first clear, which is where a frame ends.
    // `eglSwapBuffers(display, surface)` with null handles fits both the shape
    // and the position.
    //
    // PROVISIONAL, and cheap to falsify: if this is right the frame counter
    // starts moving and --frames begins to bound a run. If it is wrong the
    // counter stays at zero and this line comes back out.
    // gl_swap_buffers takes no arguments; we had declared two.
    //
    // It also returns 1, unconditionally, on its only return path
    // (0x102615e0). Forwarding straight to the GL handler left r0 holding
    // whatever that handler last wrote, which is not the same thing -- every
    // title calls this every frame, so a stray zero here is read by any caller
    // that tests it.
    if (const HleFunction* swap = registry.find("gl", "eglSwapBuffers")) {
        registry.add(kModule, 157, "eglSwapBuffers", 0, [swap](HleCallFrame& f) {
            swap->handler(f);
            f.ret(1);
        });
    }

    // #35(cap) = glDisable. One argument, a GL capability; Sudoku passes
    // 0x0B44, GL_CULL_FACE.
    //
    // We had this as glEnable, reasoning that GL_CULL_FACE is off by default
    // so calling it must be turning it on. Plausible, and wrong: a 2D title
    // disables culling *defensively*, without caring what the default was.
    // Corrected against the DogParty import map, which names it gl_disable.
    if (const HleFunction* gl_disable = registry.find("gl", "glDisable")) {
        fn(35, "glDisable", 1,
           [gl_disable, map_target, call_with](HleCallFrame& f) {
               call_with(f, gl_disable, {map_target(f.args.next_u32())});
           });
    }

    // #40(index) -- enables the attribute array #137 has just configured.
    // One argument, and every site pairs it with the matching #137 call:
    //
    //     bl <#137>   ; (0, 4, GL_FIXED, ...)
    //     mov r0, #0
    //     bl <#40>    ; enable array 0
    //
    // ES 1.x names its arrays rather than indexing them, so the index maps
    // onto the enumerant. An earlier probe of this ordinal printed
    // (0, 320, 240, 16, ...) and it was read as a surface-creation call: those
    // were r1-r3 left over from the preceding call, which is what a probe
    // shows when it reads past the true arity.
    // Array index -> ES 1.x client-state enumerant.
    //
    // 0 and 1 were established from call sites pairing #137 with #40. Index 2
    // is the colour array, from the data itself: Tetris configures it as
    // size=4, GL_FIXED, and the values are (0.0, 0.3, 1.0, 1.0) per vertex --
    // four normalised components with an opaque alpha. A normal array would be
    // size 3 and unit length, so this is not ambiguous. Ignoring it left every
    // coloured quad drawn in whatever flat colour was current, which is what
    // Tetris's menu panel was.
    const auto client_array = [](u32 index) -> u32 {
        constexpr u32 kVertexArray = 0x8074;
        constexpr u32 kColorArray = 0x8076;
        constexpr u32 kTexCoordArray = 0x8078;
        switch (index) {
            case 0: return kVertexArray;
            case 1: return kTexCoordArray;
            case 2: return kColorArray;
            default: return 0;
        }
    };

    const HleFunction* gl_enable_client =
        registry.find("gl", "glEnableClientState");
    if (gl_enable_client != nullptr) {
        fn(40, "glEnableClientState", 1,
           [gl_enable_client, call_with, client_array](HleCallFrame& f) {
               const u32 index = f.args.next_u32();
               const u32 array = client_array(index);
               if (std::getenv("CLICKHLE_DUMP_ARRAYS") != nullptr) {
                   CLICKHLE_INFO(kCategory)
                       << "#40 enableClientState index=" << index
                       << " -> array=0x" << std::hex << array << std::dec;
               }
               if (array == 0) {
                   CLICKHLE_WARN(kCategory)
                       << "#40 array index " << index
                       << " is unidentified; ignored";
                   f.ret(0);
                   return;
               }
               call_with(f, gl_enable_client, {array});
           });
    }

    // --- matrix objects ------------------------------------------------------
    //
    // The matrix ordinals all take a leading object, and ES 1.x has no such
    // handle -- it has a *mode* and a stack per mode. Which object is which is
    // inferred from what is done to it: an object given an orthographic
    // projection is the projection matrix, and anything else is the modelview.
    //
    // Derived from behaviour rather than from the order the objects appear,
    // because order is an accident of the title and behaviour is not. Tetris
    // uses two objects and gives ortho to the first; a title that did it the
    // other way round would still be read correctly.
    static std::map<GuestAddr, u32> matrix_mode;
    constexpr u32 kProjection = 0x1701;
    constexpr u32 kModelview = 0x1700;
    // How many times #167 (glOrthof) has been called this run. LLE's
    // corpus-wide sweep (2026-09-19): every title that composes matrices via
    // #175/#125 also calls #167 at least once -- establishing a standing
    // legacy projection alongside its per-object work -- except Texas
    // Hold'em, which calls #175 five times, #125 five times, and #167 zero.
    // That is the whole corpus's answer to "does this title's #175 result
    // belong in the legacy modelview slot, under our standing ortho, or does
    // it need to stand alone as its own full transform": a title with zero
    // #167 calls has no standing projection to share, in a corpus where
    // everyone else does. See #175's `else` branch below.
    static u32 g167_calls = 0;
    // Counts #169/#171/#173/#175 calls -- any REAL transform path other than
    // #149. Sims Bowling/Sims Pool call #149 (paired 1:1 with #159) and
    // NONE of these, ever, in the whole recorded run (STATUS.md, blocker 2)
    // -- #149 is their only transform mechanism, so ignoring it (the current
    // corpus-wide default, since every past #149 attempt broke titles that
    // DO have another path) leaves modelview at permanent identity for them
    // specifically. This is the discriminator for a #149 attempt scoped to
    // exactly that shape, the same way g167_calls==0 scopes #125 to Texas
    // Hold'em alone.
    static u32 g_other_transform_calls = 0;

    // --- one matrix per object, kept here ------------------------------------
    //
    // ES 1.x has a *mode* with one stack each; this API has objects. Mapping
    // every non-projection object onto GL_MODELVIEW looked reasonable and is
    // actively destructive: Tetris drives three of them, and its per-glyph
    // sequence is
    //
    //     #169 translate(A, x, y)     <- the text cursor
    //     #165 load_identity(B)       <- a different object entirely
    //     #165 load_identity(0)
    //     draw
    //
    // Collapsed onto one stack, the load-identity on B wipes A's translation
    // before the draw. Every one of Tetris's 9858 glyph quads a frame was
    // therefore drawn at the origin, stacked on top of each other in the
    // corner -- which looks exactly like text that is never drawn at all.
    //
    // So each object keeps its own matrix here, ops mutate that, and only the
    // object acting as the modelview is pushed to GL. An object earns that role
    // by receiving a translate, scale or rotate: those are geometry transforms,
    // and an object that only ever gets load-identity and mult-matrix is
    // something else (a texture matrix, most likely).
    struct Matrix4 {
        float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    };
    static std::map<GuestAddr, Matrix4> matrix_store;
    /// Objects that have received a translate, scale or rotate. Those are
    /// geometry transforms, so such an object is the modelview; one that only
    /// ever gets load-identity is something else.
    static std::set<GuestAddr> geometry_objects;
    /// Objects #175 has read as a compose operand at least once since they
    /// were last built. Titles that reset a shared transform buffer with
    /// #165 before every rebuild never populate this (matrix_store[object]
    /// is already about to be overwritten either way, so marking it here is
    /// harmless and inert for them). Texas Hold'em's shared buffer
    /// (0x18059808) never calls #165 at all -- confirmed by call-count on
    /// live execution, 2026-09-19: #165 fires zero times on it while
    /// #169/#173 each fire once a frame -- so without this, its translate
    /// and rotate compose onto whatever #175 already consumed last frame,
    /// forever, instead of starting fresh. An approximately-270-degree
    /// rotation compounding every single frame with no reset does not
    /// converge; it spirals through multiples of 270 degrees. #169/#171/
    /// #173 check this set and reset-then-clear on the first touch after a
    /// consume, which is a strictly narrower trigger than "every frame" and
    /// requires no frame-count plumbing across translation units.
    static std::set<GuestAddr> consumed_since_build;
    /// Objects #175 has EVER read as a compose operand. LLE's reading of the
    /// real #169 firmware (2026-09-19): it takes the destination matrix as an
    /// explicit pointer argument and writes only that object -- "translate
    /// THIS matrix", not "translate the current GL_MODELVIEW". Our #169/#171/
    /// #173 handler stores per-object correctly but then unconditionally
    /// pushes the result to GL_MODELVIEW regardless, which is the right
    /// behaviour ONLY for an object meant to BE the modelview directly (the
    /// model six other titles rely on -- Tetris's per-glyph text cursor is
    /// translated and drawn with no #175 involved at all). Texas Hold'em's
    /// camera object never draws directly; it exists purely to be composed
    /// via #175 later. Once an object has been read by #175 at least once,
    /// it has revealed itself as a compose-only scratch buffer, and further
    /// #169/#171/#173 touches on it should stop leaking into GL_MODELVIEW --
    /// #175 (or #165's existing conditional push) owns installing it from
    /// then on. The first touch, before #175 has ever consumed it, still
    /// pushes directly, unchanged from today, because there is no way to
    /// know in advance which kind of object it will turn out to be.
    static std::set<GuestAddr> composed_by_175;

    // Column-major, matching GL: result = a * b.
    const auto multiply = [](const Matrix4& a, const Matrix4& b) {
        Matrix4 out;
        for (u32 col = 0; col < 4; ++col) {
            for (u32 row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (u32 k = 0; k < 4; ++k) {
                    sum += a.m[k * 4 + row] * b.m[col * 4 + k];
                }
                out.m[col * 4 + row] = sum;
            }
        }
        return out;
    };

    // Pushes an object's matrix into a GL matrix stack (GL_MODELVIEW or
    // GL_PROJECTION -- `gl_mode` is one of kModelview/kProjection).
    static GuestAddr upload_scratch = 0;
    const auto load_matrix = [call_with](HleCallFrame& f, u32 gl_mode,
                                         const Matrix4& matrix) {
        const HleRegistry& registry_ref = f.emulator.registry();
        const HleFunction* mode = registry_ref.find("gl", "glMatrixMode");
        const HleFunction* load = registry_ref.find("gl", "glLoadMatrixf");
        if (mode == nullptr || load == nullptr) {
            return;
        }
        if (upload_scratch == 0) {
            upload_scratch = f.emulator.heap().allocate(16 * sizeof(u32));
        }
        if (upload_scratch == kNullGuest) {
            return;
        }
        for (u32 i = 0; i < 16; ++i) {
            (void)f.memory.write<u32>(upload_scratch + 4 * i,
                                      endian::float_to_bits(matrix.m[i]));
        }
        call_with(f, mode, {gl_mode});
        call_with(f, load, {upload_scratch});
    };
    // Pushes an object's matrix into GL_MODELVIEW.
    const auto load_modelview = [load_matrix](HleCallFrame& f,
                                              const Matrix4& matrix) {
        load_matrix(f, kModelview, matrix);
    };


    // The firmware owns the screen projection; the game supplies model
    // transforms in pixel space.
    //
    // Pac-Man is the evidence. Its vertices are (0,0,0,1) and (320,0,0,1) in
    // 16.16 -- pixel coordinates -- and the matrix it uploads is the identity.
    // Nothing it calls establishes a projection, so the firmware must have one
    // standing. With an identity projection those coordinates clip at NDC 1 and
    // fill from the screen centre to the top-right: exactly the 160x120
    // quadrant of white we were getting, with 3,249 draw calls behind it.
    //
    // So we install the screen ortho once, as the firmware does, and let the
    // game's matrices be the modelview.
    static bool screen_projection_set = false;
    static GuestAddr screen_matrix = 0;
    // A projection the GUEST asked for via #167 before a GL context existed.
    //
    // #167 used to mark the projection "set" unconditionally and then issue a
    // glMatrixMode/glLoadMatrixf pair that went nowhere, because the real GL
    // context is created lazily by the first swap. That is the same
    // lazy-context shape already fixed three times elsewhere in this file
    // (ensure_screen_projection's own latch, the blend enable, glGenTextures)
    // -- a fourth instance, this time in the handler for the title's OWN
    // ortho. Measured on iQuiz (2026-09-12): `#167 display_open=0`, so its
    // ortho was discarded, the latch was spent so the firmware default never
    // went in either, and PROJECTION stayed at GL's identity for the whole
    // run -- geometry spanning x=60..260 sits far outside the [-1,1] clip
    // range, so all 60 of its draws were clipped away and every frame read
    // 0/320 non-black pixels.
    //
    // Dropping it is not an option (the title is entitled to its own
    // orientation, per the table below), and neither is substituting the
    // firmware default. So it is remembered and re-applied on the first draw
    // that has a context to put it in.
    static bool pending_guest_projection = false;
    static Matrix4 pending_guest_ortho{};
    const HleFunction* gl_matrix_mode = registry.find("gl", "glMatrixMode");
    const HleFunction* gl_load_matrix = registry.find("gl", "glLoadMatrixf");
    const auto ensure_screen_projection = [gl_matrix_mode, gl_load_matrix,
                                           call_with](HleCallFrame& f) {
        if (std::getenv("CLICKHLE_DUMP_SCREENPROJ") != nullptr) {
            static u32 calls = 0;
            if (calls < 10) {
                ++calls;
                CLICKHLE_INFO(kCategory)
                    << "ensure_screen_projection: set=" << screen_projection_set
                    << " mode_null=" << (gl_matrix_mode == nullptr)
                    << " load_null=" << (gl_load_matrix == nullptr)
                    << " display_open=" << f.emulator.display().is_open();
            }
        }
        if (screen_projection_set || gl_matrix_mode == nullptr ||
            gl_load_matrix == nullptr) {
            return;
        }
        // A title can issue its first draw before its first buffer swap --
        // Texas Hold'em and all three SAT Prep titles do. The real GL context
        // does not exist until Display::open(), which the swap path creates
        // lazily, so a glMatrixMode/glLoadMatrixf pair issued here before that
        // has no context to land in and is silently discarded. Spending the
        // one-shot latch on that no-op left the *real* context, created a
        // moment later, stuck at GL's own default identity projection for the
        // rest of the run -- every draw clipped away, four titles rendering
        // solid black despite thousands of correct draw calls behind them.
        // Wait for the context to actually exist before consuming the latch.
        if (!f.emulator.display().is_open()) {
            return;
        }
        screen_projection_set = true;
        constexpr u32 kProjectionMode = 0x1701;
        constexpr u32 kModelviewMode = 0x1700;

        // Built and loaded directly rather than issued as glOrtho.
        //
        // glOrtho *multiplies* the current matrix, so it has to be preceded by
        // a load-identity, and it takes six arguments -- two of them on the
        // guest stack. Driving that through the guest ABI applied the
        // projection twice, shrinking a 320x240 scene by 320 into a single
        // white pixel. A matrix is one pointer argument and cannot compound.
        //
        // The firmware's standing projection, for titles that never set one.
        //
        // Column-major ortho(0, 320, 240, 0, -1, 1): y-DOWN, origin top-left.
        //
        // This is a default, not a policy. #167 is a faithful glOrtho with no
        // sign flip and no special-casing (confirmed from the firmware by the
        // LLE track), so the framework has no opinion about orientation --
        // whatever a title passes is what it gets. Titles do disagree, and they
        // are entitled to:
        //
        //     Tetris     ortho(0, 320, 0, 240)    y-up
        //     PAC-MAN    ortho(0, 320, 0, 240)    y-up
        //     Bejeweled  ortho(0, 320, 240, 0)    y-down
        //     Mahjong    never calls #167 at all
        //
        // So this matrix only applies to the Mahjong case, and y-down is what
        // Mahjong needs: with y-up its whole loading screen renders mirrored.
        const float matrix[16] = {
            2.0f / 320.0f,  0.0f,           0.0f,  0.0f,
            0.0f,          -2.0f / 240.0f,  0.0f,  0.0f,
            0.0f,           0.0f,          -1.0f,  0.0f,
           -1.0f,           1.0f,           0.0f,  1.0f,
        };
        if (screen_matrix == 0) {
            screen_matrix = f.emulator.heap().allocate(sizeof(matrix));
        }
        if (screen_matrix == kNullGuest) {
            return;
        }
        // A projection the guest asked for itself wins over the default. It
        // was built before a context existed, so this is the first chance to
        // actually land it.
        const bool use_guest = pending_guest_projection;
        for (u32 i = 0; i < 16; ++i) {
            const float v = use_guest ? pending_guest_ortho.m[i] : matrix[i];
            (void)f.memory.write<u32>(screen_matrix + 4 * i,
                                      endian::float_to_bits(v));
        }
        call_with(f, gl_matrix_mode, {kProjectionMode});
        call_with(f, gl_load_matrix, {screen_matrix});
        call_with(f, gl_matrix_mode, {kModelviewMode});
        if (use_guest) {
            pending_guest_projection = false;
            CLICKHLE_INFO(kCategory)
                << "installed the GUEST's deferred #167 projection (diag "
                << pending_guest_ortho.m[0] << ", "
                << pending_guest_ortho.m[5] << ")";
        } else {
            CLICKHLE_INFO(kCategory)
                << "installed the firmware's 320x240 screen projection";
        }
    };

    // Game matrices are the modelview.
    //
    // The import map calls these "uniform_matrix", which is the shape of a
    // shader interface: one matrix carrying the whole transform, not GL's two
    // stacks. Pac-Man is the evidence -- it uploads matrices with #125 and
    // never calls #167 at all, so a rule that only recognised a projection when
    // it saw an ortho left its projection at identity. Its geometry is in pixel
    // coordinates, so with an identity projection every triangle covered the
    // screen: 399 draw calls and a solid white frame.
    //
    // Loading the game's matrix into the projection and leaving the modelview
    // identity puts a single combined transform where it belongs.

    // #167(target, left, right, bottom, top, near, far) -- the projection.
    //
    // Probed in Tetris:
    //     #167(0x18025488, 0.0f, 320.0f, 0.0f, 240.0f, -1.0f, 1.0f)
    //
    // 0x43a00000 is 320.0f and 0x43700000 is 240.0f, so this is
    // glOrthof(0, 320, 0, 240, -1, 1) -- a textbook 2D orthographic projection
    // over the panel, with the object being configured passed first.
    //
    // This is also the honest confirmation of 320x240. An earlier probe of #40
    // appeared to show the same numbers and was register residue; three IEEE
    // floats in a projection call are not.
    const HleFunction* gl_ortho = registry.find("gl", "glOrthof");
    if (gl_ortho != nullptr) {
        fn(167, "glOrthof", 7, [](HleCallFrame& f) {
            ++g167_calls;
            const GuestAddr object = f.args.next_ptr();
            const float left = endian::bits_to_float(f.args.next_u32());
            const float right = endian::bits_to_float(f.args.next_u32());
            const float bottom = endian::bits_to_float(f.args.next_u32());
            const float top = endian::bits_to_float(f.args.next_u32());
            const float near_plane = endian::bits_to_float(f.args.next_u32());
            const float far_plane = endian::bits_to_float(f.args.next_u32());
            CLICKHLE_DEBUG(kCategory)
                << "glOrthof(" << left << ", " << right << ", " << bottom
                << ", " << top << ", " << near_plane << ", " << far_plane
                << ")";
            if (std::getenv("CLICKHLE_DUMP_167OBJ") != nullptr) {
                CLICKHLE_INFO(kCategory)
                    << "#167 object=0x" << std::hex << object << std::dec
                    << " display_open=" << f.emulator.display().is_open();
            }

            // An object handed a projection *is* the projection matrix, and a
            // title that sets its own keeps it: installing the firmware default
            // over the top forced every title to y-up, which is why Bejeweled
            // -- which explicitly asks for y-down with ortho(0, 320, 240, 0) --
            // was drawn upside down.
            matrix_mode[object] = kProjection;

            // Built and loaded rather than forwarded to glOrtho.
            //
            // glOrtho *multiplies* the current matrix. Forwarding it applies
            // the projection on top of whatever is already there, and a second
            // call then squares it -- Bejeweled calls #167 twice and collapsed
            // to two colours. The firmware's own #167 writes all sixteen
            // elements unconditionally (it has no branches at all), so a load
            // is what it actually does.
            const float width = right - left;
            const float height = top - bottom;
            const float depth = far_plane - near_plane;
            if (width == 0.0f || height == 0.0f || depth == 0.0f) {
                CLICKHLE_WARN(kCategory) << "degenerate glOrthof; ignored";
                f.ret(0);
                return;
            }
            Matrix4 ortho;
            ortho.m[0] = 2.0f / width;
            ortho.m[5] = 2.0f / height;
            ortho.m[10] = -2.0f / depth;
            ortho.m[12] = -(right + left) / width;
            ortho.m[13] = -(top + bottom) / height;
            ortho.m[14] = -(far_plane + near_plane) / depth;
            matrix_store[object] = ortho;

            // No GL context yet: the load below would be silently discarded.
            // Remember it and let ensure_screen_projection apply it on the
            // first draw that has somewhere to put it, and crucially do NOT
            // claim the projection is set -- spending the latch on a no-op is
            // exactly what left iQuiz at GL's identity projection, black for
            // the whole run. See pending_guest_projection above.
            if (!f.emulator.display().is_open()) {
                pending_guest_projection = true;
                pending_guest_ortho = ortho;
                CLICKHLE_INFO(kCategory)
                    << "#167 before any GL context; deferring the guest's "
                       "projection until one exists";
                f.ret(0);
                return;
            }
            screen_projection_set = true;

            const HleRegistry& registry_ref = f.emulator.registry();
            const HleFunction* mode = registry_ref.find("gl", "glMatrixMode");
            const HleFunction* load = registry_ref.find("gl", "glLoadMatrixf");
            if (mode == nullptr || load == nullptr) {
                f.ret(0);
                return;
            }
            static GuestAddr ortho_scratch = 0;
            if (ortho_scratch == 0) {
                ortho_scratch = f.emulator.heap().allocate(16 * sizeof(u32));
            }
            if (ortho_scratch == kNullGuest) {
                f.ret(0);
                return;
            }
            for (u32 i = 0; i < 16; ++i) {
                (void)f.memory.write<u32>(ortho_scratch + 4 * i,
                                          endian::float_to_bits(ortho.m[i]));
            }
            {
                HleCallFrame inner(f.cpu, f.memory, f.emulator);
                inner.cpu.set_reg(0, kProjection);
                mode->handler(inner);
            }
            {
                HleCallFrame inner(f.cpu, f.memory, f.emulator);
                inner.cpu.set_reg(0, ortho_scratch);
                load->handler(inner);
            }
            f.ret(0);
        });
    }

    // #165 gl_load_identity(object), #169 translate, #171 scale, #173 rotate.
    // Each selects the object's matrix mode, then applies the operation.
    const HleFunction* gl_identity = registry.find("gl", "glLoadIdentity");
    if (gl_identity != nullptr) {
        fn(165, "glLoadIdentity", 1,
           [load_modelview](HleCallFrame& f) {
               const GuestAddr object = f.args.next_ptr();
               // One-off (2026-09-19): is Texas Hold'em's shared transform
               // buffer (0x18059808) ever reset between objects, or does its
               // translate+rotate compose onto whatever the previous object
               // left behind? CLICKHLE_DUMP_165=1.
               if (std::getenv("CLICKHLE_DUMP_165") != nullptr &&
                   object == 0x18059808) {
                   CLICKHLE_INFO(kCategory) << "#165 reset 0x18059808";
               }
               if (std::getenv("CLICKHLE_DUMP_CURSOR") != nullptr &&
                   object == 0x180254c8) {
                   CLICKHLE_INFO(kCategory) << "#165 reset cursor 0x180254c8";
               }
               // Resets *this object*, and touches GL only if this object is
               // the one driving geometry. Issuing glLoadIdentity here
               // unconditionally is what wiped Tetris's text cursor: the game
               // resets a different matrix between every glyph.
               matrix_store[object] = Matrix4{};
               consumed_since_build.erase(object);
               if (geometry_objects.count(object) != 0) {
                   load_modelview(f, matrix_store[object]);
               }
               f.ret(0);
               return;
           });
    }

    struct MatrixOp {
        u32 ordinal;
        const char* name;
        u8 arity;      ///< including the leading object
    };
    // Argument counts come from the DogParty import map; the leading object is
    // dropped and the rest passed through in GL's order.
    for (const MatrixOp& op : {MatrixOp{169, "glTranslatef", 4},
                               MatrixOp{171, "glScalef", 4},
                               MatrixOp{173, "glRotatef", 5}}) {
        const HleFunction* target = registry.find("gl", op.name);
        if (target == nullptr) {
            continue;
        }
        fn(op.ordinal, op.name, op.arity,
           [op, multiply, load_modelview](HleCallFrame& f) {
               ++g_other_transform_calls;
               const GuestAddr object = f.args.next_ptr();
               u32 words[4] = {0, 0, 0, 0};
               const u32 count = op.arity - 1u;
               for (u32 i = 0; i < count && i < 4; ++i) {
                   words[i] = f.args.next_u32();
               }
               if (std::getenv("CLICKHLE_DUMP_165") != nullptr &&
                   object == 0x18059808) {
                   CLICKHLE_INFO(kCategory)
                       << "#" << op.ordinal << " on 0x18059808 args="
                       << endian::bits_to_float(words[0]) << " "
                       << endian::bits_to_float(words[1]) << " "
                       << endian::bits_to_float(words[2]) << " "
                       << endian::bits_to_float(words[3]);
               }
               // Arguments are IEEE floats here, not the 16.16 the geometry
               // uses. Checked rather than assumed: Tetris's translates read
               // (160, 238), (20, 60), (-25, -50) as floats and 17184, 17262 as
               // 16.16. The API is genuinely mixed -- the vertex arrays are
               // GL_FIXED and #175 uploads a fixed matrix -- so each ordinal
               // has to be established separately.
               const float x = endian::bits_to_float(words[0]);
               const float y = endian::bits_to_float(words[1]);
               const float z = endian::bits_to_float(words[2]);

               // Receiving a geometry transform is what marks an object as the
               // modelview.
               geometry_objects.insert(object);
               // First touch since #175 last consumed this object: start the
               // next build fresh instead of composing onto what was already
               // handed off. A title that resets via #165 before every reuse
               // never reaches this (matrix_store[object] is about to be
               // overwritten by that reset either way), so this is inert for
               // the six titles already relying on the accumulate-onto-
               // current model. It matters for a title like Texas Hold'em,
               // which never calls #165 on its shared transform buffer at
               // all (confirmed zero #165 calls against one #169/#173 call
               // each per frame, 2026-09-19) -- without this, translate and
               // rotate compose onto last frame's already-consumed result
               // forever, and a rotation compounding every frame with no
               // reset spirals rather than converging.
               // 2026-09-11/12: Tetris's text cursor (0x180254c8) is broken
               // (glyphs collapse to one position) and this reset is part of
               // why, but scoping or removing it here is NOT the fix -- see
               // agent-comms and STATUS.md. Root cause traced further: the
               // guest itself writes a fresh 64-byte matrix into this object
               // via a raw `str` loop (0x180039c8, a 16-slot ring-buffer
               // copy-out), completely outside any GL ordinal, which our
               // host-side matrix_store cache never observes -- so we keep
               // accumulating onto stale data regardless of this reset's
               // scoping. LLE and oracle independently confirmed (2026-09-12,
               // by disassembly/structural sweep and by direct firmware
               // execution respectively) that guest memory is the real
               // source of truth here, not a fixable heuristic on our
               // reset. THREE implementation attempts at a guest-memory-
               // backed matrix_store (unconditional; gated on "matrix_store
               // already has an entry"; scoped to the 4 addresses LLE's
               // sweep confirmed) were each tried live and each produced a
               // worse regression than this known bug -- up to and including
               // breaking the plain idle title screen with no user input at
               // all, a mechanism not yet understood. Reverted all three;
               // see STATUS.md for the full trail. Left exactly as shipped
               // below pending a correctly-implemented fix.
               if (consumed_since_build.erase(object) != 0) {
                   matrix_store[object] = Matrix4{};
               }
               Matrix4& current = matrix_store[object];

               Matrix4 delta;
               if (op.ordinal == 169) {          // translate
                   delta.m[12] = x;
                   delta.m[13] = y;
                   delta.m[14] = z;
               } else if (op.ordinal == 171) {   // scale
                   delta.m[0] = x;
                   delta.m[5] = y;
                   delta.m[10] = z;
               } else {                          // rotate(angle, x, y, z)
                   const float angle = x * 3.14159265358979f / 180.0f;
                   const float ax = y;
                   const float ay = z;
                   const float az = endian::bits_to_float(words[3]);
                   const float len = std::sqrt(ax * ax + ay * ay + az * az);
                   const float nx = len > 0.0f ? ax / len : 0.0f;
                   const float ny = len > 0.0f ? ay / len : 0.0f;
                   const float nz = len > 0.0f ? az / len : 1.0f;
                   const float c = std::cos(angle);
                   const float s = std::sin(angle);
                   const float t = 1.0f - c;
                   delta.m[0] = t * nx * nx + c;
                   delta.m[1] = t * nx * ny + s * nz;
                   delta.m[2] = t * nx * nz - s * ny;
                   delta.m[4] = t * nx * ny - s * nz;
                   delta.m[5] = t * ny * ny + c;
                   delta.m[6] = t * ny * nz + s * nx;
                   delta.m[8] = t * nx * nz + s * ny;
                   delta.m[9] = t * ny * nz - s * nx;
                   delta.m[10] = t * nz * nz + c;
               }
               current = multiply(current, delta);
               // One-off: is #169's delta itself changing per glyph, or is
               // ClickHLE re-resetting/misreading the same object every call?
               // Tetris's text cursor specifically -- CLICKHLE_DUMP_CURSOR=1.
               if (op.ordinal == 169 && std::getenv("CLICKHLE_DUMP_CURSOR") != nullptr) {
                   CLICKHLE_INFO(kCategory)
                       << "#169 obj=0x" << std::hex << object << std::dec
                       << " delta=(" << x << "," << y << "," << z << ")"
                       << " current.trans=(" << current.m[12] << ","
                       << current.m[13] << "," << current.m[14] << ")";
               }
               // Push directly only for an object that has never revealed
               // itself as #175 compose-only scratch (see composed_by_175
               // above). LLE, 2026-09-19: real #169 firmware takes the
               // destination as an explicit pointer and writes only that
               // matrix -- "translate THIS object", not "translate the
               // legacy current modelview". An object #175 has already
               // consumed at least once is not meant to draw on its own.
               // TRIED (2026-09-19): the composed_by_175 gate alone stops
               // FUTURE re-corruption but not the first-ever push, which is
               // still wrong for a title like Texas Hold'em where the object
               // is camera-relative scratch, never a per-draw modelview.
               // Live evidence: with #125 now routing dest=0x180dc6c0
               // correctly, the title-art quad (drawn via a *different* path
               // -- #169/#173's own direct push, not #125) still inherited
               // the camera's rotate+translate untouched. Adding
               // g167_calls == 0 -- same corpus-unique-to-Texas-Hold'em
               // discriminator used elsewhere -- suppresses the direct push
               // ENTIRELY for this one title, first touch included, while
               // leaving every other title (where #167 fires early, per
               // LLE's confirmation) completely unaffected. Screenshot every
               // title before trusting this, not just compat_table.py --
               // that already missed one real regression today.
               const bool gated =
                   composed_by_175.count(object) != 0 || g167_calls == 0;
               if (const char* watch = std::getenv("CLICKHLE_WATCH_OBJ")) {
                   if (std::strcmp(watch, "all") == 0 ||
                       object == std::strtoul(watch, nullptr, 0)) {
                       CLICKHLE_INFO(kCategory)
                           << "#" << op.ordinal << " on obj=0x" << std::hex
                           << object << std::dec << " gated=" << gated
                           << " composed=" << (composed_by_175.count(object) != 0)
                           << " g167_calls=" << g167_calls;
                   }
               }
               if (std::getenv("CLICKHLE_DUMP_165") != nullptr &&
                   object == 0x18059808) {
                   CLICKHLE_INFO(kCategory)
                       << "#" << op.ordinal << " on 0x18059808 gated="
                       << gated << " composed_by_175.size()="
                       << composed_by_175.size();
               }
               if (op.ordinal == 169 && std::getenv("CLICKHLE_DUMP_CURSOR") != nullptr) {
                   CLICKHLE_INFO(kCategory)
                       << "#169 obj=0x" << std::hex << object << std::dec
                       << " gated=" << gated << " composed="
                       << (composed_by_175.count(object) != 0)
                       << " g167_calls=" << g167_calls;
               }
               if (!gated) {
                   load_modelview(f, current);
               }
               f.ret(0);
               return;
               {
                   static std::set<std::string> seen;
                   char key[80];
                   std::snprintf(key, sizeof(key), "%u:%x:%x:%x", op.ordinal,
                                 words[0], words[1], words[2]);
                   if (seen.insert(key).second && seen.size() <= 8) {
                       CLICKHLE_INFO(kCategory)
                           << "#" << op.ordinal << " raw 0x" << std::hex
                           << words[0] << " 0x" << words[1] << " 0x"
                           << words[2] << std::dec << "  as16.16("
                           << static_cast<double>(static_cast<i32>(words[0])) / 65536.0
                           << ", "
                           << static_cast<double>(static_cast<i32>(words[1])) / 65536.0
                           << ")  asfloat("
                           << endian::bits_to_float(words[0]) << ", "
                           << endian::bits_to_float(words[1]) << ")";
                   }
               }
           });
    }

    // #37 gl_draw_arrays(mode, first, count) -- the draw call itself, and the
    // reason everything above exists.
    const HleFunction* gl_draw = registry.find("gl", "glDrawArrays");
    if (gl_draw != nullptr) {
        fn(37, "glDrawArrays", 3,
           [gl_draw, ensure_screen_projection,
            install_texel_scale](HleCallFrame& f) {
               // The draw's own arguments have to survive the setup. Installing
               // the projection issues several GL calls through the registers,
               // and doing that first left glDrawArrays reading whatever the
               // last of them had put in r0-r3 -- 3,248 draws collapsed to a
               // single white pixel.
               const u32 saved[4] = {f.cpu.reg(0), f.cpu.reg(1), f.cpu.reg(2),
                                     f.cpu.reg(3)};
               ensure_screen_projection(f);
               install_texel_scale(f);
               for (u32 i = 0; i < 4; ++i) {
                   f.cpu.set_reg(i, saved[i]);
               }
               f.args.reset();
               gl_draw->handler(f);
           });
    }

    // #36 disables an attribute array, the counterpart of #40.
    const HleFunction* gl_disable_client =
        registry.find("gl", "glDisableClientState");
    if (gl_disable_client != nullptr) {
        fn(36, "glDisableClientState", 1,
           [gl_disable_client, call_with, client_array](HleCallFrame& f) {
               const u32 index = f.args.next_u32();
               const u32 array = client_array(index);
               if (array == 0) {
                   f.ret(0);
                   return;
               }
               call_with(f, gl_disable_client, {array});
           });
    }

    // Textures. #4 and #45 line up with GL's own argument order, so they
    // forward directly.
    const HleFunction* gl_bind = registry.find("gl", "glBindTexture");
    const HleFunction* gl_enable_cap = registry.find("gl", "glEnable");
    const HleFunction* gl_texture_scale = registry.find("gl", "glScalef");
    const HleFunction* gl_texture_identity =
        registry.find("gl", "glLoadIdentity");
    const HleFunction* gl_blend_func = registry.find("gl", "glBlendFunc");
    const HleFunction* gl_tex_parameter_i = registry.find("gl", "glTexParameterf");
    if (gl_bind != nullptr) {
        fn(4, "glBindTexture", 2,
           [gl_bind, gl_enable_cap, gl_matrix_mode, gl_texture_scale,
            gl_texture_identity, gl_blend_func, gl_tex_parameter_i, map_target,
            call_with](HleCallFrame& f) {
               const u32 target = map_target(f.args.next_u32());
               const u32 texture = f.args.next_u32();
               g_bound_texture = texture;
               if (std::getenv("CLICKHLE_DUMP_GEN") != nullptr) {
                   CLICKHLE_INFO(kCategory)
                       << "#4 glBindTexture(target=0x" << std::hex << target
                       << ", texture=" << std::dec << texture << ")";
               }
               // Texturing has to be switched on, and nothing switches it on.
               //
               // CORRECTED 2026-09-17, from the oracle's firmware measurement.
               // The old reason given here was "the firmware has a gl_disable
               // and no gl_enable". That is false: `OpenGLES#39` IS `glEnable`,
               // named by RetailOS's own assertion handler in both images.
               //
               // The behaviour below is still right, but the evidence for it is
               // narrower than what was claimed. What is true is about the
               // *corpus*, not the firmware: no title imports #39, while #35
               // (`glDisable`) is imported and called. So these games bind
               // textures and expect them sampled without ever enabling
               // anything, and turn things off when they want them off. ES 1.x
               // defaults the other way round, and
               // without this every bound texture was sampled by a disabled
               // texture unit -- Bejeweled uploaded four, bound them 130 times,
               // and drew 99% white.
               // Same latch-vs-lazy-context bug as ensure_screen_projection: a
               // title whose first glBindTexture happens before its first
               // buffer swap fires this against a GL context that does not
               // exist yet (Display::open() is lazy, on the first
               // eglSwapBuffers). The enable calls are then silent no-ops, but
               // the one-shot latch is already spent -- on Ms. PAC-MAN, texture
               // 2's GL_TEXTURE_2D loss was independently caught and fixed
               // per-draw in install_texel_scale(), which is why that half of
               // this bug was invisible; GL_BLEND has no such backstop, so it
               // silently stayed off for the *entire run*, and a colour-key
               // sprite sheet (opaque magenta behind the glyphs, meant to
               // blend to transparent) rendered as a solid magenta rectangle.
               static bool texturing_enabled = false;
               if (!texturing_enabled && gl_enable_cap != nullptr &&
                   f.emulator.display().is_open()) {
                   texturing_enabled = true;
                   call_with(f, gl_enable_cap, {kGlTexture2D});
                   CLICKHLE_INFO(kCategory) << "enabled texturing";

                   // Blending is on by default for the same reason, and the
                   // same evidence: the firmware exposes gl_disable and no
                   // gl_enable, so a game that wants blending simply never
                   // turns it off. ES 1.x defaults it off, and an alpha-only
                   // texture drawn without it is a solid rectangle -- which is
                   // what Tetris's fonts and menu panels were. A title that
                   // genuinely wants it off still says so through #35.
                   constexpr u32 kGlBlend = 0x0BE2;
                   constexpr u32 kSrcAlpha = 0x0302;
                   constexpr u32 kOneMinusSrcAlpha = 0x0303;
                   call_with(f, gl_enable_cap, {kGlBlend});
                   if (gl_blend_func != nullptr) {
                       call_with(f, gl_blend_func,
                                 {kSrcAlpha, kOneMinusSrcAlpha});
                   }
                   CLICKHLE_INFO(kCategory) << "enabled alpha blending";
               }
               call_with(f, gl_bind, {target, texture});
               // #4 returns 1. Measured by the oracle on both RetailOS images.
               //
               // Their caveat is the more useful half and I am recording it
               // here because it applies to every return value in this file:
               // #4 returns r0 = 1 for a LEGAL target (0x0DE1, 20 instructions)
               // and r0 = 1 for an ILLEGAL one (0x0000, 17344 instructions).
               // Same stop, same value, 866x the work. "It returned 1" is
               // therefore not evidence the call did anything, and a return
               // value alone cannot tell a success from a validated failure.
               f.ret(1);

               // Clamp to edge, every bind.
               //
               // GL defaults to GL_REPEAT, and these sheets are NPOT (322x242,
               // 220x22, 196x17) where ES 1.x does not permit REPEAT at all. A
               // coordinate a hair past the edge then wraps and samples from
               // the opposite side, which is what put a copy of the upper menu
               // along the bottom bar.
               //
               // Done here rather than trusting the guest to set it: #101 was
               // dropping every parameter it did set (see above), so nothing
               // has ever established what these titles actually request.
               if (gl_tex_parameter_i != nullptr) {
                   constexpr u32 kWrapS = 0x2802;
                   constexpr u32 kWrapT = 0x2803;
                   constexpr u32 kClampToEdge = 0x812F;
                   // float_to_bits: glTexParameterf reads its third argument
                   // as a FLOAT. Passing the enum as a plain integer made
                   // 0x812F a denormal rather than 33071, which GL rejected
                   // with GL_INVALID_ENUM -- and since glGetError accumulates,
                   // the error surfaced on the NEXT glTexImage2D and looked
                   // like every texture upload in the run was failing.
                   const u32 clamp_bits =
                       endian::float_to_bits(static_cast<float>(kClampToEdge));
                   call_with(f, gl_tex_parameter_i, {target, kWrapS, clamp_bits});
                   call_with(f, gl_tex_parameter_i, {target, kWrapT, clamp_bits});

                   // And the FILTERS, which matter more than the wrap.
                   //
                   // GL's default minification filter is
                   // GL_NEAREST_MIPMAP_LINEAR, which requires a full mipmap
                   // chain. These titles upload level 0 only, so every texture
                   // was INCOMPLETE -- and an incomplete texture samples solid
                   // white.
                   //
                   // That is exactly Ms. PAC-MAN: 35,519 draws with correct
                   // geometry, correct matrices, real texture contents and
                   // 15-50% non-zero alpha, rendering a screen that is
                   // (255,255,255) in every one of its 76,800 pixels.
                   //
                   // It is also, almost certainly, the original "Bejeweled
                   // uploaded four textures, bound them 130 times and came out
                   // 99% white" note above -- which was treated as a texturing
                   // -enable problem and only half fixed.
                   constexpr u32 kMinFilter = 0x2801;
                   constexpr u32 kMagFilter = 0x2800;
                   constexpr u32 kLinear = 0x2601;
                   const u32 linear_bits =
                       endian::float_to_bits(static_cast<float>(kLinear));
                   call_with(f, gl_tex_parameter_i, {target, kMinFilter, linear_bits});
                   call_with(f, gl_tex_parameter_i, {target, kMagFilter, linear_bits});
               }

               // Rescale texel coordinates into GL's normalised space for
               // whatever is now bound. Done on bind rather than once, because
               // the divisor is the *texture's* size and every sheet differs.
               const auto size = g_texture_size.find(texture);
               if (size != g_texture_size.end() && gl_matrix_mode != nullptr &&
                   gl_texture_scale != nullptr && gl_texture_identity != nullptr &&
                   size->second.first != 0 && size->second.second != 0) {
                   constexpr u32 kTextureMode = 0x1702;
                   constexpr u32 kModelviewMode = 0x1700;
                   // Investigation hook for the banded backgrounds (#60).
                   //
                   // Zuma uploads its backgrounds at 322x242 and 322x222 --
                   // 320x240 with a one-pixel border. If its texel coordinates
                   // are relative to the CONTENT while we divide by the
                   // ALLOCATION, every coordinate is off by 320/322 and the
                   // error accumulates down a background drawn in strips.
                   // CLICKHLE_TEXEL_INSET=N divides by (size - N) instead.
                   float inset = 0.0f;
                   if (const char* v = std::getenv("CLICKHLE_TEXEL_INSET")) {
                       inset = static_cast<float>(std::atof(v));
                   }
                   const float divisor_w =
                       static_cast<float>(size->second.first) - inset;
                   const float divisor_h =
                       static_cast<float>(size->second.second) - inset;
                   call_with(f, gl_matrix_mode, {kTextureMode});
                   call_with(f, gl_texture_identity, {});
                   call_with(f, gl_texture_scale,
                             {endian::float_to_bits(
                                  divisor_w != 0.0f ? 1.0f / divisor_w : 1.0f),
                              endian::float_to_bits(
                                  divisor_h != 0.0f ? 1.0f / divisor_h : 1.0f),
                              endian::float_to_bits(1.0f)});
                   call_with(f, gl_matrix_mode, {kModelviewMode});
               }
           });
    }
    forward(45, "glGenTextures", 2);
    forward(0, "glActiveTexture", 1);
    forward(84, "glPixelStorei", 2);
    // #90/#138: named by reading the firmware's own assert strings, not
    // guessed -- LLE's asserted_names.py mapped every `bl` to the shared
    // "Error inside %s" formatter back to its owning export, cross-checked
    // against 46 already-known names as a control (2026-09-19). Both GL
    // entry points already exist in gles.cpp; this was only a missing
    // ordinal->name link, the same gap #84 had before it got one above.
    forward(90, "glScissor", 4);
    forward(138, "glViewport", 4);

    // The matrix uploads and the constant colour.
    //
    //   #125 gl_uniform_matrix_4fv(4)   float matrix
    //   #149 gl_uniform_matrix_4xv(4)   16.16 fixed matrix
    //   #175 gl_mult_matrix(3)
    //   #148 gl_uniform_4xv(3)          the constant colour every quad is
    //                                   tinted by
    //   #147 gl_uniform_4x_scalar(5)
    //
    // Each leads with the matrix object, as #167 and #165 do, so the object
    // selects the mode and the pointer that follows is the data. The names in
    // the import map are "uniform" because the reversing saw a shader-style
    // interface; ES 1.x reaches the same state through the matrix stack and the
    // colour register, which is what the inherited layer implements.
    // #175(dest, a, b) -- object composition. Registered on its own, because
    // it is NOT the same shape as #125/#149 and sharing a handler with them was
    // a real bug: the rewrite that fixed #175 silently turned Tetris's 55,963
    // #125 calls per 200 frames into no-ops, and the frames stayed plausible.
    if (registry.find("gl", "glLoadMatrixf") != nullptr) {
        fn(175, "gl_matrix_multiply", 3,
           [multiply, load_modelview, load_matrix](HleCallFrame& f) {
               ++g_other_transform_calls;
               const GuestAddr dest = f.args.next_ptr();
               const GuestAddr a = f.args.next_ptr();
               const GuestAddr b = f.args.next_ptr();

               // dest = a * b, and all three are matrix objects. Confirmed from
               // firmware: 0x594 bytes, fully unrolled, no branches, reading
               // a[12] and b[3] directly -- there is no object header, so an
               // "object" and "sixteen floats" are the same thing.
               //
               // We had it as glMultMatrixx with the second operand as matrix
               // data, which sampled an object header and decoded as a
               // convincing identity. Every draw was then positioned by an
               // identity matrix and no frame looked obviously wrong.
               matrix_store[dest] = multiply(matrix_store[a], matrix_store[b]);
               consumed_since_build.insert(a);
               consumed_since_build.insert(b);
               composed_by_175.insert(a);
               composed_by_175.insert(b);

               // Ground truth, not the host-side cache: an operand this
               // session has never seen written through a tracked ordinal
               // (#165/#169/#171/#173) has no reason to be trusted as
               // identity just because matrix_store defaults to identity.
               // Found chasing LLE's question about Texas Hold'em's `a`
               // (2026-09-19): it is NOT identity in guest memory. It is a
               // textbook OpenGL perspective projection --
               //   col0 (1.81,0,0,0) col1 (0,2.41,0,0)
               //   col2 (0,0,-1.0004,-1) col3 (0,0,-0.2,0)
               // -- identifiable purely by shape: the bottom row (m[3], m[7],
               // m[11], m[15]) is (0, 0, -1, 0), the perspective-divide
               // signature (w' = -z), which identity's (0,0,0,1) can never
               // produce. Something writes this straight to guest memory,
               // never through an ordinal we track, which is why
               // matrix_store had no idea and treated it as an ordinary
               // identity-default modelview object -- composing it away into
               // GL_MODELVIEW instead of routing it to GL_PROJECTION where
               // its own shape says it belongs.
               const auto read_matrix = [&f](GuestAddr addr) {
                   Matrix4 m;
                   for (u32 i = 0; i < 16; ++i) {
                       const u32 raw = f.memory.read<u32>(addr + 4 * i).value_or(0);
                       m.m[i] = endian::bits_to_float(raw);
                   }
                   return m;
               };
               const auto looks_like_perspective = [](const Matrix4& m) {
                   constexpr float kEps = 0.05f;
                   return std::fabs(m.m[3]) < kEps && std::fabs(m.m[7]) < kEps &&
                          std::fabs(m.m[11] + 1.0f) < kEps &&
                          std::fabs(m.m[15]) < kEps;
               };
               // A complete, self-sufficient screen-space orthographic
               // projection: bottom row (0,0,0,1) like identity, but a
               // diagonal far too small to be a real modelview scale --
               // 2/320 and 2/240 specifically, found live on the title-art
               // quad's own composition (2026-09-19): mem_a =
               // diag(0.00625, 0.00833, 0.015625, 1). That is not "close to
               // identity", it is the exact firmware screen ortho, already
               // complete on its own. Distinguishing it from a real per-
               // object modelview (which this corpus's titles scale by
               // roughly unit magnitude, not by 1/160) by the diagonal being
               // an order of magnitude smaller than any real object would
               // use.
               const auto looks_like_screen_ortho = [](const Matrix4& m) {
                   constexpr float kEps = 0.001f;
                   return std::fabs(m.m[3]) < kEps && std::fabs(m.m[7]) < kEps &&
                          std::fabs(m.m[11]) < kEps &&
                          std::fabs(m.m[15] - 1.0f) < kEps &&
                          std::fabs(m.m[0]) > 0.0001f && std::fabs(m.m[0]) < 0.05f &&
                          std::fabs(m.m[5]) > 0.0001f && std::fabs(m.m[5]) < 0.05f;
               };
               const Matrix4 mem_a = read_matrix(a);
               const Matrix4 mem_b = read_matrix(b);
               const bool a_is_projection = looks_like_perspective(mem_a);
               const bool b_is_projection = looks_like_perspective(mem_b);
               if (std::getenv("CLICKHLE_DUMP_MODE") != nullptr) {
                   const u32 mode_word =
                       f.memory.read<u32>(0x180597b8).value_or(0xffffffff);
                   CLICKHLE_INFO(kCategory)
                       << "#175 mode=0x" << std::hex << mode_word << std::dec
                       << " dest=0x" << std::hex << dest << " a=0x" << a
                       << " b=0x" << b << std::dec
                       << " a_persp=" << a_is_projection
                       << " b_persp=" << b_is_projection;
                   if (dest == 0x180dc6c0) {
                       CLICKHLE_INFO(kCategory)
                           << "  mem_a=" << mem_a.m[0] << "," << mem_a.m[5]
                           << "," << mem_a.m[10] << "," << mem_a.m[15]
                           << " trans=" << mem_a.m[12] << "," << mem_a.m[13]
                           << "," << mem_a.m[14] << " bound=" << g_bound_texture;
                   }
               }

               // We install the screen projection ourselves, so what GL needs
               // is the other operand -- the model transform. When neither
               // operand is the projection, the composed result is it.
               const auto role_of = [](GuestAddr object) {
                   const auto it = matrix_mode.find(object);
                   return it == matrix_mode.end() ? kModelview : it->second;
               };
               // Sticky once seen: a title whose real projection was found by
               // shape has a standing projection now, same as if #167 had
               // set one, and the "no #167 anywhere, force identity" branch
               // below must stop clobbering it back to identity on every
               // OTHER #175 call site that composes a plain, non-projection
               // object pair (Texas Hold'em has 5 call sites; only one feeds
               // this shape).
               const bool a_is_ortho = looks_like_screen_ortho(mem_a);
               const bool b_is_ortho = looks_like_screen_ortho(mem_b);
               if (std::getenv("CLICKHLE_DUMP_MODE") != nullptr &&
                   dest == 0x180dc6c0) {
                   CLICKHLE_INFO(kCategory)
                       << "  a_is_ortho=" << a_is_ortho
                       << " b_is_ortho=" << b_is_ortho
                       << " bound=" << g_bound_texture;
               }
               static bool shape_projection_installed = false;
               if (a_is_ortho || b_is_ortho) {
                   // Unlike the perspective case below, the OTHER operand
                   // here is not a useful per-object transform -- it's
                   // Texas Hold'em's shared camera buffer (0x18059808),
                   // constant for the whole frame, along for the ride on
                   // every #175 call regardless of whether this particular
                   // draw wants it. Found live (2026-09-19): the title-art
                   // quad's own composition has mem_a as the EXACT firmware
                   // screen ortho (diag 0.00625, 0.00833, 0.015625, 1) --
                   // already complete, self-sufficient, needing nothing
                   // composed in. Installing ortho alone as GL_PROJECTION
                   // and IDENTITY as GL_MODELVIEW (not the other operand --
                   // that's what makes this different from the perspective
                   // branch) is what a flat 2D screen-space quad needs.
                   shape_projection_installed = true;
                   load_matrix(f, kProjection, a_is_ortho ? mem_a : mem_b);
                   load_matrix(f, kModelview, Matrix4{});
                   // matrix_store[dest] was already set above to the stale
                   // multiply(mem_a, mem_b) -- ortho composed with the
                   // camera, exactly the contamination this branch exists to
                   // avoid. #125 reads matrix_store[dest] independently,
                   // later, and will silently overwrite the identity just
                   // installed above with that stale value if it isn't also
                   // corrected here. Found live (2026-09-19): the projection
                   // fix alone visibly took effect, then vanished by the
                   // time the draw actually ran, purely from #125 re-reading
                   // an inconsistent cache -- not a second rendering bug.
                   matrix_store[dest] = Matrix4{};
               } else if (a_is_projection || b_is_projection) {
                   if (!shape_projection_installed) {
                       shape_projection_installed = true;
                       CLICKHLE_INFO(kCategory)
                           << "#175: operand shape identifies a real "
                              "perspective projection outside #167 -- "
                              "routing by shape instead of composing it "
                              "into the modelview";
                   }
                   load_matrix(f, kProjection, a_is_projection ? mem_a : mem_b);
                   load_matrix(f, kModelview, a_is_projection ? mem_b : mem_a);
               } else if (role_of(a) == kProjection) {
                   if (std::getenv("CLICKHLE_DUMP_MODE") != nullptr) {
                       CLICKHLE_INFO(kCategory)
                           << "#175 role_of(a)==Projection, pushing "
                              "matrix_store[b=0x"
                           << std::hex << b << "] as modelview, trans=("
                           << matrix_store[b].m[12] << ","
                           << matrix_store[b].m[13] << ","
                           << matrix_store[b].m[14] << ")" << std::dec;
                   }
                   load_modelview(f, matrix_store[b]);
               } else if (role_of(b) == kProjection) {
                   if (std::getenv("CLICKHLE_DUMP_MODE") != nullptr) {
                       CLICKHLE_INFO(kCategory)
                           << "#175 role_of(b)==Projection, pushing "
                              "matrix_store[a=0x"
                           << std::hex << a << "] as modelview, diag=("
                           << matrix_store[a].m[0] << ","
                           << matrix_store[a].m[5] << ","
                           << matrix_store[a].m[10] << ") trans=("
                           << matrix_store[a].m[12] << ","
                           << matrix_store[a].m[13] << ","
                           << matrix_store[a].m[14] << ")" << std::dec;
                   }
                   load_modelview(f, matrix_store[a]);
               } else {
                   // Neither operand was ever tagged by #167, and neither
                   // looks like a projection by shape either -- across the
                   // whole corpus, that combination (composing via #175 with
                   // no #167 anywhere) only happens in Texas Hold'em (LLE,
                   // 2026-09-19). Every other title that reaches this branch
                   // also calls #167 elsewhere, so `ensure_screen_projection`
                   // never had to act (#167 sets the same latch) and its
                   // standing ortho is not this title's problem. A title with
                   // truly zero #167 calls has no standing projection of its
                   // own to share the pipeline with -- #175's composed
                   // result IS its whole transform, uniform-style, not a
                   // modelview to be multiplied under a separate legacy
                   // ortho. Forcing the projection to identity lets it stand
                   // alone instead of being projected twice -- unless a real
                   // one was already found by shape (above), in which case
                   // THAT is the standing projection now and must not be
                   // clobbered back to identity by an unrelated #175 call
                   // composing two ordinary, non-projection objects.
                   // TRIED dropping !shape_projection_installed (2026-09-19):
                   // made it strictly worse (solid black instead of the
                   // small visible border) and proved something more useful
                   // than either result alone -- with PR forced back to
                   // identity, texture 4's MV was STILL the camera's
                   // rotate+translate (0.4087 diag, trans 0,0,-128),
                   // completely unchanged. That matrix isn't coming from
                   // either #175 composer at all; #169/#173 push it to
                   // GL_MODELVIEW directly on every call (see their handler),
                   // and nothing resets it back to identity before this draw.
                   // Projection was never the real blocker for this specific
                   // quad -- reverted to sticky, which is the better of the
                   // two known states, kept pending the actual answer: what
                   // is supposed to reset modelview to identity for a flat
                   // 2D overlay draw, and why isn't it happening.
                   if (g167_calls == 0 && !shape_projection_installed &&
                       f.emulator.display().is_open()) {
                       const HleRegistry& registry_ref = f.emulator.registry();
                       const HleFunction* mode =
                           registry_ref.find("gl", "glMatrixMode");
                       const HleFunction* load_identity =
                           registry_ref.find("gl", "glLoadIdentity");
                       if (mode != nullptr && load_identity != nullptr) {
                           HleCallFrame proj(f.cpu, f.memory, f.emulator);
                           proj.cpu.set_reg(0, kProjection);
                           mode->handler(proj);
                           HleCallFrame ident(f.cpu, f.memory, f.emulator);
                           load_identity->handler(ident);
                           HleCallFrame back(f.cpu, f.memory, f.emulator);
                           back.cpu.set_reg(0, kModelview);
                           mode->handler(back);
                       }
                   }
                   if (std::getenv("CLICKHLE_DUMP_MODE") != nullptr) {
                       CLICKHLE_INFO(kCategory)
                           << "#175 else branch, pushing matrix_store[dest=0x"
                           << std::hex << dest << "] as modelview, diag=("
                           << matrix_store[dest].m[0] << ","
                           << matrix_store[dest].m[5] << ","
                           << matrix_store[dest].m[10] << ") trans=("
                           << matrix_store[dest].m[12] << ","
                           << matrix_store[dest].m[13] << ","
                           << matrix_store[dest].m[14] << ")" << std::dec;
                       // LLE's ask (2026-09-12): does the shape-detector's
                       // guest-memory read (mem_a/mem_b) ever disagree with
                       // the host-side cache (matrix_store[a]/[b]) actually
                       // driving this push? If so, that's a second real bug
                       // (cache/guest-memory divergence, same architectural
                       // gap the Tetris ring-buffer thread was about).
                       const Matrix4& cached_a = matrix_store[a];
                       const Matrix4& cached_b = matrix_store[b];
                       const bool a_diverges =
                           std::fabs(mem_a.m[0] - cached_a.m[0]) > 0.0001f ||
                           std::fabs(mem_a.m[5] - cached_a.m[5]) > 0.0001f ||
                           std::fabs(mem_a.m[12] - cached_a.m[12]) > 0.0001f ||
                           std::fabs(mem_a.m[13] - cached_a.m[13]) > 0.0001f;
                       const bool b_diverges =
                           std::fabs(mem_b.m[0] - cached_b.m[0]) > 0.0001f ||
                           std::fabs(mem_b.m[5] - cached_b.m[5]) > 0.0001f ||
                           std::fabs(mem_b.m[12] - cached_b.m[12]) > 0.0001f ||
                           std::fabs(mem_b.m[13] - cached_b.m[13]) > 0.0001f;
                       if (a_diverges || b_diverges) {
                           CLICKHLE_INFO(kCategory)
                               << "  DIVERGENCE a_diverges=" << a_diverges
                               << " b_diverges=" << b_diverges
                               << " mem_a.diag=(" << mem_a.m[0] << ","
                               << mem_a.m[5] << ") cached_a.diag=("
                               << cached_a.m[0] << "," << cached_a.m[5]
                               << ") mem_b.diag=(" << mem_b.m[0] << ","
                               << mem_b.m[5] << ") cached_b.diag=("
                               << cached_b.m[0] << "," << cached_b.m[5] << ")";
                       }
                   }
                   // FIX (2026-09-12): push guest-memory-backed operands,
                   // not the host cache. Corpus-wide live check
                   // (CLICKHLE_DUMP_MODE across all 20 titles, 300 frames
                   // each) confirms this branch is reached by Mini Golf
                   // ONLY -- zero hits anywhere else -- so this is scoped by
                   // the code path itself, not a discriminator that could
                   // misfire elsewhere. Mini Golf's specific case: guest
                   // memory at both operand addresses is genuinely identity
                   // (confirmed by a full-run --watch showing zero runtime
                   // writes -- it's the compile-time default, unchanged for
                   // the entire run), but matrix_store[a] had diverged to a
                   // near-ortho scale from an earlier, unrelated #175 compose
                   // that happened to write the same cache slot -- a
                   // self-inflicted cache bug, not a guest bypass like
                   // Tetris's. `matrix_store[dest]` (used by #125's own,
                   // already-correctly-gated shader-uniform read) is left
                   // untouched -- only what gets pushed to MODELVIEW here
                   // changes.
                   load_modelview(f, multiply(mem_a, mem_b));
               }
               f.ret(0);
           });
    }

    // #125 and #149 hand a matrix to the pipeline: (?, ?, ?, source), the
    // object in the last argument. Tetris calls #125(0, 1, 0, 0x18025508), and
    // 0x18025508 is exactly the object #175 composed into, so this is "use that
    // matrix for the draw that follows". #125 is the busiest ordinal in the
    // title -- 55,963 calls in 200 frames.
    //
    // AND WE DELIBERATELY DO NOT FORWARD IT. This is the least satisfying thing
    // in this file and it is recorded honestly rather than dressed up.
    //
    // Every attempt to honour it renders worse than ignoring it:
    //
    //   forwarded as glLoadMatrixf, reading the object as raw floats
    //       Mahjong 1870 -> 2 colours, Bejeweled 2779 -> 1562
    //   forwarded, with GL_PROJECTION identity'd when the composed matrix
    //   already contains a projection
    //       Bejeweled 2779 -> 952, PAC-MAN 2479 -> 1
    //   the same, with matrices read from and written back to guest memory
    //   instead of a host-side store
    //       Mahjong 2, PAC-MAN 1, Zuma 216 -> 2
    //
    // Ignored, the per-op modelview loads from #169/#171/#173/#175 carry the
    // transform and six titles render correctly.
    //
    // So the model is wrong somewhere upstream -- most likely in how the screen
    // projection is installed, since every failure mode above is a projection
    // being applied twice or not at all. Honouring #125 correctly is blocked on
    // understanding that, not on #125 itself. Filed with the LLE track.
    //
    // The registration exists so the ordinal counts as implemented and does not
    // stop a run; it is a no-op with a reason, not an oversight.
    //
    // TRIED for #149 (2026-09-19): oracle named it glUniformMatrix4xvAPPLE
    // from the firmware assert string -- the 'x' suffix matches
    // glOrthox/glLoadMatrixx/glColor4x's fixed-point convention, so the
    // hypothesis was "#149 is #125's untried GL_FIXED twin; every past
    // regression forwarded raw floats and never converted." Implemented:
    // read the 16 words at the source pointer as GL_FIXED (/65536.0) and
    // load_modelview the result. Empirically wrong -- SAT Prep Mathematics's
    // splash, correctly rendering off the identity default (see
    // ensure_screen_projection above), went solid black with this active.
    // Reverted. #149 joins #125 in "every attempt renders worse than
    // ignoring it," now for a second, different reason: the conversion
    // itself may be wrong (word order, which argument is really the
    // pointer), or #149 may not mean "install this as the modelview" at
    // all for this title. Left as a no-op; not a dead end to re-attempt
    // blindly, but the fixed-point theory alone was not sufficient.
    // TRIED for #125 (2026-09-19): matrix_store[source] lookup instead of
    // re-reading the source argument as raw guest-memory floats (the
    // mechanism every one of the three attempts in the table above actually
    // used). Different mechanism, same result: Tetris -- 55,963 #125 calls,
    // the heaviest user in the corpus -- went from its correct rendered
    // content to 76,799/76,800 solid black at frame 60. Caught by a manual
    // screenshot check, NOT by compat_table.py --check, which stayed green;
    // the automated check compares draw/call counts, not rendered pixels,
    // and a regression that changes what a draw looks like without changing
    // how many draws happen is invisible to it. Reverted immediately.
    // #125 now joins the list with FOUR failed approaches, not three, and
    // this session's actual conclusion is sharper than "wrong mechanism":
    // #125 for Texas Hold'em DID show real, new, correctly-positioned
    // geometry (a white, untextured 3D panel, previously fully off-screen)
    // -- so dest=0x180dc6c0's composition is very likely the right value for
    // THAT title -- but forwarding #125 globally, for every title, is not
    // safe, because #175's per-title role (whether a given dest is meant to
    // install directly) is not something #125's own call shape can tell
    // apart. Whatever the real distinguishing signal is (per-title? per the
    // g167_calls==0 condition already used elsewhere? something else),
    // #125 cannot be turned on unconditionally until that signal is known.
    // TRIED #149 AGAIN, scoped this time (2026-09-12): every past attempt
    // (see the long comment above) forwarded #149 for every title and broke
    // one that already had a working transform path. Sims Bowling and Sims
    // Pool are the opposite shape -- #149 (paired 1:1 with #159) is their
    // ONLY transform call, ever -- so scoped an install-as-modelview attempt
    // on g_other_transform_calls == 0 (same discriminator shape as
    // g167_calls for #125), a true no-op for every other title. Verified
    // safe: `ctest` 8/8, `compat_table.py --check` 20/20 0 regressed, and a
    // full 20-title screenshot sweep including every title from the failed-
    // attempts table above (Bejeweled 2779, Tetris 9874, Mahjong 1877 --
    // all matched their known-good colour counts exactly, confirming the
    // scoping is a real no-op for them, not a coincidence).
    //
    // Did NOT fix Sims Bowling/Pool, and traced far enough to see why this
    // specific approach can't: dumped #149's actual matrix content
    // (CLICKHLE_DUMP_149=1) -- diag(1.6, -2.13, 1, 1) trans(-1, 1, 0). That
    // translate is not an arbitrary per-object offset -- it is EXACTLY the
    // firmware's own standard ortho translate (see the screen-projection
    // matrix built in ensure_screen_projection above, same -1/1/0). Cross-
    // checked the actual draw state (CLICKHLE_DUMP_ELEMENTS=1, added to
    // `gles.cpp`'s `glDrawElements`): PROJECTION is ALREADY a correct,
    // separately-installed ortho for this title (`0.00625, -0.00833, -1, 1`
    // trans `-1, 1, 0`) by the time #149's matrix would land in MODELVIEW
    // too -- installing it as modelview ON TOP of an already-correct
    // projection scales every vertex by an extra 1.6x/-2.13x before the
    // projection's own pixel-to-NDC conversion runs, pushing geometry
    // outside the [-1,1] clip range instead of positioning it. Exactly the
    // failure shape the existing comment already predicted ("#149 may not
    // mean 'install this as the modelview' at all for this title") --
    // confirmed with real numbers this time. Reverted the install; kept
    // g_other_transform_calls (harmless, currently unused) and both
    // diagnostics (CLICKHLE_DUMP_149 here, CLICKHLE_DUMP_ELEMENTS in
    // gles.cpp) since whoever picks this up next will want them. Real fix
    // needs to know what #149's matrix actually IS if not a modelview --
    // possibly the projection itself (in which case something else is
    // wrongly ALSO installing an ortho into PROJECTION for this title), or
    // a genuinely different uniform slot unrelated to either matrix stack.
    // SOLVED (2026-09-12): #149's matrix IS the PROJECTION, and ignoring it
    // was the whole bug for this title family. Every earlier attempt tried it
    // as a MODELVIEW (see the long history above) and correctly concluded
    // that stacking it on top of an already-installed ortho pushes geometry
    // out of the clip range. The missing step was measuring the geometry it
    // is supposed to project.
    //
    // The measurement that settled it, on Royal Solitaire (a black screen
    // with 9197 draws, every texture confirmed to contain real art, and
    // every GL state check -- blend, colour mask, logic op, depth, filters,
    // texture completeness, viewport, clear colour -- passing):
    //
    //   vertex extents, per draw:  xy=[0,1.25] x [0,0.9375]
    //   #149's matrix:             diag(1.59999, -2.13333) trans(-1, 1, 0)
    //
    // 2/1.25 == 1.6 and 2/0.9375 == 2.1333 exactly. #149's matrix is the
    // ortho that maps THIS title's coordinate space onto NDC. Meanwhile
    // ensure_screen_projection had installed the firmware's 320x240 ortho
    // (diag 0.00625, -0.00833), so geometry spanning 1.25 units was scaled
    // by 2/320 and collapsed into a sub-pixel sliver at the screen edge --
    // which is why a run with thousands of correct draws read 0/320
    // non-black pixels on a mid-screen scanline.
    //
    // Titles alternate between two projections here (Royal Solitaire sends
    // diag(1.6, -2.133) for its 1.25-space geometry and diag(0.00624,
    // 0.00833) for 320-space geometry, from two different source addresses),
    // so this has to follow whatever the guest last uploaded rather than be
    // installed once.
    //
    // Scoping is inherent rather than a discriminator: a corpus-wide sweep
    // (CLICKHLE_DUMP_149, all 20 titles, 300 frames each) shows #149 is
    // called ONLY by LOST (6574), SAT Prep Math/Reading/Writing (2467/2427/
    // 2467), Ms. PAC-MAN (1195), Royal Solitaire (704), Sims Pool (596),
    // Sims Bowling (521), Sudoku (300) and musika (1). Every title that
    // currently renders correctly -- Zuma, Mini Golf, Cubis 2, Texas
    // Hold'em, PAC-MAN, Tetris, Mahjong, Bejeweled, Vortex, iQuiz -- calls
    // it exactly ZERO times, so this cannot reach them at all. (That also
    // corrects an earlier guess in STATUS.md that iQuiz's blank screen was a
    // #149 victim: it never calls #149, so its cause is something else.)
    // Argument order independently confirmed from the firmware by the LLE
    // track (2026-09-12, #149's entry at 0x10268770): this is a genuine
    // glUniformMatrix4xv shape -- r0 = location, r1 = count, r2 = transpose,
    // r3 = value pointer. Matches what was already being read here.
    //
    // The firmware splits on `location` -- `cmp r5,#4 / subhs r5,r5,#4` at
    // 0x102687d0, biasing locations >= 4 into a different target than 0..3
    // (oracle measured two distinct write-target configurations either side
    // of that boundary). Applying every call to GL_PROJECTION regardless of
    // location is therefore only safe if no title uses the high range.
    //
    // MEASURED, and it is safe: across all six titles that call #149 --
    // Royal Solitaire (704 calls), Sims Pool (596), Sims Bowling (521),
    // LOST (6574), SAT Prep Mathematics (2467), Ms. PAC-MAN (1195) -- every
    // single call is `location=0 count=1 transpose=0`, with no exceptions.
    // So the >= 4 branch is never exercised by any real title, `count` is
    // never more than one matrix (the firmware loops over it), and no
    // transpose is ever requested -- which is exactly what this handler
    // assumes. If a title ever shows up using location >= 4, this needs to
    // route by location instead, and the diagnostic below will say so.
    // Named from the firmware's OWN assert strings by the LLE track
    // (asserted_names.py, cross-checked 46/46 against already-known names):
    // this is **glUniformMatrix4xvAPPLE**, not `glLoadMatrixx`.
    //
    // The old label was not merely inaccurate, it was actively misleading and
    // probably cost this project real time: "glLoadMatrixx" says
    // *fixed-function matrix load*, which invites exactly the assumption every
    // previous attempt made -- that the matrix belongs on the modelview stack.
    // "glUniformMatrix4xv" says *a uniform upload whose meaning is per
    // program*, which is what it is, and which is why the right answer turned
    // out to be the projection. Keeping the truthful name so the next reader
    // starts from the right mental model.
    fn(149, "glUniformMatrix4xvAPPLE", 4, [load_matrix](HleCallFrame& f) {
        const u32 location = f.args.next_u32();
        const u32 count = f.args.next_u32();
        const u32 transpose = f.args.next_u32();
        const GuestAddr source = f.args.next_ptr();
        Matrix4 m;
        bool complete = true;
        std::vector<u32> raw_words;
        raw_words.reserve(19);
        // The header words go into the record too: the packet's identity on
        // the wire includes which uniform slot it targets, and the research
        // session is looking for tags/constants to match against
        // rserver.bin's dispatcher, not just the numeric payload.
        raw_words.push_back(location);
        raw_words.push_back(count);
        raw_words.push_back(transpose);
        for (u32 i = 0; i < 16; ++i) {
            const auto raw = f.memory.read<u32>(source + 4 * i);
            if (!raw.has_value()) {
                complete = false;
                break;
            }
            raw_words.push_back(*raw);
            m.m[i] = static_cast<float>(static_cast<i32>(*raw)) / 65536.0f;
        }
        dump_uniform_packet(149, raw_words);
        if (std::getenv("CLICKHLE_DUMP_149") != nullptr) {
            CLICKHLE_INFO(kCategory)
                << "#149 location=" << location << " count=" << count
                << " transpose=" << transpose << " source=0x" << std::hex
                << source << std::dec
                << " complete=" << complete << " diag=" << m.m[0] << ","
                << m.m[5] << "," << m.m[10] << "," << m.m[15]
                << " trans=" << m.m[12] << "," << m.m[13] << "," << m.m[14];
        }
        // A partially-unmapped source is not a matrix; installing whatever
        // was read so far would be worse than leaving the projection alone.
        if (complete) {
            load_matrix(f, kProjection, m);
        }
        f.ret(0);
    });

    // TRYING #125 AGAIN, properly scoped this time (2026-09-19): the
    // unconditional matrix_store[source] version above broke Tetris (see the
    // comment there) despite using the right mechanism, because nothing
    // about #125's call shape says whether THIS title's composed dest is
    // meant to install directly. Gating on g167_calls == 0 -- the exact,
    // corpus-verified-unique condition (LLE's sweep) that already safely
    // selects Texas Hold'em alone for the projection fix above. Tetris,
    // Bejeweled, Mahjong, PAC-MAN, Zuma, Vortex, iQuiz, Mini Golf, Cubis 2
    // all call #167 somewhere, so g167_calls is never 0 for them by the time
    // their own #125 calls run -- this should be a true no-op for every
    // title except the one it's aimed at. Verify with compat_table.py AND a
    // manual Tetris screenshot before trusting this -- the automated check
    // alone already missed one real regression on this exact ordinal today.
    if (registry.find("gl", "glLoadMatrixf") != nullptr) {
        fn(125, "gl_uniform_matrix_4fv", 4, [load_modelview](HleCallFrame& f) {
            (void)f.args.next_ptr();  // object
            (void)f.args.next_u32();  // ?
            (void)f.args.next_u32();  // ?
            const GuestAddr source = f.args.next_ptr();
            if (std::getenv("CLICKHLE_DUMP_MODE") != nullptr) {
                CLICKHLE_INFO(kCategory)
                    << "#125 source=0x" << std::hex << source << std::dec
                    << " found=" << (matrix_store.count(source) != 0)
                    << " bound=" << g_bound_texture
                    << " g167_calls=" << g167_calls
                    << " gated=" << (g167_calls != 0);
            }
            if (g167_calls == 0) {
                const auto it = matrix_store.find(source);
                if (it != matrix_store.end()) {
                    load_modelview(f, it->second);
                }
            }
            f.ret(0);
        });
    }

    // #148 gl_uniform_4xv(object, ?, pointer) -- the constant colour every
    // quad is tinted by.
    //
    // The "v" matters: it is a *pointer* to four 16.16 values, and glColor4x
    // takes four scalars. Handing it the pointer instead tinted every fragment
    // with an address -- Pac-Man came out solid white and Bejeweled solid
    // magenta, a whole screen of one colour with 3,249 draw calls behind it.
    const HleFunction* gl_color = registry.find("gl", "glColor4x");
    if (gl_color != nullptr) {
        // The reference map names this gl_uniform_4xv, and the slot argument
        // is real: Tetris drives slot 1 with both (0,0,0,1) and
        // (0.8,0.8,0.9,1). Only slot 1 has ever been observed, so it is
        // treated as the colour -- but the slot is read rather than skipped, so
        // a title using a second slot shows up as a warning instead of
        // silently repainting everything.
        // Firmware's own name (LLE's assert-string extraction):
        // **glUniform4xvAPPLE**. Same caveat as #149's rename above -- it is a
        // per-program uniform upload, and treating it as unconditionally "the
        // constant colour" is an assumption, not the name's meaning. That
        // assumption is CORRECT for Cubis 2 / Tetris / Zuma / Bejeweled
        // (proven: forcing it white turns Cubis 2's cubes grey and deletes its
        // score text) and WRONG for Sims Bowling/Pool, whose near-black value
        // crushes the whole scene. Unresolved -- see STATUS.md.
        fn(148, "glUniform4xvAPPLE", 3, [gl_color, call_with](HleCallFrame& f) {
            (void)f.args.next_ptr();   // the object
            const u32 slot = f.args.next_u32();
            const GuestAddr values = f.args.next_ptr();
            u32 rgba[4] = {0, 0, 0, 0};
            for (u32 i = 0; i < 4; ++i) {
                rgba[i] = f.memory.read<u32>(values + 4 * i).value_or(0);
            }
            dump_uniform_packet(148, {rgba[0], rgba[1], rgba[2], rgba[3]});
            if (std::getenv("CLICKHLE_DUMP_148") != nullptr) {
                CLICKHLE_INFO(kCategory)
                    << "#148 slot=" << slot << " raw=0x" << std::hex << rgba[0]
                    << ",0x" << rgba[1] << ",0x" << rgba[2] << ",0x" << rgba[3]
                    << std::dec << "  as16.16=" << (rgba[0] / 65536.0) << ","
                    << (rgba[1] / 65536.0) << "," << (rgba[2] / 65536.0) << ","
                    << (rgba[3] / 65536.0);
            }
            if (slot != 1) {
                static std::set<u32> warned;
                if (warned.insert(slot).second) {
                    CLICKHLE_WARN(kCategory)
                        << "#148 slot " << slot
                        << " is not the colour slot; ignored";
                }
                f.ret(0);
                return;
            }
            // EXPERIMENT (env-gated, not a fix): force the constant colour to
            // opaque white to isolate whether #148's tint is what darkens the
            // Sims titles. If the scene brightens to match the reference
            // screenshot, the tint is being applied where it should not be.
            if (std::getenv("CLICKHLE_EXP_148_WHITE") != nullptr) {
                call_with(f, gl_color, {0x10000, 0x10000, 0x10000, 0x10000});
                f.ret(0);
                return;
            }
            call_with(f, gl_color, {rgba[0], rgba[1], rgba[2], rgba[3]});
        });
    }

    // #147 gl_uniform_4x_scalar(object, ?, x, y, z, w) -- the constant colour
    // again, but given as four scalars rather than through a pointer. The "4x"
    // is four 16.16 values; the "v" that #148 has is what makes that one a
    // vector.
    const HleFunction* gl_color_scalar = registry.find("gl", "glColor4x");
    if (gl_color_scalar != nullptr) {
        fn(147, "glColor4x", 5,
           [gl_color_scalar, call_with](HleCallFrame& f) {
               (void)f.args.next_ptr();  // the object
               const u32 r = f.args.next_u32();
               const u32 g = f.args.next_u32();
               const u32 b = f.args.next_u32();
               const u32 a = f.args.next_u32();
               call_with(f, gl_color_scalar, {r, g, b, a});
           });
    }

    // #99 gl_tex_image_2d(9) and #105 gl_tex_sub_image_2d(9).
    //
    // Nine arguments: five in registers is already more than call_with can
    // place, so these are marshalled onto the guest stack the target reads.
    // GL's own order for glTexImage2D is
    // (target, level, internalformat, width, height, border, format, type,
    //  pixels) -- nine, which is exactly the count the import map gives, so the
    // ordinal passes them straight through.
    for (const auto& [ordinal, name] :
         {std::pair<u32, const char*>{99, "glTexImage2D"},
          std::pair<u32, const char*>{105, "glTexSubImage2D"}}) {
        const HleFunction* target = registry.find("gl", name);
        if (target == nullptr) {
            continue;
        }
        fn(ordinal, name, 9, [target, map_target, label = std::string(name)](HleCallFrame& f) {
            u32 words[9];
            for (u32& word : words) {
                word = f.args.next_u32();
            }
            words[0] = map_target(words[0]);
            if (g_bound_texture != 0) {
                g_texture_size[g_bound_texture] = {words[3], words[4]};
            }
            // What a title actually uploads: format, size and where the pixels
            // are. The on-disk asset formats are known (.pix is BMP, .lcd5 is
            // RGB565), but a game decodes them itself, so what reaches GL is
            // the only thing that decides how a frame looks.
            // Implausible dimensions get dumped regardless of the sample
            // limit. musika uploads 12 textures at 23644x1 and 5 at 77824x1,
            // both past GL's maximum dimension, so GL rejects them outright
            // with GL_INVALID_VALUE and that content never renders. Whether
            // the guest really asks for those, or we mis-derive them the way
            // the Vortex header bug did, is answered by seeing the raw
            // arguments -- which the 6-sample cap was hiding, because these
            // uploads happen well after the first six.
            static u32 dumped = 0;
            const bool implausible = words[3] > 4096 || words[4] > 4096;
            if (dumped < 6 || implausible) {
                ++dumped;
                CLICKHLE_INFO(kCategory)
                    << label << " level=" << words[1] << " internalformat=0x"
                    << std::hex << words[2] << std::dec << " " << words[3]
                    << "x" << words[4] << " border=" << words[5]
                    << " format=0x" << std::hex << words[6] << " type=0x"
                    << words[7] << " pixels=0x" << words[8] << std::dec
                    << (implausible ? "   <- EXCEEDS GL MAX DIMENSION" : "");
            }
            // Four in registers, the rest back onto the stack in the order the
            // target will read them.
            for (u32 i = 0; i < 4; ++i) {
                f.cpu.set_reg(i, words[i]);
            }
            const GuestAddr sp = f.cpu.reg(13);
            for (u32 i = 4; i < 9; ++i) {
                (void)f.memory.write<u32>(sp + (i - 4) * 4, words[i]);
            }
            f.args.reset();
            target->handler(f);
            // gles.cpp's glTexImage2D/glTexSubImage2D are void -- ret_void
            // leaves r0 as whatever the arg marshalling above last wrote,
            // which is not a real answer. LLE's return-convention sweep
            // (work/return_conventions.py, 11/11 spot-checks elsewhere)
            // found both ordinals end in `mov r0,#2`, a literal it could
            // read directly rather than infer -- static-only, not yet
            // independently confirmed by oracle execution, but not a guess
            // either. #21 glCopyTexImage2D below was NOT in that finding
            // and is deliberately left alone.
            f.ret(2);
        });
    }

    // #19 gl_compressed_tex_image_2d(8) and #21 gl_copy_tex_image_2d(8).
    // Eight arguments, so four in registers and four back onto the guest stack
    // in the order the target reads them, as #99 does.
    for (const auto& [ordinal, name] :
         {std::pair<u32, const char*>{19, "glCompressedTexImage2D"},
          std::pair<u32, const char*>{21, "glCopyTexImage2D"}}) {
        const HleFunction* target = registry.find("gl", name);
        if (target == nullptr) {
            continue;
        }
        fn(ordinal, name, 8, [target, map_target, ordinal, label = std::string(name)](HleCallFrame& f) {
            u32 words[8];
            for (u32& word : words) {
                word = f.args.next_u32();
            }
            words[0] = map_target(words[0]);
            if (g_bound_texture != 0) {
                g_texture_size[g_bound_texture] = {words[3], words[4]};
            }
            // What a title actually uploads: format, size and where the pixels
            // are. The on-disk asset formats are known (.pix is BMP, .lcd5 is
            // RGB565), but a game decodes them itself, so what reaches GL is
            // the only thing that decides how a frame looks.
            static u32 dumped = 0;
            if (dumped < 6) {
                ++dumped;
                CLICKHLE_INFO(kCategory)
                    << label << " level=" << words[1] << " internalformat=0x"
                    << std::hex << words[2] << std::dec << " " << words[3]
                    << "x" << words[4] << " border=" << words[5]
                    << " format=0x" << std::hex << words[6] << " type=0x"
                    << words[7] << std::dec;
            }
            for (u32 i = 0; i < 4; ++i) {
                f.cpu.set_reg(i, words[i]);
            }
            const GuestAddr sp = f.cpu.reg(13);
            for (u32 i = 4; i < 8; ++i) {
                (void)f.memory.write<u32>(sp + (i - 4) * 4, words[i]);
            }
            f.args.reset();
            target->handler(f);
            // Only #19 was in LLE's "ends in mov r0,#2" finding -- #21
            // glCopyTexImage2D was not, so it keeps the old undefined-r0
            // behaviour rather than guessing it shares the same constant.
            if (ordinal == 19) {
                f.ret(2);
            }
        });
    }

    // #38 gl_draw_elements(mode, count, type, indices) -- GL's own order, so
    // it forwards. Like #37 it has to install the screen projection first.
    const HleFunction* gl_draw_elements = registry.find("gl", "glDrawElements");
    if (gl_draw_elements != nullptr) {
        fn(38, "glDrawElements", 4,
           [gl_draw_elements, ensure_screen_projection](HleCallFrame& f) {
               const u32 saved[4] = {f.cpu.reg(0), f.cpu.reg(1), f.cpu.reg(2),
                                     f.cpu.reg(3)};
               ensure_screen_projection(f);
               for (u32 i = 0; i < 4; ++i) {
                   f.cpu.set_reg(i, saved[i]);
               }
               f.args.reset();
               gl_draw_elements->handler(f);
           });
    }

    // #101 and #53 answer zero, per the import map. Among the ordinals that
    // block titles purely by being absent.

    // #101(a, b, c) -- THREE arguments, not zero. The firmware special-cases
    // `b == 0x8066` (a GL enum) after converting `c` through a float routine,
    // which reads as a parameter setter rather than a no-op.
    //
    // Declared with the right arity so the arguments are not read off the end
    // of the stack, and logged rather than acted on: what it sets is not
    // established, and inventing a target would be the #152 mistake again.
    // The values identify it exactly. Mahjong calls
    // #101(3553, 0x8066, 0x3f800000):
    //
    //     3553       = 0x0DE1 = GL_TEXTURE_2D
    //     0x8066     =          GL_TEXTURE_PRIORITY
    //     0x3f800000 =          1.0f
    //
    // So it is glTexParameterf(target, pname, value), and the float routine the
    // firmware runs the third argument through is a fixed-to-float conversion.
    // Note the target arrives as plain GL_TEXTURE_2D here, not the 0x84F5 the
    // texture ordinals use.
    if (const HleFunction* gl_tex_parameter =
            registry.find("gl", "glTexParameterf")) {
        fn(101, "glTexParameterf", 3,
           [gl_tex_parameter, call_with, map_target](HleCallFrame& f) {
               // map_target, for consistency with the other texture
               // ordinals. NOT a fix for anything, and the claim that used to
               // sit here was wrong twice over.
               //
               // I wrote that #101 was dropping every texture parameter every
               // title sets, because it was not mapping 0x84F5. Measured, both
               // halves are false:
               //
               //  - No title in the corpus EVER passes 0x84F5. All four titles
               //    checked (Zuma, Mahjong, Tetris, Bejeweled) pass 0x0DE1
               //    directly, which was never being dropped.
               //  - Only MAHJONG calls #101 at all, 33 times. Zuma, Tetris and
               //    Bejeweled never call it. I had attributed Mahjong's count
               //    to Zuma.
               //
               // So the bottom-bar improvement came entirely from the
               // clamp-to-edge below, not from this line.
               //
               // Keeping the mapping anyway, because LLE established that
               // 0x84F5 is GL_TEXTURE_RECTANGLE -- a genuinely different
               // target that the firmware's validator accepts alongside
               // 0x0DE1 and 0x8513, NOT a spelling of 2D. If a title ever does
               // pass it, mapping it to 2D is a SILENT RETARGET rather than a
               // translation, and rectangle's unnormalised coordinates are
               // exactly the difference that would show up as stretched
               // strips. Left as-is only because nothing exercises it.
               const u32 target = map_target(f.args.next_u32());
               const u32 pname = f.args.next_u32();
               const u32 value = f.args.next_u32();
               call_with(f, gl_tex_parameter, {target, pname, value});
           });
    } else {
        fn(101, "glTexParameterf", 3, [](HleCallFrame& f) { f.ret(0); });
    }
    fn(53, "zero", 0, [](HleCallFrame& f) { f.ret(0); });

    // #158 gl_query_0x3000 and #159 gl_one return constants. Named for what
    // they answer because that is all that was established about them -- the
    // reference project reports the same, having driven these titles
    // successfully without knowing more.
    // Answers the constant it is named for. Tetris polls it 58 times in 60
    // frames without moving on, and gating it on whether the render server has
    // started made no difference -- Tetris never calls #152 at all. So this is
    // not the thing being waited on, and the constant stands.
    // #158(key, n) -- a KEYED QUERY, not a constant.
    //
    //     switch (key) {
    //       case 0x3f000: return 1;
    //       case 0x3f001: return 2;
    //       case 0x3f002: return 3;
    //       case 0x3f003: <goes to hardware>
    //       default:      return <a global>;
    //     }
    //
    // We had it as arity 0 returning a constant 0x3000 -- the name was taken
    // from the value because nothing about it was understood. That is exactly
    // the #152 failure: a confident constant that answers every question
    // wrongly while letting the title run. Bejeweled polls this once per frame
    // after its audio stall and Mahjong calls it too.
    //
    // Sweeping the *return* value changed nothing precisely because the key was
    // being ignored: the answer has to depend on the question.
    fn(158, "gl_query", 2, [](HleCallFrame& f) {
        const u32 key = f.args.next_u32();
        (void)f.args.next_u32();
        {
            static std::set<u32> seen;
            if (seen.insert(key).second) {
                CLICKHLE_INFO(kCategory)
                    << "#158 key 0x" << std::hex << key << std::dec;
            }
        }
        u32 answer = 0;
        switch (key) {
            case 0x3f000: answer = 1; break;
            case 0x3f001: answer = 2; break;
            case 0x3f002: answer = 3; break;
            default:
                // 0x3f003 goes to hardware and the default returns a global we
                // do not model. Both are logged rather than guessed.
                static std::set<u32> warned;
                if (warned.insert(key).second) {
                    CLICKHLE_WARN(kCategory)
                        << "#158 key 0x" << std::hex << key
                        << " is not one of the three known answers" << std::dec;
                }
                break;
        }
        f.ret(answer);
    });

    // #159(selector) -- switches on its one argument, reading a different
    // global per case. Not a constant either, but which global each case reads
    // is not established, so the previous answer of 1 is kept for the cases we
    // cannot distinguish rather than replaced by a different invention.
    fn(159, "gl_query_selected", 1, [](HleCallFrame& f) {
        const u32 selector = f.args.next_u32();
        // LLE traced this (2026-09-13) and it is NOT a constant query: the
        // firmware bounds-checks r0 against 50 cases and jumps a real table.
        // Cases 1-41 pick a different PAIR of global objects each; 42-49 index
        // two parallel arrays at 8-byte stride (8 entries -- plausibly per
        // texture unit). The shared handler is a caching BIND: compare each
        // chosen object against a cached slot, and on a miss enqueue a real
        // hardware command through a sibling of the #148/#149 ring encoder,
        // then branch on bit 0x10000 of the selected object.
        //
        // So the name is wrong and the hardcoded 1 ignores all of it. What is
        // NOT yet established is whether any title depends on the returned
        // integer, as opposed to the side effects -- and the side effects are
        // hardware commands to a co-processor ClickHLE does not emulate at
        // all, so they may be irrelevant here even though they are real.
        //
        // CLICKHLE_EXP_159 sweeps the answer so that question is settled by
        // measurement rather than left open.
        static const char* exp = std::getenv("CLICKHLE_EXP_159");
        if (exp != nullptr) {
            if (std::strcmp(exp, "selector") == 0) {
                f.ret(selector);
                return;
            }
            f.ret(static_cast<u32>(std::strtoul(exp, nullptr, 0)));
            return;
        }
        f.ret(1);
    });

    // #137(index, size, type, normalized, stride, pointer) -- the vertex
    // attribute array setter, and the busiest ordinal in the API at 207 sites.
    //
    //     mov r6, #12 ; add r6, r6, #5120    ; r6 = 0x140C = GL_FIXED
    //     mov r2, r6 ; mov r3, #0 ; mov r1, #4 ; mov r0, #0   -> (0, 4, ...)
    //     stm sp, {r2, r3}                   ; stride and pointer
    //     mov r1, #2 ; mov r0, #1                             -> (1, 2, ...)
    //
    // Index 0 with size 4 is position; index 1 with size 2 is texture
    // coordinates. GL_FIXED confirms the 16.16 pipeline from the games
    // themselves. The inherited layer is ES 1.x fixed-function, so the index
    // dispatches onto the two named pointer setters rather than an
    // attribute-indexed one.
    const HleFunction* gl_vertex = registry.find("gl", "glVertexPointer");
    const HleFunction* gl_texcoord = registry.find("gl", "glTexCoordPointer");
    const HleFunction* gl_colour = registry.find("gl", "glColorPointer");
    if (gl_vertex != nullptr && gl_texcoord != nullptr) {
        fn(137, "vertex_attrib_pointer", 6,
           [gl_vertex, gl_texcoord, gl_colour, call_with](HleCallFrame& f) {
               const u32 index = f.args.next_u32();
               const u32 size = f.args.next_u32();
               const u32 type = f.args.next_u32();
               (void)f.args.next_u32();  // normalized; ES 1.x has no such flag
               const u32 stride = f.args.next_u32();
               const GuestAddr pointer = f.args.next_ptr();
               if (std::getenv("CLICKHLE_DUMP_VERTPTR") != nullptr &&
                   index == 0) {
                   CLICKHLE_INFO(kCategory)
                       << "#137 index=0 (position) pointer=0x" << std::hex
                       << pointer << std::dec << " size=" << size
                       << " bound=" << g_bound_texture;
               }

               // One-shot dump of the first position array: whether the
               // numbers are pixels, normalised, or 16.16 decides everything
               // downstream.
               static bool dumped[8] = {};
               if (index < 8 && !dumped[index]) {
                   dumped[index] = true;
                   std::string text;
                   for (u32 i = 0; i < 8; ++i) {
                       const u32 word =
                           f.memory.read<u32>(pointer + 4 * i).value_or(0);
                       char buffer[48];
                       std::snprintf(buffer, sizeof(buffer), "0x%08x(%.3f) ",
                                     word,
                                     static_cast<double>(
                                         static_cast<i32>(word)) / 65536.0);
                       text += buffer;
                   }
                   CLICKHLE_INFO(kCategory)
                       << "attrib" << index << " size=" << size
                       << " type=0x" << std::hex
                       << type << std::dec << " stride=" << stride << " : "
                       << text;
               }

               // Index 2 is the colour array -- see the client_array comment
               // above for the evidence. Anything beyond that is still unknown,
               // and guessing would put the wrong data in the wrong stream.
               const HleFunction* target = index == 0   ? gl_vertex
                                           : index == 1 ? gl_texcoord
                                           : index == 2 ? gl_colour
                                                        : nullptr;
               if (target == nullptr) {
                   CLICKHLE_WARN(kCategory)
                       << "#137 attribute index " << index
                       << " is unidentified; ignored";
                   f.ret(0);
                   return;
               }
               call_with(f, target, {size, type, stride, pointer});
           });
    }

    // The render server: the firmware's drawing subsystem, which is what
    // `rserver.bin` belongs to and why titles load that file before drawing.
    //
    //   #152 gl_start_render_server(3)
    //   #164 gl_set_render_server_image(3)   -- hands it the loaded rserver.bin
    //   #153 gl_stop_render_server(1)
    //   #158 gl_query_0x3000(0)              -- polled while waiting
    //
    // Accepted and reported started. We have no firmware render server and do
    // not need one -- the inherited GL layer is the renderer -- so what these
    // must do is stop the game waiting. Tetris polls #158 fifty-eight times in
    // sixty frames and gets no further, which is what waiting for a subsystem
    // that never starts looks like.
    fn(152, "start_render_server", 3, [](HleCallFrame& f) {
        g_render_server_running = true;
        CLICKHLE_DEBUG(kCategory) << "render server started";
        f.ret(1);
    });
    // Arguments are variable, so they carry state a real renderer would need:
    // Ms. Pac-Man and Lost both call #164(1, r5, r4) from a wrapper, and TWA
    // has a site passing (0, 0, 0). Logged rather than discarded, so the values
    // themselves become the next piece of evidence.
    // CORRECTED SIGNATURE (LLE, 2026-09-12, firmware 0x10261384): this is
    // `(enable, image_data_ptr, byte_size)` -- a SOURCE IMAGE UPLOAD, not a
    // target-size registration. The firmware gates on arg0 (null zeroes the
    // rest), then resizes an internal buffer to `byte_size`, resets its write
    // cursor, and memcpys the whole image into a persistent object. So
    // discarding these arguments loses real pixel data, not an inert hint.
    //
    // CORRECTION (2026-09-12): the "SOURCE IMAGE UPLOAD" reading above is
    // wrong, tested directly. Captured Sims Bowling's real #164 payload live
    // (CLICKHLE_DUMP_164_RAW, 105020 bytes) and inspected it offline: it is
    // NOT pixel data. The 512-byte lead-in is all zero (not a palette with
    // real entries), and the body that follows is dominated by small,
    // heavily-repeated byte values with an unmistakable command-stream
    // signature -- e.g. the 4-byte pattern `XX 10 00 00` recurs 113 times
    // with `XX` counting DOWN in clean runs (0xea,0xe9,0xe8,0xe7,0xe6,0xe5,
    // 0xe0, then later 0x3a,0x3e,0x42,0x46,0x4a,0x4e...). No rendered image
    // -- paletted, RGB565, or otherwise -- produces a monotonically
    // descending opcode-sized field repeated like that; real pixel rows do
    // not look like this. This is a GPU display list / command buffer for
    // VideoCore II, the same coprocessor whose instruction encoding nobody
    // has cracked (see research/11-the-videocore-runtime.md and the Vortex
    // icon-texture investigation in agent-comms/STATUS.md). So #164 is not a
    // routable "draw this bitmap" fix -- decoding it faithfully means
    // interpreting an undocumented, NDA-only GPU ISA, the same wall Vortex's
    // icons and #148's tint both hit. Left as a no-op; #152/#153/#164 remain
    // acknowledgement stubs so titles that merely poll for the render server
    // to be "ready" don't stall, which is the actual, achievable half of
    // this ordinal's job.
    fn(164, "set_render_server_image", 3, [](HleCallFrame& f) {
        const u32 enable = f.args.next_u32();
        const GuestAddr image = f.args.next_ptr();
        const u32 size = f.args.next_u32();
        CLICKHLE_DEBUG(kCategory)
            << "render server image enable=" << enable << " 0x" << std::hex
            << image << std::dec << ", " << size << " bytes";
        // Characterise the payload we are currently throwing away: its size
        // against plausible full-screen formats, and whether it holds real
        // varied pixels rather than zeroes.
        if (std::getenv("CLICKHLE_DUMP_164") != nullptr) {
            const char* guess = "?";
            if (size == 320u * 240u * 2u) guess = "320x240 16bpp";
            else if (size == 320u * 240u * 4u) guess = "320x240 32bpp";
            else if (size == 320u * 240u) guess = "320x240 8bpp/paletted";
            u64 nonzero = 0;
            if (enable != 0 && image != kNullGuest && size > 0 &&
                size <= 4u * 1024u * 1024u) {
                if (const auto* p = static_cast<const u8*>(
                        static_cast<const void*>(f.memory.host_ptr(image, size)))) {
                    for (u32 i = 0; i < size; ++i) {
                        if (p[i] != 0) ++nonzero;
                    }
                }
            }
            CLICKHLE_INFO(kCategory)
                << "#164 enable=" << enable << " ptr=0x" << std::hex << image
                << std::dec << " size=" << size << " (" << guess << ") nonzero="
                << nonzero << "/" << size;
        }
        // Dump the raw payload to a file for offline reinterpretation under
        // different format hypotheses -- same technique as gles.cpp's
        // CLICKHLE_DUMP_RAWSRC for the Vortex icon investigation.
        if (const char* dir = std::getenv("CLICKHLE_DUMP_164_RAW")) {
            if (enable != 0 && image != kNullGuest && size > 0 &&
                size <= 4u * 1024u * 1024u) {
                if (const auto* p = static_cast<const u8*>(
                        static_cast<const void*>(f.memory.host_ptr(image, size)))) {
                    std::ostringstream path;
                    path << dir << "/render_server_image_0x" << std::hex
                         << image << std::dec << "_" << size << ".bin";
                    std::ofstream out(path.str(), std::ios::binary);
                    out.write(reinterpret_cast<const char*>(p),
                              static_cast<std::streamsize>(size));
                }
            }
        }
        f.ret(1);
    });
    // Every one of the 8 call sites across TWA and Lost passes a literal 0, and
    // the return value is dead at all of them -- r0 is overwritten by the next
    // instruction or the next call's argument setup. So 0 is returned rather
    // than 1: nothing observes it, and changing it would be an unmotivated
    // behaviour change dressed up as a fix.
    //
    // The warning is the falsification test. If a title ever passes something
    // other than 0, there is more than one render server and this is wrong.
    fn(153, "stop_render_server", 1, [](HleCallFrame& f) {
        const u32 which = f.args.next_u32();
        if (which != 0) {
            CLICKHLE_WARN(kCategory)
                << "#153 called with " << which << ", not 0 -- more than one "
                << "render server, and this reading is wrong";
        }
        g_render_server_running = false;
        f.ret(0);
    });

    // Both #152 and #153 were misread once: #152 as a screen-size query writing
    // 320x240 through what were taken for out-pointers, #153 as a no-op
    // selector. The 320x240 those out-pointers "confirmed" was never evidence;
    // the real confirmation is the projection in #167.
    //
    // The corrected readings were added and the superseded ones were left in
    // place. Because HleRegistry::add keeps the *first* registration, the
    // withdrawn versions are the ones that ran -- for months, while the
    // comments above them said they had been withdrawn. The duplicate guard
    // reported it on every run and it went unread.
    //
    // Still not established: what the render server is handed and what it hands
    // back.

    // Count what is actually reachable by ordinal. registry.size() counts the
    // named GL entry points too, which made this line read 136/179 when four
    // ordinals were implemented -- the exact kind of inflated coverage number
    // this module is careful not to produce.
    u32 implemented = 0;
    for (u32 ordinal = 0; ordinal < kOrdinalCount; ++ordinal) {
        if (registry.find(kModule, ordinal) != nullptr) {
            ++implemented;
        }
    }
    CLICKHLE_DEBUG(kCategory)
        << kModule << ": " << implemented << "/" << kOrdinalCount
        << " ordinals implemented";
}

}  // namespace clickhle

// The "Metadata" library: 152 ordinals, the largest after OpenGLES.
//
// It is the iPod's own music library. Three of the ordinals are named --
// now-playing playlist, now-playing index, playlist track count -- and that is
// enough to say what the library is for: these games could quiz you on your
// music and play your own tracks, which is why something peripheral-looking has
// 152 entries.
//
// Everything here reports an **empty library**: no playlist, no track, nothing
// to count. That is what a freshly synced iPod with no music looks like, and it
// is a real answer rather than a stub -- a game asking how many tracks are in a
// playlist and being told none has been told something true about this device.
//
// The alternative would be inventing a music library, and a game that then
// asked for track seven's title would be lied to twice.

#include "clickhle/hle/modules.h"

#include <string>
#include <utility>

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

}  // namespace

void register_metadata(HleRegistry& registry) {
    const auto fn = [&registry](u32 ordinal, std::string name, u8 arity,
                                HleHandler handler) {
        registry.add(kModule, ordinal, std::move(name), arity,
                     std::move(handler));
    };

    // #0 is what iQuiz asks for first, and it is not among the three the
    // reference map names. Its caller keeps the result and passes it straight
    // back in as an argument to the next call:
    //
    //     bl   <Metadata#0>
    //     mov  r1, r0
    //     mov  r0, r4
    //
    // So it yields a handle or count that the rest of the library is indexed
    // by. Answering zero says "nothing here", which is consistent with the
    // empty library the rest of this module reports.
    fn(0, "library_root", 0, [](HleCallFrame& f) {
        CLICKHLE_DEBUG(kCategory) << "library_root -> 0 (empty library)";
        f.ret(0);
    });

    // #2 has the same shape as #0 -- its result is passed straight into the
    // next call as an argument -- so it is another handle or count into the
    // library, and zero is the answer an empty one gives.
    // Arity 0, not 1. The firmware decompiles as `int ()` and touches no
    // argument register (LLE track). This was the only arity conflict in a
    // scrape of every handler we register, which is worth as much as the fix.
    fn(2, "library_query", 0, [](HleCallFrame& f) {
        (void)f.args.next_u32();
        f.ret(0);
    });

    // #1/#3 -- musika's exit/save path reaches these after #119 unblocked its
    // main menu. Arity 1, per oracle's direct firmware-call measurement
    // (`oracle/out/slot_measurements.csv`: both `r0:scalar`) -- an earlier
    // guess of arity 2 here, by analogy to #29/#31/#45/#47, was wrong.
    // Answered 0, the same "empty library" answer as the rest of this
    // module. Corpus-checked: musika is the only title reaching either
    // (`compat_table.py --check`, 20/20, 0 regressed), and with these two
    // answered it completes a full menu -> exit -> save cycle with zero
    // unimplemented calls, where it previously halted here.
    //
    // NOT the "music required to play musika" gate -- tried answering #62
    // (now_playing_playlist) and #134 (playlist_track_count) as a non-empty
    // one-track library and it made no difference to that dialog. LLE traced
    // why: #62 is called from small wrapper functions that tail-jump straight
    // into #134/#119/#125/#127 after it, so the gate is downstream of what
    // those wrappers do with the return values, not a direct read of #62/#134
    // themselves. Still open -- see hle-to-lle.md/hle-to-oracle.md.
    fn(1, "library_query", 1, [](HleCallFrame& f) {
        (void)f.args.next_u32();
        f.ret(0);
    });
    fn(3, "library_query", 1, [](HleCallFrame& f) {
        (void)f.args.next_u32();
        f.ret(0);
    });

    // #29/#31/#45/#47 -- more library queries, all called with a small leading
    // constant and a pointer to somewhere in the image. Answered zero, for the
    // same reason as #0 and #2: this device has no music on it, and every
    // answer here has to agree with every other or a title will believe in a
    // library it cannot then enumerate.
    for (const u32 ordinal : {29u, 31u, 45u, 47u}) {
        fn(ordinal, "library_query", 2, [](HleCallFrame& f) {
            (void)f.args.next_u32();
            (void)f.args.next_u32();
            f.ret(0);
        });
    }

    fn(62, "now_playing_playlist", 0,
       [](HleCallFrame& f) { f.ret(kNoPlaylist); });
    // Arity was 0; three independent methods agree it is wrong. oracle
    // measured at least one argument consumed on a clean return; LLE's
    // static read (after fixing a metric that had been discounting
    // pass-through registers as "no evidence") makes it 4, consuming
    // r1/r2/r3. The claim of 0 was the only outlier among the three
    // (2026-09-19). What the four arguments MEAN is still unconfirmed --
    // this corrects the declared signature, not the answer -- so the
    // return stays 0, the same honest "empty library" answer the rest of
    // this module gives.
    fn(125, "now_playing_index", 4, [](HleCallFrame& f) {
        (void)f.args.next_u32();
        (void)f.args.next_u32();
        (void)f.args.next_u32();
        (void)f.args.next_u32();
        f.ret(0);
    });
    fn(134, "playlist_track_count", 0, [](HleCallFrame& f) { f.ret(0); });

    // #119 -- another library query, and the ONLY thing standing between
    // musika and running at all. Answered 0, with the reasoning stated
    // because the value is NOT confirmed:
    //
    // - Both other tracks tried and could not settle it. LLE: zero direct
    //   `bl` callers anywhere in musika's binary (encoding-level scan), so
    //   it is reached through a runtime-indexed function-pointer table; the
    //   trail bottoms out in the game's own internal PLT, several layers
    //   past anything statically reachable. oracle: called it against real
    //   firmware with BOTH of musika's exact captured argument sets and got
    //   the identical dead end each time (`Lost` at pc 0x0021a41c, 752
    //   instructions, byte-identical tails) -- the failure is
    //   argument-independent, a warm machine missing boot-time state, so
    //   execution cannot give ground truth either.
    //
    // - Measured call shape (`--probe`): two sites, 0x1802dc9c and
    //   0x1802dd84, args `(0xFFFFFFFF, 0x1, 0x1, 0x1, 0x20000050, 0x2, 0x3,
    //   <retaddr>)` and `(0xFFFFFFFF, 0x3, 0x2025bff4, 0x0, ...)`. arg0 is
    //   exactly kNoPlaylist -- the same "nothing selected" sentinel #0 and
    //   #62 already return -- which is at least consistent with this being
    //   another empty-library query.
    //
    // - Why answer at all rather than leave it unregistered: unregistered
    //   means musika HALTS on its first call, at frame 1, having rendered
    //   nothing. With 0 it runs and draws its real main menu (logo plus
    //   play / visualizer / volume / options / help / exit, 19201 colours,
    //   152113 draw calls over 7000 frames). Every other answer is a guess
    //   too, and 0 is the one consistent with the empty library the rest of
    //   this module reports.
    //
    // - Why this is safe to try: a corpus sweep shows **musika is the only
    //   title that calls #119 at all** (2 calls in 200 frames; every other
    //   title: zero). So this cannot reach anything that currently works.
    //
    // If a title ever depends on a real value here this will need revisiting
    // -- see the Settings#0 note in settings.cpp for what a confidently
    // wrong return value costs.
    fn(119, "library_query", 8, [](HleCallFrame& f) {
        for (u32 i = 0; i < 8; ++i) {
            (void)f.args.next_u32();
        }
        CLICKHLE_DEBUG(kCategory) << "#119 -> 0 (empty library)";
        f.ret(0);
    });

    CLICKHLE_DEBUG(kCategory) << kModule << ": empty library reported";
}

}  // namespace clickhle

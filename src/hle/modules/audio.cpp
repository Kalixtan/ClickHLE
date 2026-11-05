// The "Audio" library: 61 ordinals, of which the shape of about thirty is
// known.
//
// Two things live here. A *sound effect* is a small object with a great many
// field setters -- create it, point it at a buffer, set a dozen properties,
// play it. A *stream* is the music path, registered once and then played,
// repeated or shuffled.
//
// Nothing decodes yet. The assets are .wav and .m4a beside each game, and what
// the firmware accepts is not established, so every entry here is bookkeeping:
// objects are created and remembered, properties are stored, and play is
// recorded. A game gets consistent answers and does not stall; it does not get
// sound.
//
// That distinction is deliberate. Returning plausible handles is not a stub in
// the sense this project avoids -- the guest's observable contract is "I asked
// for a sound object and got one back that behaves consistently" -- but the
// audio itself is honestly absent, and the coverage table should be read that
// way.

#include "clickhle/hle/modules.h"

#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <utility>

#include "clickhle/common/log.h"
#include "clickhle/audio/audio_device.h"
#include "clickhle/core/emulator.h"
#include "clickhle/hle/call_frame.h"
#include "clickhle/hle/registry.h"

namespace clickhle {
namespace {

constexpr std::string_view kCategory = "audio";
constexpr const char* kModule = "Audio";

/// One sound-effect object. The fields are named by their offset in the guest's
/// own structure because that is all that was established about most of them --
/// the setters are distinguishable by ordinal, not by meaning.
struct Sound {
    u32 buffer = 0;
    u32 repeat = 0;
    u32 state = 0;
    /// obj[+0x40] -- the observer-list head that #17 walks.
    ///
    /// Written by #21 (oracle, measured 2026-09-19: #21 is the only slot that
    /// writes +0x40, which makes it the missing link in this chain and almost
    /// certainly audio_sfx_set_next). 0 on a fresh object, so #17 degenerates
    /// to a plain setter until something links a chain.
    u32 next = 0;
    /// Latched so one configured voice is queued once, not on every
    /// subsequent property write.
    bool started = false;
    std::map<u32, u32> fields;  ///< by the ordinal that set it
};

std::map<u32, Sound> g_sounds;
/// The firmware's handle is a **table index**, so 0 is a perfectly valid one and
/// the failure value is -1, never 0 (LLE, AUDIO.md: `0x10292820` returns the
/// index or -1). The comment here used to say zero was the failure value, which
/// is the opposite of the truth.
///
/// Starting at 1 anyway, and deliberately: nothing in the corpus depends on the
/// numbering, and a handle that is never 0 keeps our own `find`-miss paths
/// distinguishable from a real handle. Where it matters is the failure value,
/// which is now -1 below.
u32 g_next_sound = 1;

}  // namespace

namespace {

/// Everything needed to actually play one voice, as the guest configured it.
///
/// The mapping was measured, not guessed, and it checks out against the
/// shipped assets exactly: for Tetris's Drop.wav (91968 bytes on disk) the
/// guest sets length 91924, which is the file minus its 44-byte RIFF header,
/// and Move.wav gives 14412 - 44 = 14368. So #7 hands over the .wav's data
/// chunk with the header already stripped.
///
///   #7  buffer   pointer to raw PCM
///   #8  length   bytes
///   #10 rate     samples/sec      (22050 across the whole corpus)
///   #11 channels                  (2)
///   #12 bits     per sample        (16)
constexpr u32 kFieldLength = 8;
constexpr u32 kFieldRate = 10;
constexpr u32 kFieldChannels = 11;
constexpr u32 kFieldBits = 12;

/// Optional capture of everything handed to the device, as a .wav.
///
/// This is how "audio works" gets verified without listening to it: the dump
/// can be compared byte-for-byte against the game's own asset file. Sounding
/// plausible is not evidence; being identical to the source PCM is.
void append_wav_dump(const void* pcm, u32 bytes, u32 rate, u16 channels,
                     u16 bits) {
    const char* path = std::getenv("CLICKHLE_AUDIO_DUMP");
    if (path == nullptr || bytes == 0) {
        return;
    }
    static std::ofstream out(path, std::ios::binary | std::ios::trunc);
    static u32 total = 0;
    if (!out.is_open()) {
        return;
    }
    if (total == 0) {
        // Header written once with placeholder sizes, patched on each append
        // so the file is playable even if the run is cut short.
        const u32 zero = 0;
        const u16 fmt = 1;
        const u32 byte_rate = rate * channels * (bits / 8u);
        const u16 block_align = static_cast<u16>(channels * (bits / 8u));
        const u32 fmt_size = 16;
        out.write("RIFF", 4); out.write(reinterpret_cast<const char*>(&zero), 4);
        out.write("WAVEfmt ", 8);
        out.write(reinterpret_cast<const char*>(&fmt_size), 4);
        out.write(reinterpret_cast<const char*>(&fmt), 2);
        out.write(reinterpret_cast<const char*>(&channels), 2);
        out.write(reinterpret_cast<const char*>(&rate), 4);
        out.write(reinterpret_cast<const char*>(&byte_rate), 4);
        out.write(reinterpret_cast<const char*>(&block_align), 2);
        out.write(reinterpret_cast<const char*>(&bits), 2);
        out.write("data", 4);
        out.write(reinterpret_cast<const char*>(&zero), 4);
    }
    out.write(static_cast<const char*>(pcm), bytes);
    total += bytes;
    const u32 riff_size = 36 + total;
    out.seekp(4); out.write(reinterpret_cast<const char*>(&riff_size), 4);
    out.seekp(40); out.write(reinterpret_cast<const char*>(&total), 4);
    out.seekp(0, std::ios::end);
    out.flush();
}

}  // namespace

/// Start a voice once it has BOTH a buffer and a length -- whichever arrives
/// last wins.
///
/// The order is not the same across titles, which is why this cannot live in
/// one ordinal. Tetris configures fields, attaches the buffer, then sets the
/// state. PAC-MAN sets the state FIRST and attaches the buffer afterwards, so
/// triggering only on the state write plays nothing at all for it.
///
/// Guarded so a voice is started once: `started` latches, and is cleared when
/// the voice is stopped or re-pointed at a new buffer.
void try_start_voice(HleCallFrame& f, u32 handle) {
    // OPT-IN until the timing is right. The format is decoded and verified,
    // but WHEN to read the PCM is not: the guest registers a buffer before
    // filling it, and reuses the same addresses across sounds, so reading at
    // the trigger captures stale bytes. Verified by comparison -- none of
    // Tetris's eleven assets appears verbatim in what we queue, even though
    // every declared length matches its asset exactly.
    //
    // Playing the wrong bytes is worse than silence: it sounds like working
    // audio while being wrong, which is the hardest kind of bug to notice.
    // So default behaviour is unchanged and this is behind CLICKHLE_AUDIO=1
    // for continued work.
    static const bool enabled = [] {
        const char* v = std::getenv("CLICKHLE_AUDIO");
        return v != nullptr && *v == '1';
    }();
    if (!enabled) {
        return;
    }
    const auto it = g_sounds.find(handle);
    if (it == g_sounds.end()) {
        return;
    }
    Sound& snd = it->second;
    if (snd.started || snd.state != 1 || snd.buffer == 0) {
        return;
    }
    const auto get = [&snd](u32 k, u32 fallback) {
        const auto f2 = snd.fields.find(k);
        return f2 != snd.fields.end() ? f2->second : fallback;
    };
    const u32 bytes = get(kFieldLength, 0);
    const u32 rate = get(kFieldRate, 22050);
    const auto channels = static_cast<u16>(get(kFieldChannels, 2));
    const auto bits = static_cast<u16>(get(kFieldBits, 16));
    if (bytes == 0 || channels == 0 || bits == 0) {
        return;
    }
    const void* pcm = f.memory.host_ptr(snd.buffer, bytes);
    if (pcm == nullptr) {
        CLICKHLE_WARN(kCategory)
            << "sfx handle=" << handle << ": " << bytes << " bytes at 0x"
            << std::hex << snd.buffer << std::dec
            << " are not mapped; not played";
        return;
    }
    snd.started = true;
    if (AudioDevice* dev = f.emulator.open_audio(rate, channels, bits)) {
        (void)dev->queue(pcm, bytes);
    }
    append_wav_dump(pcm, bytes, rate, channels, bits);
    CLICKHLE_DEBUG(kCategory)
        << "sfx play handle=" << handle << " " << bytes << " bytes @" << rate
        << "/" << bits << "/" << channels;
}

void register_audio(HleRegistry& registry) {
    const auto fn = [&registry](u32 ordinal, std::string name, u8 arity,
                                HleHandler handler) {
        registry.add(kModule, ordinal, std::move(name), arity,
                     std::move(handler));
    };

    fn(0, "sfx_create", 2, [](HleCallFrame& f) {
        const u32 handle = g_next_sound++;
        g_sounds[handle] = Sound{};
        CLICKHLE_DEBUG(kCategory) << "sfx_create -> " << handle;
        f.ret(handle);
    });

    fn(1, "sfx_release", 1, [](HleCallFrame& f) {
        g_sounds.erase(f.args.next_u32());
        f.ret(0);
    });

    fn(2, "sfx_play", 1, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        // Marked playing so #39 can answer honestly. Inference, not firmware:
        // #39 tests state == 1 and #4 writes 1, so a voice that has been told
        // to play should read back as playing. Without this, a title that
        // starts a sound and then asks whether it is running is told no.
        const auto it = g_sounds.find(handle);
        if (it != g_sounds.end()) {
            it->second.state = 1;
        }
        // Everything the guest configured on this voice before starting it.
        // Implementing real playback needs a PCM pointer, a LENGTH and a
        // format, and only the pointer is currently identified -- the length
        // has to be one of the unnamed field setters (8-15, 18), so print
        // them all at the moment they matter and let the values say which.
        if (std::getenv("CLICKHLE_DUMP_SFXPLAY") != nullptr &&
            it != g_sounds.end()) {
            std::string fields;
            for (const auto& [ordinal, value] : it->second.fields) {
                char t[48];
                std::snprintf(t, sizeof(t), " #%u=0x%x(%u)", ordinal, value,
                              value);
                fields += t;
            }
            CLICKHLE_INFO(kCategory)
                << "sfx_play handle=" << handle << " buffer=0x" << std::hex
                << it->second.buffer << std::dec
                << " repeat=" << it->second.repeat << " fields:" << fields;
        }
        CLICKHLE_DEBUG(kCategory) << "sfx_play " << handle << " (silent)";
        f.ret(0);
    });

    // The voice-state family. #4 writes 1, #3 writes 2, #5 writes 3, and #39
    // reads back "is it 1?" -- all walking the same circular list of voices
    // behind a handle (`+0x3d` state, `+0x40` next).
    //
    // The VALUES are certain, from the firmware. The NAMES are inference: with
    // #5 long established as stop, 1/2/3 reads naturally as playing / paused /
    // stopped, which makes #4 resume and #3 pause. But #2 is already play, so
    // whether #4 is "resume" or "play from the start" is not settled -- the LLE
    // track flagged that it cannot be resolved from the firmware alone, and it
    // is not resolvable from this side either while nothing is audible.
    //
    // We keep one state per handle rather than a ring: no title dereferences a
    // framework return value (section 49), so the ring's layout is not visible
    // to a game and a scalar is indistinguishable from it.
    constexpr u32 kVoicePlaying = 1;
    constexpr u32 kVoicePaused = 2;
    constexpr u32 kVoiceStopped = 3;

    const auto set_state = [](u32 state) {
        return [state](HleCallFrame& f) {
            const u32 handle = f.args.next_u32();
            const auto it = g_sounds.find(handle);
            if (it != g_sounds.end()) {
                it->second.state = state;
            }
            f.ret(1);
        };
    };

    fn(4, "sfx_resume", 1, set_state(kVoicePlaying));
    fn(3, "sfx_pause", 1, set_state(kVoicePaused));
    fn(5, "sfx_stop", 1, set_state(kVoiceStopped));

    // Reads the state byte and answers "is this voice playing?".
    fn(39, "sfx_is_playing", 1, [kVoicePlaying](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        const auto it = g_sounds.find(handle);
        f.ret(it != g_sounds.end() && it->second.state == kVoicePlaying ? 1u
                                                                       : 0u);
    });

    // #37(handle) -- a property of a sub-object hung off `+0x34`, returning
    // that object's `+0x2c`, or 0 when there is no sub-object.
    //
    // Answered as "there is none". We do not model the sub-object and we do not
    // know which property it holds, so 0 is the firmware's own answer for the
    // case we are actually in rather than a fabricated value.
    fn(37, "sfx_get_attached_property", 1, [](HleCallFrame& f) {
        (void)f.args.next_u32();
        f.ret(0);
    });

    fn(7, "sfx_set_buffer", 2, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        const u32 buffer = f.args.next_u32();
        g_sounds[handle].buffer = buffer;
        g_sounds[handle].started = false;  // re-pointed: allow it to play again
        // Is the "buffer" a guest pointer to PCM, or an opaque id? Everything
        // about implementing real audio depends on which, so look rather than
        // assume: print the value and, if it is mapped, the first bytes there.
        // A RIFF/WAVE header or plausible PCM says pointer; unmapped or tiny
        // says id.
        if (std::getenv("CLICKHLE_DUMP_SFXBUF") != nullptr) {
            std::string head;
            std::string ascii;
            for (u32 i = 0; i < 16; ++i) {
                const auto b = f.memory.read<u8>(buffer + i);
                if (!b.has_value()) { head = "<unmapped>"; break; }
                char t[4];
                std::snprintf(t, sizeof(t), "%02x ", *b);
                head += t;
                ascii += (*b >= 32 && *b < 127) ? static_cast<char>(*b) : '.';
            }
            CLICKHLE_INFO(kCategory)
                << "sfx_set_buffer handle=" << handle << " buffer=0x"
                << std::hex << buffer << std::dec << " bytes: " << head
                << " |" << ascii << "|";
        }
        try_start_voice(f, handle);
        f.ret(0);
    });

    // The unnamed property setters. All take (handle, value) and are told apart
    // only by ordinal, so that is how the value is filed -- inventing names for
    // fields whose meaning is unknown would read as knowledge we do not have.
    for (const u32 ordinal : {8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 18u}) {
        fn(ordinal, "sfx_set_field_" + std::to_string(ordinal), 2,
           [ordinal](HleCallFrame& f) {
               const u32 handle = f.args.next_u32();
               const u32 value = f.args.next_u32();
               g_sounds[handle].fields[ordinal] = value;
               // #7's pointer is now known to be raw 16-bit PCM with the RIFF
               // header stripped, so the remaining unknown for real playback
               // is the LENGTH -- which has to arrive through one of these
               // unnamed setters. Logging every one against the known .wav
               // sizes is what identifies which.
               if (std::getenv("CLICKHLE_DUMP_SFXFIELDS") != nullptr) {
                   CLICKHLE_INFO(kCategory)
                       << "sfx_set_field #" << ordinal << " handle=" << handle
                       << " value=" << value << " (0x" << std::hex << value
                       << std::dec << ")";
               }
               f.ret(0);
           });
    }

    fn(16, "sfx_set_repeat", 2, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        g_sounds[handle].repeat = f.args.next_u32();
        f.ret(0);
    });

    // #17 stores the state byte at obj[+0x3d] and then WALKS the observer list
    // at obj[+0x40], applying the same byte down the chain:
    //
    //     strb r4, [r0, #0x3d]
    //     ldr  r0, [r0, #0x40]     ; list head
    //     cmp  r0, #0
    //     cmpne r0, r1             ; and not the object itself
    //     bne  0x1026048c          ; apply to the next one too
    //
    // On a fresh object +0x40 is 0, so this is a plain setter until #21 links
    // something. The walk is implemented anyway: a title that does build a
    // chain would otherwise have state applied to one voice instead of all of
    // them, which is a silent wrong-behaviour rather than a crash.
    // Logged because #17 is the strongest play-trigger candidate: no title
    // calls #2 sfx_play, but #17 IS called, and per LLE's AUDIO.md it writes
    // the state byte 1=playing / 2=paused / 3=stopped.
    fn(17, "sfx_set_state", 2, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        const u32 state = f.args.next_u32();
        if (std::getenv("CLICKHLE_DUMP_SFXFIELDS") != nullptr) {
            CLICKHLE_INFO(kCategory)
                << "sfx_set_state handle=" << handle << " state=" << state
                << (state == 1 ? "  <- PLAYING" : "");
        }
        if (state == 1) {
            try_start_voice(f, handle);
        }
        // Bounded by the table size: the firmware's guard is "not the object
        // itself", which does not protect against a longer cycle, but we can
        // afford to be safer than the hardware here.
        u32 current = handle;
        for (std::size_t steps = 0; steps <= g_sounds.size(); ++steps) {
            const auto it = g_sounds.find(current);
            if (it == g_sounds.end()) {
                break;
            }
            it->second.state = state;
            if (state != 1) {
                it->second.started = false;
            }
            const u32 next = it->second.next;
            if (next == 0 || next == handle) {
                break;  // the firmware's own two stop conditions
            }
            current = next;
        }
        f.ret(0);
    });

    // #21 writes obj[+0x40], the list head #17 walks. Measured by oracle as the
    // only slot that touches that offset.
    //
    // NOT EXERCISED by any title in the dynamic ordinal survey -- but that
    // survey is a lower bound per run, so this is implemented rather than left
    // to the nullary-zero fallback, where a title that did call it would have
    // its link silently dropped.
    fn(21, "sfx_set_next", 2, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        g_sounds[handle].next = f.args.next_u32();
        f.ret(0);
    });

    // #7 and #23 are a ROUND-TRIP ON THE SAME WORD, and this returned the wrong
    // one.
    //
    // The firmware (LLE, AUDIO.md, 2026-09-18) is unambiguous: `#7
    // audio_sfx_set_buffer` at 0x102603e0 is `obj[0x04] = arg1`, and `#23`
    // at 0x10260140 is `return obj[0x04]`. One field, written by one slot and
    // read by the other.
    //
    // We wrote #7's argument into `.buffer` and answered #23 from `.state` --
    // two different fields -- so a title stored a buffer, read it back, and got
    // whatever its last state happened to be. "A title that cannot read back
    // what it just stored has no way to proceed", which is exactly where
    // Mahjong stopped: #0, #7, #10, #11, #12, #17, #18, #23, each once, then
    // silence.
    //
    // The old comment here -- "answering the state they last set is the only
    // consistent thing to do" -- was reasoning from the guess that #23 was a
    // state getter. It is not; it is the buffer getter.
    fn(23, "sfx_get_buffer", 1, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        const auto it = g_sounds.find(handle);
        f.ret(it == g_sounds.end() ? 0 : it->second.buffer);
    });

    // The music stream. Registered once, then driven.
    // #40(name, a, b, c) -- registers a named stream and posts it to the audio
    // server. The FIRST ARGUMENT IS A STRING and the firmware strlen/strcpys
    // it; the third is a byte, not a word.
    //
    // We had this at arity 0 returning a constant 1, which discarded all four
    // arguments including the name -- a title registering a named stream was
    // silently ignored. Nothing audible depends on it yet, but the name is the
    // most informative thing a title tells us about its audio.
    fn(40, "stream_register", 4, [](HleCallFrame& f) {
        const GuestAddr name_address = f.args.next_ptr();
        const u32 a = f.args.next_u32();
        const u32 b = f.args.next_u32() & 0xFFu;  // a byte in the firmware
        const u32 c = f.args.next_u32();
        std::string name;
        for (u32 i = 0; name_address != 0 && i < 128; ++i) {
            const auto ch = f.memory.read<u8>(name_address + i);
            if (!ch.has_value() || *ch == 0) {
                break;
            }
            name.push_back(static_cast<char>(*ch));
        }
        // The "name" is empty for most titles and the fourth argument looks
        // like a byte count, which fits (buffer, ?, ?, length) better than a
        // filename. Dump the bytes at arg0 so the values decide: PCM-shaped
        // data says buffer, printable text says name.
        if (std::getenv("CLICKHLE_DUMP_STREAMREG") != nullptr) {
            std::string head;
            for (u32 i = 0; i < 16; ++i) {
                const auto by = f.memory.read<u8>(name_address + i);
                if (!by.has_value()) { head = "<unmapped>"; break; }
                char t[4];
                std::snprintf(t, sizeof(t), "%02x ", *by);
                head += t;
            }
            // 22050 Hz 16-bit stereo is what every .wav beside these games
            // is, so express the length that way as a sanity check.
            const double secs = static_cast<double>(c) / (22050.0 * 2 * 2);
            CLICKHLE_INFO(kCategory)
                << "stream_register arg0=0x" << std::hex << name_address
                << std::dec << " bytes: " << head << " len=" << c
                << " (" << secs << "s @22050/16/stereo)";
        }
        CLICKHLE_INFO(kCategory)
            << "stream_register \"" << name << "\" (" << a << ", " << b << ", "
            << c << ")";
        f.ret(1);
    });

    // #41(a) and #44(a, b) -- marshal their arguments into a heap block and
    // post it to the audio server. Fire-and-forget: the int they return is the
    // result of the *post*, not of the work, so 0 is success.
    fn(41, "audio_post_1", 1, [](HleCallFrame& f) {
        CLICKHLE_DEBUG(kCategory) << "audio_post_1 " << f.args.next_u32();
        f.ret(0);
    });
    fn(44, "audio_post_2", 2, [](HleCallFrame& f) {
        const u32 a = f.args.next_u32();
        CLICKHLE_DEBUG(kCategory)
            << "audio_post_2 " << a << ", " << f.args.next_u32();
        f.ret(0);
    });
    // #43(a) and #48(a) marshal their argument into a heap block and post it
    // to the audio server, exactly like #41/#44. Discarding the argument makes
    // a title that starts a stream look like one that never asked.
    fn(43, "stream_play", 1, [](HleCallFrame& f) {
        CLICKHLE_DEBUG(kCategory) << "stream_play " << f.args.next_u32();
        f.ret(0);
    });
    fn(48, "stream_repeat", 1, [](HleCallFrame& f) {
        CLICKHLE_DEBUG(kCategory) << "stream_repeat " << f.args.next_u32();
        f.ret(0);
    });
    fn(50, "set_shuffle", 1, [](HleCallFrame& f) {
        (void)f.args.next_u32();
        f.ret(0);
    });

    // Constants. Named for what they answer, which is all that was established.
    fn(52, "query_ff", 0, [](HleCallFrame& f) { f.ret(0xFF); });
    for (const u32 ordinal : {42u, 45u, 46u, 47u, 49u, 51u, 53u, 55u, 56u,
                              60u}) {
        fn(ordinal, "zero", 0, [](HleCallFrame& f) { f.ret(0); });
    }

    CLICKHLE_DEBUG(kCategory) << "audio registered (silent)";
}

}  // namespace clickhle

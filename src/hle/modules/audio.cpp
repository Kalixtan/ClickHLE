// The "Audio" library: 61 ordinals, of which the shape of about thirty is
// known.
//
// Two things live here. A *sound effect* is a small object with a great many
// field setters -- create it, point it at a buffer, set a dozen properties,
// play it. A *stream* is the music path, registered once and then played,
// repeated or shuffled.
//
// SFX playback consumes the PCM buffer the guest configured. Compressed music
// streams are registered by the game and decoded from its installed assets.

#include "clickhle/hle/modules.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <set>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "clickhle/common/log.h"
#include "clickhle/audio/audio_device.h"
#include "clickhle/audio/audio_decoder.h"
#include "clickhle/core/emulator.h"
#include "clickhle/hle/call_frame.h"
#include "clickhle/hle/registry.h"

namespace clickhle {
namespace {

constexpr std::string_view kCategory = "audio";
constexpr const char* kModule = "Audio";

using Sound = Emulator::AudioState::Sound;
constexpr u32 kMusicVoiceKey = 0xFFFFFFFFu;

bool valid_sound_handle(HleCallFrame& f, u32 handle) {
    const auto& sounds = f.emulator.audio_state().sounds;
    if (sounds.contains(handle)) {
        return true;
    }
    CLICKHLE_ERROR(kCategory) << "invalid SFX handle " << handle
                              << " (RetailOS table lookup returns null)";
    // The firmware callers immediately dereference the lookup result. Stop at
    // that boundary rather than silently manufacturing a new sound object.
    f.cpu.request_stop();
    return false;
}

template <typename T>
std::optional<T> read_sound_member(HleCallFrame& f, u32 handle, u32 offset) {
    if (!valid_sound_handle(f, handle)) return std::nullopt;
    const GuestAddr object =
        f.emulator.audio_state().sounds.at(handle).guest_object;
    const auto value = f.memory.read<T>(object + offset);
    if (!value) {
        CLICKHLE_ERROR(kCategory) << "unreadable SFX field at 0x" << std::hex
                                  << object + offset;
        f.cpu.request_stop();
    }
    return value;
}

void set_sound_state(HleCallFrame& f, std::map<u32, Sound>& sounds,
                     u32 handle, u32 state) {
    const auto first = sounds.find(handle);
    if (first == sounds.end()) return;
    const GuestAddr root = first->second.guest_object;
    GuestAddr current = root;
    std::set<GuestAddr> visited;
    while (current != 0 && !visited.contains(current)) {
        visited.insert(current);
        if (!f.memory.write<u8>(current + 0x3d, static_cast<u8>(state))) {
            CLICKHLE_ERROR(kCategory) << "invalid SFX chain pointer 0x"
                                      << std::hex << current;
            f.cpu.request_stop();
            return;
        }
        for (auto& [id, sound] : sounds) {
            (void)id;
            if (sound.guest_object == current) {
                sound.state = state & 0xffu;
                if (state == 3) sound.started = false;
                break;
            }
        }
        const auto next = f.memory.read<u32>(current + 0x40);
        if (!next) {
            CLICKHLE_ERROR(kCategory) << "unreadable SFX chain link at 0x"
                                      << std::hex << current + 0x40;
            f.cpu.request_stop();
            return;
        }
        current = *next;
        if (current == root) break;
    }
}

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

/// Play the guest's PCM when it explicitly starts a sound. The audio setters
/// give us the buffer address, length and format. Tetris's configured length
/// matches its WAV data chunk exactly, and the bytes at #7's pointer are PCM.
bool try_start_voice(HleCallFrame& f, u32 handle) {
    auto& sounds = f.emulator.audio_state().sounds;
    const auto it = sounds.find(handle);
    if (it == sounds.end()) {
        return false;
    }
    Sound& snd = it->second;
    const u32 length = snd.fields.at(kFieldLength);
    const u32 rate = snd.fields.at(kFieldRate);
    const u32 channels = snd.fields.at(kFieldChannels);
    const u32 bits = snd.fields.at(kFieldBits);
    if (snd.buffer == 0 || length == 0 || length > 64u * 1024u * 1024u ||
        rate == 0 || channels == 0 || channels > 2 ||
        (bits != 8 && bits != 16) ||
        length % (channels * (bits / 8u)) != 0) {
        CLICKHLE_WARN(kCategory) << "sfx handle=" << handle
                                  << ": invalid guest PCM format or length";
        return false;
    }
    std::vector<u8> pcm(length);
    if (!f.memory.read_bytes(snd.buffer, pcm.data(), length)) {
        CLICKHLE_WARN(kCategory) << "sfx handle=" << handle
                                  << ": unreadable guest PCM buffer 0x"
                                  << std::hex << snd.buffer;
        return false;
    }
    AudioDevice* dev = f.emulator.open_audio(rate, static_cast<u16>(channels),
                                              static_cast<u16>(bits));
    if (dev == nullptr ||
        !dev->play(handle, pcm.data(), length, rate,
                   static_cast<u16>(channels), static_cast<u16>(bits), false)) {
        return false;
    }
    snd.started = true;
    set_sound_state(f, sounds, handle, 1);
    append_wav_dump(pcm.data(), length, rate, static_cast<u16>(channels),
                    static_cast<u16>(bits));
    CLICKHLE_DEBUG(kCategory)
        << "sfx play handle=" << handle << " " << length
        << " guest PCM bytes @" << rate << "/" << bits << "/" << channels;
    return true;
}

bool start_stream(HleCallFrame& f, u32 index) {
    auto& state = f.emulator.audio_state();
    if (index >= state.stream_assets.size() || state.stream_assets[index].empty()) {
        return false;
    }
    auto audio = decode_game_audio(state.stream_assets[index]);
    if (!audio) {
        CLICKHLE_WARN(kCategory)
            << "stream " << index << ": unsupported or unreadable audio "
            << state.stream_assets[index].string();
        return false;
    }
    AudioDevice* device = f.emulator.open_audio(
        audio->sample_rate, audio->channels, audio->bits_per_sample);
    if (device == nullptr ||
        !device->play(kMusicVoiceKey, audio->pcm.data(),
                      static_cast<u32>(audio->pcm.size()), audio->sample_rate,
                      audio->channels, audio->bits_per_sample,
                      state.stream_repeat_internal_mode != 0)) {
        return false;
    }
    CLICKHLE_INFO(kCategory)
        << "music stream " << index << " started from "
        << state.stream_assets[index].string();
    return true;
}

void register_audio(HleRegistry& registry) {
    const auto fn = [&registry](u32 ordinal, std::string name, u8 arity,
                                HleHandler handler) {
        registry.add(kModule, ordinal, std::move(name), arity,
                     std::move(handler));
    };

    fn(0, "sfx_create", 0, [](HleCallFrame& f) {
        auto& sounds = f.emulator.audio_state().sounds;
        u32 handle = 0;
        while (sounds.contains(handle) && handle != 0xFFFFFFFFu) {
            ++handle;
        }
        if (handle == 0xFFFFFFFFu) {
            f.ret(0xFFFFFFFFu);
            return;
        }
        Sound sound{};
        sound.guest_object = f.emulator.heap().allocate(0x4c);
        if (sound.guest_object == 0) {
            CLICKHLE_ERROR(kCategory) << "sfx_create could not allocate guest object";
            f.cpu.request_stop();
            return;
        }
        for (u32 offset = 0; offset < 0x4c; offset += 4) {
            f.store<u32>(sound.guest_object + offset, 0);
        }
        f.store<u32>(sound.guest_object, 0x0066d260);
        f.store<u32>(sound.guest_object + 0x10, 0xac44);
        f.store<u32>(sound.guest_object + 0x14, 1);
        f.store<u32>(sound.guest_object + 0x18, 0x10);
        f.store<u32>(sound.guest_object + 0x1c, 0x7fff);
        f.store<u32>(sound.guest_object + 0x20, 1000);
        f.store<u32>(sound.guest_object + 0x30, 0x7fff);
        f.store<u32>(sound.guest_object + 0x38, 1);
        f.store<u32>(sound.guest_object + 0x44, 0x7fff);
        f.store<u32>(sound.guest_object + 0x48, 0);
        sounds.emplace(handle, sound);
        CLICKHLE_DEBUG(kCategory) << "sfx_create -> " << handle;
        f.ret(handle);
    });

    fn(1, "sfx_release", 1, [](HleCallFrame& f) {
        auto& sounds = f.emulator.audio_state().sounds;
        const u32 handle = f.args.next_u32();
        const auto it = sounds.find(handle);
        if (it != sounds.end()) {
            if (AudioDevice* device = f.emulator.audio()) {
                device->stop_voice(handle);
            }
            (void)f.emulator.heap().release(it->second.guest_object);
            sounds.erase(it);
        }
        f.ret_void();
    });

    fn(2, "sfx_play", 1, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        auto& sounds = f.emulator.audio_state().sounds;
        if (!valid_sound_handle(f, handle)) {
            return;
        }
        // #2 is the explicit SFX play operation. #17 only changes the state
        // byte and is commonly used while registering a voice.
        const bool started = try_start_voice(f, handle);
        // Optional field dump for a guest that configures a format we cannot
        // play. The ordinary path reads its buffer, length and format directly.
        const auto it = sounds.find(handle);
        if (std::getenv("CLICKHLE_DUMP_SFXPLAY") != nullptr &&
            it != sounds.end()) {
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
        CLICKHLE_DEBUG(kCategory) << "sfx_play " << handle
                                  << (started ? " (played)" : " (no readable PCM)");
        f.ret_void();
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
    // Keep the state byte on every object in the linked ring, as RetailOS does.
    constexpr u32 kVoicePlaying = 1;
    constexpr u32 kVoicePaused = 2;
    constexpr u32 kVoiceStopped = 3;

    const auto set_state = [](u32 state) {
        return [state](HleCallFrame& f) {
            const u32 handle = f.args.next_u32();
            if (!valid_sound_handle(f, handle)) {
                return;
            }
            set_sound_state(f, f.emulator.audio_state().sounds, handle, state);
            if (AudioDevice* device = f.emulator.audio()) {
                if (state == kVoicePaused) {
                    device->pause_voice(handle, true);
                } else if (state == kVoicePlaying) {
                    device->pause_voice(handle, false);
                } else if (state == kVoiceStopped) {
                    device->stop_voice(handle);
                }
            }
            f.ret_void();
        };
    };

    fn(4, "sfx_resume", 1, set_state(kVoicePlaying));
    fn(3, "sfx_pause", 1, set_state(kVoicePaused));
    fn(5, "sfx_stop", 1, set_state(kVoiceStopped));

    // Reads the state byte and answers "is this voice playing?".
    fn(39, "sfx_is_playing", 1, [kVoicePlaying](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        const auto state = read_sound_member<u8>(f, handle, 0x3d);
        if (state) f.ret(*state == kVoicePlaying ? 1u : 0u);
    });

    // #37 returns zero when no sub-object is attached; otherwise it reads the
    // word at subobject+0x2c. Keep the guest pointer traversal intact even
    // though Audio#0 initializes the attachment pointer to null.
    fn(37, "sfx_get_attached_property", 1, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        if (!valid_sound_handle(f, handle)) {
            return;
        }
        const GuestAddr object =
            f.emulator.audio_state().sounds.at(handle).guest_object;
        const auto attached = read_sound_member<u32>(f, handle, 0x34);
        if (!attached) {
            CLICKHLE_ERROR(kCategory) << "unreadable SFX attachment pointer at 0x"
                                      << std::hex << object + 0x34;
            f.cpu.request_stop();
            return;
        }
        if (*attached == 0) {
            f.ret(0);
            return;
        }
        const auto property = f.memory.read<u32>(*attached + 0x2c);
        if (!property) {
            CLICKHLE_ERROR(kCategory) << "invalid SFX attachment pointer 0x"
                                      << std::hex << *attached;
            f.cpu.request_stop();
            return;
        }
        f.ret(*property);
    });

    fn(7, "sfx_set_buffer", 2, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        const u32 buffer = f.args.next_u32();
        auto& sounds = f.emulator.audio_state().sounds;
        if (!valid_sound_handle(f, handle)) {
            return;
        }
        Sound& sound = sounds.at(handle);
        sound.buffer = buffer;
        f.store<u32>(sound.guest_object + 0x04, buffer);
        sound.started = false;  // re-pointed: allow it to play again
        if (AudioDevice* device = f.emulator.audio()) {
            device->stop_voice(handle);
        }
        // The caller's pointer normally identifies raw PCM already loaded by
        // the game. Keep an opt-in prefix dump for titles with other layouts.
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
        f.ret_void();
    });

    // The unnamed property setters. All take (handle, value) and are told apart
    // only by ordinal, so that is how the value is filed -- inventing names for
    // fields whose meaning is unknown would read as knowledge we do not have.
    // RetailOS declares #9's value as a byte and stores it at +0x0c. Keep the
    // same byte-width behavior for the other byte properties, #17 (+0x3d) and
    // #18 (+0x3e), rather than retaining bits that the firmware discards.
    for (const u32 ordinal : {8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u,
                              18u, 19u, 20u}) {
        fn(ordinal, "sfx_set_field_" + std::to_string(ordinal), 2,
           [ordinal](HleCallFrame& f) {
               const u32 handle = f.args.next_u32();
               u32 value = f.args.next_u32();
               if (!valid_sound_handle(f, handle)) {
                   return;
               }
               if (ordinal == 9 || ordinal == 18) {
                   value &= 0xFFu;
               }
               f.emulator.audio_state().sounds.at(handle).fields[ordinal] = value;
               const u32 offset = ordinal == 8 ? 0x08 : ordinal == 9 ? 0x0c :
                   ordinal == 10 ? 0x10 : ordinal == 11 ? 0x14 :
                   ordinal == 12 ? 0x18 : ordinal == 13 ? 0x1c :
                   ordinal == 14 ? 0x24 : ordinal == 15 ? 0x20 :
                   ordinal == 18 ? 0x3e : ordinal == 19 ? 0x2c : 0x28;
               if (ordinal == 9 || ordinal == 18)
                   f.store<u8>(f.emulator.audio_state().sounds.at(handle).guest_object + offset,
                               static_cast<u8>(value));
               else
                   f.store<u32>(f.emulator.audio_state().sounds.at(handle).guest_object + offset, value);
               // These fields also supply the playback length and PCM format.
               if (std::getenv("CLICKHLE_DUMP_SFXFIELDS") != nullptr) {
                   CLICKHLE_INFO(kCategory)
                       << "sfx_set_field #" << ordinal << " handle=" << handle
                       << " value=" << value << " (0x" << std::hex << value
                       << std::dec << ")";
               }
               f.ret_void();
           });
    }

    fn(16, "sfx_set_repeat", 2, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        const u32 repeat = f.args.next_u32();
        if (!valid_sound_handle(f, handle)) {
            return;
        }
        f.emulator.audio_state().sounds.at(handle).repeat = repeat;
        f.store<u32>(f.emulator.audio_state().sounds.at(handle).guest_object + 0x38, repeat);
        f.ret_void();
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
        const u32 state = f.args.next_u32() & 0xFFu;
        if (!valid_sound_handle(f, handle)) {
            return;
        }
        if (std::getenv("CLICKHLE_DUMP_SFXFIELDS") != nullptr) {
            CLICKHLE_INFO(kCategory)
                << "sfx_set_state handle=" << handle << " state=" << state
                << (state == 1 ? "  <- PLAYING" : "");
        }
        set_sound_state(f, f.emulator.audio_state().sounds, handle, state);
        f.ret_void();
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
        const u32 next = f.args.next_u32();
        if (!valid_sound_handle(f, handle)) {
            return;
        }
        auto& sound = f.emulator.audio_state().sounds.at(handle);
        sound.next = next;
        f.store<u32>(sound.guest_object + 0x40, next);
        f.ret_void();
    });

    // #22 stores two words at +0x44 and +0x48. The first is initialized to
    // 0x7fff by #0; neither field has a recovered public getter.
    fn(22, "sfx_set_auxiliary", 3, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        const u32 first = f.args.next_u32();
        const u32 second = f.args.next_u32();
        if (!valid_sound_handle(f, handle)) {
            return;
        }
        auto& sound = f.emulator.audio_state().sounds.at(handle);
        sound.fields[22] = first;
        sound.auxiliary_48 = second;
        f.store<u32>(sound.guest_object + 0x44, first);
        f.store<u32>(sound.guest_object + 0x48, second);
        f.ret_void();
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
        const auto value = read_sound_member<u32>(f, handle, 0x04);
        if (value) f.ret(*value);
    });

    // Field readers mirror the direct loads in RetailOS. Names remain neutral
    // where the property's meaning is not established by game use.
    for (const auto& [ordinal, offset] :
         {std::pair<u32, u32>{24, 0x08}, {25, 0x0c}, {26, 0x10},
          {27, 0x14}, {28, 0x18}, {29, 0x1c}, {30, 0x20},
          {32, 0x24}, {34, 0x3e}, {35, 0x2c}, {36, 0x28}}) {
        fn(ordinal, "sfx_get_field_" + std::to_string(ordinal), 1,
           [ordinal, offset](HleCallFrame& f) {
               const u32 handle = f.args.next_u32();
               if (ordinal == 25 || ordinal == 34) {
                   const auto value = read_sound_member<u8>(f, handle, offset);
                   if (value) f.ret(*value);
               } else {
                   const auto value = read_sound_member<u32>(f, handle, offset);
                   if (value) f.ret(*value);
               }
           });
    }
    fn(31, "sfx_get_repeat", 1, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        const auto value = read_sound_member<u32>(f, handle, 0x38);
        if (value) f.ret(*value);
    });
    fn(33, "sfx_get_state", 1, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        const auto value = read_sound_member<u8>(f, handle, 0x3d);
        if (value) f.ret(*value);
    });
    fn(38, "sfx_get_next", 1, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        const auto value = read_sound_member<u32>(f, handle, 0x40);
        if (value) f.ret(*value);
    });

    // The music stream. Registered once, then driven.
    // #40(name, a, b, c) -- copies the name into a message, then posts it to
    // the audio server. The first argument is a string; the third argument is
    // a byte. Ghidra's ARM decompilation also shows a 64-bit return made from
    // (arg1, arg0), so preserve both result registers even though the companion
    // emulator models this as a stream index.
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
        auto& audio = f.emulator.audio_state();
        std::filesystem::path asset = audio.pending_stream_asset;
        audio.pending_stream_asset.clear();
        if (asset.empty() && !name.empty()) {
            const std::string extension =
                std::filesystem::path(name).extension().string();
            if (extension == ".m4a" || extension == ".mp3" ||
                extension == ".aac" || extension == ".mp4") {
                asset = f.emulator.vfs().resolve(name);
                if (asset.empty()) asset = f.emulator.vfs().resolve_media(name);
            }
        }
        audio.stream_assets.push_back(std::move(asset));
        // RetailOS queues one registration record per call. Its message task
        // is not modeled, but Metadata#134 exposes the resulting stream count.
        if (audio.registered_stream_count != 0xFFFFFFFFu) {
            ++audio.registered_stream_count;
        }
        f.ret64((static_cast<u64>(a) << 32) | name_address);
    });

    // These wrappers post audio-server messages and return the server's post
    // status. The firmware dispatcher applies its state changes before the
    // queue status is produced. The audio engine itself is still absent.
    fn(41, "audio_post_1", 1, [](HleCallFrame& f) {
        auto& state = f.emulator.audio_state();
        const u32 command = f.args.next_u32();
        // RetailOS message 0x6600000e moves to the preceding stream when a
        // stream index is selected. Its -1 sentinel delegates to the player
        // service, which ClickHLE does not emulate.
        if (state.stream_index > 0) {
            --state.stream_index;
        }
        CLICKHLE_DEBUG(kCategory) << "audio previous-stream " << command
                                  << " -> " << state.stream_index;
        f.ret(0);
    });
    fn(44, "audio_post_2", 2, [](HleCallFrame& f) {
        const u32 a = f.args.next_u32();
        CLICKHLE_DEBUG(kCategory)
            << "audio_post_2 " << a << ", " << f.args.next_u32();
        f.ret(0);
    });
    // #43(a) and #48(a) marshal their argument into a heap block and post it
    // to the audio server, exactly like #41/#44. The local RetailOS dispatcher
    // ignores #43's payload when updating context+0x114; the player-service
    // interpretation of that payload as a stream index is corroborated by the
    // comparison emulator, so retain it as host-side player state.
    fn(43, "stream_play", 1, [](HleCallFrame& f) {
        auto& state = f.emulator.audio_state();
        state.stream_index = static_cast<i64>(f.args.next_u32());
        // The RetailOS dispatcher for message 0x66000011 sets context+0x114
        // to 1. The host plays a registered bundled audio asset when one was
        // identified through the game's resource-name lookup.
        state.stream_control = 1;
        if (state.stream_index >= 0) {
            static_cast<void>(start_stream(
                f, static_cast<u32>(state.stream_index)));
        }
        CLICKHLE_DEBUG(kCategory) << "stream_play " << state.stream_index;
        f.ret(0);
    });
    fn(48, "stream_repeat", 1, [](HleCallFrame& f) {
        const u32 raw = f.args.next_u32();
        // The wrapper posts the raw word; the player task consumes its low
        // byte and clamps unsupported modes to "all" (2), as observed in the
        // independent emulator. It stores one/all in the inverse internal
        // representation exposed by #47 (RetailOS maps internal 1 -> public 2
        // and internal 2 -> public 1). The consumer itself is outside this
        // firmware image, so this mapping follows comparison research.
        const u32 public_mode = std::min(raw & 0xFFu, 2u);
        f.emulator.audio_state().stream_repeat_internal_mode =
            public_mode == 1 ? 2u : (public_mode == 2 ? 1u : 0u);
        CLICKHLE_DEBUG(kCategory) << "stream_repeat " << raw;
        f.ret(0);
    });
    fn(50, "set_shuffle", 1, [](HleCallFrame& f) {
        f.emulator.audio_state().stream_shuffle = f.args.next_u32() != 0;
        f.ret(0);
    });

    // #54/#55 expose the stream service field at +0x14. The retail setter
    // ignores values outside the unsigned 16-bit range; its constructor's
    // initial value is still unknown, so the emulator starts at zero.
    fn(54, "stream_get_level", 0, [](HleCallFrame& f) {
        f.ret(f.emulator.audio_state().stream_level);
    });
    fn(55, "stream_set_level", 1, [](HleCallFrame& f) {
        const u32 level = f.args.next_u32();
        if (level < 0x8000u) {
            f.emulator.audio_state().stream_level = level;
        }
        f.ret_void();
    });

    // #56 returns context+0x114 directly unless it contains the -1 fallback
    // sentinel, in which case RetailOS queries the player service. The player
    // service is absent here, so that fallback remains inactive.
    fn(56, "stream_get_state", 0, [](HleCallFrame& f) {
        const i32 control = f.emulator.audio_state().stream_control;
        f.ret(control < 0 ? 0u : static_cast<u32>(control) & 0xFFu);
    });

    // #58 initializes the two-word audio-format result to (4, 1). This small
    // exported helper's output is directly visible in the firmware body.
    fn(58, "audio_format_defaults", 1, [](HleCallFrame& f) {
        const GuestAddr out = f.args.next_ptr();
        if (!f.store<u16>(out, 4) || !f.store<u16>(out + 2, 1)) {
            f.cpu.request_stop();
            return;
        }
        f.ret_void();
    });

    // Constants. Named for what they answer, which is all that was established.
    fn(52, "query_ff", 0, [](HleCallFrame& f) { f.ret(0xFF); });
    // #53 posts the player-volume message. RetailOS clamps the input to 255
    // and scales it against a player-owned quantity before dispatch. Keep the
    // clamped request observable, but return the common successful-post status
    // while the player task is absent.
    fn(53, "set_player_volume", 1, [](HleCallFrame& f) {
        const u32 volume = std::min(f.args.next_u32(), 0xFFu);
        CLICKHLE_DEBUG(kCategory) << "set_player_volume " << volume
                                  << " (player task unavailable)";
        f.ret(0);
    });
    fn(42, "stream_select_first", 0, [](HleCallFrame& f) {
        // Message 0x66000010 resets the selected stream index to zero.
        f.emulator.audio_state().stream_index = 0;
        f.ret(0);
    });
    fn(45, "stream_stop", 0, [](HleCallFrame& f) {
        // Message 0x66000015 clears context+0x114.
        f.emulator.audio_state().stream_control = 0;
        if (AudioDevice* device = f.emulator.audio()) {
            device->stop_voice(kMusicVoiceKey);
        }
        f.ret(0);
    });
    fn(46, "stream_toggle_pause", 0, [](HleCallFrame& f) {
        auto& control = f.emulator.audio_state().stream_control;
        // Message 0x66000013 switches 1 <-> 2; other states are unchanged.
        if (control == 1) {
            control = 2;
        } else if (control == 2) {
            control = 1;
        }
        if (AudioDevice* device = f.emulator.audio()) {
            device->pause_voice(kMusicVoiceKey, control == 2);
        }
        f.ret(0);
    });
    fn(47, "stream_get_repeat", 0, [](HleCallFrame& f) {
        const u32 internal =
            f.emulator.audio_state().stream_repeat_internal_mode;
        f.ret(internal == 1 ? 2u : (internal == 2 ? 1u : 0u));
    });
    // #49 reads player fields: inactive -> 0; active -> 1 normally, 2 when
    // shuffle is enabled. The player object is absent, so derive activity and
    // shuffle from the stream commands modeled above (#43/#45/#46/#50).
    fn(49, "stream_get_play_mode", 0, [](HleCallFrame& f) {
        const auto& state = f.emulator.audio_state();
        if (state.stream_control <= 0) {
            f.ret(0);
        } else {
            f.ret(state.stream_shuffle ? 2u : 1u);
        }
    });
    // #51 needs the absent player service's position and duration. #60 reads
    // the player position, clamped to duration, and is zero while inactive.
    // Keep these at their measured inactive-player value until a stream clock
    // is implemented rather than returning synthetic position data.
    for (const u32 ordinal : {51u, 60u}) {
        fn(ordinal, "zero", 0, [ordinal](HleCallFrame& f) {
            if (std::getenv("CLICKHLE_AUDIO_CALL_TRACE") != nullptr) {
                CLICKHLE_INFO(kCategory)
                    << "Audio#" << ordinal << " registers r0-r3=0x" << std::hex
                    << f.cpu.reg(0) << ",0x" << f.cpu.reg(1) << ",0x"
                    << f.cpu.reg(2) << ",0x" << f.cpu.reg(3) << std::dec;
            }
            f.ret(0);
        });
    }

    CLICKHLE_DEBUG(kCategory) << "audio HLE registered";
}

}  // namespace clickhle

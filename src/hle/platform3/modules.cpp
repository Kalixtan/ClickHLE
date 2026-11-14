#include "clickhle/hle/modules.h"
#include "clickhle/core/emulator.h"
#include "clickhle/hle/call_frame.h"
#include "clickhle/hle/registry.h"
#include "clickhle/hle/vfs.h"
#include "clickhle/common/log.h"
#include "clickhle/audio/audio_device.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <optional>

namespace clickhle {
namespace {

// Distinguish an absent FAT path from an unreadable device directory.
std::optional<bool> user_database_exists(const std::filesystem::path& root) {
    if (root.empty()) return std::nullopt;
    auto current = root;
    for (const char* component : {"ipod_control", "device", "users"}) {
        std::error_code error;
        std::filesystem::directory_iterator it(current, error), end;
        if (error) return std::nullopt;
        bool found = false;
        for (; it != end; it.increment(error)) {
            if (error) return std::nullopt;
            auto name = it->path().filename().string();
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (name == component) { current = it->path(); found = true; break; }
        }
        if (error) return std::nullopt;
        if (!found) return false;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(current, error)) || error)
            return std::nullopt;
    }
    return true;
}

} // namespace

void register_classic_graphics(HleRegistry& registry);
void register_classic_gpu_graphics(HleRegistry& registry);
void register_classic_metadata(HleRegistry& registry);

void register_classic_modules(HleRegistry& registry, bool gpu) {
    // Only shared framework hashes are eligible for these implementations.
    register_input(registry);
    register_fileio(registry);
    register_classic_metadata(registry);
    register_classic_graphics(registry);
    if (gpu) register_classic_gpu_graphics(registry);
    registry.add("miscTBD", 8, "classic_resource_path", 4, [](HleCallFrame& frame) {
        const u32 mode = frame.args.next_u32();
        const GuestAddr output = frame.args.next_ptr();
        const GuestAddr size_address = frame.args.next_ptr();
        const GuestAddr name_address = frame.args.next_ptr();
        const auto capacity = frame.memory.read<u32>(size_address);
        if (!capacity || !name_address || mode > 4) {
            frame.cpu.request_stop();
            return;
        }

        // Provisional: use the installed directory name as the game GUID and
        // the external-resource flag. Internal-resource launch metadata is not
        // represented here yet.
        const auto root = frame.emulator.vfs().root();
        auto lower = [](std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        };
        if (lower(root.parent_path().filename().string()) != "games_ro" ||
            lower(root.parent_path().parent_path().filename().string()) != "ipod_control" ||
            root.filename().empty()) {
            CLICKHLE_ERROR("classic") << "7G resource path needs an installed game identity";
            frame.cpu.request_stop();
            return;
        }
        std::string name;
        for (u32 i = 0; i < 4096; ++i) {
            const auto c = frame.memory.read<u8>(name_address + i);
            if (!c) { frame.cpu.request_stop(); return; }
            if (*c == 0) break;
            name.push_back(static_cast<char>(*c));
            if (i == 4095) { frame.cpu.request_stop(); return; }
        }
        if (!name.empty() && name.front() == '/') name.erase(0, 1);
        static constexpr const char* directories[] = {
            "games_RO", "gamedata_RW", "gamestats_WO", "gamedata_ShareRW", "games_RO"};
        std::string path = std::string("iPod_Control/") + directories[mode];
        if (mode != 3) path += "/" + root.filename().string();
        // The helper omits both the separator and suffix for an empty name.
        // Here name_address is non-null, so even an empty string gets '/'.
        path += "/" + name;
        const auto length = static_cast<u32>(path.size());
        if (static_cast<i32>(*capacity) < static_cast<i32>(length)) {
            frame.ret(0xffffffffu);
            return;
        }
        if (!output) {
            if (!frame.store<u32>(size_address, 0xffffffffu)) { frame.cpu.request_stop(); return; }
            frame.ret(0xffffffffu);
            return;
        }
        // Retail uses strncpy followed by strlen on the destination. If the
        // capacity exactly equals the path length, strlen continues into the
        // caller's existing bytes until it finds a terminator.
        if (*capacity > 1024 * 1024) { frame.cpu.request_stop(); return; }
        std::vector<u8> copied(*capacity, 0);
        std::copy(path.begin(), path.end(), copied.begin());
        if (!frame.memory.write_bytes(output, copied.data(), *capacity)) {
            frame.cpu.request_stop(); return;
        }
        u32 result = 0;
        for (; result < 1024 * 1024; ++result) {
            const auto c = frame.memory.read<u8>(output + result);
            if (!c) { frame.cpu.request_stop(); return; }
            if (*c == 0) break;
        }
        if (result == 1024 * 1024 || !frame.store<u32>(size_address, result)) {
            frame.cpu.request_stop(); return;
        }
        frame.ret(result);
    });
    for (u32 ordinal : {0u, 1u, 2u}) {
        registry.add("Users", ordinal, "classic_user_lookup", ordinal == 0 ? 0 : 1,
                     [ordinal](HleCallFrame& frame) {
            if (ordinal != 0) (void)frame.args.next_u32();
            auto& vfs = frame.emulator.vfs();
            const auto present = user_database_exists(vfs.device_root());
            // 0x0812b648 loads this file at service startup and accepts an
            // absent-file error. #2 searches 16-bit record IDs, and returns
            // null for an empty list. Empty-list startup is provisional;
            // parsing persisted records and mutation are not implemented yet.
            if (!present || *present) {
                CLICKHLE_ERROR("classic") << "7G Users database is not implemented";
                frame.cpu.request_stop();
                return;
            }
            frame.ret(0);
        });
    }
    registry.add("Settings", 0, "classic_get_setting", 3, [](HleCallFrame& frame) {
        const GuestAddr name = frame.args.next_ptr();
        const GuestAddr output = frame.args.next_ptr();
        const GuestAddr size = frame.args.next_ptr();
        if (!output) { frame.ret(0xffffffcfu); return; } // -49
        std::string key;
        for (u32 i = 0; i < 64; ++i) {
            const auto c = frame.memory.read<u8>(name + i);
            if (!c) { frame.cpu.request_stop(); return; }
            if (*c == 0) break;
            key.push_back(static_cast<char>(*c));
            if (i == 63) { frame.cpu.request_stop(); return; }
        }
        if (key == "TimeFormat") {
            const auto capacity = frame.memory.read<u32>(size);
            if (!capacity) { frame.cpu.request_stop(); return; }
            if ((*capacity & 0x80000000u) || *capacity < 3) {
                frame.ret(0xffffffd0u); return;
            }
            const char* override_value = std::getenv("CLICKHLE_TIME_FORMAT");
            // The 7G table maps service values 0x60a9/0x60aa to these
            // strings. The source and default of the persisted value remain
            // provisional, shared with the device settings UI.
            const bool use_24 = frame.emulator.device_settings().time_format_24.value_or(
                override_value && std::strcmp(override_value, "24") == 0);
            if (!frame.store<u32>(size, 3) ||
                !frame.memory.write_bytes(output, use_24 ? "24" : "12", 3)) {
                frame.cpu.request_stop(); return;
            }
            frame.ret(0); return;
        }
        if (key == "HoldSwitch") {
            const auto capacity = frame.memory.read<u32>(size);
            if (!capacity) { frame.cpu.request_stop(); return; }
            if (*capacity < 1) { frame.ret(0xffffffd0u); return; }
            // 0x08088ec4 copies one byte from the hold-switch service.
            if (!frame.store<u32>(size, 1) ||
                !frame.store<u8>(output, frame.emulator.device_settings().hold_switch ? 1 : 0)) {
                frame.cpu.request_stop(); return;
            }
            frame.ret(0); return;
        }
        if (key != "Language") { frame.ret(0xffffffceu); return; } // -50
        const auto capacity = frame.memory.read<u32>(size);
        if (!capacity) { frame.cpu.request_stop(); return; }
        if (*capacity < 4) { frame.ret(0xffffffd0u); return; } // -48
        // Provisional: RetailOS checks its settings service before dispatch;
        // the service's availability/permission state is not modeled yet.
        const u32 language = frame.emulator.device_settings().language.value_or(0);
        if (!frame.store<u32>(size, 4) || !frame.store<u32>(output, language)) {
            frame.cpu.request_stop(); return;
        }
        frame.ret(0);
    });
    registry.add("Audio", 14, "classic_audio_enqueue_file", 4, [](HleCallFrame& frame) {
        const GuestAddr name = frame.args.next_ptr();
        (void)frame.args.next_ptr(); // Used only by the unimplemented object-input branch.
        const u32 flags = frame.args.next_u32();
        const u32 parameter = frame.args.next_u32();
        if ((flags & 0xff) != 0) {
            // Temporary stub for the firmware's object-input variant.
            frame.ret(0);
            return;
        }
        std::string path;
        if (name) {
            for (u32 i = 0; i < 4096; ++i) {
                const auto byte = frame.memory.read<u8>(name + i);
                if (!byte) { frame.cpu.request_stop(); return; }
                if (*byte == 0) break;
                path.push_back(static_cast<char>(*byte));
                if (i == 4095) { frame.cpu.request_stop(); return; }
            }
        }
        GuestAddr name_copy = 0;
        if (name) {
            name_copy = frame.emulator.heap().allocate(static_cast<u32>(path.size() + 1));
            if (!name_copy) { frame.cpu.request_stop(); return; }
            if (!frame.memory.write_bytes(name_copy, path.c_str(), static_cast<u32>(path.size() + 1))) {
                (void)frame.emulator.heap().release(name_copy);
                frame.cpu.request_stop();
                return;
            }
        }
        // The real worker constructs a media item from a device-relative path.
        // Container parsing and the original device's list are not yet modeled;
        // a resolved file becomes a provisional host-side playlist entry.
        const auto host_path = frame.emulator.vfs().resolve(path, Vfs::Namespace::kDevice);
        CLICKHLE_DEBUG("classic") << "7G audio queue: " << path
                                   << (host_path.empty() ? " (not found)" : " (resolved)");
        frame.emulator.classic_audio().enqueue_track(host_path, parameter, name_copy);
        frame.ret(name_copy);
    });
    registry.add("Audio", 16, "classic_audio_enable", 0, [](HleCallFrame& frame) {
        // Message 0x66000017 sets context +0x0d to one. The subsequent
        // audio-device call at 0x0816f3f4 is not modeled yet.
        frame.emulator.classic_audio().enqueue_stream_enable();
        frame.ret_void();
    });
    registry.add("Audio", 17, "classic_audio_control", 1, [](HleCallFrame& frame) {
        // The firmware posts one byte, then returns the queue/lock result;
        // the public return contract and worker timing are not established.
        frame.emulator.classic_audio().enqueue_control(frame.args.next_u32());
        frame.ret_void();
    });
    registry.add("Audio", 18, "classic_audio_select_track", 3, [](HleCallFrame& frame) {
        const u32 index = frame.args.next_u32();
        const u32 flags = frame.args.next_u32() & 0xff;
        const u32 position = frame.args.next_u32();
        // 0x0817ed28 dispatches the index, byte flags and position to the
        // device. Only ordinary selection from the start is modeled.
        // Non-default selection modes are temporarily treated as silent
        // selections while the graphics backend is being developed.
        (void)flags;
        (void)position;
        frame.emulator.classic_audio().enqueue_track_selection(index);
        frame.ret_void();
    });
    registry.add("Audio", 19, "classic_audio_stream_reset", 0, [](HleCallFrame& frame) {
        // Message 0x6600001a reaches 0x0817edcc, which clears +0x10/+0x14.
        // Its audio-device and display-service side effects remain unmodeled.
        frame.emulator.classic_audio().enqueue_stream_reset();
        frame.ret_void();
    });
    registry.add("Audio", 29, "classic_audio_stream_value", 0, [](HleCallFrame& frame) {
        // The server's +0x144 starts at -1; its fallback reads the device's
        // active stream info or returns zero when no stream exists. Stream
        // creation and subsequent server overrides are not modeled yet.
        frame.ret(frame.emulator.classic_audio().stream_value());
    });
    registry.add("Audio", 33, "classic_audio_device_state", 0, [](HleCallFrame& frame) {
        // Fallback at 0x0816fb9c reads the byte. Its later update paths are
        // not fully modeled, so this initially reflects constructor state.
        frame.ret(frame.emulator.classic_audio().device_state());
    });
    registry.add("Audio", 42, "classic_audio_mode_stub", 0,
                 [](HleCallFrame& frame) { frame.ret(0); });
    registry.add("Audio", 47, "classic_current_volume", 0, [](HleCallFrame& frame) {
        frame.ret(frame.emulator.classic_audio().current_volume());
    });
    registry.add("Audio", 48, "classic_volume_limit", 0, [](HleCallFrame& frame) {
        frame.ret(frame.emulator.classic_audio().volume_limit());
    });
    registry.add("SoundEffect", 0, "classic_create_sound_effect", 0,
                 [](HleCallFrame& frame) {
        // 0x082beaac creates an object; 0x083c97a4 assigns a table index.
        // Manager availability, allocation failures and full object state are
        // not yet modeled; only the successful creation path is represented.
        frame.ret(frame.emulator.classic_audio().create_effect());
    });
    registry.add("SoundEffect", 2, "classic_play_sound_effect", 1,
                 [](HleCallFrame& frame) {
        const u32 handle = frame.args.next_u32();
        auto& audio = frame.emulator.classic_audio();
        auto* effect = audio.effect(handle);
        if (!effect) { frame.ret(0); return; }
        // Temporary silent playback: preserve the known handle/state path,
        // without rejecting mixer formats or opening the host audio device.
        effect->playing = true;
        audio.set_effect_field(handle, 0x4c, 1, true);
        audio.set_effect_field(handle, 0x3d, 1, true);
        frame.ret(1);
    });
    registry.add("SoundEffect", 6, "classic_sound_effect_command", 1,
                 [](HleCallFrame& frame) {
        // The 7G wrapper resolves the handle and calls 0x081a6318, a BX LR.
        // Manager availability is provisionally assumed, as for creation.
        frame.ret(frame.emulator.classic_audio().effect(frame.args.next_u32()) ? 1 : 0);
    });
    registry.add("SoundEffect", 7, "classic_set_sound_buffer", 2,
                 [](HleCallFrame& frame) {
        // 0x082bf0b8 stores a non-null pointer in effect +0x04.
        const u32 handle = frame.args.next_u32();
        const u32 address = frame.args.next_u32();
        frame.ret(frame.emulator.classic_audio().set_effect_buffer(handle, address));
    });
    registry.add("SoundEffect", 8, "classic_set_sound_field_08", 2,
                 [](HleCallFrame& frame) {
        // 0x082bf0fc stores the supplied word in effect +0x08.
        const u32 handle = frame.args.next_u32();
        const u32 value = frame.args.next_u32();
        frame.ret(frame.emulator.classic_audio().set_effect_field_08(handle, value));
    });
    // 7G wrappers 0x082bf038..0x082bf450 each store one field after
    // validating the sound-object index. Field meanings remain under study.
    struct SoundField { u32 ordinal; u32 offset; bool byte; };
    constexpr SoundField sound_fields[] = {
        {9, 0x0c, true}, {10, 0x10, false}, {11, 0x14, false},
        {12, 0x18, false}, {13, 0x1c, false}, {14, 0x24, false},
        {15, 0x20, false}, {16, 0x38, false}, {18, 0x3e, true},
        {19, 0x2c, false}, {20, 0x28, false},
    };
    for (const auto field : sound_fields) {
        registry.add("SoundEffect", field.ordinal, "classic_set_sound_field", 2,
                     [field](HleCallFrame& frame) {
            const u32 handle = frame.args.next_u32();
            const u32 value = frame.args.next_u32();
            frame.ret(frame.emulator.classic_audio().set_effect_field(
                handle, field.offset, value, field.byte));
        });
    }
    registry.add("SoundEffect", 17, "classic_set_sound_state", 2,
                 [](HleCallFrame& frame) {
        const u32 handle = frame.args.next_u32();
        const u32 value = frame.args.next_u32();
        auto& audio = frame.emulator.classic_audio();
        const auto link = audio.effect_field(handle, 0x40, false);
        if (!link) { frame.ret(0); return; }
        // 0x082bf1bc traverses +0x40 and may update the player at +0x34.
        // Linked objects and players are not yet represented.
        const auto player = audio.effect_field(handle, 0x34, false);
        if (*link || ((value & 0xffu) == 3 && player.value_or(0))) {
            frame.cpu.request_stop();
            return;
        }
        if ((value & 0xffu) == 3) {
            auto* effect = audio.effect(handle);
            if (effect && effect->playing) {
                if (auto* device = frame.emulator.audio()) device->stop_voice(handle);
                effect->playing = false;
            }
        }
        frame.ret(audio.set_effect_field(handle, 0x3d, value, true));
    });
    registry.add("SoundEffect", 22, "classic_set_sound_fields_44_48", 3,
                 [](HleCallFrame& frame) {
        // 0x082bf234 updates both words for a valid object index.
        const u32 handle = frame.args.next_u32();
        const u32 first = frame.args.next_u32();
        const u32 second = frame.args.next_u32();
        auto& audio = frame.emulator.classic_audio();
        if (!audio.set_effect_field(handle, 0x44, first, false)) {
            frame.ret(0);
            return;
        }
        frame.ret(audio.set_effect_field(handle, 0x48, second, false));
    });
    constexpr SoundField sound_reads[] = {
        {23, 0x04, false}, {24, 0x08, false}, {25, 0x0c, true},
        {26, 0x10, false}, {27, 0x14, false}, {28, 0x18, false},
        {29, 0x1c, false}, {30, 0x20, false}, {31, 0x38, false},
        {32, 0x24, false}, {35, 0x3d, true}, {36, 0x3e, true},
        {37, 0x2c, false}, {38, 0x28, false},
    };
    for (const auto field : sound_reads) {
        registry.add("SoundEffect", field.ordinal, "classic_get_sound_field", 1,
                     [field](HleCallFrame& frame) {
            const u32 handle = frame.args.next_u32();
            const auto value = frame.emulator.classic_audio().effect_field(
                handle, field.offset, field.byte);
            if (field.ordinal == 35 || field.ordinal == 36) {
                frame.ret(value ? static_cast<u32>(static_cast<i32>(
                    static_cast<i8>(*value))) : 0xffffffffu);
            } else {
                frame.ret(value.value_or(0));
            }
        });
    }
    registry.add("SoundEffect", 33, "classic_sound_constant_1000", 0,
                 [](HleCallFrame& frame) { frame.ret(1000); });
    registry.add("SoundEffect", 34, "classic_sound_constant_zero", 0,
                 [](HleCallFrame& frame) { frame.ret(0); });
    // Temporary silent fallbacks for the remaining 7G audio exports. These
    // are not firmware implementations and must not count as parity.
    for (u32 ordinal = 0; ordinal < 54; ++ordinal) {
        if (!registry.find("Audio", ordinal))
            registry.add("Audio", ordinal, "classic_audio_stub", 0,
                         [](HleCallFrame& frame) { frame.ret(0); });
    }
    for (u32 ordinal = 0; ordinal < 42; ++ordinal) {
        if (!registry.find("SoundEffect", ordinal))
            registry.add("SoundEffect", ordinal, "classic_sound_effect_stub", 0,
                         [](HleCallFrame& frame) { frame.ret(0); });
    }
    registry.add("miscTBD", 12, "classic_timer", 1, [](HleCallFrame& frame) {
        const GuestAddr out = frame.args.next_ptr();
        // 7G OSOS reads TECNT and stores/returns it. Rockbox names the same
        // counter USEC_TIMER; its exact startup phase is not yet verified.
        const u32 value = static_cast<u32>(frame.emulator.system_clock_microsecond_count());
        if (!frame.store<u32>(out, value)) {
            frame.cpu.request_stop();
            return;
        }
        frame.ret(value);
    });
    registry.add("miscTBD", 2, "classic_display_level", 0, [](HleCallFrame& frame) {
        // The reset routine at 0x080b313c initializes the device byte to 50.
        // Restoring the real persisted preferences is not implemented yet.
        frame.ret(frame.emulator.device_settings().display_level.value_or(50));
    });
    registry.add("miscTBD", 3, "classic_backlight_dim", 1, [](HleCallFrame& frame) {
        const u32 requested = frame.args.next_u32();
        auto& settings = frame.emulator.device_settings();
        const u32 level = settings.display_level.value_or(50);
        // 0x081b0f68 applies the normal level when clearing the request, or
        // caps it at 20 when setting it. At 20 or below no update is sent.
        if (!requested || level > 20)
            settings.applied_backlight_level = requested ? 20 : level;
        settings.backlight_dim_state = static_cast<u8>(requested);
        // The panel backlight/PWM output is not yet represented.
        frame.ret_void();
    });
    registry.add("miscTBD", 6, "classic_battery_level", 0, [](HleCallFrame& frame) {
        // The 7G service scales its raw charge value to 0..22; values above
        // 22 are negated by 0x082bc41c. Host charge is only a proxy and does
        // not reproduce the firmware's charging state or smoothing.
        const u32 raw = host_battery_percent() * 256u / 100u;
        frame.ret((raw * 22u) >> 8);
    });
    registry.add("miscTBD", 7, "classic_power_status", 0, [](HleCallFrame& frame) {
        // 0x080a9fdc combines two power-service inputs, a signed mode and
        // an inhibit flag. Defaults represent disconnected inputs; mapping
        // these to the real PMU and persisted state remains incomplete.
        const auto& power = frame.emulator.device_settings();
        frame.ret(!power.power_status_inhibited &&
                  (power.power_input_a || (power.power_input_b && power.power_mode > 4)));
    });
    registry.add("miscTBD", 5, "classic_local_time", 1, [](HleCallFrame& frame) {
        const GuestAddr output = frame.args.next_ptr();
        std::time_t stamp = std::time(nullptr);
        if (const char* pin = std::getenv("CLICKHLE_EPOCH")) {
            if (std::strcmp(pin, "now") != 0) {
                const long long epoch = std::strtoll(pin, nullptr, 10);
                if (epoch > 0) stamp = static_cast<std::time_t>(static_cast<u64>(epoch) +
                    frame.emulator.system_clock_microsecond_count() / 1000000u);
            }
        }
        std::tm local{};
#if defined(_WIN32)
        if (localtime_s(&local, &stamp) != 0) { frame.ret(0); return; }
#else
        if (!localtime_r(&stamp, &local)) { frame.ret(0); return; }
#endif
        // 0x082bc4b0 expands the 7G RTC fields into six 32-bit words.
        const u32 fields[] = {static_cast<u32>(local.tm_sec), static_cast<u32>(local.tm_min),
                              static_cast<u32>(local.tm_hour), static_cast<u32>(local.tm_mday),
                              static_cast<u32>(local.tm_mon + 1), static_cast<u32>(local.tm_year + 1900)};
        for (u32 i = 0; i < 6; ++i) {
            if (!frame.store<u32>(output + i * 4, fields[i])) {
                frame.cpu.request_stop(); return;
            }
        }
        frame.ret(1);
    });
}

} // namespace clickhle

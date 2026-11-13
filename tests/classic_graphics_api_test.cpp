#include "clickhle/core/emulator.h"
#include "clickhle/hle/call_frame.h"
#include "clickhle/hle/classic_memory.h"
#include "clickhle/hle/modules.h"
#include "clickhle/hle/registry.h"
#include "clickhle/hle/vfs.h"

namespace clickhle {
void register_classic_graphics(HleRegistry& registry);
}

int main() {
    using namespace clickhle;
    AddressSpace memory;
    if (!memory.map(0x1000, 0x1000, AddressSpace::kRW, "test")) return 1;
    auto cpu = make_cpu_core("", memory);
    if (!cpu) return 1;
    Emulator emulator;
    HleRegistry registry;
    register_classic_graphics(registry);
    if (!registry.find("OpenGLES", 38) || !registry.find("OpenGLES", 39)) return 1;
    classic::GraphicsContext surface_context;
    surface_context.set_surface_handles(0x1234, 0x5678);
    if (surface_context.surface_handle(0x3059) != 0x1234 ||
        surface_context.surface_handle(0x305a) != 0x5678 ||
        surface_context.surface_handle(0x3058) != 0) return 1;
    auto call = [&](u32 ordinal) {
        const auto* function = registry.find("OpenGLES", ordinal);
        if (!function) return false;
        HleCallFrame frame(*cpu, memory, emulator);
        function->handler(frame);
        return true;
    };

    cpu->set_reg(reg::kR0, 0x3059);
    if (!call(160) || cpu->reg(reg::kR0) != 0) return 1;
    cpu->set_reg(reg::kR0, 0x12345678);
    if (!call(140) || cpu->reg(reg::kR0) != 0x12345678) return 1;
    if (!call(163) || cpu->reg(reg::kR0) != 1) return 1;
    cpu->set_reg(reg::kR1, 0x1000);
    cpu->set_reg(reg::kR2, 0x1000);
    if (!call(141) || cpu->reg(reg::kR0) != 1 || memory.read<u32>(0x1000) != 0) return 1;
    cpu->set_reg(reg::kR1, 0x1000);
    cpu->set_reg(reg::kR2, 0x1004);
    if (!call(141) || memory.read<u32>(0x1000) != 1 || memory.read<u32>(0x1004) != 0) return 1;
    cpu->set_reg(reg::kR1, 0);
    cpu->set_reg(reg::kR2, 0);
    if (!call(141) || cpu->reg(reg::kR0) != 1) return 1;

    HleRegistry platform_registry;
    register_classic_modules(platform_registry);
    emulator.vfs().mount("/iPod_Control/Games_RO/88908");
    const auto* resource_path = platform_registry.find("miscTBD", 8);
    const char expected_path[] = "iPod_Control/games_RO/88908/a0.m4a";
    if (!resource_path || !memory.write_bytes(0x1100, "/a0.m4a", 8)) return 1;
    auto get_path = [&](u32 capacity, u32 destination) {
        if (!memory.write<u32>(0x1200, capacity)) return false;
        cpu->set_reg(reg::kR0, 0);
        cpu->set_reg(reg::kR1, destination);
        cpu->set_reg(reg::kR2, 0x1200);
        cpu->set_reg(reg::kR3, 0x1100);
        HleCallFrame frame(*cpu, memory, emulator);
        resource_path->handler(frame);
        return true;
    };
    constexpr u32 path_length = sizeof(expected_path) - 1;
    if (!get_path(path_length - 1, 0x1300) || cpu->reg(reg::kR0) != 0xffffffffu ||
        memory.read<u32>(0x1200) != path_length - 1) return 1;
    if (!get_path(path_length + 4, 0x1300) || cpu->reg(reg::kR0) != path_length ||
        memory.read<u32>(0x1200) != path_length) return 1;
    char actual_path[sizeof(expected_path)]{};
    if (!memory.read_bytes(0x1300, actual_path, sizeof(actual_path)) ||
        std::string(actual_path) != expected_path) return 1;
    if (!memory.write_bytes(0x1300, "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", 51) ||
        !get_path(path_length, 0x1300) || cpu->reg(reg::kR0) != 50 ||
        memory.read<u32>(0x1200) != 50) return 1;
    if (!get_path(path_length, 0) || cpu->reg(reg::kR0) != 0xffffffffu ||
        memory.read<u32>(0x1200) != 0xffffffffu) return 1;
    if (!memory.write<u32>(0x1200, 100)) return 1;
    cpu->set_reg(reg::kR0, 3);
    cpu->set_reg(reg::kR1, 0x1300);
    cpu->set_reg(reg::kR2, 0x1200);
    cpu->set_reg(reg::kR3, 0x1100);
    HleCallFrame shared_path_frame(*cpu, memory, emulator);
    resource_path->handler(shared_path_frame);
    const char shared_path[] = "iPod_Control/gamedata_ShareRW/a0.m4a";
    char shared_output[sizeof(shared_path)]{};
    if (cpu->reg(reg::kR0) != sizeof(shared_path) - 1 ||
        !memory.read_bytes(0x1300, shared_output, sizeof(shared_output)) ||
        std::string(shared_output) != shared_path) return 1;
    const auto* display_level = platform_registry.find("miscTBD", 2);
    if (!display_level) return 1;
    HleCallFrame default_level_frame(*cpu, memory, emulator);
    display_level->handler(default_level_frame);
    if (cpu->reg(reg::kR0) != 50) return 1;
    emulator.device_settings().display_level = 73;
    HleCallFrame custom_level_frame(*cpu, memory, emulator);
    display_level->handler(custom_level_frame);
    if (cpu->reg(reg::kR0) != 73) return 1;
    const auto* dim = platform_registry.find("miscTBD", 3);
    if (!dim) return 1;
    cpu->set_reg(reg::kR0, 1);
    HleCallFrame dim_frame(*cpu, memory, emulator);
    dim->handler(dim_frame);
    if (emulator.device_settings().backlight_dim_state != 1 ||
        emulator.device_settings().applied_backlight_level != 20) return 1;
    cpu->set_reg(reg::kR0, 0);
    HleCallFrame undim_frame(*cpu, memory, emulator);
    dim->handler(undim_frame);
    if (emulator.device_settings().backlight_dim_state != 0 ||
        emulator.device_settings().applied_backlight_level != 73) return 1;
    const auto* battery = platform_registry.find("miscTBD", 6);
    if (!battery) return 1;
    HleCallFrame battery_frame(*cpu, memory, emulator);
    battery->handler(battery_frame);
    if (cpu->reg(reg::kR0) > 22) return 1;
    const auto* power_status = platform_registry.find("miscTBD", 7);
    if (!power_status) return 1;
    auto& power = emulator.device_settings();
    power.power_input_b = true;
    power.power_mode = 5;
    HleCallFrame power_frame(*cpu, memory, emulator);
    power_status->handler(power_frame);
    if (cpu->reg(reg::kR0) != 1) return 1;
    power.power_status_inhibited = true;
    HleCallFrame inhibited_power_frame(*cpu, memory, emulator);
    power_status->handler(inhibited_power_frame);
    if (cpu->reg(reg::kR0) != 0) return 1;
    const auto* local_time = platform_registry.find("miscTBD", 5);
    if (!local_time) return 1;
    cpu->set_reg(reg::kR0, 0x1080);
    HleCallFrame time_frame(*cpu, memory, emulator);
    local_time->handler(time_frame);
    if (cpu->reg(reg::kR0) != 1 || memory.read<u32>(0x1080).value_or(61) > 60 ||
        memory.read<u32>(0x1088).value_or(24) > 23 ||
        memory.read<u32>(0x1090).value_or(0) < 1 ||
        memory.read<u32>(0x1090).value_or(13) > 12) return 1;
    const auto* device_state = platform_registry.find("Audio", 33);
    const auto* stream_position = platform_registry.find("Audio", 29);
    if (!stream_position) return 1;
    HleCallFrame stream_position_frame(*cpu, memory, emulator);
    stream_position->handler(stream_position_frame);
    if (cpu->reg(reg::kR0) != 0) return 1;
    if (!device_state) return 1;
    emulator.classic_audio().set_device_state(2);
    HleCallFrame device_state_frame(*cpu, memory, emulator);
    device_state->handler(device_state_frame);
    if (cpu->reg(reg::kR0) != 2) return 1;
    const auto* settings = platform_registry.find("Settings", 0);
    if (!settings || !memory.write_bytes(0x1040, "Language", 9) ||
        !memory.write<u32>(0x1050, 4)) return 1;
    emulator.device_settings().language = 3;
    cpu->set_reg(reg::kR0, 0x1040);
    cpu->set_reg(reg::kR1, 0x1060);
    cpu->set_reg(reg::kR2, 0x1050);
    HleCallFrame settings_frame(*cpu, memory, emulator);
    settings->handler(settings_frame);
    if (cpu->reg(reg::kR0) != 0 || memory.read<u32>(0x1060) != 3 ||
        memory.read<u32>(0x1050) != 4) return 1;
    if (!memory.write<u32>(0x1050, 3)) return 1;
    cpu->set_reg(reg::kR0, 0x1040);
    HleCallFrame short_settings_frame(*cpu, memory, emulator);
    settings->handler(short_settings_frame);
    if (cpu->reg(reg::kR0) != 0xffffffd0u || memory.read<u32>(0x1050) != 3) return 1;
    if (!memory.write_bytes(0x1040, "TimeFormat", 11) || !memory.write<u32>(0x1050, 3)) return 1;
    emulator.device_settings().time_format_24 = true;
    cpu->set_reg(reg::kR0, 0x1040);
    cpu->set_reg(reg::kR1, 0x1060);
    cpu->set_reg(reg::kR2, 0x1050);
    HleCallFrame time_format_frame(*cpu, memory, emulator);
    settings->handler(time_format_frame);
    if (cpu->reg(reg::kR0) != 0 || memory.read<u32>(0x1050) != 3 ||
        memory.read<u8>(0x1060) != '2' || memory.read<u8>(0x1061) != '4' ||
        memory.read<u8>(0x1062) != 0) return 1;
    if (!memory.write_bytes(0x1040, "HoldSwitch", 11) || !memory.write<u32>(0x1050, 1)) return 1;
    emulator.device_settings().hold_switch = true;
    cpu->set_reg(reg::kR0, 0x1040);
    cpu->set_reg(reg::kR1, 0x1060);
    cpu->set_reg(reg::kR2, 0x1050);
    HleCallFrame hold_frame(*cpu, memory, emulator);
    settings->handler(hold_frame);
    if (cpu->reg(reg::kR0) != 0 || memory.read<u32>(0x1050) != 1 ||
        memory.read<u8>(0x1060) != 1) return 1;
    const auto* audio = platform_registry.find("Audio", 17);
    if (!audio) return 1;
    cpu->set_reg(reg::kR0, 0x12345678);
    HleCallFrame audio_frame(*cpu, memory, emulator);
    audio->handler(audio_frame);
    if (emulator.classic_audio().pending_count() != 1 ||
        emulator.classic_audio().control() != 0) return 1;
    emulator.classic_audio().service();
    if (emulator.classic_audio().pending_count() != 0 ||
        emulator.classic_audio().control() != 0x78) return 1;
    const auto* play_sound = platform_registry.find("SoundEffect", 2);
    const auto* set_sound_state = platform_registry.find("SoundEffect", 17);
    const auto* sound_command = platform_registry.find("SoundEffect", 6);
    if (!play_sound || !set_sound_state || !sound_command) return 1;
    cpu->set_reg(reg::kR0, 99);
    HleCallFrame invalid_sound_frame(*cpu, memory, emulator);
    play_sound->handler(invalid_sound_frame);
    if (cpu->reg(reg::kR0) != 0) return 1;
    auto& sounds = emulator.classic_audio();
    const u32 sound = sounds.create_effect();
    cpu->set_reg(reg::kR0, sound);
    HleCallFrame sound_command_frame(*cpu, memory, emulator);
    sound_command->handler(sound_command_frame);
    if (cpu->reg(reg::kR0) != 1 || sounds.effect(sound)->playing) return 1;
    if (!sounds.set_effect_buffer(sound, 0x1400) ||
        !sounds.set_effect_field_08(sound, 4) || !memory.write<u32>(0x1400, 0)) return 1;
    cpu->set_reg(reg::kR0, sound);
    HleCallFrame play_sound_frame(*cpu, memory, emulator);
    play_sound->handler(play_sound_frame);
    if (cpu->reg(reg::kR0) != 1 || !sounds.effect(sound)->playing ||
        sounds.effect_field(sound, 0x3d, true) != 1) return 1;
    cpu->set_reg(reg::kR0, sound);
    cpu->set_reg(reg::kR1, 3);
    HleCallFrame stop_sound_frame(*cpu, memory, emulator);
    set_sound_state->handler(stop_sound_frame);
    if (cpu->reg(reg::kR0) != 1 || sounds.effect(sound)->playing ||
        sounds.effect_field(sound, 0x3d, true) != 3) return 1;

    if (!memory.map(0x40000000, 0x30000, AddressSpace::kRW, "classic heap")) return 1;
    classic::RegionHeap heap(memory, 0x40000000);
    classic::ArenaLayout layout{{}, 0, {{0x40010000, 0x10000}}};
    classic::Arena arena(heap, layout, 0x10000);
    arena.initialize();
    HleRegistry memory_registry;
    register_classic_memory(memory_registry, arena, 1);
    const auto* alloc = memory_registry.find("MemoryAlloc", 0);
    const auto* release = memory_registry.find("MemoryAlloc", 1);
    if (!alloc || !release || memory_registry.find("MemoryAlloc", 2)) return 1;
    cpu->set_reg(reg::kR0, 128);
    HleCallFrame alloc_frame(*cpu, memory, emulator);
    alloc->handler(alloc_frame);
    const auto pointer = cpu->reg(reg::kR0);
    if (pointer != 0x4001ff78) return 1;
    HleCallFrame free_frame(*cpu, memory, emulator);
    release->handler(free_frame);
    if (memory.read<u32>(0x40000000) != 0xfff0) return 1;
}

#include "clickhle/core/emulator.h"
#include "clickhle/hle/call_frame.h"
#include "clickhle/hle/modules.h"

#undef NDEBUG
#include <cassert>
#include <array>
#include <chrono>
#include <thread>

int main() {
    clickhle::Emulator emulator;
    auto& memory = emulator.memory();
    assert(memory.map(0x1000, 0x2000, clickhle::AddressSpace::kRW, "audio-test"));

    // A short stereo S16 clip. The playback call must read this guest buffer,
    // without needing any host WAV file or installer asset.
    std::array<clickhle::u8, 4096> pcm{};
    for (std::size_t i = 1; i < pcm.size(); i += 2) pcm[i] = 0x40;
    assert(memory.write_bytes(0x1000, pcm.data(),
                              static_cast<clickhle::u32>(pcm.size())));
    auto& sound = emulator.audio_state().sounds[0];
    sound.guest_object = 0x2000;
    sound.buffer = 0x1000;
    sound.fields[8] = static_cast<clickhle::u32>(pcm.size());
    sound.fields[10] = 22050;
    sound.fields[11] = 2;
    sound.fields[12] = 16;

    auto cpu = clickhle::make_cpu_core("unicorn", memory);
    assert(cpu);
    cpu->reset();
    cpu->set_reg(0, 0);
    clickhle::HleRegistry registry;
    clickhle::register_audio(registry);
    const auto* play = registry.find("Audio", 2);
    assert(play);
    clickhle::HleCallFrame call(*cpu, memory, emulator);
    play->handler(call);
    assert(sound.started && sound.state == 1);
    assert(emulator.audio() && emulator.audio()->is_open());
    assert(memory.read<clickhle::u8>(0x203d).value_or(0) == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // An invalid guest buffer cannot be replaced by a similarly named host
    // asset; playback stays stopped until the guest supplies readable PCM.
    sound.started = false;
    sound.buffer = 0x4000;
    call.args.reset();
    play->handler(call);
    assert(!sound.started);
}

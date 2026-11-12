#include "clickhle/core/emulator.h"
#include "clickhle/hle/call_frame.h"
#include "graphics_api.h"
#include "g5/api.h"
#include <array>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace clickhle;
int main() {
    Emulator emulator;
    auto &memory = emulator.memory();
    if (!memory.map(0x1000, 0x1000, AddressSpace::kRW, "g5-test")) return 1;
    auto cpu = make_cpu_core("unicorn", memory);
    if (!cpu) return 1;
    cpu->reset();
    GraphicsApi reference([](std::span<const u8>) {
        throw std::runtime_error("Pure matrix call submitted VC2 packets");
    });
    std::mt19937 random(731);
    std::array<u8, 4096> original{}, expected{}, actual{};
    for (u32 ordinal : {165u,166u,169u,170u,171u,172u,175u,176u}) {
        const bool fixed = ordinal == 166 || ordinal == 170 || ordinal == 172 || ordinal == 176;
        for (unsigned trial = 0; trial < 512; ++trial) {
            for (unsigned i = 0; i < original.size(); i += 4) {
                const u32 value = fixed ? static_cast<u32>(random()) :
                    endian::float_to_bits(static_cast<float>(static_cast<int>(random() % 20001) - 10000) / 64);
                endian::store_le<u32>(reinterpret_cast<std::byte *>(original.data() + i), value);
            }
            // Includes exact and partial overlap of destination and sources.
            cpu->set_reg(0, 0x1100 + (trial % 4) * 16);
            cpu->set_reg(1, ordinal >= 175 ? 0x1100 : static_cast<u32>(random()));
            cpu->set_reg(2, ordinal >= 175 ? 0x1120 : static_cast<u32>(random()));
            cpu->set_reg(3, static_cast<u32>(random()));
            cpu->set_reg(13, 0x1f00);
            if (!fixed && ordinal < 175)
                for (unsigned r = 1; r < 4; ++r)
                    cpu->set_reg(r, endian::float_to_bits(static_cast<float>(static_cast<int>(random()%201)-100)/8));
            if (!memory.write_bytes(0x1000, original.data(), original.size())) return 1;
            HleCallFrame before(*cpu, memory, emulator);
            const u32 result = reference.call(ordinal, before);
            if (!memory.read_bytes(0x1000, expected.data(), expected.size())) return 1;
            if (!memory.write_bytes(0x1000, original.data(), original.size())) return 1;
            HleCallFrame after(*cpu, memory, emulator);
            const auto replacement = g5_graphics_call(ordinal, after);
            if (!memory.read_bytes(0x1000, actual.data(), actual.size())) return 1;
            if (!replacement || *replacement != result || expected != actual || !reference.pending_packets().empty()) {
                std::cerr << "G5 differs at ordinal " << ordinal << ", case " << trial << '\n';
                return 1;
            }
        }
    }
    HleCallFrame unsupported(*cpu, memory, emulator);
    if (g5_graphics_call(37, unsupported)) return 1;
    std::cout << "4096 matrix cases match return values and complete mapped memory\n";
}

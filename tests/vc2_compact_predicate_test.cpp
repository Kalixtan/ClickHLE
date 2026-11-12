#include "core.h"
#include "clickhle/video/videocore.h"

#include <array>
#include <iostream>

int main() {
    constexpr std::array<clickhle::u16, 4> destinations{
        0xa0e0, 0xa4e0, 0xa8e0, 0xace0};

    for (unsigned group = 0; group < destinations.size(); ++group) {
        std::array<clickhle::u8, 4096> interpreted{};
        for (bool jit : {false, true}) {
            if (jit && !clickhle::videocore_jit_available()) break;
            clickhle::vc2::Core variant;
            variant.set_jit_enabled(jit);
            variant.vrf.fill(0x5a);
            variant.r[0] = 0x748f0; // A shader data address, not a repeat count.
            variant.write(0x200, 0x2226, 2);
            variant.write(0x202, destinations[group], 2);
            variant.write(0x204, 0x05c0, 2);
            variant.write(0x206, 0x4330, 2); // Sleep after one instruction.
            for (unsigned lane = 0; lane < 16; ++lane)
                variant.vc[lane] = lane % 2 != 0;
            variant.r[31] = 0x200;
            const bool ran = variant.run(100);
            if (!ran || variant.r[31] != 0x206) {
                std::cerr << "group " << group << " jit " << jit << ": "
                          << variant.error << ", pc 0x" << std::hex << variant.r[31]
                          << std::dec << ", jit instructions " << variant.jit_instructions << '\n';
                return 1;
            }
            const unsigned column = 32 + 4 * group;
            for (unsigned lane = 0; lane < 16; ++lane) {
                const auto expected = lane % 2 ? 0x5a : 0;
                const auto row = (32 + lane) * 64;
                if (variant.vrf[row + column] != expected ||
                    variant.vrf[row + column + 16] != expected) {
                    std::cerr << "group " << group << " jit " << jit
                              << " lane " << lane << " differs\n";
                    return 1;
                }
            }
            if (jit && variant.vrf != interpreted) {
                std::cerr << "group " << group << " JIT differs from interpreter\n";
                return 1;
            }
            if (!jit) interpreted = variant.vrf;
        }
    }
    return 0;
}

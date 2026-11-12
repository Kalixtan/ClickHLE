#include "core.h"
#include "clickhle/video/videocore.h"

#include <algorithm>
#include <array>
#include <iostream>

namespace {
using namespace clickhle;
using vc2::Core;

void store_half(Core &core, unsigned row, unsigned column, u32 value) {
    core.vrf[row * 64 + column] = static_cast<u8>(value);
    core.vrf[row * 64 + column + 16] = static_cast<u8>(value >> 8);
}

u16 load_half(const Core &core, unsigned row, unsigned column) {
    return static_cast<u16>(core.vrf[row * 64 + column] |
                            (u16(core.vrf[row * 64 + column + 16]) << 8));
}

bool check(bool jit) {
    Core core;
    core.set_jit_enabled(jit);
    // Advance a planar 16.16 interpolant, then clamp to unsigned 0.16.
    // The firmware uses this chain for colour and alpha, including negative
    // reflection alpha. Expected results below use independent 32-bit math.
    constexpr std::array<u16, 22> code{
        0x2042, 0x01e0, 0x03a0, 0x792e, 0x4007, // Low ADD/SETF.
        0x2112, 0x00e0, 0x0320, 0x790e, 0x4007, // High ADC/SETF.
        0x2222, 0x21e0, 0x0004,                 // Copy low half.
        0x2022, 0x21e0, 0x07ff, 0xfc0e, 0x1807, // IFNZ: 65535.
        0x2222, 0x21e0, 0x0500,                 // IFN: zero.
        0x4330};
    for (unsigned i = 0; i < code.size(); ++i)
        core.write(0x200 + 2 * i, code[i], 2);

    for (u32 delta : {0u, 1u, 0xffffu, 0xfffffb83u, 0xffffffffu}) {
        for (unsigned high = 0; high < 65536; high += 16) {
            core.r.fill(0);
            core.r[31] = 0x200;
            core.vrf.fill(0xa5);
            core.sleeping = false;
            std::array<u64, 16> sums{};
            for (unsigned lane = 0; lane < 16; ++lane) {
                const u32 input = ((high + lane) << 16) | (lane * 4369);
                sums[lane] = u64(input) + delta;
                store_half(core, 1, lane, input);
                store_half(core, 0, lane, input >> 16);
                store_half(core, 56, 32 + lane, delta);
                store_half(core, 56, lane, delta >> 16);
            }
            if (!core.run(100)) {
                std::cerr << core.error << '\n';
                return false;
            }
            for (unsigned lane = 0; lane < 16; ++lane) {
                const u32 sum = static_cast<u32>(sums[lane]);
                const auto expected = std::clamp<std::int64_t>(
                    vc2::sign_extend(sum, 32), 0, 65535);
                if (load_half(core, 1, 32 + lane) != expected ||
                    load_half(core, 0, lane) != (sum >> 16) ||
                    load_half(core, 1, lane) != (sum & 65535) ||
                    core.vn[lane] != bool(sum & 0x80000000u) ||
                    core.vz[lane] != ((sum >> 16) == 0) ||
                    core.vc[lane] != (sums[lane] > 0xffffffffu)) {
                    std::cerr << "Alpha clamp mismatch: JIT=" << jit
                              << " sum=0x" << std::hex << sum << '\n';
                    return false;
                }
            }
        }
    }
    if (jit && !core.jit_instructions) {
        std::cerr << "JIT test did not execute native instructions\n";
        return false;
    }
    return true;
}
} // namespace

int main() {
    if (!check(false)) return 1;
    if (clickhle::videocore_jit_available() && !check(true)) return 1;
    return 0;
}

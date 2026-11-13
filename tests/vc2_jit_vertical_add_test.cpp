#include "core.h"
#include "jit.h"

#include <array>
#include <iostream>
#include <random>

using namespace clickhle;

int main() {
    // Forms found in the RenderServer shader programs. The carry rule remains
    // firmware-derived and has no retained silicon confirmation.
    constexpr std::array<std::array<u16, 5>, 8> instructions{{
        {0x2046, 0x20f1, 0x07ff, 0x7d0e, 0x4007},
        {0x2046, 0x21f1, 0x0fff, 0x7d0e, 0x4007},
        {0x2046, 0x24f1, 0x27ff, 0x7d0e, 0x4007},
        {0x2046, 0x25f1, 0x2fff, 0x7d0e, 0x4007},
        {0x2046, 0x28f1, 0x47ff, 0x7d0e, 0x4007},
        {0x2046, 0x29f1, 0x4fff, 0x7d0e, 0x4007},
        {0x2046, 0x2cf1, 0x67ff, 0x7d0e, 0x4007},
        {0x2046, 0x2df1, 0x6fff, 0x7d0e, 0x4007},
    }};
    vc2::Core native, reference;
    vc2::Jit jit;
    std::mt19937 random(9321);
    unsigned checked = 0;
    u32 pc = 0x10000;
    for (const auto &sample : instructions) {
        for (u16 high : {u16{0}, u16{1}, u16{31}, u16{32}, u16{63}}) {
            for (u16 low : {u16{0}, u16{1}, u16{511}, u16{512}, u16{1023}}) {
                auto code = sample;
                code[2] = (code[2] & 0xfc00) | low;
                code[3] = (code[3] & 0x03ff) | (high << 10);
                for (auto *core : {&native, &reference})
                    for (unsigned i = 0; i < code.size(); ++i)
                        core->write(pc + 2 * i, code[i], 2);
                for (unsigned i = 0; i < 63; ++i) {
                    native.r[31] = pc;
                    if (jit.try_execute(native, 32, code[0])) return 1;
                }
                for (unsigned trial = 0; trial < 16; ++trial) {
                    for (unsigned i = 0; i < native.vrf.size(); ++i) {
                        const u8 value = trial % 5 == 0 ? 0 : trial % 5 == 1 ? 255 :
                            static_cast<u8>(random());
                        native.vrf[i] = reference.vrf[i] = value;
                    }
                    for (unsigned i = 0; i < 32; ++i)
                        native.r[i] = reference.r[i] = static_cast<u32>(random());
                    for (unsigned i = 0; i < 16; ++i) {
                        native.acc[i] = reference.acc[i] = static_cast<i32>(random());
                        native.acclo[i] = reference.acclo[i] = static_cast<u32>(random());
                        native.vz[i] = reference.vz[i] = (random() & 1) != 0;
                        native.vn[i] = reference.vn[i] = (random() & 1) != 0;
                        native.vc[i] = reference.vc[i] = (random() & 1) != 0;
                    }
                    native.r[30] = reference.r[30] = (trial % 4) * 16;
                    native.r[31] = reference.r[31] = pc;
                    native.recent_pc_count = reference.recent_pc_count = 0;
                    native.recent_pcs.fill(0);
                    reference.recent_pcs.fill(0);
                    if (jit.try_execute(native, 32, code[0]) != 1) {
                        std::cerr << "Vertical add form did not compile\n";
                        return 1;
                    }
                    reference.step();
                    if (native.r != reference.r || native.vrf != reference.vrf ||
                        native.acc != reference.acc || native.acclo != reference.acclo ||
                        native.vz != reference.vz || native.vn != reference.vn ||
                        native.vc != reference.vc || native.recent_pcs != reference.recent_pcs ||
                        native.recent_pc_count != reference.recent_pc_count) {
                        std::cerr << "Vertical add mismatch at PC " << pc << " trial " << trial << '\n';
                        return 1;
                    }
                    ++checked;
                }
                pc += 16;
            }
        }
    }
    std::cout << checked << " vertical add executions matched the interpreter\n";
}

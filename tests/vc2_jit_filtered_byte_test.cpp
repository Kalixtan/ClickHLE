#include "core.h"
#include "jit.h"

#include <array>
#include <iostream>
#include <random>

using namespace clickhle;

int main() {
    // Includes all coordinate values found in the archived shader programs,
    // plus additional valid combinations to exercise operand overlap.
    constexpr std::array<u16, 24> destinations{
        0x00f5, 0x23f5, 0x27f5, 0x2bf5, 0x2ff5, 0x40f5,
        0x60f5, 0x61f5, 0x62f5, 0x63f5, 0x64f5, 0x65f5, 0x66f5,
        0x67f5, 0x68f5, 0x69f5, 0x6af5, 0x6bf5, 0x6cf5, 0x6df5,
        0x6ef5, 0x6ff5, 0xa0f5, 0xe0f5
    };
    constexpr std::array<u16, 18> sources{
        0x00e1, 0x010d, 0x01a1, 0x110d, 0x211d, 0x311d,
        0x412d, 0x512d, 0x613d, 0x713d, 0x810d, 0x910d,
        0xa11d, 0xb11d, 0xc12d, 0xd12d, 0xe13d, 0xf13d
    };
    vc2::Core native, reference;
    vc2::Jit jit;
    std::mt19937 random(9321);
    unsigned checked = 0;
    u32 pc = 0x10000;
    for (u16 destination : destinations) {
        for (u16 source : sources) {
            for (u16 modifier : {u16{0x8007}, u16{0x8407}}) {
                const std::array<u16, 5> code{0x219c, destination, source, 0x780e, modifier};
                for (auto *core : {&native, &reference})
                    for (unsigned i = 0; i < code.size(); ++i)
                        core->write(pc + 2 * i, code[i], 2);
                // Compile on the normal hotness threshold without executing
                // the interpreter for the warmup visits.
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
                        std::cerr << "Filtered byte form did not compile\n";
                        return 1;
                    }
                    reference.step();
                    if (native.r != reference.r || native.vrf != reference.vrf ||
                        native.acc != reference.acc || native.acclo != reference.acclo ||
                        native.vz != reference.vz || native.vn != reference.vn ||
                        native.vc != reference.vc || native.recent_pcs != reference.recent_pcs ||
                        native.recent_pc_count != reference.recent_pc_count) {
                        std::cerr << "Filtered byte mismatch at PC " << pc << " trial " << trial << '\n';
                        return 1;
                    }
                    ++checked;
                }
                pc += 16;
            }
        }
    }
    std::cout << checked << " filtered byte executions matched the interpreter\n";
}

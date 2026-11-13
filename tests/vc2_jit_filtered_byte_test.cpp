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
    // Four dependent filter operations appear consecutively in the uploaded
    // shaders. Compare the entire region, including overlapping columns.
    constexpr std::array<u16, 20> sequence{
        0x219c, 0x64f5, 0x110d, 0x780e, 0x8407,
        0x219c, 0x64f5, 0x311d, 0x780e, 0x8007,
        0x219c, 0x64f5, 0x512d, 0x780e, 0x8007,
        0x219c, 0x64f5, 0x713d, 0x780e, 0x8007
    };
    const u32 sequence_pc = 0x30ff0; // Cross a watched code-page boundary.
    for (auto *core : {&native, &reference})
        for (unsigned i = 0; i < sequence.size(); ++i)
            core->write(sequence_pc + 2 * i, sequence[i], 2);
    for (unsigned i = 0; i < 63; ++i) {
        native.r[31] = sequence_pc;
        if (jit.try_execute(native, 32, sequence[0])) return 1;
    }
    for (unsigned trial = 0; trial < 512; ++trial) {
        for (unsigned i = 0; i < native.vrf.size(); ++i) {
            const u8 value = trial % 4 == 0 ? 0 : trial % 4 == 1 ? 255 : static_cast<u8>(random());
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
        native.r[31] = reference.r[31] = sequence_pc;
        native.recent_pc_count = reference.recent_pc_count = 0;
        native.recent_pcs.fill(0);
        reference.recent_pcs.fill(0);
        if (jit.try_execute_cached(native, 3)) {
            std::cerr << "Filtered region exceeded instruction budget\n";
            return 1;
        }
        if (jit.try_execute(native, 32, sequence[0]) != 4) {
            std::cerr << "Filtered instructions did not form a region\n";
            return 1;
        }
        for (unsigned i = 0; i < 4; ++i) reference.step();
        if (native.r != reference.r || native.vrf != reference.vrf ||
            native.acc != reference.acc || native.acclo != reference.acclo ||
            native.vz != reference.vz || native.vn != reference.vn ||
            native.vc != reference.vc || native.recent_pcs != reference.recent_pcs ||
            native.recent_pc_count != reference.recent_pc_count) {
            std::cerr << "Filtered region mismatch at trial " << trial << '\n';
            return 1;
        }
    }
    // Vary destinations and region lengths, including destination/input
    // overlaps. Earlier stores must remain visible to later instructions.
    u32 varied_pc = 0x60000;
    for (u16 destination : {u16{0x00f5}, u16{0x23f5}, u16{0x40f5},
                            u16{0x64f5}, u16{0x6ff5}, u16{0xa0f5}, u16{0xe0f5}}) {
        for (unsigned length : {2u, 3u, 4u}) {
            auto program = sequence;
            for (unsigned i = 0; i < length; ++i) program[5 * i + 1] = destination;
            for (auto *core : {&native, &reference})
                for (unsigned i = 0; i < length * 5; ++i)
                    core->write(varied_pc + 2 * i, program[i], 2);
            for (unsigned i = 0; i < 63; ++i) {
                native.r[31] = varied_pc;
                if (jit.try_execute(native, 32, program[0])) return 1;
            }
            for (unsigned trial = 0; trial < 16; ++trial) {
                for (unsigned i = 0; i < native.vrf.size(); ++i)
                    native.vrf[i] = reference.vrf[i] = trial == 0 ? 255 : static_cast<u8>(random());
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
                native.r[31] = reference.r[31] = varied_pc;
                native.recent_pc_count = reference.recent_pc_count = 0;
                native.recent_pcs.fill(0);
                reference.recent_pcs.fill(0);
                if (jit.try_execute(native, 32, program[0]) != length) {
                    std::cerr << "Varied filtered region did not compile\n";
                    return 1;
                }
                for (unsigned i = 0; i < length; ++i) reference.step();
                if (native.r != reference.r || native.vrf != reference.vrf ||
                    native.acc != reference.acc || native.acclo != reference.acclo ||
                    native.vz != reference.vz || native.vn != reference.vn ||
                    native.vc != reference.vc || native.recent_pcs != reference.recent_pcs ||
                    native.recent_pc_count != reference.recent_pc_count) {
                    std::cerr << "Varied filtered region mismatch\n";
                    return 1;
                }
            }
            varied_pc += 64;
        }
    }
    // The same bytes at a different guest address must reuse the native
    // object, while each guest location keeps its own invalidation watch.
    const auto objects = jit.loaded_object_count();
    const u32 relocated_pc = 0x50000;
    for (unsigned i = 0; i < sequence.size(); ++i)
        native.write(relocated_pc + 2 * i, sequence[i], 2);
    for (unsigned i = 0; i < 63; ++i) {
        native.r[31] = relocated_pc;
        if (jit.try_execute(native, 32, sequence[0])) return 1;
    }
    native.r[31] = relocated_pc;
    if (jit.try_execute(native, 32, sequence[0]) != 4 ||
        jit.loaded_object_count() != objects) {
        std::cerr << "Relocated filtered region did not reuse its native object\n";
        return 1;
    }
    native.write(sequence_pc + 20, 0x219e, 2);
    native.r[31] = sequence_pc;
    if (jit.try_execute_cached(native, 32)) {
        std::cerr << "Changed filtered region was not invalidated\n";
        return 1;
    }
    native.r[31] = relocated_pc;
    if (jit.try_execute_cached(native, 32) != 4) {
        std::cerr << "Changing one region invalidated relocated code\n";
        return 1;
    }
    std::cout << checked << " filtered byte executions matched the interpreter\n";
}

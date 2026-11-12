#include "core.h"
#include <array>
#include <iostream>
#include <random>

using namespace clickhle;
using vc2::Core;

namespace {
// Disassembled memcpy: alignment dispatch, forward word/halfword/byte loops,
// then restore r6 and return. Both firmware images contain this whole routine.
constexpr std::array<u16, 33> code{
    0xf0a0, 0x2ea4, 0x3c01, 0xce43, 0x6223, 0x0103, 0xc00a,
    0xce06, 0x0b02, 0xc40a, 0x7813, 0x7963, 0x0ac2, 0xc4fe,
    0x3e04, 0x6004, 0x3009, 0xce06, 0x0d02, 0xc006, 0x7c15,
    0x7d65, 0x0cf2, 0xc0fe, 0xe0a0, 0xce06, 0x0b02, 0xc2f8,
    0x7e14, 0x7b64, 0x0ae2, 0xc2fe, 0x3e72};

bool check(unsigned test) {
    Core native;
    const u32 entry = test & 1 ? 0x2ff0 : 0x200;
    for (unsigned i = 0; i < code.size(); ++i) native.write(entry + 2 * i, code[i], 2);
    native.write(0x800, 0x4330, 2); // Sleep after return.
    std::mt19937 random(test);
    for (auto &r : native.r) r = static_cast<u32>(random());
    for (auto &v : native.vrf) v = static_cast<u8>(random());
    std::array<u8, 8192> bytes;
    for (auto &b : bytes) b = static_cast<u8>(random());
    native.copy_in(0x10000, bytes);
    native.r[1] = 0x10400 + (test % 4);
    native.r[0] = test % 3 == 0 ? native.r[1] + (test % 17) :
                  test % 3 == 1 ? native.r[1] - (test % 17) : 0x11000 + ((test / 4) % 4);
    constexpr std::array<u32, 12> lengths{0, 1, 2, 3, 4, 7, 31, 32, 65, 511, 0xffffffff, 0x80000000};
    native.r[2] = lengths[(test / 16) % lengths.size()];
    native.r[25] = 0x18000;
    native.r[26] = 0x800;
    native.r[30] &= 15; // Random arithmetic flags, no interrupts in isolated call.
    native.r[31] = entry;
    auto reference = native.fork_reference();
    u64 count = 0;
    while (native.r[31] != 0x800) {
        const unsigned n = native.run_replacement(1 + test % 32);
        if (!n || n > 1 + test % 32 || count > 10000) {
            std::cerr << "Replacement failed case " << test << " at " << std::hex << native.r[31] << '\n';
            return false;
        }
        for (unsigned i = 0; i < n; ++i) reference->step();
        if (native.r != reference->r || native.recent_pcs != reference->recent_pcs ||
            native.recent_pc_count != reference->recent_pc_count) {
            std::cerr << "Register/history mismatch case " << test << " after " << count + n << '\n';
            return false;
        }
        count += n;
    }
    const auto difference = native.state_difference(*reference);
    if (!difference.empty()) {
        std::cerr << "State mismatch case " << test << ": " << difference << '\n';
        return false;
    }
    // A modified signature must fall back before touching any state.
    native.r[31] = entry;
    native.write(entry + 64, 0x4230, 2);
    reference = native.fork_reference();
    if (native.run_replacement(32) || !native.state_difference(*reference).empty()) return false;
    return true;
}

bool check_scheduler() {
    Core native;
    for (unsigned i = 0; i < code.size(); ++i) native.write(0x200 + 2 * i, code[i], 2);
    native.write(0x800, 0x4330, 2);
    native.r[0] = 0x14000;
    native.r[1] = 0x10000;
    native.r[2] = 4096;
    native.r[25] = 0x18000;
    native.r[26] = 0x800;
    native.r[31] = 0x200;
    std::array<u8, 4096> bytes;
    for (unsigned i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<u8>(i);
    native.copy_in(0x10000, bytes);
    // Timer expiry falls inside the copy, exercising the normal service cadence.
    native.write(0x10001808, 2);
    auto reference = native.fork_reference();
    native.set_g5_enabled(true);
    if (!native.run(10000) || !reference->run(10000)) return false;
    const auto difference = native.state_difference(*reference);
    if (!difference.empty() || native.instructions != reference->instructions ||
        !native.replacement_instructions) {
        std::cerr << "Scheduled replacement mismatch: " << difference << '\n';
        return false;
    }
    return true;
}
}

int main() {
    for (unsigned i = 0; i < 384; ++i) if (!check(i)) return 1;
    if (!check_scheduler()) return 1;
    std::cout << "384 memcpy contracts match interpreter RAM, registers, flags and instruction history\n";
}

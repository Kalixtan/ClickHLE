#include "core.h"
#include "jit.h"

#include <iostream>

using namespace clickhle;

int main() {
    vc2::Core native, reference;
    vc2::Jit jit;
    for (auto *core : {&native, &reference}) {
        core->write(0x200, 0x6c12, 2);
        core->write(0x202, 0xffff, 2);
    }

    unsigned cached = 0;
    for (unsigned trial = 0; trial < 160; ++trial) {
        if (trial == 110) {
            native.write(0x200, 0x6c22, 2);
            reference.write(0x200, 0x6c22, 2);
        }
        for (unsigned reg = 0; reg < 32; ++reg)
            native.r[reg] = reference.r[reg] = trial * 31 + reg;
        native.r[31] = reference.r[31] = 0x200;
        native.recent_pc_count = reference.recent_pc_count = 0;
        native.recent_pcs.fill(0);
        reference.recent_pcs.fill(0);

        unsigned executed = jit.try_execute_cached(native, 32);
        if (trial == 110 && executed) {
            std::cerr << "Changed guest code ran from the old block\n";
            return 1;
        }
        if (executed) ++cached;
        else executed = jit.try_execute(native, 32, static_cast<u16>(native.read(0x200, 2)));
        if (!executed) continue;
        for (unsigned i = 0; i < executed; ++i) reference.step();
        if (native.r != reference.r || native.recent_pcs != reference.recent_pcs ||
            native.recent_pc_count != reference.recent_pc_count) {
            std::cerr << "Cached block differs from interpreter at trial " << trial << '\n';
            return 1;
        }
    }
    if (cached < 80) {
        std::cerr << "Cached path did not execute enough blocks\n";
        return 1;
    }
    std::cout << "Cached blocks match the interpreter and invalidate on code writes\n";
}

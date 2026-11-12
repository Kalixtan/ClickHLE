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

    vc2::Core relocated, relocated_reference;
    vc2::Jit shared_jit;
    for (auto *core : {&relocated, &relocated_reference}) {
        for (u32 pc : {0x200u, 0x2200u}) {
            core->write(pc, 0x6c12, 2);
            core->write(pc + 2, 0xffff, 2);
        }
    }
    std::size_t first_objects = 0;
    for (u32 pc : {0x200u, 0x2200u}) {
        unsigned compiled = 0;
        for (unsigned trial = 0; trial < 80; ++trial) {
            relocated.r[2] = relocated_reference.r[2] = trial;
            relocated.r[31] = relocated_reference.r[31] = pc;
            const unsigned count = shared_jit.try_execute(relocated, 32, 0x6c12);
            if (!count) continue;
            for (unsigned i = 0; i < count; ++i) relocated_reference.step();
            if (relocated.r != relocated_reference.r) {
                std::cerr << "Relocated block differs from interpreter\n";
                return 1;
            }
            ++compiled;
        }
        if (!compiled) {
            std::cerr << "Relocated block did not compile\n";
            return 1;
        }
        if (pc == 0x200) first_objects = shared_jit.loaded_object_count();
        else if (!first_objects || shared_jit.loaded_object_count() != first_objects) {
            std::cerr << "Identical relocated blocks did not share native code\n";
            return 1;
        }
    }
    std::size_t changed_objects = 0;
    for (u16 word : {u16{0x6c22}, u16{0x6c12}}) {
        relocated.write(0x200, word, 2);
        relocated_reference.write(0x200, word, 2);
        relocated.r[2] = relocated_reference.r[2] = 7;
        relocated.r[31] = relocated_reference.r[31] = 0x200;
        if (shared_jit.try_execute_cached(relocated, 32)) {
            std::cerr << "Changed relocated code ran from the old block\n";
            return 1;
        }
        const unsigned count = shared_jit.try_execute(relocated, 32, word);
        if (!count) return 1;
        for (unsigned i = 0; i < count; ++i) relocated_reference.step();
        if (relocated.r != relocated_reference.r) return 1;
        if (word == 0x6c22) changed_objects = shared_jit.loaded_object_count();
        else if (shared_jit.loaded_object_count() != changed_objects) {
            std::cerr << "Reuploaded code did not reuse its retained native object\n";
            return 1;
        }

        relocated.r[2] = relocated_reference.r[2] = 7;
        relocated.r[31] = relocated_reference.r[31] = 0x2200;
        const unsigned other_count = shared_jit.try_execute_cached(relocated, 32);
        if (!other_count) {
            std::cerr << "Invalidating one block disturbed another address\n";
            return 1;
        }
        for (unsigned i = 0; i < other_count; ++i) relocated_reference.step();
        if (relocated.r != relocated_reference.r) return 1;
    }
    vc2::Core relative, relative_reference;
    vc2::Jit relative_jit;
    for (auto *core : {&relative, &relative_reference}) {
        for (u32 pc : {0x200u, 0x2200u}) {
            core->write(pc, 0x4002, 2);
            core->write(pc + 2, 0xffff, 2);
        }
    }
    first_objects = 0;
    for (u32 pc : {0x200u, 0x2200u}) {
        unsigned compiled = 0;
        for (unsigned trial = 0; trial < 80; ++trial) {
            relative.r[2] = relative_reference.r[2] = trial;
            relative.r[31] = relative_reference.r[31] = pc;
            const unsigned count = relative_jit.try_execute(relative, 32, 0x4002);
            if (!count) continue;
            for (unsigned i = 0; i < count; ++i) relative_reference.step();
            if (relative.r != relative_reference.r) {
                std::cerr << "PC-relative block differs from interpreter\n";
                return 1;
            }
            ++compiled;
        }
        if (!compiled) return 1;
        if (pc == 0x200) first_objects = relative_jit.loaded_object_count();
        else if (relative_jit.loaded_object_count() <= first_objects) {
            std::cerr << "Different PC-relative constants shared native code\n";
            return 1;
        }
    }
    std::cout << "Cached blocks match the interpreter, invalidate and share across addresses\n";
}

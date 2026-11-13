#include "clickhle/hle/modules.h"

#include "clickhle/common/log.h"
#include "clickhle/hle/registry.h"
#include "clickhle/core/emulator.h"

namespace clickhle {

void register_all_modules(HleRegistry& registry) {
    register_misc(registry);
    register_input(registry);
    register_settings(registry);
    register_fileio(registry);
    register_audio(registry);
    register_metadata(registry);

    CLICKHLE_DEBUG("hle") << "registered " << registry.size() << " functions";
}

void gles_statistics(Emulator& emulator, u64& frames, u64& draws) {
    auto* vc = emulator.videocore();
    const auto* classic = emulator.classic_graphics();
    frames = vc ? vc->frames() : classic ? classic->frames() : 0;
    draws = vc ? vc->draws() : 0;
}

const std::map<u32, u64>& gles_draws_by_texture(Emulator&) {
    // Firmware texture attribution has not been decoded yet.
    static const std::map<u32, u64> empty;
    return empty;
}

}  // namespace clickhle

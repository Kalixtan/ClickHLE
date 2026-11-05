#include "clickhle/hle/modules.h"

#include "clickhle/common/log.h"
#include "clickhle/hle/registry.h"

namespace clickhle {

void register_all_modules(HleRegistry& registry) {
    // Order matters: the GL implementation registers by name, and
    // register_opengles() resolves those names as it builds the ordinal table.
    register_libgles(registry);
    register_opengles(registry);
    register_misc(registry);
    register_input(registry);
    register_settings(registry);
    register_fileio(registry);
    register_audio(registry);
    register_metadata(registry);

    CLICKHLE_DEBUG("hle") << "registered " << registry.size() << " functions";
}

}  // namespace clickhle

// Stand-in for the OpenGL ES layer in builds without SDL3 or OpenGL.
//
// Registers nothing. The coverage report then shows the GLES entry points as
// unimplemented, which is exactly true for such a build -- better than
// registering handlers that silently do nothing and reporting 100%.

#include "clickhle/common/log.h"
#include "clickhle/hle/modules.h"
#include "clickhle/hle/registry.h"

namespace clickhle {

void register_libgles(HleRegistry&) {
    CLICKHLE_DEBUG("gles")
        << "built without a video backend; OpenGL ES entry points are "
           "unavailable";
}

void gles_statistics(u64& frames, u64& draws) {
    frames = 0;
    draws = 0;
}

// Both of these are called from coredll_user and ddraw, which are built
// whether or not there is a video backend, so they need a definition here as
// well or this configuration does not link.


void gles_present_frame(HleCallFrame&) {}

}  // namespace clickhle

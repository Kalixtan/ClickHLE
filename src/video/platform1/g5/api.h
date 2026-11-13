#pragma once

#include "clickhle/common/types.h"
#include <optional>

namespace clickhle {
struct HleCallFrame;

// Calls with no VC2-visible state may be handled here. Returning nullopt
// leaves the original packet builder and JIT path in charge of the call.
std::optional<u32> g5_graphics_call(u32 ordinal, HleCallFrame &frame);
}

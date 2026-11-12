#include "clickhle/loader/framework_abi.h"

#include <array>

namespace clickhle {
namespace {

struct FrameworkAbi {
    std::string_view name;
    std::string_view hash;
    u32 count;
};

// RetailOS framework descriptors verified against the Platform 1 firmware.
// Add another version here only after its ordinals have been matched to HLE.
constexpr std::array kSupportedAbis{
    FrameworkAbi{"OpenGLES", "041f4da520603c37d8ef9b879efbf280", 179},
    FrameworkAbi{"Metadata", "71def8ce4eedefcd5b42dc23a5f45d4d", 152},
    FrameworkAbi{"Audio", "d9c859f2325e3831f974c1218c35672d", 61},
    FrameworkAbi{"AsyncFileIO", "0012f0601105c3a75125314e17aa989a", 17},
    FrameworkAbi{"miscTBD", "4fa949cb7829a3264e908a7296366cf8", 15},
    FrameworkAbi{"Filesytem", "9aff0ee8f4485b4006ef4c0e578c3c0c", 4},
    FrameworkAbi{"Settings", "91e11cfe3c1f4eda92a085db26c09dba", 3},
    FrameworkAbi{"InputEvents", "c73357d0487174bd02f378e54437e1e6", 2},
};

} // namespace

bool has_compatible_framework_abi(std::string_view name, std::string_view hash,
                                  u32 count) {
    if (count == 0) return false;
    for (const auto& abi : kSupportedAbis)
        if (abi.name == name && abi.hash == hash && abi.count == count)
            return true;
    return false;
}

} // namespace clickhle

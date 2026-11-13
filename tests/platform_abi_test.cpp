#include "clickhle/loader/framework_abi.h"

using namespace clickhle;

int main() {
    constexpr auto classic_misc = "19e25e15bcdcd093cf8942d70b281c22";
    constexpr auto g5_misc = "4fa949cb7829a3264e908a7296366cf8";
    constexpr auto g5_input = "c73357d0487174bd02f378e54437e1e6";
    constexpr auto classic_gles = "7daec616da05031a6ca668c781fc9cb4";
    constexpr auto classic_audio = "53e188bf0a31a3d5ac65e830b9ba00be";
    constexpr auto classic_effect = "da7170db2b0cce143bdabd43f78cf78d";
    constexpr auto classic_users = "216d2e51d4e6aa78dfa613c4efcc8caf";
    constexpr auto classic_memory = "db0bd0e3bf455411bb92f89b96a9e69b";
    constexpr auto classic_settings = "8ff51a998545a21c301ebf70f4590e95";
    if (!supports_framework_abi(1, "miscTBD", g5_misc, 15) ||
        supports_framework_abi(1, "miscTBD", classic_misc, 13) ||
        supports_framework_abi(3, "miscTBD", g5_misc, 15) ||
        !supports_framework_abi(3, "miscTBD", classic_misc, 13) ||
        supports_framework_abi(3, "miscTBD", classic_misc, 12) ||
        !supports_framework_abi(2, "miscTBD", classic_misc, 13) ||
        !supports_framework_abi(4, "miscTBD", classic_misc, 13) ||
        !supports_framework_abi(3, "InputEvents", g5_input, 2) ||
        !supports_framework_abi(3, "OpenGLES", classic_gles, 170) ||
        supports_framework_abi(3, "OpenGLES", classic_gles, 179) ||
        !supports_framework_abi(3, "Audio", classic_audio, 54) ||
        supports_framework_abi(1, "Audio", classic_audio, 54) ||
        supports_framework_abi(3, "Audio", classic_audio, 61) ||
        !supports_framework_abi(3, "SoundEffect", classic_effect, 42) ||
        supports_framework_abi(1, "SoundEffect", classic_effect, 42) ||
        !supports_framework_abi(3, "Users", classic_users, 10) ||
        supports_framework_abi(1, "Users", classic_users, 10) ||
        !supports_framework_abi(3, "MemoryAlloc", classic_memory, 3) ||
        supports_framework_abi(1, "MemoryAlloc", classic_memory, 3) ||
        supports_framework_abi(3, "MemoryAlloc", classic_memory, 2) ||
        supports_framework_abi(3, "MemoryAlloc", classic_audio, 3) ||
        !supports_framework_abi(3, "Settings", classic_settings, 1) ||
        supports_framework_abi(1, "Settings", classic_settings, 1) ||
        supports_framework_abi(3, "Settings", classic_settings, 3) ||
        supports_framework_abi(5, "miscTBD", classic_misc, 13)) return 1;
}

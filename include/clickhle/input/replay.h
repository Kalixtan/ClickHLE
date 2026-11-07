#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "clickhle/common/types.h"

namespace clickhle {

struct ReplayEntry {
    u64 polls = 0;
    u32 port_word = 0;
};

// Each line is "<input-port reads> <0x7000c140 value>". The value is held
// for that many reads. 0x8000001a releases the wheel and every button.
[[nodiscard]] bool parse_replay(std::string_view text,
                                std::vector<ReplayEntry>& entries,
                                std::string& error);

} // namespace clickhle

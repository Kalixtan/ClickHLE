#pragma once

#include <string_view>

#include "clickhle/common/types.h"

namespace clickhle {

// An import table is safe to dispatch only when both its hash and size match
// the framework version implemented by the HLE registry.
[[nodiscard]] bool has_compatible_framework_abi(std::string_view name,
                                                std::string_view hash, u32 count);

} // namespace clickhle

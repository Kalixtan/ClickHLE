#pragma once

#include "clickhle/common/types.h"
#include <array>

namespace clickhle::classic {

// Fixed-point rotation builder from the 7G software OpenGL implementation.
[[nodiscard]] std::array<i32, 16> rotation_matrix(i32 angle, i32 x, i32 y, i32 z);
[[nodiscard]] std::array<i32, 16> multiply_matrices(const std::array<i32, 16>& left,
                                                    const std::array<i32, 16>& right);

} // namespace clickhle::classic

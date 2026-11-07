#pragma once
#include "core.h"
#include <stdexcept>

namespace clickhle::vc2 {
using VectorLanes = std::array<std::int64_t, 16>;

// These kernels receive unpacked byte/halfword operands. Keep 64-bit results:
// narrowing before the destination store would change overflow and sign rules.
// Dispatch is outside the lane loop so compilers can vectorize each operation.
template<VectorKernel kernel>
VectorLanes vector_math(const VectorLanes &a, const VectorLanes &b, std::int64_t mask) {
    static_assert(kernel != VectorKernel::General);
    if constexpr (kernel == VectorKernel::Move)
        return b;
    VectorLanes result{};
    for (unsigned i = 0; i < 16; ++i) {
        if constexpr (kernel == VectorKernel::Add)
            result[i] = a[i] + b[i];
        else if constexpr (kernel == VectorKernel::Subtract)
            result[i] = a[i] - b[i];
        else if constexpr (kernel == VectorKernel::ReverseSubtract)
            result[i] = b[i] - a[i];
        else if constexpr (kernel == VectorKernel::And)
            result[i] = a[i] & b[i];
        else if constexpr (kernel == VectorKernel::Or)
            result[i] = a[i] | b[i];
        else if constexpr (kernel == VectorKernel::Complement)
            result[i] = a[i] ^ mask;
        else if constexpr (kernel == VectorKernel::Multiply)
            result[i] = a[i] * b[i];
        else
            result[i] = b[i];
    }
    return result;
}

inline VectorLanes vector_math(VectorKernel kernel, const VectorLanes &a,
                               const VectorLanes &b, std::int64_t mask) {
    switch (kernel) {
    case VectorKernel::Add: return vector_math<VectorKernel::Add>(a, b, mask);
    case VectorKernel::Subtract: return vector_math<VectorKernel::Subtract>(a, b, mask);
    case VectorKernel::ReverseSubtract: return vector_math<VectorKernel::ReverseSubtract>(a, b, mask);
    case VectorKernel::And: return vector_math<VectorKernel::And>(a, b, mask);
    case VectorKernel::Or: return vector_math<VectorKernel::Or>(a, b, mask);
    case VectorKernel::Complement: return vector_math<VectorKernel::Complement>(a, b, mask);
    case VectorKernel::Multiply: return vector_math<VectorKernel::Multiply>(a, b, mask);
    case VectorKernel::Move: return b;
    default: throw std::logic_error("VC2 general operation used as a plain vector kernel");
    }
}
} // namespace clickhle::vc2

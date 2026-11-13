#include "clickhle/video/classic_graphics.h"
#include "clickhle/video/classic_rotation.h"
#include <algorithm>

namespace clickhle::classic {

namespace {
std::array<i32, 16> identity_matrix() {
    std::array<i32, 16> result{};
    result[0] = result[5] = result[10] = result[15] = 0x10000;
    return result;
}
} // namespace

GraphicsContext::GraphicsContext() {
    // 0x0824af98 constructs identity matrix stacks and derived matrices.
    for (auto& matrix : matrices_) matrix = identity_matrix();
    combined_matrix_ = identity_matrix();
    derived_modelview_ = derived_inverse_ = identity_matrix();
}

bool GraphicsContext::load_identity() noexcept {
    u32 index = matrix_mode_ - 0x1700;
    if (index == 2) {
        if (active_texture_ >= 2) return false;
        texture_matrix_identity_[active_texture_] = true;
        index += active_texture_;
    }
    if (index >= matrices_.size()) return false;
    matrices_[index] = identity_matrix();
    matrix_identity_[index] = true;
    // Modelview-changing APIs still stop. Its inverse calculation rounds the
    // diagonal to 0xffff; original ARM execution confirms these values.
    if (index == 0) {
        derived_modelview_ = derived_inverse_ = identity_matrix();
        for (u32 i : {0u, 5u, 10u}) {
            derived_modelview_[i] = 0xffff;
            derived_inverse_[i] = 0xffff;
        }
        derived_inverse_[15] = 0xffff;
        derived_modelview_identity_ = false;
    }
    if (index == 0) {
        combined_matrix_ = matrices_[1];
        combined_identity_ = matrix_identity_[1];
    } else if (index == 1) {
        combined_matrix_ = matrices_[0];
        combined_identity_ = matrix_identity_[0];
    }
    return true;
}

bool GraphicsContext::rotate(i32 angle, i32 x, i32 y, i32 z) noexcept {
    return multiply_matrix(rotation_matrix(angle, x, y, z));
}

bool GraphicsContext::multiply_matrix(const std::array<i32, 16>& matrix) noexcept {
    u32 index = matrix_mode_ - 0x1700;
    if (index == 2) {
        if (active_texture_ >= 2) return false;
        index += active_texture_;
    }
    // Modelview rotation needs the unimplemented inverse/normal transforms.
    if (index == 0 || index >= matrices_.size()) return false;
    matrices_[index] = multiply_matrices(matrices_[index], matrix);
    matrix_identity_[index] = false;
    if (index == 1) {
        combined_matrix_ = matrices_[index]; // modelview is still identity
        combined_identity_ = false;
    } else {
        texture_matrix_identity_[active_texture_] = false;
    }
    return true;
}

bool GraphicsContext::load_matrix(const std::array<i32, 16>& matrix) noexcept {
    u32 index = matrix_mode_ - 0x1700;
    if (index == 2) {
        if (active_texture_ >= 2) return false;
        index += active_texture_;
    }
    if (index == 0) {
        // An explicit identity load is tagged non-identity by 0x0824c3b0.
        // Other modelview values require inverse/normal calculations.
        if (matrix != identity_matrix() || !load_identity()) return false;
        matrix_identity_[0] = false;
        combined_identity_ = false;
        return true;
    }
    if (index >= matrices_.size()) return false;
    matrices_[index] = matrix;
    matrix_identity_[index] = false;
    if (index == 1) {
        combined_matrix_ = matrix;
        combined_identity_ = false;
    } else {
        texture_matrix_identity_[active_texture_] = false;
    }
    return true;
}

void GraphicsContext::set_blend_factors(u32 source, u32 destination) noexcept {
    auto factor = [](u32 value, bool is_source) -> std::optional<u8> {
        if (value <= 1) return static_cast<u8>(value);
        if (value >= 0x302 && value <= 0x305) return static_cast<u8>(value - 0x302 + 4);
        if (is_source && value >= 0x306 && value <= 0x308)
            return static_cast<u8>(value == 0x308 ? 8 : value - 0x306 + 2);
        if (!is_source && value >= 0x300 && value <= 0x301)
            return static_cast<u8>(value - 0x300 + 2);
        return std::nullopt;
    };
    const auto src = factor(source, true);
    const auto dst = factor(destination, false);
    if (!src || !dst) { invalid_enum(); return; }
    blend_source_ = *src;
    blend_destination_ = *dst;
}

void GraphicsContext::set_vertex_pointer(u32 size, u32 type, u32 stride,
                                         GuestAddr pointer) noexcept {
    // The 7G validator tests type before component count and signed stride.
    if (type != 0x1400 && type != 0x1402 && type != 0x1406 && type != 0x140c) {
        invalid_enum();
        return;
    }
    if (size < 2 || size > 4 || (stride & 0x80000000u)) {
        invalid_value();
        return;
    }
    if (stride == 0) {
        const u32 bytes = type == 0x1400 ? 1 : type == 0x1402 ? 2 : 4;
        stride = size * bytes;
    }
    vertex_array_ = VertexArray{size, type, pointer, stride, std::nullopt};
}

bool GraphicsContext::set_texture_pointer(u32 size, u32 type, u32 stride,
                                          GuestAddr pointer) noexcept {
    if (type != 0x1400 && type != 0x1402 && type != 0x1406 && type != 0x140c) {
        invalid_enum();
        return true;
    }
    if (size < 2 || size > 4 || (stride & 0x80000000u)) {
        invalid_value();
        return true;
    }
    if (client_texture_ >= texture_arrays_.size()) return false;
    if (stride == 0) stride = size * (type == 0x1400 ? 1 : type == 0x1402 ? 2 : 4);
    texture_arrays_[client_texture_] = VertexArray{size, type, pointer, stride, std::nullopt};
    return true;
}

NamePool::NamePool() : names_(64), remaining_(64) {
    for (u32 i = 0; i < 63; ++i) names_[i] = ((i + 1) << 1) | 1;
    names_[63] = 0xffffffffu;
    // The firmware constructor consumes name zero for the default object.
    (void)allocate();
}

void NamePool::grow() {
    const u32 old_size = static_cast<u32>(names_.size());
    names_.resize(old_size * 2);
    for (u32 i = old_size; i + 1 < names_.size(); ++i)
        names_[i] = ((i + 1) << 1) | 1;
    names_.back() = 0xffffffffu;
    head_ = old_size;
    remaining_ = old_size;
}

u32 NamePool::allocate() {
    if (remaining_ == 0) grow();
    if (head_ >= names_.size()) return 0xffffffffu;
    const u32 name = head_;
    const u32 link = names_[name];
    head_ = link == 0xffffffffu ? link : link >> 1;
    names_[name] = 0;
    --remaining_;
    return name;
}

bool NamePool::bind(u32 name) {
    if (name >= 0x1000000u) return false;
    if (name >= names_.size()) {
        const u32 old_size = static_cast<u32>(names_.size());
        const u32 new_size = std::max(old_size * 2, name + 1);
        names_.resize(new_size);
        for (u32 i = old_size; i + 1 < new_size; ++i)
            names_[i] = ((i + 1) << 1) | 1;
        names_.back() = 0xffffffffu;
        head_ = old_size;
        remaining_ = new_size - old_size;
    }
    const u32 entry = names_[name];
    if (entry & 1u) {
        const u32 next = entry == 0xffffffffu ? 0xffffffffu : entry >> 1;
        if (head_ == name) {
            head_ = next;
        } else {
            u32 previous = head_;
            while (previous < names_.size()) {
                const u32 link = names_[previous];
                if (link == 0xffffffffu) return false;
                if ((link >> 1) == name) break;
                previous = link >> 1;
            }
            if (previous >= names_.size()) return false;
            names_[previous] = next == 0xffffffffu ? next : (next << 1) | 1;
        }
    }
    // An even entry denotes a reserved name or an existing object.
    // 0x083a9958 does not change the pool's remaining-count field here.
    names_[name] = 2;
    return true;
}

bool GraphicsContext::bind_texture(u32 name) {
    if (active_texture_ >= bound_textures_.size() || !names_.bind(name)) return false;
    bound_textures_[active_texture_] = name;
    return true;
}

void GraphicsContext::invalid_value() {
    if (error_ == 0) error_ = 0x501;
}

void GraphicsContext::invalid_enum() {
    if (error_ == 0) error_ = 0x500;
}

} // namespace clickhle::classic

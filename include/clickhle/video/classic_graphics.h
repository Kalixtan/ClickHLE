#pragma once

#include "clickhle/common/types.h"
#include "clickhle/video/classic_pixel.h"
#include <array>
#include <optional>
#include <vector>

namespace clickhle::classic {

struct DrawSurface {
    GuestAddr pixels = 0;
    u32 byte_size = 0;
    u32 stride = 0;
    u8 format = 0;
    ClearRectangle bounds{};
};

enum class ClearResult { complete, missing_surface, unsupported_buffers, invalid_memory };

// Name allocation follows the 7G software GL context constructor and #52.
// Creating a current context at game startup remains provisional.
class NamePool {
public:
    NamePool();
    [[nodiscard]] u32 allocate();
    bool bind(u32 name);

private:
    std::vector<u32> names_;
    u32 head_ = 0;
    u32 remaining_ = 0;
    void grow();
};

class GraphicsContext {
public:
    struct VertexArray {
        u32 size;
        u32 type;
        GuestAddr pointer;
        u32 stride;
        // Captured buffer binding is not yet modeled.
        std::optional<u32> buffer;
    };
    GraphicsContext();
    [[nodiscard]] u32 allocate_name() { return names_.allocate(); }
    [[nodiscard]] u32 allocate_other_name() { return other_names_.allocate(); }
    bool bind_texture(u32 name);
    void invalid_value();
    void invalid_enum();
    void set_active_texture(u32 unit) noexcept { active_texture_ = unit; }
    [[nodiscard]] u32 active_texture() const noexcept { return active_texture_; }
    void set_client_texture(u32 unit) noexcept { client_texture_ = unit; }
    void set_matrix_mode(u32 mode) noexcept { matrix_mode_ = mode; }
    bool load_identity() noexcept;
    bool rotate(i32 angle, i32 x, i32 y, i32 z) noexcept;
    bool multiply_matrix(const std::array<i32, 16>& matrix) noexcept;
    bool load_matrix(const std::array<i32, 16>& matrix) noexcept;
    void set_blend(bool enabled) noexcept { blend_enabled_ = enabled; }
    void set_blend_factors(u32 source, u32 destination) noexcept;
    void set_vertex_pointer(u32 size, u32 type, u32 stride, GuestAddr pointer) noexcept;
    bool set_texture_pointer(u32 size, u32 type, u32 stride, GuestAddr pointer) noexcept;
    [[nodiscard]] const std::optional<VertexArray>* texture_array() const noexcept {
        return client_texture_ < texture_arrays_.size() ? &texture_arrays_[client_texture_] : nullptr;
    }
    [[nodiscard]] const std::optional<VertexArray>& vertex_array() const noexcept {
        return vertex_array_;
    }
    [[nodiscard]] bool vertex_array_enabled() const noexcept { return client_arrays_[0].value_or(false); }
    [[nodiscard]] const std::array<i32, 16>& combined_matrix() const noexcept { return combined_matrix_; }
    void set_alpha_function(u32 function, u32 reference) noexcept {
        if (function < 0x200 || function > 0x207) { invalid_enum(); return; }
        alpha_function_ = static_cast<u8>(function - 0x200);
        alpha_reference_ = reference;
    }
    [[nodiscard]] std::optional<u8> alpha_function() const noexcept { return alpha_function_; }
    [[nodiscard]] std::optional<u32> alpha_reference() const noexcept { return alpha_reference_; }
    void set_depth_test(bool enabled) noexcept { depth_test_enabled_ = enabled; }
    bool set_client_array(u32 capability, bool enabled) noexcept {
        if (capability == 0x8078) {
            if (client_texture_ >= client_texture_arrays_.size()) return false;
            client_texture_arrays_[client_texture_] = enabled;
            return true;
        }
        if (capability < 0x8074 || capability > 0x8076) return false;
        client_arrays_[capability - 0x8074] = enabled;
        return true;
    }
    bool set_texture_environment(u32 unit, u8 mode) noexcept {
        if (unit >= texture_environment_.size()) return false;
        texture_environment_[unit] = mode;
        return true;
    }
    [[nodiscard]] std::optional<u32> texture_environment(u32 unit) const noexcept {
        constexpr std::array<u32, 6> modes{0x2101, 0x1e01, 0x0be2, 0x0104, 0x2100, 0x8570};
        if (unit >= texture_environment_.size() || !texture_environment_[unit] ||
            *texture_environment_[unit] >= modes.size()) return std::nullopt;
        return modes[*texture_environment_[unit]];
    }
    bool set_texture_enabled(u32 unit, bool enabled) noexcept {
        if (unit >= texture_enabled_.size()) return false;
        texture_enabled_[unit] = enabled;
        return true;
    }
    [[nodiscard]] u32 error() const noexcept { return error_; }
    u32 take_error() noexcept {
        const u32 result = error_;
        error_ = 0;
        return result;
    }
    void note_present() noexcept { ++frames_; }
    void note_draw() noexcept { ++draws_; }
    [[nodiscard]] u64 frames() const noexcept { return frames_; }
    [[nodiscard]] u64 draws() const noexcept { return draws_; }
    void set_surface_handles(GuestAddr draw, GuestAddr read) noexcept {
        draw_handle_ = draw;
        read_handle_ = read;
    }
    [[nodiscard]] GuestAddr surface_handle(u32 selector) const noexcept {
        return selector == 0x3059 ? draw_handle_ : selector == 0x305a ? read_handle_ : 0;
    }
    void set_clear_color(const std::array<u32, 4>& color) noexcept { clear_color_ = color; }
    void set_current_color(const std::array<u32, 4>& color) noexcept { current_color_ = color; }
    [[nodiscard]] const std::optional<std::array<u32, 4>>& current_color() const noexcept {
        return current_color_;
    }
    [[nodiscard]] const std::array<u32, 4>& clear_color() const noexcept { return clear_color_; }
    void bind_draw_surface(DrawSurface surface) { draw_surface_ = surface; }
    void unbind_draw_surface() { draw_surface_.reset(); }
    [[nodiscard]] const std::optional<DrawSurface>& draw_surface() const noexcept { return draw_surface_; }
    void set_color_mask(std::array<u8, 4> mask) noexcept { color_mask_ = mask; }
    [[nodiscard]] const std::array<u8, 4>& color_mask() const noexcept { return color_mask_; }
    void set_scissor(ClearRectangle rect, bool enabled) noexcept { scissor_ = rect; scissor_enabled_ = enabled; }
    [[nodiscard]] bool scissor_enabled() const noexcept { return scissor_enabled_; }
    [[nodiscard]] ClearRectangle clear_rectangle() const noexcept {
        return scissor_enabled_ ? scissor_ : draw_surface_->bounds;
    }

private:
    NamePool names_;
    NamePool other_names_;
    u32 error_ = 0;
    u32 active_texture_ = 0;
    std::array<u32, 2> bound_textures_{};
    u32 client_texture_ = 0;
    u32 matrix_mode_ = 0x1700;
    using Matrix = std::array<i32, 16>;
    std::array<Matrix, 4> matrices_{};
    std::array<bool, 4> matrix_identity_{true, true, true, true};
    Matrix combined_matrix_{};
    bool combined_identity_ = true;
    Matrix derived_modelview_{};
    Matrix derived_inverse_{};
    bool derived_modelview_identity_ = true;
    std::array<std::optional<bool>, 2> texture_matrix_identity_{};
    // Initial enable states remain unverified; populated by API calls.
    std::optional<bool> blend_enabled_;
    std::optional<u8> blend_source_;
    std::optional<u8> blend_destination_;
    // 7G stores the compare enum and reference at context +0x9c9/+0x9cc.
    // Alpha testing during rasterization is not yet implemented.
    std::optional<u8> alpha_function_;
    std::optional<u32> alpha_reference_;
    std::optional<bool> depth_test_enabled_;
    std::optional<VertexArray> vertex_array_;
    std::array<std::optional<VertexArray>, 2> texture_arrays_{};
    std::array<std::optional<bool>, 3> client_arrays_{};
    std::array<std::optional<bool>, 2> client_texture_arrays_{};
    std::array<std::optional<u8>, 2> texture_environment_{u8{4}, u8{4}};
    std::array<std::optional<bool>, 2> texture_enabled_{};
    u64 frames_ = 0;
    u64 draws_ = 0;
    GuestAddr draw_handle_ = 0;
    GuestAddr read_handle_ = 0;
    std::array<u32, 4> clear_color_{};
    std::optional<std::array<u32, 4>> current_color_;
    std::optional<DrawSurface> draw_surface_;
    std::array<u8, 4> color_mask_{1, 1, 1, 1};
    ClearRectangle scissor_{};
    bool scissor_enabled_ = false;
};

} // namespace clickhle::classic

namespace clickhle {
class AddressSpace;
namespace classic {
[[nodiscard]] ClearResult clear_bound_surface(GraphicsContext& context, AddressSpace& memory, u32 mask);
// Read a format-5 surface into top-down RGB rows. This does not apply the
// compositor's blending, color tables or panel transfer function.
[[nodiscard]] std::optional<std::vector<u8>> read_surface_rgb(const DrawSurface& surface,
                                                              const AddressSpace& memory);
}
}

#pragma once

#include "clickhle/common/types.h"
#include <memory>
#include <string>
#include <span>

namespace clickhle::classic {

// Experimental desktop renderer. A context belongs to the emulation thread.
class GpuRenderer {
public:
    GpuRenderer();
    ~GpuRenderer();
    GpuRenderer(const GpuRenderer&) = delete;
    GpuRenderer& operator=(const GpuRenderer&) = delete;
    bool initialise(std::string& error);
    bool bind_texture(u32 target, u32 name);
    bool texture_image(u32 target, i32 level, i32 internal_format, i32 width,
                       i32 height, i32 border, u32 format, u32 type, const void* pixels);
    bool texture_sub_image(u32 target, i32 level, i32 x, i32 y, i32 width,
                           i32 height, u32 format, u32 type, const void* pixels);
    bool texture_parameter(u32 target, u32 parameter, i32 value);
    bool draw_arrays(u32 mode, i32 first, i32 count, std::span<const float> xy,
                     std::span<const float, 16> matrix, std::span<const float, 4> color,
                     bool cull_face_enabled,
                     std::span<u8> rgb565, u32 stride_pixels, u32 width, u32 height);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace clickhle::classic

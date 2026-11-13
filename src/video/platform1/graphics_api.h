#pragma once

#include "clickhle/common/types.h"
#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <span>
#include <vector>

namespace clickhle {
struct HleCallFrame;

// Host-side graphics state and RenderServer packet construction. No host GPU
// or firmware ARM execution is involved; packets are consumed by the VC2 core.
class GraphicsApi {
  public:
    using Submit = std::function<void(std::span<const u8>)>;
    struct Transport {
        std::function<std::vector<u8>(u32)> readback;
        std::function<u32()> completed;
        std::function<void(u32)> wait;
    };
    explicit GraphicsApi(Submit submit);
    GraphicsApi(Submit submit, Transport transport);
    void reset(HleCallFrame &frame);
    u32 call(u32 ordinal, HleCallFrame &frame);
    void queue(std::array<u32, 4> header, std::span<const u8> payload = {});
    void flush();
    void finish();
    [[nodiscard]] std::span<const u8> pending_packets() const {
        return std::span(staging_).first(pending_size_);
    }

  private:
    Submit submit_;
    Transport transport_;
    u32 completion_ = 0;
    std::array<u8, 0x2000> staging_{};
    std::size_t pending_size_ = 0;
    u32 active_texture_ = 0, client_active_texture_ = 0, next_texture_ = 1, error_ = 0;
    u8 depth_write_mask_ = 1;
    std::array<u32, 3> textures_{};
    std::map<u32, bool> enabled_{{0xbd0, true}}; // Dithering starts enabled in RetailOS.
    u16 textures_dirty_ = 0;
    u32 draw(u32 ordinal, HleCallFrame &frame, const std::array<u32, 16> &args);
    u16 attributes_enabled_ = 0, attributes_dirty_ = 0;
    struct Attribute { u32 size = 0, type = 0, stride = 0, address = 0; bool normalized = false; u32 buffer = 0; };
    std::array<Attribute, 16> attributes_{};
    struct Buffer {
        u32 address = 0, size = 0, usage = 0x88e4, access = 0x88b9, completion = 0;
        bool exists = false, mapped = false, dirty = false;
    };
    std::array<Buffer, 32> buffers_{};
    u32 array_buffer_ = 0, element_buffer_ = 0, next_buffer_ = 1;
    u32 buffers(u32 ordinal, HleCallFrame &frame, const std::array<u32, 16> &args);
    std::array<bool, 50> programs_loaded_{};
    std::array<std::array<std::vector<u8>, 2>, 8> custom_programs_{};
    std::array<u32, 5> string_addresses_{};
    u32 pack_alignment_ = 4, unpack_alignment_ = 4;
    u32 select_program(u32 selector);
    u32 selected_program_ = 0;
    u32 load_program(HleCallFrame &frame, const std::array<u32, 16> &args);
    void uniform(u32 slot, std::span<const u32> values);
    void fragments(u32 type, u32 object, std::span<const u8> prefix,
                   std::span<const u8> data, u32 flags = 0);
    void capability(u32 key, bool enabled);
    u32 raster(u32 ordinal, const std::array<u32, 16> &args);
    u32 state_query(u32 ordinal, HleCallFrame &frame, const std::array<u32, 16> &args);
    u32 string_query(HleCallFrame &frame, u32 name);
    u32 attribute_value(u32 ordinal, HleCallFrame &frame, const std::array<u32, 16> &args);
    u32 uniforms(u32 ordinal, HleCallFrame &frame, const std::array<u32, 16> &args);
    u32 matrix(u32 ordinal, HleCallFrame &frame, const std::array<u32, 16> &args);
    u32 read_pixels(HleCallFrame &frame, const std::array<u32, 16> &args);
    bool overlay_enabled_ = false;
    u32 overlay_width_ = 0, overlay_height_ = 0;
    std::array<u32, 16> overlay_vertices_{};
    std::array<u32, 8> overlay_coordinates_{};
    u32 overlay(u32 ordinal, HleCallFrame &frame, const std::array<u32, 16> &args);
};
} // namespace clickhle

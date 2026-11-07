#pragma once

#include "clickhle/common/types.h"
#include <array>
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
    explicit GraphicsApi(Submit submit);
    u32 call(u32 ordinal, HleCallFrame &frame);
    void queue(std::array<u32, 4> header, std::span<const u8> payload = {});
    void flush();
    [[nodiscard]] std::span<const u8> pending_packets() const { return pending_; }

  private:
    Submit submit_;
    std::vector<u8> pending_;
    std::array<u8, 0x2000> staging_{};
    u32 active_texture_ = 0, next_texture_ = 0, error_ = 0;
    std::array<u32, 3> textures_{};
    std::map<u32, bool> enabled_;
    u16 textures_dirty_ = 0;
    u32 draw(u32 ordinal, HleCallFrame &frame, const std::array<u32, 16> &args);
    u16 attributes_enabled_ = 0, attributes_dirty_ = 0;
    struct Attribute { u32 size = 0, type = 0, stride = 0, address = 0; bool normalized = false; };
    std::array<Attribute, 16> attributes_{};
    std::array<bool, 50> programs_loaded_{};
    u32 pack_alignment_ = 4, unpack_alignment_ = 4;
    u32 select_program(u32 selector);
    void uniform(u32 slot, std::span<const u32> values);
    void fragments(u32 type, u32 object, std::span<const u8> prefix,
                   std::span<const u8> data);
    void capability(u32 key, bool enabled);
    u32 matrix(u32 ordinal, HleCallFrame &frame, const std::array<u32, 16> &args);
};
} // namespace clickhle

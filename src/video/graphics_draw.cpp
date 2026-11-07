#include "graphics_api.h"
#include "clickhle/hle/call_frame.h"
#include <stdexcept>

namespace clickhle {
u32 GraphicsApi::draw(u32 ordinal, HleCallFrame &frame, const std::array<u32, 16> &a) {
    const bool indexed = ordinal == 38;
    u32 mode = a[0], count = a[indexed ? 1 : 2];
    if (static_cast<i32>(count) < 0 || mode > 7)
        throw std::runtime_error("Invalid draw arguments");
    switch (mode) {
    case 1: count &= ~1u; break;
    case 2: mode = 3; if (count < 2) return 0; break;
    case 3: mode = 2; if (count < 2) return 0; break;
    case 4: count -= count % 3; break;
    case 5: case 6: if (count < 3) return 0; break;
    case 7: count &= ~3u; break;
    }
    if (!count) return 0;
    for (u32 i = 0; i < textures_.size(); ++i)
        if (textures_dirty_ & (1u << i)) queue({0, ((0x241 + i) << 16) | 2, 0, textures_[i]});
    for (u32 i = 0; i < attributes_.size(); ++i)
        if (attributes_dirty_ & (1u << i))
            queue({0, ((0x201 + i) << 16) | ((attributes_enabled_ & (1u << i)) ? 4u : 0u), 0, 0});
    bool uploaded = false;
    for (u32 i = 0; i < attributes_.size(); ++i) {
        if (!(attributes_enabled_ & (1u << i))) continue;
        const auto &attribute = attributes_[i];
        const u32 width = attribute.size * 4;
        const u32 stride = attribute.stride ? attribute.stride : width;
        if (!width || count > 0x1000000 / width)
            throw std::runtime_error("Invalid vertex upload size");
        std::vector<u8> data;
        data.reserve(count * width);
        for (u32 vertex = 0; vertex < count; ++vertex) {
            u32 index = a[1] + vertex;
            if (indexed) {
                if (a[2] != 0x1401 && a[2] != 0x1403)
                    throw std::runtime_error("Unsupported vertex index type");
                const u32 index_width = a[2] == 0x1401 ? 1u : 2u;
                const uint64_t address = uint64_t(a[3]) + uint64_t(vertex) * index_width;
                if (address > 0xffffffff)
                    throw std::runtime_error("Vertex index address overflow");
                if (index_width == 1) {
                    const auto value = frame.memory.read<u8>(static_cast<u32>(address));
                    if (!value) throw std::runtime_error("Unmapped vertex index");
                    index = *value;
                } else {
                    const auto value = frame.memory.read<u16>(static_cast<u32>(address));
                    if (!value) throw std::runtime_error("Unmapped vertex index");
                    index = *value;
                }
            }
            const uint64_t address = uint64_t(attribute.address) + uint64_t(index) * stride;
            const auto *source = address <= 0xffffffff ? frame.memory.host_ptr(static_cast<u32>(address), width) : nullptr;
            if (!source) throw std::runtime_error("Unmapped vertex data");
            const auto *bytes = reinterpret_cast<const u8 *>(source);
            data.insert(data.end(), bytes, bytes + width);
        }
        const u32 object = i ? 32 - i : 0;
        fragments(1, object, {}, data);
        queue({0, ((0x221 + i) << 16) | 2, 0, object});
        uploaded = true;
    }
    queue({2, mode, indexed || uploaded ? 0u : a[1], count});
    return 0;
}
} // namespace clickhle

#include "graphics_api.h"
#include "clickhle/hle/call_frame.h"
#include <stdexcept>

namespace clickhle {
u32 GraphicsApi::draw(u32 ordinal, HleCallFrame &frame, const std::array<u32, 16> &a) {
    const bool indexed = ordinal == 38;
    // DrawArrays allocates a transport serial even when the draw is empty;
    // DrawElements does not (RetailOS 0x10263d8c).
    if (!indexed) {
        if (completion_ == 0xffffffff) {
            if (!transport_.wait) throw std::runtime_error("Graphics serial wrap requires a VC2 transport");
            transport_.wait(completion_);
        }
        ++completion_;
    }
    u32 mode = a[0], count = a[indexed ? 1 : 2];
    if (static_cast<i32>(count) < 0 || mode > 7) {
        error_ = 0x501;
        throw std::runtime_error("Invalid draw arguments");
    }
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
        if (!indexed && attribute.buffer) {
            auto &buffer = buffers_[attribute.buffer];
            if (buffer.dirty) {
                const auto *source = frame.memory.host_ptr(buffer.address, buffer.size);
                if (buffer.size && !source) throw std::runtime_error("Unmapped vertex buffer storage");
                fragments(1, attribute.buffer, {},
                          std::span(reinterpret_cast<const u8 *>(source), buffer.size));
                buffer.dirty = false;
            }
            if (stride != width)
                throw std::runtime_error("RetailOS rejects strided vertex buffer draws");
            queue({0, ((0x221 + i) << 16) | 2, 0, attribute.buffer});
            continue;
        }
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
                const uint64_t base = element_buffer_ ? buffers_[element_buffer_].address : 0;
                const uint64_t address = base + a[3] + uint64_t(vertex) * index_width;
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
            // The original indexed path uses the buffer base directly;
            // the attribute pointer is used only for client arrays.
            const u32 base = attribute.buffer ? buffers_[attribute.buffer].address : attribute.address;
            const uint64_t address = uint64_t(base) + uint64_t(index) * stride;
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

#include "clickhle/video/classic_graphics.h"
#include "clickhle/core/address_space.h"

namespace clickhle::classic {

std::optional<std::vector<u8>> read_surface_rgb(const DrawSurface& surface,
                                                const AddressSpace& memory) {
    if (surface.format != 5 || surface.bounds.x != 0 || surface.bounds.y != 0)
        return std::nullopt;
    const u64 pixels = u64(surface.bounds.width) * surface.bounds.height;
    if (pixels > 320u * 240u || surface.byte_size == 0) return std::nullopt;
    const auto* data = memory.host_ptr(surface.pixels, surface.byte_size);
    if (!data) return std::nullopt;
    const std::span<const u8> source(reinterpret_cast<const u8*>(data), surface.byte_size);
    std::vector<u8> rgb(static_cast<std::size_t>(pixels * 3));
    if (!rgb565_to_rgb(source, surface.stride, surface.bounds.width, surface.bounds.height, rgb))
        return std::nullopt;
    return rgb;
}

ClearResult clear_bound_surface(GraphicsContext& context, AddressSpace& memory, u32 mask) {
    const auto& surface = context.draw_surface();
    if (!surface) return ClearResult::missing_surface;
    if ((mask & ~0x4500u) != 0) {
        context.invalid_value();
        return ClearResult::complete;
    }
    // Depth and stencil clears require their own surface and mask semantics.
    if ((mask & 0x500u) != 0) return ClearResult::unsupported_buffers;
    if ((mask & 0x4000u) == 0) return ClearResult::complete;
    if (surface->format < 4 || surface->format > 7) return ClearResult::unsupported_buffers;
    if (surface->byte_size == 0 || !memory.host_ptr(surface->pixels, surface->byte_size))
        return ClearResult::invalid_memory;
    std::vector<u8> pixels(surface->byte_size);
    if (!memory.read_bytes(surface->pixels, pixels.data(), surface->byte_size))
        return ClearResult::invalid_memory;
    if (!clear_surface_color(pixels, surface->format, surface->stride, surface->bounds,
                             context.clear_rectangle(), context.clear_color(), context.color_mask()))
        return ClearResult::invalid_memory;
    if (!memory.write_bytes(surface->pixels, pixels.data(), surface->byte_size))
        return ClearResult::invalid_memory;
    return ClearResult::complete;
}

} // namespace clickhle::classic

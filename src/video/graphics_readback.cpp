#include "graphics_api.h"
#include "clickhle/hle/call_frame.h"
#include <algorithm>
#include <stdexcept>

namespace clickhle {
u32 GraphicsApi::read_pixels(HleCallFrame &frame, const std::array<u32, 16> &a) {
    const u32 width = a[2], height = a[3];
    if (static_cast<i32>(width) < 0 || static_cast<i32>(height) < 0 ||
        a[4] != 0x1908 || a[5] != 0x1401) {
        error_ = static_cast<i32>(width) < 0 || static_cast<i32>(height) < 0 ? 0x501 : 0x500;
        throw std::runtime_error("RetailOS graphics API #87 enters its fatal diagnostic handler");
    }
    if (!transport_.wait || !transport_.readback)
        throw std::runtime_error("Graphics readback requires a VC2 transport");
    queue({12, (a[0] & 0xffff) | (a[1] << 16),
               (width & 0xffff) | (height << 16), 0});
    finish();
    // RetailOS 0x10274fdc ignores the requested source offset. Every call
    // reads from the pointer published by VC2 at 0xc000000c, even per row.
    const uint64_t row_size = uint64_t(width) * 4;
    if (row_size > 0xffffffff || row_size * height > 0xffffffff)
        throw std::runtime_error("Graphics readback size overflow");
    const u32 row_bytes = static_cast<u32>(row_size);
    const u32 alignment = pack_alignment_;
    uint64_t destination = (uint64_t(a[6]) + alignment - 1) & ~uint64_t(alignment - 1);
    const auto store = [&](uint64_t address, std::span<const u8> bytes) {
        if (bytes.empty()) return;
        if (address > 0xffffffff || address + bytes.size() > 0x100000000ull ||
            !frame.memory.write_bytes(static_cast<u32>(address), bytes.data(), static_cast<u32>(bytes.size())))
            throw std::runtime_error("Invalid graphics readback destination");
    };
    if (!height) return 0;
    if (row_bytes % alignment) {
        // Preserve the original backwards-row alignment rule, including its
        // extra alignment-sized gap after an unaligned subtraction.
        destination += row_size * (height - 1);
        for (u32 row = 0; row < height; ++row) {
            store(destination, transport_.readback(row_bytes));
            destination -= row_bytes;
            if (destination % alignment)
                destination = (destination & ~uint64_t(alignment - 1)) - alignment;
        }
    } else if (width == 320) {
        const auto pixels = transport_.readback(row_bytes * height);
        for (u32 row = 0; row < height; ++row)
            store(destination + uint64_t(row) * row_bytes,
                  std::span(pixels).subspan(uint64_t(height - row - 1) * row_bytes, row_bytes));
    } else {
        destination += row_size * (height - 1);
        for (u32 row = 0; row < height; ++row) {
            store(destination, transport_.readback(row_bytes));
            destination -= row_bytes;
        }
    }
    return 0;
}
} // namespace clickhle

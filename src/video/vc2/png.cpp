// A small RGB PNG writer using uncompressed DEFLATE blocks.
#include "png.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <vector>
namespace clickhle::vc2 {
namespace {
void big(std::vector<u8> &out, u32 value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<u8>(value >> shift));
}
void chunk(std::vector<u8> &out, const char *type, std::span<const u8> data) {
    big(out, static_cast<u32>(data.size()));
    const auto start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    u32 crc = ~0u;
    for (std::size_t i = start; i < out.size(); ++i) {
        crc ^= out[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    big(out, ~crc);
}
} // namespace
void write_rgb_png(const std::filesystem::path &path, u32 width, u32 height,
                   std::span<const u8> rgb) {
    if (static_cast<u64>(width) * height * 3 != rgb.size())
        throw std::runtime_error("Invalid RGB framebuffer dimensions");
    std::vector<u8> scan;
    scan.reserve(rgb.size() + height);
    for (u32 row = 0; row < height; ++row) {
        scan.push_back(0);
        auto first = rgb.begin() + static_cast<std::ptrdiff_t>(row * width * 3);
        scan.insert(scan.end(), first, first + width * 3);
    }
    std::vector<u8> z{0x78, 0x01};
    for (std::size_t offset = 0; offset < scan.size();) {
        u16 count = static_cast<u16>(std::min<std::size_t>(65535, scan.size() - offset));
        z.push_back(offset + count == scan.size() ? 1 : 0);
        z.push_back(static_cast<u8>(count));
        z.push_back(static_cast<u8>(count >> 8));
        u16 inverse = static_cast<u16>(~count);
        z.push_back(static_cast<u8>(inverse));
        z.push_back(static_cast<u8>(inverse >> 8));
        z.insert(z.end(), scan.begin() + static_cast<std::ptrdiff_t>(offset),
                 scan.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;
    }
    u32 a = 1, b = 0;
    for (u8 byte : scan) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    big(z, (b << 16) | a);
    std::vector<u8> png{137, 80, 78, 71, 13, 10, 26, 10}, header;
    big(header, width);
    big(header, height);
    header.insert(header.end(), {8, 2, 0, 0, 0});
    chunk(png, "IHDR", header);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(png.data()),
                 static_cast<std::streamsize>(png.size()));
    if (!output)
        throw std::runtime_error("Could not save VC2 PNG");
}
} // namespace clickhle::vc2

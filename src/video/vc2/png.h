#pragma once
#include "clickhle/common/types.h"
#include <filesystem>
#include <span>
namespace clickhle::vc2 {
void write_rgb_png(const std::filesystem::path &path, u32 width, u32 height,
                   std::span<const u8> rgb);
}

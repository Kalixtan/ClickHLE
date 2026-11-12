#pragma once

#include <filesystem>

namespace clickhle {

// Validates and installs an IPG/ZIP package into the virtual iPod drive.
// Throws std::runtime_error with a user-facing reason on failure.
void install_game_package(const std::filesystem::path& package,
                          const std::filesystem::path& hdd,
                          const std::filesystem::path& keys);

} // namespace clickhle

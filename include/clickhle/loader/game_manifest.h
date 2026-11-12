#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "clickhle/common/types.h"

namespace clickhle {

struct GameVariant {
    u32 platform_id = 0;
    u32 platform_version = 0;
    std::filesystem::path executable;
};

struct InstalledGame {
    std::string guid;
    std::string name;
    std::filesystem::path directory;
    std::vector<GameVariant> variants;
};

// Reads the small, XML-based Platforms section shipped with installed games.
// A missing or malformed manifest returns no game.
[[nodiscard]] std::optional<InstalledGame>
read_game_manifest(const std::filesystem::path &directory);

[[nodiscard]] std::vector<InstalledGame>
scan_installed_games(const std::filesystem::path &hdd_root);

[[nodiscard]] std::vector<u32> platforms_for_executable(const std::filesystem::path &executable);

} // namespace clickhle

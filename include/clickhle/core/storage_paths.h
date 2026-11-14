#pragma once

#include <filesystem>
#include <string>

namespace clickhle {

struct StoragePaths {
    std::filesystem::path device_root;
    std::filesystem::path game_data;
    std::filesystem::path game_stats;
    std::filesystem::path shared_data;
};

// An installed game keeps its Games_RO directory name as its save ID. The
// frontend's HDD is the active device, even when the executable came from a
// different copy of that drive.
[[nodiscard]] StoragePaths storage_paths_for_game(
    const std::filesystem::path& executable,
    const std::filesystem::path& hdd_root);

// Create the empty directory tree expected by the game-facing iPod filesystem.
// A minimal empty iTunesDB accepted by ClickHLE's reader is created only when
// the device has no database yet; existing device data is never replaced.
[[nodiscard]] bool initialize_ipod_hdd(const std::filesystem::path& hdd_root,
                                       std::string* error = nullptr);

} // namespace clickhle

#include "clickhle/core/storage_paths.h"

namespace clickhle {

StoragePaths storage_paths_for_game(const std::filesystem::path& executable,
                                    const std::filesystem::path& hdd_root) {
    const auto directory = executable.parent_path();
    const auto game_root = directory.filename() == "Executables"
        ? directory.parent_path() : directory;
    const auto games_root = game_root.parent_path();
    const auto control_root = games_root.parent_path();
    const bool installed = games_root.filename() == "Games_RO" &&
                           control_root.filename() == "iPod_Control";
    std::filesystem::path device_root = hdd_root;
    if (device_root.empty()) {
        device_root = installed ? control_root.parent_path()
                                : std::filesystem::current_path() / "hdd";
    }
    const auto title_id = installed ? game_root.filename() : executable.stem();
    const auto control = device_root / "iPod_Control";
    return {
        device_root,
        control / "GameData_RW" / title_id,
        control / "GameStats_WO" / title_id,
        control / "GameData_ShareRW",
    };
}

} // namespace clickhle

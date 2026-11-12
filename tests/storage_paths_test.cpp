#include "clickhle/core/storage_paths.h"
#include "clickhle/hle/vfs.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("clickhle-storage-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto source = root / "source" / "iPod_Control" / "Games_RO" /
        "88908" / "Executables" / "Minigolf.bin";
    const auto hdd = root / "hdd";
    const auto paths = clickhle::storage_paths_for_game(source, hdd);
    const auto control = hdd / "iPod_Control";
    bool valid = paths.device_root == hdd &&
        paths.game_data == control / "GameData_RW" / "88908" &&
        paths.game_stats == control / "GameStats_WO" / "88908" &&
        paths.shared_data == control / "GameData_ShareRW";

    const auto loose = clickhle::storage_paths_for_game(root / "games" / "Demo.bin", hdd);
    valid = valid && loose.game_data == control / "GameData_RW" / "Demo";

    clickhle::Vfs vfs;
    vfs.set_save_root(paths.game_data);
    constexpr std::array<char, 4> data{'s', 'a', 'v', 'e'};
    const auto writer = vfs.open("Save.DAT", clickhle::Vfs::Mode::kUpdate, true,
                                 clickhle::Vfs::Namespace::kData);
    valid = valid && writer != 0 && vfs.write(writer, data.data(), 4) == 4;
    valid = valid && vfs.close(writer);
    valid = valid && vfs.open("Save.DAT", clickhle::Vfs::Mode::kWrite, false,
                               clickhle::Vfs::Namespace::kData) == 0;
    const auto reader = vfs.open("save.dat", clickhle::Vfs::Mode::kRead, false,
                                 clickhle::Vfs::Namespace::kData);
    std::array<char, 4> loaded{};
    valid = valid && reader != 0 && vfs.read(reader, loaded.data(), 4) == 4 &&
        loaded == data;
    valid = valid && vfs.close(reader) &&
        fs::exists(paths.game_data / "Save.DAT") &&
        !fs::exists(root / "source" / "iPod_Control" / "GameData_RW");
    fs::remove_all(root);
    if (!valid) std::cerr << "save path or save readback failed\n";
    return valid ? 0 : 1;
}

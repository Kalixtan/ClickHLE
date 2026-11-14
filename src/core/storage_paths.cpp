#include "clickhle/core/storage_paths.h"

#include <array>
#include <fstream>
#include <string_view>

namespace clickhle {
namespace {

using EmptyDatabase = std::array<unsigned char, 88>;

void write_u32(EmptyDatabase& bytes, std::size_t offset, unsigned value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<unsigned char>(value >> (i * 8));
}

void write_record(EmptyDatabase& bytes, std::size_t offset,
                  std::string_view tag, unsigned header, unsigned total) {
    for (std::size_t i = 0; i < tag.size(); ++i)
        bytes[offset + i] = static_cast<unsigned char>(tag[i]);
    write_u32(bytes, offset + 4, header);
    write_u32(bytes, offset + 8, total);
}

EmptyDatabase make_empty_music_database() {
    EmptyDatabase bytes{};
    write_record(bytes, 0, "mhbd", 32, static_cast<unsigned>(bytes.size()));
    write_record(bytes, 32, "mhsd", 16, 28);
    write_u32(bytes, 44, 1);
    write_record(bytes, 48, "mhlt", 12, 12);
    write_u32(bytes, 56, 0);
    write_record(bytes, 60, "mhsd", 16, 28);
    write_u32(bytes, 72, 2);
    write_record(bytes, 76, "mhlp", 12, 12);
    write_u32(bytes, 84, 0);
    return bytes;
}

} // namespace

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

bool initialize_ipod_hdd(const std::filesystem::path& hdd_root,
                         std::string* error) {
    auto fail = [error](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (error) error->clear();
    if (hdd_root.empty()) return fail("HDD path is empty");

    const auto control = hdd_root / "iPod_Control";
    for (const auto& directory : {
             control / "Games_RO", control / "GameData_RW",
             control / "GameStats_WO", control / "GameData_ShareRW",
             control / "iTunes", control / "Music"}) {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) return fail("cannot create " + directory.string() + ": " + ec.message());
    }

    const auto database = control / "iTunes" / "iTunesDB";
    std::error_code ec;
    if (std::filesystem::exists(database, ec)) {
        if (ec) return fail("cannot inspect " + database.string() + ": " + ec.message());
        return true;
    }
    if (ec) return fail("cannot inspect " + database.string() + ": " + ec.message());

    const auto bytes = make_empty_music_database();
    std::ofstream file(database, std::ios::binary | std::ios::out);
    if (!file) return fail("cannot create " + database.string());
    file.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    if (!file) return fail("cannot write " + database.string());
    return true;
}

} // namespace clickhle

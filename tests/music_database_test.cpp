#include "clickhle/loader/music_database.h"
#include "clickhle/hle/vfs.h"

#undef NDEBUG
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

using clickhle::u8;
using clickhle::u32;

void word(std::vector<u8>& bytes, std::size_t at, u32 value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[at + i] = static_cast<u8>(value >> (i * 8));
}

std::size_t start(std::vector<u8>& bytes, const char* tag, u32 header) {
    const auto at = bytes.size();
    bytes.resize(at + header, 0);
    for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<u8>(tag[i]);
    word(bytes, at + 4, header);
    return at;
}

void finish(std::vector<u8>& bytes, std::size_t at) {
    word(bytes, at + 8, static_cast<u32>(bytes.size() - at));
}

void string(std::vector<u8>& bytes, u32 type, const char* value) {
    const auto at = start(bytes, "mhod", 24);
    word(bytes, at + 12, type);
    const auto payload = bytes.size();
    bytes.resize(payload + 16, 0);
    word(bytes, payload, 2); // UTF-8
    u32 length = 0;
    while (value[length]) bytes.push_back(static_cast<u8>(value[length++]));
    word(bytes, payload + 4, length);
    finish(bytes, at);
}

} // namespace

int main() {
    std::vector<u8> bytes;
    const auto root = start(bytes, "mhbd", 32);
    const auto tracks = start(bytes, "mhsd", 16);
    word(bytes, tracks + 12, 1);
    const auto track_list = start(bytes, "mhlt", 12);
    word(bytes, track_list + 8, 1);
    const auto track = start(bytes, "mhit", 0x9c);
    word(bytes, track + 12, 2);
    word(bytes, track + 16, 42);
    word(bytes, track + 40, 3210);
    word(bytes, track + 112, 0x1234);
    string(bytes, 1, "Song");
    string(bytes, 2, ":iPod_Control:Music:F00:song.mp3");
    finish(bytes, track);
    finish(bytes, tracks);

    const auto playlists = start(bytes, "mhsd", 16);
    word(bytes, playlists + 12, 2);
    const auto playlist_list = start(bytes, "mhlp", 12);
    word(bytes, playlist_list + 8, 2);
    for (unsigned i = 0; i < 2; ++i) {
        const auto playlist = start(bytes, "mhyp", 36);
        word(bytes, playlist + 12, 1);
        word(bytes, playlist + 16, i == 0 ? 0 : 2);
        word(bytes, playlist + 20, i == 0 ? 1 : 0);
        string(bytes, 1, i == 0 ? "Library" : "Games");
        if (i == 1) {
            for (u32 member_index = 0; member_index < 2; ++member_index) {
                const auto member = start(bytes, "mhip", 36);
                word(bytes, member + 12, 1);
                word(bytes, member + 24, 43 - member_index);
                const auto order = start(bytes, "mhod", 24);
                word(bytes, order + 12, 100);
                bytes.resize(bytes.size() + 4);
                word(bytes, order + 24, 2 - member_index);
                finish(bytes, order);
                finish(bytes, member);
            }
        }
        finish(bytes, playlist);
    }
    finish(bytes, playlists);
    finish(bytes, root);

    const auto path = std::filesystem::temp_directory_path() /
        ("clickhle-musicdb-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    clickhle::MusicDatabase db;
    std::string error;
    const bool loaded = db.load(path, &error);
    std::filesystem::remove(path);
    assert(loaded && error.empty());
    assert(db.tracks.size() == 1 && db.tracks[0].id == 42);
    assert(db.tracks[0].title == "Song" && db.tracks[0].duration_ms == 3210);
    assert(db.tracks[0].path == ":iPod_Control:Music:F00:song.mp3");
    assert(db.playlists.size() == 2 && db.playlists[0].master);
    assert(db.playlists[1].title == "Games");
    assert((db.playlists[1].track_ids == std::vector<u32>{42, 43}));

    bytes[0] = 'x';
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    assert(!db.load(path, &error) && db.tracks.empty() && db.playlists.empty());
    std::filesystem::remove(path);

    const auto volume = path.string() + "-hdd";
    const auto music = std::filesystem::path(volume) /
        "iPod_Control" / "Music" / "F00";
    std::filesystem::create_directories(music);
    {
        std::ofstream out(music / "Song.MP3");
        out << "sample";
    }
    clickhle::Vfs vfs;
    vfs.set_device_root(volume);
    assert(vfs.resolve_media(":iPod_Control:Music:f00:song.mp3") ==
           music / "Song.MP3");
    assert(vfs.resolve_media(":iPod_Control:Music:..:private").empty());
    std::filesystem::remove_all(volume);
}

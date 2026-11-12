#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "clickhle/common/types.h"

namespace clickhle {

/// The media index stored at iPod_Control/iTunes/iTunesDB. File names remain
/// device paths; callers resolve them against the mounted iPod volume.
struct MusicDatabase {
    struct Track {
        u32 id = 0;
        u64 persistent_id = 0;
        u32 duration_ms = 0;
        std::string title, path, album, artist, genre;
    };
    struct Playlist {
        u64 persistent_id = 0;
        bool master = false;
        std::string title;
        std::vector<u32> track_ids;
    };

    std::vector<Track> tracks;
    std::vector<Playlist> playlists;

    /// Returns false for a missing or malformed database. No partial data is
    /// published if parsing fails.
    bool load(const std::filesystem::path& path, std::string* error = nullptr);
};

}  // namespace clickhle

#include "clickhle/loader/music_database.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string_view>
#include <utility>

namespace clickhle {
namespace {

struct Record {
    std::size_t start = 0, header_end = 0, end = 0;
};

class Reader {
public:
    explicit Reader(const std::vector<u8>& bytes) : bytes_(bytes) {}

    bool in(std::size_t at, std::size_t length, std::size_t end) const {
        return at <= end && length <= end - at && end <= bytes_.size();
    }
    u32 word(std::size_t at) const {
        return static_cast<u32>(bytes_[at]) |
               static_cast<u32>(bytes_[at + 1]) << 8 |
               static_cast<u32>(bytes_[at + 2]) << 16 |
               static_cast<u32>(bytes_[at + 3]) << 24;
    }
    u64 double_word(std::size_t at) const {
        return static_cast<u64>(word(at)) | static_cast<u64>(word(at + 4)) << 32;
    }
    bool tag(std::size_t at, std::string_view expected) const {
        return in(at, 4, bytes_.size()) &&
               std::memcmp(bytes_.data() + at, expected.data(), 4) == 0;
    }
    bool record(std::size_t at, std::size_t end, std::string_view expected,
                Record& out, std::size_t min_header = 12) const {
        if (!in(at, 12, end) || !tag(at, expected)) return false;
        const u32 header = word(at + 4), total = word(at + 8);
        if (header < min_header || total < header || !in(at, total, end)) return false;
        out = {at, at + header, at + total};
        return true;
    }
    std::string text(const Record& record) const {
        // mhod's payload is: encoding, byte length, two reserved words, text.
        if (!in(record.header_end, 16, record.end)) return {};
        const u32 encoding = word(record.header_end);
        const u32 length = word(record.header_end + 4);
        const std::size_t begin = record.header_end + 16;
        if (!in(begin, length, record.end)) return {};
        if (encoding == 2) {
            return {reinterpret_cast<const char*>(bytes_.data() + begin), length};
        }
        if (encoding != 0 && encoding != 1) return {};
        // UTF-16LE in older databases. Preserve non-ASCII text as UTF-8.
        std::string result;
        for (std::size_t i = 0; i + 1 < length; i += 2) {
            u32 code = static_cast<u32>(bytes_[begin + i]) |
                       static_cast<u32>(bytes_[begin + i + 1]) << 8;
            if (code >= 0xd800 && code <= 0xdbff && i + 3 < length) {
                const u32 low = static_cast<u32>(bytes_[begin + i + 2]) |
                                static_cast<u32>(bytes_[begin + i + 3]) << 8;
                if (low >= 0xdc00 && low <= 0xdfff) {
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                    i += 2;
                }
            }
            if (code < 0x80) result.push_back(static_cast<char>(code));
            else if (code < 0x800) {
                result.push_back(static_cast<char>(0xc0 | (code >> 6)));
                result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
            } else if (code < 0x10000) {
                result.push_back(static_cast<char>(0xe0 | (code >> 12)));
                result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
            } else {
                result.push_back(static_cast<char>(0xf0 | (code >> 18)));
                result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
                result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
            }
        }
        return result;
    }

private:
    const std::vector<u8>& bytes_;
};

bool parse_tracks(const Reader& read, const Record& list, MusicDatabase& db) {
    std::size_t at = list.header_end;
    const u32 count = read.word(list.start + 8); // mhlt has no total length
    if (count > 1'000'000) return false;
    for (u32 i = 0; i < count; ++i) {
        Record track;
        if (!read.record(at, list.end, "mhit", track, 0x9c)) return false;
        MusicDatabase::Track item;
        item.id = read.word(at + 16);
        item.duration_ms = read.word(at + 40);
        item.persistent_id = read.double_word(at + 112);
        std::size_t child = track.header_end;
        const u32 children = read.word(at + 12);
        if (children > 4096) return false;
        for (u32 j = 0; j < children; ++j) {
            Record data;
            if (!read.record(child, track.end, "mhod", data, 24)) return false;
            const u32 type = read.word(child + 12);
            switch (type) {
            case 1: item.title = read.text(data); break;
            case 2: item.path = read.text(data); break;
            case 3: item.album = read.text(data); break;
            case 4: item.artist = read.text(data); break;
            case 5: item.genre = read.text(data); break;
            default: break;
            }
            child = data.end;
        }
        db.tracks.push_back(std::move(item));
        at = track.end;
    }
    return true;
}

bool parse_playlists(const Reader& read, const Record& list, MusicDatabase& db) {
    std::size_t at = list.header_end;
    const u32 count = read.word(list.start + 8); // mhlp has no total length
    if (count > 100'000) return false;
    for (u32 i = 0; i < count; ++i) {
        Record playlist;
        if (!read.record(at, list.end, "mhyp", playlist, 36)) return false;
        MusicDatabase::Playlist item;
        item.master = read.word(at + 20) != 0;
        item.persistent_id = read.double_word(at + 28);
        std::size_t child = playlist.header_end;
        const u32 strings = read.word(at + 12);
        const u32 members = read.word(at + 16);
        if (strings > 4096 || members > 1'000'000) return false;
        for (u32 j = 0; j < strings; ++j) {
            Record data;
            if (!read.record(child, playlist.end, "mhod", data, 24)) return false;
            if (read.word(child + 12) == 1) item.title = read.text(data);
            child = data.end;
        }
        std::vector<std::pair<u32, u32>> ordered_members;
        for (u32 j = 0; j < members; ++j) {
            Record member;
            if (!read.record(child, playlist.end, "mhip", member, 36)) return false;
            u32 position = j;
            std::size_t extra = member.header_end;
            const u32 extra_count = read.word(child + 12);
            if (extra_count > 4096) return false;
            for (u32 k = 0; k < extra_count; ++k) {
                Record data;
                if (!read.record(extra, member.end, "mhod", data, 24)) return false;
                if (read.word(extra + 12) == 100 &&
                    read.in(extra + 24, 4, data.end))
                    position = read.word(extra + 24);
                extra = data.end;
            }
            ordered_members.emplace_back(position, read.word(child + 24));
            child = member.end;
        }
        std::stable_sort(ordered_members.begin(), ordered_members.end(),
                         [](const auto& a, const auto& b) {
                             return a.first < b.first;
                         });
        for (const auto& [position, id] : ordered_members) {
            (void)position;
            item.track_ids.push_back(id);
        }
        db.playlists.push_back(std::move(item));
        at = playlist.end;
    }
    return true;
}

} // namespace

bool MusicDatabase::load(const std::filesystem::path& path, std::string* error) {
    tracks.clear();
    playlists.clear();
    auto fail = [&](const char* reason) {
        tracks.clear(); playlists.clear();
        if (error) *error = reason;
        return false;
    };
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return fail("iTunesDB is missing");
    const auto length = file.tellg();
    if (length < 12 || length > 128 * 1024 * 1024) return fail("invalid iTunesDB size");
    std::vector<u8> bytes(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), length))
        return fail("cannot read iTunesDB");
    const Reader read(bytes);
    Record root;
    if (!read.record(0, bytes.size(), "mhbd", root, 0x20))
        return fail("invalid iTunesDB header");
    std::size_t at = root.header_end;
    while (at < root.end) {
        Record dataset;
        if (!read.record(at, root.end, "mhsd", dataset, 16))
            return fail("invalid iTunesDB dataset");
        const u32 type = read.word(at + 12);
        const std::size_t list_at = dataset.header_end;
        if (type == 1 && read.tag(list_at, "mhlt")) {
            if (!read.in(list_at, 12, dataset.end))
                return fail("invalid iTunesDB track list");
            const u32 header = read.word(list_at + 4);
            if (header < 12 || !read.in(list_at, header, dataset.end))
                return fail("invalid iTunesDB track list");
            if (!parse_tracks(read, {list_at, list_at + header, dataset.end}, *this))
                return fail("invalid iTunesDB track");
        } else if (type == 2 && read.tag(list_at, "mhlp")) {
            if (!read.in(list_at, 12, dataset.end))
                return fail("invalid iTunesDB playlist list");
            const u32 header = read.word(list_at + 4);
            if (header < 12 || !read.in(list_at, header, dataset.end))
                return fail("invalid iTunesDB playlist list");
            if (!parse_playlists(read, {list_at, list_at + header, dataset.end}, *this))
                return fail("invalid iTunesDB playlist");
        }
        at = dataset.end;
    }
    if (error) error->clear();
    return true;
}

} // namespace clickhle

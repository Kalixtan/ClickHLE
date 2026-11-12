#pragma once

#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "clickhle/common/types.h"

namespace clickhle {

/// The guest's view of the filesystem.
///
/// A click-wheel game asks for plain relative names -- Lost and the Sims titles
/// open `"rserver.bin"`, Sudoku opens `"savefile.dat"` -- and those live in the
/// **game directory**, alongside `Executables/` rather than inside it. That is
/// the installed layout iTunes produces:
///
///     Games_RO/<5-hex-id>/
///         rserver.bin              <- what the game asks for
///         soundbank_*.dat
///         Executables/<Name>.bin   <- the image we load
///
/// So the root is the *parent* of the directory holding the executable. Rooting
/// at the executable's own directory, which is the obvious default, puts every
/// asset one level out of reach.
///
/// Everything is confined beneath the root. A guest path that climbs out with
/// `..`, or that is absolute, is refused rather than resolved -- a game is not
/// supposed to reach the rest of the host and there is no reason to let a
/// malformed or hostile path try.
class Vfs {
public:
    /// `executable` is the path the emulator was given. The root is derived
    /// from it, as described above.
    void mount_for_executable(const std::filesystem::path& executable);

    /// Mounts an explicit root, for tests and for `--content-root`.
    void mount(std::filesystem::path root);

    /// GameData_RW/<game-id>, or a standalone executable's save directory.
    void set_save_root(std::filesystem::path root);
    void set_stats_root(std::filesystem::path root);
    void set_shared_root(std::filesystem::path root);

    enum class Namespace { kGame, kData, kStats, kShared, kDevice };

    /// The root of the emulated iPod volume (contains iPod_Control).
    void set_device_root(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path& save_root() const noexcept {
        return save_root_;
    }
    [[nodiscard]] const std::filesystem::path& device_root() const noexcept {
        return device_root_;
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

    /// Maps a guest path to a host path, or returns empty if it escapes the
    /// root or is malformed.
    ///
    /// Case-insensitive on the host, deliberately: the iPod's filesystem is
    /// FAT and the games were built against it, so a title asking for
    /// `"RSERVER.BIN"` must find `rserver.bin`. A Linux host would not.
    [[nodiscard]] std::filesystem::path resolve(
        std::string guest_path, Namespace name_space = Namespace::kGame) const;

    /// Resolve a media path from iTunesDB (colon-separated, volume-relative).
    [[nodiscard]] std::filesystem::path resolve_media(std::string device_path) const;

    [[nodiscard]] const std::filesystem::path& root_for(Namespace name_space) const noexcept;

    /// Resolve beneath a specific root, honouring the same confinement and
    /// case rules.
    [[nodiscard]] std::filesystem::path resolve_under(
        const std::filesystem::path& root, std::string guest_path) const;

    /// kUpdate is what a save file is opened with: read *and* write, created if
    /// absent, and never truncated.
    ///
    /// The distinction is not academic. Every title that opens a save --
    /// PAC-MAN, Mini Golf, Sudoku, Royal Solitaire, Mahjong -- hands the open a
    /// destination buffer that is empty at the time of the call (Sudoku's is
    /// still full of the 0xA5 fill pattern), so the open is a *load*. Treating
    /// it as a truncating write both destroyed the save and denied the game the
    /// data it asked for, which is why no save could ever round-trip.
    enum class Mode { kRead, kWrite, kUpdate };

    /// Opens a guest path and returns a handle, or 0 on failure.
    [[nodiscard]] u32 open(const std::string& guest_path, Mode mode,
                           bool allow_write = true,
                           Namespace name_space = Namespace::kGame);

    [[nodiscard]] bool close(u32 handle);

    /// Remove an existing saved file or empty directory, never a game asset.
    [[nodiscard]] bool remove_saved(const std::string& guest_path,
                                    Namespace name_space = Namespace::kData);

    /// Bytes read, or -1 if the handle is unknown.
    [[nodiscard]] i64 read(u32 handle, void* buffer, u32 length);
    [[nodiscard]] i64 write(u32 handle, const void* buffer, u32 length);
    [[nodiscard]] bool writable(u32 handle) const;

    /// Size in bytes, or -1 if the handle is unknown.
    [[nodiscard]] i64 size(u32 handle) const;

    [[nodiscard]] bool seek(u32 handle, i64 offset);
    [[nodiscard]] i64 tell(u32 handle) const;

    [[nodiscard]] std::size_t open_count() const noexcept {
        return files_.size();
    }

    /// Guest paths that could not be resolved, in first-seen order.
    ///
    /// Worth collecting rather than only logging: a title failing to render is
    /// usually a title that could not find something, and the list of what it
    /// asked for and did not get is the first question to ask.
    [[nodiscard]] const std::vector<std::string>& missing() const noexcept {
        return missing_;
    }

private:
    struct OpenFile {
        std::FILE* handle = nullptr;
        std::string guest_path;
        Mode mode = Mode::kRead;
        bool allow_write = false;
    };

    std::filesystem::path root_;
    std::filesystem::path save_root_;
    std::filesystem::path stats_root_;
    std::filesystem::path shared_root_;
    std::filesystem::path device_root_;
    std::map<u32, OpenFile> files_;
    std::vector<std::string> missing_;
    /// Handles start above zero because the guest tests for zero as failure.
    u32 next_handle_ = 1;
};

}  // namespace clickhle

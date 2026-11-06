#include "clickhle/hle/vfs.h"

#include <algorithm>
#include <cctype>

#include "clickhle/common/log.h"

namespace clickhle {
namespace {

constexpr std::string_view kCategory = "vfs";

std::string to_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

/// Finds `name` in `directory` ignoring case.
///
/// The iPod's filesystem is FAT, so the games were built against
/// case-insensitive lookup and some ask for a name that does not match the
/// stored spelling. A Linux host would simply not find it, and the failure
/// would look like a missing asset rather than a naming mismatch.
std::filesystem::path find_insensitive(const std::filesystem::path& directory,
                                       const std::string& name) {
    std::error_code error;
    const std::filesystem::path exact = directory / name;
    if (std::filesystem::exists(exact, error)) {
        return exact;
    }
    const std::string wanted = to_lower(name);
    for (std::filesystem::directory_iterator it(directory, error), end;
         it != end && !error; it.increment(error)) {
        if (to_lower(it->path().filename().string()) == wanted) {
            return it->path();
        }
    }
    return {};
}

}  // namespace

void Vfs::mount_for_executable(const std::filesystem::path& executable) {
    // <game>/Executables/<name>.bin -> <game>. The assets a title asks for by
    // bare name sit beside Executables/, not inside it.
    std::filesystem::path directory = executable.parent_path();
    if (directory.filename() == "Executables") {
        directory = directory.parent_path();
    }
    mount(directory.empty() ? std::filesystem::current_path() : directory);
}

void Vfs::mount(std::filesystem::path root) {
    std::error_code error;
    root_ = std::filesystem::weakly_canonical(root, error);
    if (error || root_.empty()) {
        root_ = std::move(root);
    }
    CLICKHLE_INFO(kCategory) << "root " << root_.string();
}

void Vfs::set_save_root(std::filesystem::path root) {
    std::error_code error;
    std::filesystem::create_directories(root, error);
    save_root_ = std::filesystem::weakly_canonical(root, error);
    if (error || save_root_.empty()) {
        save_root_ = std::move(root);
    }
    CLICKHLE_INFO(kCategory) << "saves " << save_root_.string();
}

std::filesystem::path Vfs::resolve(std::string guest_path) const {
    // A written file shadows the shipped one: a save is found where the game
    // left it, not where the game was installed.
    if (!save_root_.empty()) {
        std::filesystem::path saved = resolve_under(save_root_, guest_path);
        if (!saved.empty()) {
            return saved;
        }
    }
    return resolve_under(root_, guest_path);
}

std::filesystem::path Vfs::resolve_under(const std::filesystem::path& root,
                                         std::string guest_path) const {
    if (guest_path.empty() || root.empty()) {
        return {};
    }
    // The guest may spell separators either way; normalise before splitting so
    // a backslash cannot smuggle a component past the checks below.
    std::replace(guest_path.begin(), guest_path.end(), '\\', '/');

    std::filesystem::path current = root;
    std::size_t start = 0;
    while (start <= guest_path.size()) {
        const std::size_t slash = guest_path.find('/', start);
        const std::string part =
            guest_path.substr(start, slash == std::string::npos
                                         ? std::string::npos
                                         : slash - start);
        start = slash == std::string::npos ? guest_path.size() + 1 : slash + 1;
        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            // Refused outright rather than resolved and then range-checked.
            // Nothing legitimate needs it, and a path that tries is either
            // malformed or hostile.
            CLICKHLE_WARN(kCategory)
                << "refusing '" << guest_path << "': escapes the game root";
            return {};
        }
        const std::filesystem::path found = find_insensitive(current, part);
        if (found.empty()) {
            return {};
        }
        current = found;
    }
    return current;
}

u32 Vfs::open(const std::string& guest_path, Mode mode, bool allow_write) {
    // Writes never touch the game directory. Resolving first and then writing
    // wherever the file happened to be found would modify the user's copy the
    // moment a title saved over something it shipped with.
    if (mode == Mode::kWrite || mode == Mode::kUpdate) {
        const std::filesystem::path base =
            save_root_.empty() ? root_ : save_root_;
        std::string leaf = guest_path;
        std::replace(leaf.begin(), leaf.end(), '\\', '/');
        if (leaf.find("..") != std::string::npos || leaf.front() == '/') {
            CLICKHLE_WARN(kCategory)
                << "refusing to write '" << guest_path << "'";
            return 0;
        }
        const std::filesystem::path host = base / leaf;
        std::error_code error;
        std::filesystem::create_directories(host.parent_path(), error);
        // "r+b" keeps what is already there, which is the whole point of an
        // update open; it only fails when the file does not exist yet, and then
        // "w+b" creates it. Never "wb" -- that truncates a save on open.
        std::FILE* file = nullptr;
        if (mode == Mode::kUpdate) {
            file = std::fopen(host.string().c_str(), "r+b");
        }
        if (file == nullptr) {
            file = std::fopen(host.string().c_str(),
                              mode == Mode::kUpdate ? "w+b" : "wb");
        }
        if (file == nullptr) {
            CLICKHLE_WARN(kCategory)
                << "cannot write '" << guest_path << "' at " << host.string();
            return 0;
        }
        const u32 handle = next_handle_++;
        files_[handle] = OpenFile{file, guest_path, mode,
                                  allow_write && mode != Mode::kRead};
        CLICKHLE_DEBUG(kCategory)
            << "write '" << guest_path << "' -> " << host.string();
        return handle;
    }

    std::filesystem::path host = resolve(guest_path);
    if (host.empty()) {
        if (mode == Mode::kRead) {
            if (std::find(missing_.begin(), missing_.end(), guest_path) ==
                missing_.end()) {
                missing_.push_back(guest_path);
            }
            CLICKHLE_INFO(kCategory) << "missing '" << guest_path << "'";
            return 0;
        }
        // A file being written need not exist yet. Only the directory has to,
        // and resolve() cannot find a name that is not there, so build the
        // host path from the parent instead.
        const std::size_t slash = guest_path.find_last_of("/\\");
        const std::string leaf = slash == std::string::npos
                                     ? guest_path
                                     : guest_path.substr(slash + 1);
        const std::filesystem::path parent =
            slash == std::string::npos
                ? root_
                : resolve(guest_path.substr(0, slash));
        if (parent.empty() || leaf.empty()) {
            return 0;
        }
        host = parent / leaf;
    }

    std::FILE* file = std::fopen(host.string().c_str(),
                                 mode == Mode::kRead ? "rb" : "wb");
    if (file == nullptr) {
        CLICKHLE_WARN(kCategory)
            << "cannot open '" << guest_path << "' at " << host.string();
        return 0;
    }
    const u32 handle = next_handle_++;
    files_[handle] = OpenFile{file, guest_path, mode,
                              allow_write && mode != Mode::kRead};
    CLICKHLE_DEBUG(kCategory)
        << "open '" << guest_path << "' -> handle " << handle;
    return handle;
}

bool Vfs::close(u32 handle) {
    const auto it = files_.find(handle);
    if (it == files_.end()) {
        return false;
    }
    std::fclose(it->second.handle);
    files_.erase(it);
    return true;
}

i64 Vfs::read(u32 handle, void* buffer, u32 length) {
    const auto it = files_.find(handle);
    if (it == files_.end()) {
        return -1;
    }
    return static_cast<i64>(
        std::fread(buffer, 1, length, it->second.handle));
}

i64 Vfs::write(u32 handle, const void* buffer, u32 length) {
    const auto it = files_.find(handle);
    if (it == files_.end() || !it->second.allow_write) {
        return -1;
    }
    return static_cast<i64>(
        std::fwrite(buffer, 1, length, it->second.handle));
}

bool Vfs::writable(u32 handle) const {
    const auto it = files_.find(handle);
    return it != files_.end() && it->second.allow_write;
}

i64 Vfs::size(u32 handle) const {
    const auto it = files_.find(handle);
    if (it == files_.end()) {
        return -1;
    }
    std::FILE* file = it->second.handle;
    const long here = std::ftell(file);
    if (here < 0 || std::fseek(file, 0, SEEK_END) != 0) {
        return -1;
    }
    const long end = std::ftell(file);
    std::fseek(file, here, SEEK_SET);
    return end < 0 ? -1 : static_cast<i64>(end);
}

bool Vfs::seek(u32 handle, i64 offset) {
    const auto it = files_.find(handle);
    if (it == files_.end()) {
        return false;
    }
    return std::fseek(it->second.handle, static_cast<long>(offset),
                      SEEK_SET) == 0;
}

i64 Vfs::tell(u32 handle) const {
    const auto it = files_.find(handle);
    if (it == files_.end()) {
        return -1;
    }
    const long here = std::ftell(it->second.handle);
    return here < 0 ? -1 : static_cast<i64>(here);
}

}  // namespace clickhle

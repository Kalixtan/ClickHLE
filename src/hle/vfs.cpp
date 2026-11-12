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

bool valid_component(std::string_view part) {
    for (const char raw : part) {
        const auto c = static_cast<unsigned char>(raw);
        if (c < 32 || c == 127 ||
            std::string_view("\\:*?\"<>|").find(static_cast<char>(c)) != std::string_view::npos)
            return false;
    }
    return true;
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

std::filesystem::path prepare_directory(std::filesystem::path root) {
    std::error_code error;
    std::filesystem::create_directories(root, error);
    const auto canonical = std::filesystem::weakly_canonical(root, error);
    return error || canonical.empty() ? std::move(root) : canonical;
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
    save_root_ = prepare_directory(std::move(root));
    CLICKHLE_INFO(kCategory) << "saves " << save_root_.string();
}

void Vfs::set_stats_root(std::filesystem::path root) {
    stats_root_ = prepare_directory(std::move(root));
    CLICKHLE_INFO(kCategory) << "stats " << stats_root_.string();
}

void Vfs::set_shared_root(std::filesystem::path root) {
    shared_root_ = prepare_directory(std::move(root));
    CLICKHLE_INFO(kCategory) << "shared " << shared_root_.string();
}

void Vfs::set_device_root(std::filesystem::path root) {
    if (root.empty()) { device_root_.clear(); return; }
    std::error_code error;
    device_root_ = std::filesystem::weakly_canonical(root, error);
    if (error) device_root_ = std::move(root);
    CLICKHLE_INFO(kCategory) << "device " << device_root_.string();
}

const std::filesystem::path& Vfs::root_for(Namespace name_space) const noexcept {
    switch (name_space) {
    case Namespace::kGame: return root_;
    case Namespace::kData: return save_root_;
    case Namespace::kStats: return stats_root_;
    case Namespace::kShared: return shared_root_;
    case Namespace::kDevice: return device_root_;
    }
    return root_;
}

std::filesystem::path Vfs::resolve(std::string guest_path,
                                   Namespace name_space) const {
    return resolve_under(root_for(name_space), std::move(guest_path));
}

std::filesystem::path Vfs::resolve_media(std::string device_path) const {
    if (device_root_.empty()) return {};
    // iTunesDB stores paths as :iPod_Control:Music:Fxx:name.ext. A leading
    // colon denotes the volume root; never interpret it as a host path.
    if (!device_path.empty() && device_path.front() == ':') {
        std::replace(device_path.begin(), device_path.end(), ':', '/');
    }
    const auto first = device_path.find_first_not_of('/');
    if (first == std::string::npos) return {};
    device_path.erase(0, first);
    constexpr std::string_view prefix = "ipod_control";
    if (device_path.size() <= prefix.size() ||
        to_lower(device_path.substr(0, prefix.size())) != prefix ||
        device_path[prefix.size()] != '/') return {};
    return resolve(device_path, Namespace::kDevice);
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
        if (!valid_component(part)) return {};
        const std::filesystem::path found = find_insensitive(current, part);
        if (found.empty()) {
            return {};
        }
        std::error_code error;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(found, error)) || error)
            return {};
        current = found;
    }
    return current;
}

u32 Vfs::open(const std::string& guest_path, Mode mode, bool allow_write,
              Namespace name_space) {
    // Games_RO is immutable. Each writable namespace has its own root.
    if (mode != Mode::kRead && !allow_write) return 0;
    if ((name_space == Namespace::kGame || name_space == Namespace::kDevice) &&
        mode != Mode::kRead) return 0;
    const auto& base = root_for(name_space);
    if (base.empty()) return 0;
    if (mode == Mode::kWrite || mode == Mode::kUpdate) {
        std::string leaf = guest_path;
        std::replace(leaf.begin(), leaf.end(), '\\', '/');
        // Guest paths are rooted at the game's virtual filesystem. A leading
        // slash names that root, not the host root (Crystal Defenders opens
        // "/userdata" this way). Keep all writes under the save directory.
        const auto first = leaf.find_first_not_of('/');
        leaf = first == std::string::npos ? std::string{} : leaf.substr(first);
        if (leaf.empty() || leaf.find("..") != std::string::npos) {
            CLICKHLE_WARN(kCategory)
                << "refusing to write '" << guest_path << "'";
            return 0;
        }
        // Reuse each existing spelling in the save area. FAT treats Save.DAT
        // and save.dat as one file, including when a parent directory differs
        // only by case. Missing components keep the guest's spelling.
        std::filesystem::path host = base;
        std::size_t start = 0;
        while (start < leaf.size()) {
            const auto slash = leaf.find('/', start);
            const auto part = leaf.substr(start, slash == std::string::npos
                ? std::string::npos : slash - start);
            if (!part.empty() && part != ".") {
                if (!valid_component(part)) return 0;
                const auto existing = find_insensitive(host, part);
                std::error_code type_error;
                if (!existing.empty() &&
                    (std::filesystem::is_symlink(std::filesystem::symlink_status(existing, type_error)) ||
                     type_error)) return 0;
                host = existing.empty() ? host / part : existing;
            }
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
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

    std::filesystem::path host = resolve(guest_path, name_space);
    if (host.empty()) {
        if (std::find(missing_.begin(), missing_.end(), guest_path) ==
            missing_.end()) {
            missing_.push_back(guest_path);
        }
        CLICKHLE_INFO(kCategory) << "missing '" << guest_path << "'";
        return 0;
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

bool Vfs::remove_saved(const std::string& guest_path, Namespace name_space) {
    if (name_space == Namespace::kGame || name_space == Namespace::kDevice ||
        guest_path.empty()) return false;
    const auto& root = root_for(name_space);
    if (root.empty()) return false;
    const auto host = resolve_under(root, guest_path);
    if (host.empty() || host == root) return false;
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(host, error);
    if (error) return false;
    const auto relative = canonical.lexically_relative(root);
    if (relative.empty() || relative.begin() == relative.end() ||
        *relative.begin() == "..") return false;
    return std::filesystem::remove(host, error) && !error;
}

i64 Vfs::read(u32 handle, void* buffer, u32 length) {
    const auto it = files_.find(handle);
    if (it == files_.end()) {
        return -1;
    }
    // C update streams need a positioning operation when switching between
    // reads and writes. A zero-distance seek preserves the guest file offset.
    if (it->second.mode == Mode::kUpdate &&
        std::fseek(it->second.handle, 0, SEEK_CUR) != 0) return -1;
    return static_cast<i64>(
        std::fread(buffer, 1, length, it->second.handle));
}

i64 Vfs::write(u32 handle, const void* buffer, u32 length) {
    const auto it = files_.find(handle);
    if (it == files_.end() || !it->second.allow_write) {
        return -1;
    }
    if (it->second.mode == Mode::kUpdate &&
        std::fseek(it->second.handle, 0, SEEK_CUR) != 0) return -1;
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

#include "jit_cache.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>

#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA256.h>

namespace clickhle::vc2 {
namespace {
constexpr std::string_view kMagic = "VC2OBJ01";
constexpr std::uintmax_t kMaximumObjectSize = 16 * 1024 * 1024;

std::filesystem::path cache_directory() {
    if (const char *path = std::getenv("CLICKHLE_VC2_CACHE_DIR"))
        return *path ? std::filesystem::path(path) : std::filesystem::path{};
#ifdef _WIN32
    if (const char *path = std::getenv("LOCALAPPDATA"))
        return std::filesystem::path(path) / "ClickHLE" / "vc2";
#elif defined(__APPLE__)
    if (const char *path = std::getenv("HOME"))
        return std::filesystem::path(path) / "Library" / "Caches" / "ClickHLE" / "vc2";
#else
    if (const char *path = std::getenv("XDG_CACHE_HOME"))
        if (*path) return std::filesystem::path(path) / "clickhle" / "vc2";
    if (const char *path = std::getenv("HOME"))
        return std::filesystem::path(path) / ".cache" / "clickhle" / "vc2";
#endif
    return {};
}

std::string digest(llvm::StringRef bytes) {
    const auto hash = llvm::SHA256::hash(llvm::ArrayRef<std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()));
    return {reinterpret_cast<const char *>(hash.data()), hash.size()};
}
} // namespace

JitObjectCache::JitObjectCache() : directory_(cache_directory()) {
    if (directory_.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error || !std::filesystem::is_directory(directory_, error)) {
        directory_.clear();
        return;
    }
    std::filesystem::permissions(directory_, std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace, error);
}

std::unique_ptr<llvm::MemoryBuffer> JitObjectCache::load(std::string_view key) const {
    if (directory_.empty()) return {};
    const auto path = directory_ / (std::string(key) + ".o");
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size < kMagic.size() + 32 || size > kMaximumObjectSize) return {};
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return {};
    if (std::string_view(bytes.data(), kMagic.size()) != kMagic) return {};
    const llvm::StringRef object(bytes.data() + kMagic.size() + 32,
                                 bytes.size() - kMagic.size() - 32);
    if (std::string_view(bytes.data() + kMagic.size(), 32) != digest(object)) return {};
    return llvm::MemoryBuffer::getMemBufferCopy(object, std::string(key));
}

void JitObjectCache::store(std::string_view key, const llvm::MemoryBuffer &object) const {
    if (directory_.empty()) return;
    const auto bytes = object.getBuffer();
    if (bytes.size() + kMagic.size() + 32 > kMaximumObjectSize) return;
    const auto path = directory_ / (std::string(key) + ".o");
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temporary = path.string() + "." + std::to_string(stamp) + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return;
        const auto hash = digest(bytes);
        file.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
        file.write(hash.data(), static_cast<std::streamsize>(hash.size()));
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            file.close();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return;
        }
    }
    std::error_code error;
    std::filesystem::permissions(temporary,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, error);
    std::filesystem::rename(temporary, path, error);
    if (error) std::filesystem::remove(temporary, error);
}

} // namespace clickhle::vc2

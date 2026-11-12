#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <zip.h>

#include "clickhle/loader/eapp_image.h"
#include "clickhle/loader/game_manifest.h"

namespace {
namespace fs = std::filesystem;
using clickhle::u64;
using clickhle::u8;

struct ZipCloser {
    void operator()(zip_t *archive) const {
        if (archive)
            zip_close(archive);
    }
};
using Zip = std::unique_ptr<zip_t, ZipCloser>;

struct Stage {
    fs::path path;
    ~Stage() {
        if (!path.empty()) {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    }
};

std::string lower(std::string value) {
    for (char &c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::vector<u8> read_file(const fs::path &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("cannot read " + path.string());
    const auto length = file.tellg();
    if (length < 0)
        throw std::runtime_error("cannot size " + path.string());
    file.seekg(0);
    std::vector<u8> bytes(static_cast<std::size_t>(length));
    if (!bytes.empty() && !file.read(reinterpret_cast<char *>(bytes.data()), length))
        throw std::runtime_error("short read from " + path.string());
    return bytes;
}

void write_file(const fs::path &path, const std::vector<u8> &bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file || (!bytes.empty() && !file.write(reinterpret_cast<const char *>(bytes.data()),
                                                static_cast<std::streamsize>(bytes.size()))))
        throw std::runtime_error("cannot write " + path.string());
}

std::string sha256(const std::vector<u8> &bytes) {
    std::array<u8, EVP_MAX_MD_SIZE> digest{};
    unsigned length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("SHA-256 failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned i = 0; i < length; ++i) {
        result.push_back(hex[digest[i] >> 4]);
        result.push_back(hex[digest[i] & 15]);
    }
    return result;
}

std::array<u8, 32> find_key(const fs::path &keys, const std::vector<u8> &encrypted) {
    const std::string prefix = sha256(encrypted) + "_";
    std::optional<std::array<u8, 32>> found;
    if (!fs::is_directory(keys))
        throw std::runtime_error("keys folder is missing");
    for (const auto &entry : fs::directory_iterator(keys)) {
        const auto name = lower(entry.path().filename().string());
        if (!entry.is_regular_file() || name.rfind(prefix, 0) != 0 ||
            entry.path().extension() != ".ikey")
            continue;
        const auto bytes = read_file(entry.path());
        if (bytes.size() != 32)
            throw std::runtime_error("matching .ikey must contain exactly 32 raw bytes");
        std::array<u8, 32> key{};
        std::copy(bytes.begin(), bytes.end(), key.begin());
        if (found && *found != key)
            throw std::runtime_error("conflicting .ikey files for encrypted executable");
        found = key;
    }
    if (!found)
        throw std::runtime_error("no matching .ikey for encrypted executable " +
                                 prefix.substr(0, 64));
    return *found;
}

std::vector<u8> decrypt(const std::vector<u8> &encrypted, const std::array<u8, 32> &key) {
    const std::size_t whole = encrypted.size() & ~std::size_t{15};
    if (whole > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("encrypted executable is too large");
    std::vector<u8> plain(encrypted.size());
    auto *context = EVP_CIPHER_CTX_new();
    if (!context)
        throw std::runtime_error("AES context allocation failed");
    const auto cleanup = [&] { EVP_CIPHER_CTX_free(context); };
    int written = 0, final = 0;
    const bool okay =
        EVP_DecryptInit_ex(context, EVP_aes_128_cbc(), nullptr, key.data(), key.data() + 16) == 1 &&
        EVP_CIPHER_CTX_set_padding(context, 0) == 1 &&
        EVP_DecryptUpdate(context, plain.data(), &written, encrypted.data(),
                          static_cast<int>(whole)) == 1 &&
        EVP_DecryptFinal_ex(context, plain.data() + written, &final) == 1;
    cleanup();
    if (!okay || static_cast<std::size_t>(written + final) != whole)
        throw std::runtime_error("AES decryption failed");
    std::copy(encrypted.begin() + static_cast<std::ptrdiff_t>(whole), encrypted.end(),
              plain.begin() + static_cast<std::ptrdiff_t>(whole));
    return plain;
}

fs::path safe_zip_path(const std::string &name) {
    if (name.empty() || name[0] == '/')
        throw std::runtime_error("unsafe package path");
    const auto end = name.back() == '/' ? name.size() - 1 : name.size();
    if (end == 0)
        throw std::runtime_error("unsafe package path");
    std::string component;
    for (std::size_t i = 0; i <= end; ++i) {
        const char c = i == end ? '/' : name[i];
        if (c == '/') {
            if (component.empty() || component == "." || component == ".." ||
                component.back() == '.' || component.back() == ' ')
                throw std::runtime_error("unsafe package path: " + name);
            component.clear();
        } else {
            if (static_cast<unsigned char>(c) < 32 ||
                std::string_view("\\:*?\"<>|").find(c) != std::string_view::npos)
                throw std::runtime_error("non-FAT package path: " + name);
            component.push_back(c);
        }
    }
    return fs::path(name.substr(0, end));
}

void check_destination(const fs::path &destination) {
    for (auto path = destination; !path.empty(); path = path.parent_path()) {
        if (fs::is_symlink(fs::symlink_status(path)))
            throw std::runtime_error("destination contains a symbolic link");
        if (path == path.root_path())
            break;
    }
}

void extract(zip_t *archive, const fs::path &stage) {
    std::map<std::string, std::string> spellings;
    std::set<std::string> files, directories;
    u64 total = 0;
    const auto count = zip_get_num_entries(archive, 0);
    if (count <= 0)
        throw std::runtime_error("empty game package");
    for (zip_int64_t i = 0; i < count; ++i) {
        const char *raw = zip_get_name(archive, static_cast<zip_uint64_t>(i), 0);
        if (!raw)
            throw std::runtime_error("invalid ZIP entry name");
        const std::string name(raw);
        const auto path = safe_zip_path(name);
        const bool directory = name.back() == '/';
        zip_stat_t stat{};
        if (zip_stat_index(archive, static_cast<zip_uint64_t>(i), 0, &stat) != 0)
            throw std::runtime_error("cannot stat ZIP entry");
        if ((stat.valid & ZIP_STAT_ENCRYPTION_METHOD) && stat.encryption_method != ZIP_EM_NONE)
            throw std::runtime_error("ZIP password encryption is unsupported");
        zip_uint8_t system = 0;
        zip_uint32_t attributes = 0;
        if (zip_file_get_external_attributes(archive, static_cast<zip_uint64_t>(i), 0, &system,
                                             &attributes) == 0 &&
            system == ZIP_OPSYS_UNIX && ((attributes >> 16) & 0170000u) == 0120000u)
            throw std::runtime_error("symbolic links are not allowed in packages");

        std::string prefix;
        for (const auto &part : path) {
            if (!prefix.empty())
                prefix += '/';
            prefix += part.string();
            const auto folded = lower(prefix);
            const auto [it, inserted] = spellings.emplace(folded, prefix);
            if (!inserted && it->second != prefix)
                throw std::runtime_error("case-colliding package paths");
        }
        const auto folded = lower(path.generic_string());
        if (directory)
            directories.insert(folded);
        else if (!files.insert(folded).second)
            throw std::runtime_error("duplicate package file");
        if (stat.size > 4ull * 1024 * 1024 * 1024 || total > 8ull * 1024 * 1024 * 1024 - stat.size)
            throw std::runtime_error("package is too large");
        total += stat.size;
    }
    for (const auto &file : files) {
        if (directories.count(file))
            throw std::runtime_error("file/directory collision");
    }
    if (!files.count("manifest.plist"))
        throw std::runtime_error("package has no root Manifest.plist");
    const auto space = fs::space(stage.parent_path());
    if (space.available < total + 2ull * 1024 * 1024 * 1024)
        throw std::runtime_error("not enough space to install the package");

    std::array<char, 1024 * 1024> buffer{};
    for (zip_int64_t i = 0; i < count; ++i) {
        const std::string name(zip_get_name(archive, static_cast<zip_uint64_t>(i), 0));
        const auto target = stage / safe_zip_path(name);
        if (name.back() == '/') {
            fs::create_directories(target);
            continue;
        }
        fs::create_directories(target.parent_path());
        auto *member = zip_fopen_index(archive, static_cast<zip_uint64_t>(i), 0);
        if (!member)
            throw std::runtime_error("cannot open ZIP member: " + name);
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        if (!output) {
            zip_fclose(member);
            throw std::runtime_error("cannot write " + name);
        }
        zip_stat_t stat{};
        if (zip_stat_index(archive, static_cast<zip_uint64_t>(i), 0, &stat) != 0) {
            zip_fclose(member);
            throw std::runtime_error("cannot stat ZIP member: " + name);
        }
        u64 written = 0;
        for (;;) {
            const auto n = zip_fread(member, buffer.data(), buffer.size());
            if (n < 0) {
                zip_fclose(member);
                throw std::runtime_error("cannot read ZIP member: " + name);
            }
            if (n == 0)
                break;
            written += static_cast<u64>(n);
            if (written > stat.size) {
                zip_fclose(member);
                throw std::runtime_error("ZIP member exceeds declared size");
            }
            output.write(buffer.data(), n);
            if (!output) {
                zip_fclose(member);
                throw std::runtime_error("cannot write " + name);
            }
        }
        zip_fclose(member);
        if (written != stat.size)
            throw std::runtime_error("short ZIP member: " + name);
    }
}

void install(const fs::path &package, const fs::path &hdd, const fs::path &keys) {
    check_destination(fs::absolute(hdd));
    int error = 0;
    Zip archive(zip_open(package.string().c_str(), ZIP_RDONLY, &error));
    if (!archive)
        throw std::runtime_error("cannot open .ipg archive");
    fs::create_directories(hdd);
    Stage stage;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    stage.path = hdd / (".ipg-stage-" + std::to_string(nonce));
    if (!fs::create_directory(stage.path))
        throw std::runtime_error("cannot stage package");
    extract(archive.get(), stage.path);
    const auto game = clickhle::read_game_manifest(stage.path);
    if (!game || game->guid.empty() || game->guid.size() > 16)
        throw std::runtime_error("invalid game manifest");
    u64 guid = 0;
    const auto parsed =
        std::from_chars(game->guid.data(), game->guid.data() + game->guid.size(), guid, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != game->guid.data() + game->guid.size() ||
        guid == 0)
        throw std::runtime_error("invalid game GUID");
    std::ostringstream name;
    name << std::uppercase << std::hex << guid;
    const auto destination = hdd / "iPod_Control" / "Games_RO" / name.str();
    check_destination(fs::absolute(destination));
    if (fs::exists(destination))
        throw std::runtime_error("game is already installed");

    std::set<fs::path> executables;
    for (const auto &variant : game->variants)
        executables.insert(variant.executable);
    for (const auto &executable : executables) {
        const auto encrypted = read_file(executable);
        const auto key = find_key(keys, encrypted);
        const auto plain = decrypt(encrypted, key);
        std::string parse_error;
        const auto bytes = std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(plain.data()), plain.size());
        if (!clickhle::EappImage::parse(bytes, &parse_error))
            throw std::runtime_error("invalid key or executable: " + parse_error);
        write_file(executable, plain);
    }

    fs::create_directories(destination.parent_path());
    fs::create_directories(hdd / "iPod_Control" / "GameData_RW");
    fs::create_directories(hdd / "iPod_Control" / "GameStats_WO");
    if (fs::exists(destination))
        throw std::runtime_error("game appeared during installation");
    fs::rename(stage.path, destination);
    stage.path.clear();
    std::cout << "Installed " << game->name << " at " << destination << '\n';
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "Usage: clickhle-install <game.ipg> <hdd-folder> <keys-folder>\n";
        return 2;
    }
    try {
        install(argv[1], argv[2], argv[3]);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Installation failed: " << error.what() << '\n';
        return 1;
    }
}

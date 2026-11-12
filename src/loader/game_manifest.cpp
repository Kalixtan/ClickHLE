#include "clickhle/loader/game_manifest.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <string_view>

namespace clickhle {
namespace {

std::string_view value(std::string_view dict, std::string_view key,
                       std::string_view type) {
    const std::string marker = "<key>" + std::string(key) + "</key>";
    const auto key_at = dict.find(marker);
    if (key_at == std::string_view::npos)
        return {};
    const std::string open = "<" + std::string(type) + ">";
    const std::string close = "</" + std::string(type) + ">";
    const auto start = dict.find(open, key_at + marker.size());
    const auto next_key = dict.find("<key>", key_at + marker.size());
    if (start == std::string_view::npos ||
        (next_key != std::string_view::npos && start > next_key))
        return {};
    const auto end = dict.find(close, start + open.size());
    if (end == std::string_view::npos)
        return {};
    return dict.substr(start + open.size(), end - start - open.size());
}

std::string decode_xml(std::string_view source) {
    std::string out;
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (source[i] != '&') {
            out.push_back(source[i]);
            continue;
        }
        const auto end = source.find(';', i);
        if (end == std::string_view::npos) {
            out.push_back('&');
            continue;
        }
        const auto entity = source.substr(i, end - i + 1);
        char replacement = 0;
        if (entity == "&amp;")
            replacement = '&';
        else if (entity == "&lt;")
            replacement = '<';
        else if (entity == "&gt;")
            replacement = '>';
        else if (entity == "&quot;")
            replacement = '"';
        else if (entity == "&apos;")
            replacement = '\'';
        if (replacement) {
            out.push_back(replacement);
            i = end;
        } else
            out.push_back('&');
    }
    return out;
}

template <typename T>
bool number(std::string_view text, T &result) {
    if (text.empty())
        return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

std::string fold(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    for (char c : path) {
        if (c == '\\')
            c = '/';
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

} // namespace

std::optional<InstalledGame> read_game_manifest(const std::filesystem::path &directory) {
    std::ifstream file(directory / "Manifest.plist", std::ios::binary);
    if (!file)
        return std::nullopt;
    const std::string xml((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
    std::string_view view(xml);
    const auto key = view.find("<key>Platforms</key>");
    if (key == std::string_view::npos)
        return std::nullopt;
    const auto begin = view.find("<array>", key);
    if (begin == std::string_view::npos)
        return std::nullopt;
    const auto end = view.find("</array>", begin);
    if (end == std::string_view::npos)
        return std::nullopt;

    InstalledGame game;
    game.guid = decode_xml(value(view, "GUID", "string"));
    game.name = decode_xml(value(view, "Name", "string"));
    game.version = decode_xml(value(view, "Version", "string"));
    game.build_identifier = decode_xml(value(view, "BuildIdentifier", "string"));
    while (!game.build_identifier.empty() &&
           std::isspace(static_cast<unsigned char>(game.build_identifier.back())))
        game.build_identifier.pop_back();
    game.directory = directory;
    const auto files_key = view.find("<key>Files</key>");
    if (files_key != std::string_view::npos) {
        const auto files_begin = view.find("<array>", files_key);
        const auto files_end = files_begin == std::string_view::npos
            ? std::string_view::npos : view.find("</array>", files_begin);
        if (files_end == std::string_view::npos)
            return std::nullopt;
        const auto entries = view.substr(files_begin + 7, files_end - files_begin - 7);
        for (std::size_t at = 0; at < entries.size();) {
            const auto start = entries.find("<dict>", at);
            if (start == std::string_view::npos)
                break;
            const auto stop = entries.find("</dict>", start);
            if (stop == std::string_view::npos)
                return std::nullopt;
            const auto dict = entries.substr(start + 6, stop - start - 6);
            at = stop + 7;
            ManifestFile item;
            item.path = decode_xml(value(dict, "Path", "string"));
            if (item.path.empty() || !number(value(dict, "Size", "integer"), item.size))
                return std::nullopt;
            game.files.push_back(std::move(item));
        }
    }
    view = view.substr(begin + 7, end - begin - 7);
    for (std::size_t at = 0; at < view.size();) {
        const auto start = view.find("<dict>", at);
        if (start == std::string_view::npos)
            break;
        const auto stop = view.find("</dict>", start);
        if (stop == std::string_view::npos)
            return std::nullopt;
        const auto dict = view.substr(start + 6, stop - start - 6);
        at = stop + 7;

        GameVariant variant;
        const std::string relative = decode_xml(value(dict, "ExecutablePath", "string"));
        const std::filesystem::path path(relative);
        if (relative.empty() || path.is_absolute())
            return std::nullopt;
        if (std::find(path.begin(), path.end(), std::filesystem::path("..")) != path.end())
            return std::nullopt;
        if (!number(value(dict, "PlatformID", "integer"), variant.platform_id) ||
            !number(value(dict, "PlatformVersion", "integer"), variant.platform_version))
            return std::nullopt;
        // Older manifests may omit these display fields. The executable path
        // and platform fields above are the only fields needed to launch.
        number(value(dict, "BuildID", "integer"), variant.build_id);
        number(value(dict, "Size", "integer"), variant.size);
        variant.executable = directory / path;
        if (!std::filesystem::is_regular_file(variant.executable))
            return std::nullopt;
        game.variants.push_back(std::move(variant));
    }
    if (game.name.empty() || game.variants.empty())
        return std::nullopt;
    return game;
}

std::vector<InstalledGame> scan_installed_games(const std::filesystem::path &hdd_root) {
    std::vector<InstalledGame> games;
    const auto root = hdd_root / "iPod_Control" / "Games_RO";
    if (!std::filesystem::is_directory(root))
        return games;
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory() || entry.is_symlink())
            continue;
        if (auto game = read_game_manifest(entry.path()))
            games.push_back(std::move(*game));
    }
    std::sort(games.begin(), games.end(),
              [](const auto &a, const auto &b) { return a.name < b.name; });
    return games;
}

std::vector<u32> platforms_for_executable(const std::filesystem::path &executable) {
    const auto root = executable.parent_path().parent_path();
    const auto game = read_game_manifest(root);
    if (!game)
        return {};
    const auto wanted = fold(executable.lexically_normal().generic_string());
    std::vector<u32> ids;
    for (const auto &variant : game->variants) {
        if (fold(variant.executable.lexically_normal().generic_string()) == wanted &&
            std::find(ids.begin(), ids.end(), variant.platform_id) == ids.end())
            ids.push_back(variant.platform_id);
    }
    return ids;
}

} // namespace clickhle

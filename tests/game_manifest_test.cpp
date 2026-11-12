#include <filesystem>
#include <fstream>
#include <iostream>

#include "clickhle/loader/game_manifest.h"

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const auto directory = fs::temp_directory_path() / "clickhle_manifest_test";
    fs::remove_all(directory);
    fs::create_directories(directory / "Executables");
    {
        std::ofstream(directory / "Executables" / "game.bin").put('\0');
        std::ofstream manifest(directory / "Manifest.plist");
        manifest << R"(<plist><dict>
<key>GUID</key><string>24002</string>
<key>Name</key><string>A &amp; B</string>
<key>Version</key><string>1.2</string>
<key>BuildIdentifier</key><string>build-123</string>
<key>Platforms</key><array><dict>
<key>PlatformID</key><integer>2</integer>
<key>PlatformVersion</key><integer>3</integer>
<key>BuildID</key><integer>123</integer>
<key>Size</key><integer>4096</integer>
<key>ExecutablePath</key><string>Executables/game.bin</string>
</dict><dict>
<key>PlatformID</key><integer>3</integer>
<key>PlatformVersion</key><integer>1</integer>
<key>ExecutablePath</key><string>Executables/game.bin</string>
</dict></array></dict></plist>)";
    }
    const auto game = clickhle::read_game_manifest(directory);
    const auto platforms = clickhle::platforms_for_executable(
        directory / "Executables" / "game.bin");
    const bool valid = game && game->guid == "24002" && game->name == "A & B" &&
        game->version == "1.2" && game->build_identifier == "build-123" &&
        game->variants.size() == 2 && game->variants[0].build_id == 123 &&
        game->variants[0].size == 4096 && game->variants[1].build_id == 0 &&
        platforms == std::vector<clickhle::u32>{2, 3};
    {
        std::ofstream malformed(directory / "Manifest.plist", std::ios::trunc);
        malformed << "<key>Platforms</key><array><dict></array>";
    }
    const bool rejects_truncated = !clickhle::read_game_manifest(directory);
    fs::remove_all(directory);
    if (!valid || !rejects_truncated) {
        std::cerr << "manifest metadata or shared platform path failed\n";
        return 1;
    }
    if (argc == 2) {
        std::size_t checked = 0;
        for (const auto& entry : fs::directory_iterator(argv[1])) {
            if (!fs::exists(entry.path() / "Manifest.plist")) continue;
            if (!clickhle::read_game_manifest(entry.path())) {
                std::cerr << "failed to read " << entry.path() << '\n';
                return 1;
            }
            ++checked;
        }
        std::cout << "read " << checked << " installed manifests\n";
    }
    return 0;
}

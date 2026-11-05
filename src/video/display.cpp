#include "clickhle/video/display.h"

#include <fstream>
#include <vector>

#include "clickhle/common/endian.h"
#include "clickhle/common/log.h"

namespace clickhle {

#if CLICKHLE_HAVE_SDL
std::unique_ptr<Display> make_sdl_display();
#endif

namespace {

constexpr std::string_view kCategory = "video";

/// Stand-in used when no windowing backend is compiled in. Everything that can
/// fail, fails; everything else is a no-op. Keeps the emulator linkable and
/// testable without SDL.
class HeadlessDisplay final : public Display {
public:
    [[nodiscard]] std::string_view backend() const override { return "headless"; }
    [[nodiscard]] bool usable() const override { return false; }

    void set_headless(bool) override {}
    [[nodiscard]] int scale() const override { return 1; }
    void set_scale(int) override {}
    void set_fullscreen(bool) override {}

    [[nodiscard]] bool open(int, int, const std::string&) override {
        CLICKHLE_ERROR(kCategory)
            << "this build has no windowing backend; rebuild with SDL3 "
               "available to see anything drawn";
        return false;
    }
    void close() override {}
    [[nodiscard]] bool is_open() const override { return false; }
    void present() override {}
    [[nodiscard]] bool pump() override { return true; }
    [[nodiscard]] bool next_input(InputEvent&) override { return false; }
    void inject(Button, bool) override {}
    void set_auto_press(Button, u32) override {}
    void set_input_script(std::vector<ScriptedInput>) override {}
    [[nodiscard]] bool screenshot(const std::filesystem::path&) override {
        return false;
    }
    [[nodiscard]] std::vector<u8> read_rgba(int& width, int& height) override {
        width = 0;
        height = 0;
        return {};
    }
};

}  // namespace

std::unique_ptr<Display> Display::create() {
#if CLICKHLE_HAVE_SDL
    return make_sdl_display();
#else
    return std::make_unique<HeadlessDisplay>();
#endif
}

bool write_bmp(const std::filesystem::path& path, int width, int height,
               const u8* bgr_bottom_up) {
    if (width <= 0 || height <= 0 || bgr_bottom_up == nullptr) {
        return false;
    }
    // BMP rows are padded to a four-byte boundary.
    const auto row_bytes = static_cast<u32>(width) * 3u;
    const u32 padding = (4u - (row_bytes % 4u)) % 4u;
    const u32 image_bytes = (row_bytes + padding) * static_cast<u32>(height);
    constexpr u32 kHeaderBytes = 14 + 40;

    std::vector<std::byte> header(kHeaderBytes, std::byte{0});
    std::byte* h = header.data();
    h[0] = std::byte{'B'};
    h[1] = std::byte{'M'};
    endian::store_le<u32>(h + 2, kHeaderBytes + image_bytes);
    endian::store_le<u32>(h + 10, kHeaderBytes);
    endian::store_le<u32>(h + 14, 40);
    endian::store_le<i32>(h + 18, width);
    endian::store_le<i32>(h + 22, height);
    endian::store_le<u16>(h + 26, 1);
    endian::store_le<u16>(h + 28, 24);
    endian::store_le<u32>(h + 34, image_bytes);

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        CLICKHLE_ERROR(kCategory) << "cannot write " << path.string();
        return false;
    }
    file.write(reinterpret_cast<const char*>(header.data()),
               static_cast<std::streamsize>(header.size()));

    const std::vector<char> pad(padding, 0);
    for (int y = 0; y < height; ++y) {
        file.write(reinterpret_cast<const char*>(bgr_bottom_up +
                                                 static_cast<std::size_t>(y) *
                                                     row_bytes),
                   static_cast<std::streamsize>(row_bytes));
        if (padding != 0) {
            file.write(pad.data(), static_cast<std::streamsize>(padding));
        }
    }
    return file.good();
}

}  // namespace clickhle

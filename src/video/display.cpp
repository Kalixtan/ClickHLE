#include "clickhle/video/display.h"

#include <fstream>
#include <algorithm>
#include <deque>
#include <vector>

#include "clickhle/common/endian.h"
#include "clickhle/common/log.h"

namespace clickhle {

#if CLICKHLE_HAVE_SDL
std::unique_ptr<Display> make_sdl_display();
#endif

namespace {

constexpr std::string_view kCategory = "video";

// CPU-only presentation keeps replay input and screenshots available without SDL.
class HeadlessDisplay final : public Display {
public:
    std::string_view backend() const override { return "headless"; }
    bool usable() const override { return true; }
    void set_headless(bool) override {}
    int scale() const override { return 1; }
    void set_scale(int) override {}
    void set_fullscreen(bool) override {}
    bool open(int, int, const std::string&) override { opened_ = true; return true; }
    void close() override { opened_ = false; }
    bool is_open() const override { return opened_; }
    void present() override {}
    bool pump() override {
        while (next_ < script_.size() && script_[next_].frame <= pumps_) {
            const auto& event = script_[next_++];
            inject(event.button, event.pressed);
        }
        ++auto_pumps_;
        if (interval_ && auto_pumps_ % interval_ == 0) inject(button_, true);
        else if (interval_ && auto_pumps_ % interval_ == 3) inject(button_, false);
        ++pumps_;
        return true;
    }
    bool next_input(InputEvent& event) override {
        if (input_.empty()) return false;
        event = input_.front(); input_.pop_front(); return true;
    }
    void inject(Button button, bool pressed) override { input_.push_back({button, pressed}); }
    void set_auto_press(Button button, u32 interval) override { button_ = button; interval_ = interval; auto_pumps_ = 0; }
    void set_input_script(std::vector<ScriptedInput> script) override {
        script_ = std::move(script);
        std::stable_sort(script_.begin(), script_.end(), [](const auto& a, const auto& b) { return a.frame < b.frame; });
        next_ = 0; pumps_ = 0;
    }
private:
    bool opened_ = false;
    std::deque<InputEvent> input_;
    std::vector<ScriptedInput> script_;
    std::size_t next_ = 0;
    u32 pumps_ = 0, interval_ = 0, auto_pumps_ = 0;
    Button button_ = Button::Count;
};

}  // namespace

std::unique_ptr<Display> Display::create() {
#if CLICKHLE_HAVE_SDL
    return make_sdl_display();
#else
    return std::make_unique<HeadlessDisplay>();
#endif
}

void Display::set_frame_rgb(std::span<const u8> pixels, int width, int height) {
    if (width <= 0 || height <= 0 ||
        pixels.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3)
        return;
    rgb_.assign(pixels.begin(), pixels.end());
    frame_width_ = width;
    frame_height_ = height;
}

std::vector<u8> Display::read_rgba(int& width, int& height) {
    width = frame_width_;
    height = frame_height_;
    std::vector<u8> result(rgb_.size() / 3 * 4, 255);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto src = static_cast<std::size_t>(y * width + x) * 3;
            auto dst = static_cast<std::size_t>((height - 1 - y) * width + x) * 4;
            std::copy_n(rgb_.data() + src, 3, result.data() + dst);
        }
    }
    return result;
}

bool Display::screenshot(const std::filesystem::path& path) {
    if (rgb_.empty()) return false;
    std::vector<u8> bgr(rgb_.size());
    for (int y = 0; y < frame_height_; ++y) {
        for (int x = 0; x < frame_width_; ++x) {
            auto src = static_cast<std::size_t>(y * frame_width_ + x) * 3;
            auto dst = static_cast<std::size_t>((frame_height_ - 1 - y) * frame_width_ + x) * 3;
            for (unsigned c = 0; c < 3; ++c) bgr[dst + c] = rgb_[src + 2 - c];
        }
    }
    return write_bmp(path, frame_width_, frame_height_, bgr.data());
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

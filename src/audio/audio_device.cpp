#include "clickhle/audio/audio_device.h"

#include "clickhle/common/log.h"

namespace clickhle {

#if CLICKHLE_HAVE_SDL
std::unique_ptr<AudioDevice> make_sdl_audio_device();
#endif

namespace {

/// Accepts and discards audio. Used when no backend is compiled in, and when
/// the host has no working audio device -- a game must still be able to run
/// silently rather than stall waiting for buffers that never drain.
class SilentAudioDevice final : public AudioDevice {
public:
    [[nodiscard]] std::string_view backend() const override { return "silent"; }

    [[nodiscard]] bool open(u32, u16, u16) override {
        open_ = true;
        return true;
    }
    void close() override { open_ = false; }
    [[nodiscard]] bool is_open() const override { return open_; }

    [[nodiscard]] bool queue(const void*, u32) override { return open_; }

    // Always empty, so every buffer completes immediately and the game's
    // streaming loop keeps turning.
    [[nodiscard]] u64 queued_bytes() const override { return 0; }

    void flush() override {}
    void set_paused(bool) override {}

private:
    bool open_ = false;
};

}  // namespace

std::unique_ptr<AudioDevice> AudioDevice::create() {
#if CLICKHLE_HAVE_SDL
    return make_sdl_audio_device();
#else
    return std::make_unique<SilentAudioDevice>();
#endif
}

}  // namespace clickhle

// SDL3 audio backend. The only file besides display_sdl.cpp that includes SDL.

#include <SDL3/SDL.h>

#include "clickhle/audio/audio_device.h"
#include "clickhle/common/log.h"

namespace clickhle {
namespace {

constexpr std::string_view kCategory = "audio";

class SdlAudioDevice final : public AudioDevice {
public:
    ~SdlAudioDevice() override { close(); }

    [[nodiscard]] std::string_view backend() const override { return "sdl3"; }

    [[nodiscard]] bool open(u32 sample_rate, u16 channels,
                            u16 bits_per_sample) override {
        close();
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            CLICKHLE_WARN(kCategory) << "SDL_InitSubSystem: " << SDL_GetError();
            return false;
        }

        SDL_AudioSpec spec{};
        spec.freq = static_cast<int>(sample_rate);
        spec.channels = channels == 0 ? 1 : channels;
        // waveOut only ever offers 8-bit unsigned or 16-bit signed PCM.
        spec.format = bits_per_sample == 8 ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE;

        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                            &spec, nullptr, nullptr);
        if (stream_ == nullptr) {
            CLICKHLE_WARN(kCategory)
                << "SDL_OpenAudioDeviceStream: " << SDL_GetError();
            return false;
        }
        SDL_ResumeAudioStreamDevice(stream_);

        frame_bytes_ = static_cast<u32>(spec.channels) *
                       (bits_per_sample == 8 ? 1u : 2u);
        CLICKHLE_INFO(kCategory) << "opened " << sample_rate << " Hz, "
                               << spec.channels << " channel(s), "
                               << bits_per_sample << "-bit";
        return true;
    }

    void close() override {
        if (stream_ != nullptr) {
            SDL_DestroyAudioStream(stream_);
            stream_ = nullptr;
        }
    }

    [[nodiscard]] bool is_open() const override { return stream_ != nullptr; }

    [[nodiscard]] bool queue(const void* data, u32 bytes) override {
        if (stream_ == nullptr || data == nullptr || bytes == 0) {
            return false;
        }
        return SDL_PutAudioStreamData(stream_, data, static_cast<int>(bytes));
    }

    [[nodiscard]] u64 queued_bytes() const override {
        if (stream_ == nullptr) {
            return 0;
        }
        const int available = SDL_GetAudioStreamAvailable(stream_);
        return available > 0 ? static_cast<u64>(available) : 0;
    }

    void flush() override {
        if (stream_ != nullptr) {
            SDL_ClearAudioStream(stream_);
        }
    }

    void set_paused(bool paused) override {
        if (stream_ == nullptr) {
            return;
        }
        if (paused) {
            SDL_PauseAudioStreamDevice(stream_);
        } else {
            SDL_ResumeAudioStreamDevice(stream_);
        }
    }

private:
    SDL_AudioStream* stream_ = nullptr;
    u32 frame_bytes_ = 4;
};

}  // namespace

std::unique_ptr<AudioDevice> make_sdl_audio_device() {
    return std::make_unique<SdlAudioDevice>();
}

}  // namespace clickhle

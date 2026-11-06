// SDL3 audio backend. The only file besides display_sdl.cpp that includes SDL.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

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

        spec_ = spec;
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                            &spec, fill_stream, this);
        if (stream_ == nullptr) {
            CLICKHLE_WARN(kCategory)
                << "SDL_OpenAudioDeviceStream: " << SDL_GetError();
            return false;
        }
        SDL_ResumeAudioStreamDevice(stream_);

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
        std::lock_guard lock(voices_mutex_);
        voices_.clear();
    }

    [[nodiscard]] bool is_open() const override { return stream_ != nullptr; }

    [[nodiscard]] bool queue(const void* data, u32 bytes) override {
        if (stream_ == nullptr || data == nullptr || bytes == 0) {
            return false;
        }
        return SDL_PutAudioStreamData(stream_, data, static_cast<int>(bytes));
    }

    [[nodiscard]] bool play(u32 key, const void* data, u32 bytes,
                            u32 sample_rate, u16 channels,
                            u16 bits_per_sample, bool loop) override {
        if (stream_ == nullptr || data == nullptr || bytes == 0 ||
            sample_rate == 0 || channels == 0 || channels > 2 ||
            (bits_per_sample != 8 && bits_per_sample != 16)) {
            return false;
        }

        SDL_AudioSpec source{};
        source.freq = static_cast<int>(sample_rate);
        source.channels = channels;
        source.format = bits_per_sample == 8 ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE;
        Uint8* converted = nullptr;
        int converted_size = 0;
        if (!SDL_ConvertAudioSamples(&source,
                                    static_cast<const Uint8*>(data),
                                    static_cast<int>(bytes), &spec_, &converted,
                                    &converted_size)) {
            CLICKHLE_WARN(kCategory) << "SDL_ConvertAudioSamples: "
                                     << SDL_GetError();
            return false;
        }
        if (converted_size < 0 || converted_size % 4 != 0) {
            SDL_free(converted);
            return false;
        }
        Voice voice;
        voice.samples.resize(static_cast<std::size_t>(converted_size) / 2);
        std::memcpy(voice.samples.data(), converted,
                    static_cast<std::size_t>(converted_size));
        SDL_free(converted);
        voice.loop = loop;
        {
            std::lock_guard lock(voices_mutex_);
            voices_[key] = std::move(voice);
        }
        return true;
    }

    void stop_voice(u32 key) override {
        std::lock_guard lock(voices_mutex_);
        voices_.erase(key);
    }

    void pause_voice(u32 key, bool paused) override {
        std::lock_guard lock(voices_mutex_);
        const auto it = voices_.find(key);
        if (it != voices_.end()) {
            it->second.paused = paused;
        }
    }

    [[nodiscard]] u64 queued_bytes() const override {
        if (stream_ == nullptr) {
            return 0;
        }
        const int available = SDL_GetAudioStreamAvailable(stream_);
        return available > 0 ? static_cast<u64>(available) : 0;
    }

    void flush() override {
        {
            std::lock_guard lock(voices_mutex_);
            voices_.clear();
        }
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
    struct Voice {
        std::vector<std::int16_t> samples;
        std::size_t frame = 0;
        bool loop = false;
        bool paused = false;
    };

    static void SDLCALL fill_stream(void* userdata, SDL_AudioStream* stream,
                                    int additional_amount, int) {
        auto* self = static_cast<SdlAudioDevice*>(userdata);
        if (self == nullptr || additional_amount <= 0) {
            return;
        }
        const int aligned_size = additional_amount - (additional_amount % 4);
        if (aligned_size <= 0) {
            return;
        }
        const std::size_t frames = static_cast<std::size_t>(aligned_size) / 4;
        std::vector<std::int32_t> mix(frames * 2, 0);
        {
            std::lock_guard lock(self->voices_mutex_);
            for (auto it = self->voices_.begin(); it != self->voices_.end();) {
                Voice& voice = it->second;
                if (voice.paused || voice.samples.empty()) {
                    ++it;
                    continue;
                }
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    if (voice.frame >= voice.samples.size() / 2) {
                        if (voice.loop) {
                            voice.frame = 0;
                        } else {
                            break;
                        }
                    }
                    mix[frame * 2] += voice.samples[voice.frame * 2];
                    mix[frame * 2 + 1] += voice.samples[voice.frame * 2 + 1];
                    ++voice.frame;
                }
                if (!voice.loop && voice.frame >= voice.samples.size() / 2) {
                    it = self->voices_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        std::vector<std::int16_t> output(mix.size());
        std::transform(mix.begin(), mix.end(), output.begin(),
                       [](std::int32_t value) {
            return static_cast<std::int16_t>(
                std::clamp(value, -32768, 32767));
        });
        if (!SDL_PutAudioStreamData(stream, output.data(), aligned_size)) {
            CLICKHLE_WARN(kCategory) << "SDL_PutAudioStreamData: "
                                     << SDL_GetError();
        }
    }

    SDL_AudioStream* stream_ = nullptr;
    SDL_AudioSpec spec_{};
    std::mutex voices_mutex_;
    std::map<u32, Voice> voices_;
};

}  // namespace

std::unique_ptr<AudioDevice> make_sdl_audio_device() {
    return std::make_unique<SdlAudioDevice>();
}

}  // namespace clickhle

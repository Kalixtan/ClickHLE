#pragma once

#include <memory>
#include <string_view>

#include "clickhle/common/types.h"

namespace clickhle {

/// Host audio output.
///
/// Same shape as Display and for the same reasons: the tree must build with no
/// SDL, and nothing outside the backend may know which windowing or audio
/// library is in use.
///
/// The interface is a byte queue rather than a callback, because that is what
/// waveOut is. A game hands over buffers and asks later which have drained;
/// modelling it any other way would mean inventing a threading story the
/// emulator does not yet have.
class AudioDevice {
public:
    virtual ~AudioDevice() = default;

    [[nodiscard]] static std::unique_ptr<AudioDevice> create();

    [[nodiscard]] virtual std::string_view backend() const = 0;

    /// Inherited default of 48 kHz 16-bit stereo; the iPod's actual rate is
    /// unconfirmed. Games open whatever they
    /// like and the device resamples.
    [[nodiscard]] virtual bool open(u32 sample_rate, u16 channels,
                                    u16 bits_per_sample) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool is_open() const = 0;

    /// Queues PCM for playback. Returns false if the device is not open.
    [[nodiscard]] virtual bool queue(const void* data, u32 bytes) = 0;

    /// Starts or replaces one keyed PCM voice. Input PCM is converted to the
    /// device format before playback. The key is the guest SFX handle.
    [[nodiscard]] virtual bool play(u32 key, const void* data, u32 bytes,
                                    u32 sample_rate, u16 channels,
                                    u16 bits_per_sample, bool loop) = 0;
    virtual void stop_voice(u32 key) = 0;
    virtual void pause_voice(u32 key, bool paused) = 0;

    /// Bytes queued but not yet played. This is what tells us which of the
    /// guest's buffers have finished, so it must be honest.
    [[nodiscard]] virtual u64 queued_bytes() const = 0;

    /// Discards anything queued, for waveOutReset.
    virtual void flush() = 0;

    virtual void set_paused(bool paused) = 0;
};

}  // namespace clickhle

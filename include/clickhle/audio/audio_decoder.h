#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "clickhle/common/types.h"

namespace clickhle {

struct DecodedAudio {
    u32 sample_rate = 0;
    u16 channels = 0;
    u16 bits_per_sample = 0;
    std::vector<u8> pcm;
};

/// Decodes PCM WAV and, when FFmpeg is available at build time, common
/// compressed formats used by bundled game music.
[[nodiscard]] std::optional<DecodedAudio> decode_game_audio(
    const std::filesystem::path& path, u32 maximum_pcm_bytes = 0);

}  // namespace clickhle

#include "clickhle/audio/audio_decoder.h"

#include <array>
#include <algorithm>
#include <cstddef>
#include <fstream>

#if CLICKHLE_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}
#endif

namespace clickhle {
namespace {

u16 read_u16(const std::vector<u8>& bytes, std::size_t offset) {
    return static_cast<u16>(bytes[offset]) |
           static_cast<u16>(static_cast<u16>(bytes[offset + 1]) << 8);
}

u32 read_u32(const std::vector<u8>& bytes, std::size_t offset) {
    return static_cast<u32>(bytes[offset]) |
           (static_cast<u32>(bytes[offset + 1]) << 8) |
           (static_cast<u32>(bytes[offset + 2]) << 16) |
           (static_cast<u32>(bytes[offset + 3]) << 24);
}

bool has_tag(const std::vector<u8>& bytes, std::size_t offset,
             const std::array<char, 4>& tag) {
    if (offset > bytes.size() || bytes.size() - offset < tag.size()) {
        return false;
    }
    for (std::size_t i = 0; i < tag.size(); ++i) {
        if (bytes[offset + i] != static_cast<u8>(tag[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::optional<DecodedAudio> decode_pcm_wav(const std::filesystem::path& path,
                                           u32 maximum_pcm_bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }
    const std::streamoff end = file.tellg();
    if (end < 12 || static_cast<u64>(end) > 128u * 1024u * 1024u) {
        return std::nullopt;
    }
    std::vector<u8> bytes(static_cast<std::size_t>(end));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), end) ||
        !has_tag(bytes, 0, {'R', 'I', 'F', 'F'}) ||
        !has_tag(bytes, 8, {'W', 'A', 'V', 'E'})) {
        return std::nullopt;
    }

    u16 format = 0;
    DecodedAudio result;
    std::size_t data_offset = 0;
    u32 data_size = 0;
    std::size_t cursor = 12;
    while (cursor <= bytes.size() && bytes.size() - cursor >= 8) {
        const u32 chunk_size = read_u32(bytes, cursor + 4);
        const std::size_t chunk_offset = cursor + 8;
        if (chunk_offset > bytes.size() ||
            static_cast<u64>(chunk_size) > bytes.size() - chunk_offset) {
            return std::nullopt;
        }
        if (has_tag(bytes, cursor, {'f', 'm', 't', ' '}) && chunk_size >= 16) {
            format = read_u16(bytes, chunk_offset);
            result.channels = read_u16(bytes, chunk_offset + 2);
            result.sample_rate = read_u32(bytes, chunk_offset + 4);
            result.bits_per_sample = read_u16(bytes, chunk_offset + 14);
        } else if (has_tag(bytes, cursor, {'d', 'a', 't', 'a'})) {
            data_offset = chunk_offset;
            data_size = chunk_size;
        }
        const u64 next = static_cast<u64>(chunk_offset) + chunk_size +
                         (chunk_size & 1u);
        if (next > bytes.size()) {
            break;
        }
        cursor = static_cast<std::size_t>(next);
    }

    if (format != 1 || result.sample_rate == 0 || result.channels == 0 ||
        result.channels > 2 ||
        (result.bits_per_sample != 8 && result.bits_per_sample != 16) ||
        data_offset == 0 || data_size == 0) {
        return std::nullopt;
    }
    if (maximum_pcm_bytes != 0) {
        data_size = std::min(data_size, maximum_pcm_bytes);
    }
    const u32 bytes_per_sample = result.bits_per_sample / 8u;
    const u32 frame_size = bytes_per_sample * result.channels;
    data_size -= data_size % frame_size;
    if (data_size == 0) {
        return std::nullopt;
    }
    result.pcm.assign(bytes.begin() + static_cast<std::ptrdiff_t>(data_offset),
                      bytes.begin() + static_cast<std::ptrdiff_t>(data_offset + data_size));
    return result;
}

#if CLICKHLE_HAVE_FFMPEG
namespace {

std::optional<DecodedAudio> decode_compressed(
    const std::filesystem::path& path, u32 maximum_pcm_bytes) {
    AVFormatContext* format = nullptr;
    const std::string path_string = path.string();
    if (avformat_open_input(&format, path_string.c_str(), nullptr, nullptr) < 0) {
        return std::nullopt;
    }
    struct CloseInput {
        AVFormatContext*& format;
        ~CloseInput() { avformat_close_input(&format); }
    } close_input{format};
    if (avformat_find_stream_info(format, nullptr) < 0) {
        return std::nullopt;
    }

    const int stream_index = av_find_best_stream(
        format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (stream_index < 0) {
        return std::nullopt;
    }
    AVStream* stream = format->streams[stream_index];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
        return std::nullopt;
    }
    AVCodecContext* codec = avcodec_alloc_context3(decoder);
    if (codec == nullptr) {
        return std::nullopt;
    }
    struct FreeCodec {
        AVCodecContext* codec;
        ~FreeCodec() { avcodec_free_context(&codec); }
    } free_codec{codec};
    if (avcodec_parameters_to_context(codec, stream->codecpar) < 0 ||
        avcodec_open2(codec, decoder, nullptr) < 0 || codec->sample_rate <= 0) {
        return std::nullopt;
    }

    AVChannelLayout output_layout;
    av_channel_layout_default(&output_layout, 2);
    SwrContext* resampler = nullptr;
    const int resampler_result = swr_alloc_set_opts2(
        &resampler, &output_layout, AV_SAMPLE_FMT_S16, codec->sample_rate,
        &codec->ch_layout, codec->sample_fmt, codec->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&output_layout);
    if (resampler_result < 0 || resampler == nullptr ||
        swr_init(resampler) < 0) {
        swr_free(&resampler);
        return std::nullopt;
    }
    struct FreeResampler {
        SwrContext*& resampler;
        ~FreeResampler() { swr_free(&resampler); }
    } free_resampler{resampler};

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (packet == nullptr || frame == nullptr) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        return std::nullopt;
    }
    struct FreeDecodeFrames {
        AVPacket*& packet;
        AVFrame*& frame;
        ~FreeDecodeFrames() {
            av_packet_free(&packet);
            av_frame_free(&frame);
        }
    } free_frames{packet, frame};

    DecodedAudio result;
    result.sample_rate = static_cast<u32>(codec->sample_rate);
    result.channels = 2;
    result.bits_per_sample = 16;
    auto receive_frames = [&]() {
        int status = 0;
        while ((status = avcodec_receive_frame(codec, frame)) >= 0) {
            const int capacity = swr_get_out_samples(resampler, frame->nb_samples);
            if (capacity <= 0) {
                av_frame_unref(frame);
                continue;
            }
            const u64 byte_capacity = static_cast<u64>(capacity) * 4u;
            if (byte_capacity > 128u * 1024u * 1024u) {
                av_frame_unref(frame);
                return false;
            }
            const std::size_t old_size = result.pcm.size();
            result.pcm.resize(old_size + static_cast<std::size_t>(byte_capacity));
            std::uint8_t* output[] = {result.pcm.data() + old_size};
            const int converted = swr_convert(
                resampler, output, capacity,
                const_cast<const std::uint8_t**>(frame->extended_data),
                frame->nb_samples);
            av_frame_unref(frame);
            if (converted < 0) {
                result.pcm.resize(old_size);
                return false;
            }
            result.pcm.resize(old_size + static_cast<std::size_t>(converted) * 4u);
            if (maximum_pcm_bytes != 0 &&
                result.pcm.size() >= maximum_pcm_bytes) {
                result.pcm.resize(maximum_pcm_bytes - maximum_pcm_bytes % 4u);
                return false;
            }
        }
        return status == AVERROR(EAGAIN) || status == AVERROR_EOF;
    };

    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == stream_index &&
            avcodec_send_packet(codec, packet) >= 0 && !receive_frames()) {
            av_packet_unref(packet);
            break;
        }
        av_packet_unref(packet);
        if (maximum_pcm_bytes != 0 && result.pcm.size() >= maximum_pcm_bytes) {
            break;
        }
    }
    // AAC and other framed codecs can retain decoded samples until the
    // decoder receives its end-of-input packet. Drain those before returning.
    if (maximum_pcm_bytes == 0 || result.pcm.size() < maximum_pcm_bytes) {
        if (avcodec_send_packet(codec, nullptr) >= 0) {
            (void)receive_frames();
        }
    }
    // The resampler can also retain a small tail after the final decoded
    // frame. Convert it with a null input so the clip ends cleanly.
    if (maximum_pcm_bytes == 0 || result.pcm.size() < maximum_pcm_bytes) {
        const int capacity = swr_get_out_samples(resampler, 0);
        if (capacity > 0 && static_cast<u64>(capacity) * 4u <=
                                128u * 1024u * 1024u) {
            const std::size_t old_size = result.pcm.size();
            result.pcm.resize(old_size + static_cast<std::size_t>(capacity) * 4u);
            std::uint8_t* output[] = {result.pcm.data() + old_size};
            const int converted = swr_convert(resampler, output, capacity,
                                              nullptr, 0);
            if (converted < 0) {
                result.pcm.resize(old_size);
            } else {
                result.pcm.resize(old_size + static_cast<std::size_t>(converted) * 4u);
            }
        }
    }
    if (result.pcm.empty()) {
        return std::nullopt;
    }
    return result;
}

}  // namespace
#endif

std::optional<DecodedAudio> decode_game_audio(
    const std::filesystem::path& path, u32 maximum_pcm_bytes) {
    if (auto wav = decode_pcm_wav(path, maximum_pcm_bytes)) {
        return wav;
    }
#if CLICKHLE_HAVE_FFMPEG
    return decode_compressed(path, maximum_pcm_bytes);
#else
    return std::nullopt;
#endif
}

}  // namespace clickhle

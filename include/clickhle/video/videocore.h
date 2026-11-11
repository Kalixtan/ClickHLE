#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "clickhle/common/types.h"

namespace clickhle {
struct HleCallFrame;
enum class VideoCoreEngine { Interpreter, Jit };

[[nodiscard]] bool videocore_jit_available() noexcept;

// Native experimental video coprocessor. It owns one VC instance
// and native HLE packet-building state for the lifetime of a loaded game.
class VideoCore {
  public:
    explicit VideoCore(bool capture = false,
                       VideoCoreEngine engine = VideoCoreEngine::Interpreter);
    ~VideoCore();
    VideoCore(const VideoCore &) = delete;
    VideoCore &operator=(const VideoCore &) = delete;

    bool call(u32 ordinal, HleCallFrame &frame);
    bool submit(std::span<const u8> packets);
    void set_engine(VideoCoreEngine engine);
    [[nodiscard]] const std::string &error() const { return error_; }
    [[nodiscard]] const std::vector<u8> &pixels() const { return pixels_; }
    [[nodiscard]] u64 frames() const { return frames_; }
    [[nodiscard]] u64 draws() const { return draws_; }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool capture_ = false;
    VideoCoreEngine engine_ = VideoCoreEngine::Interpreter;
    bool initialize();
    std::string error_;
    std::vector<u8> pixels_; // RGB8, in increasing framebuffer address order.
    u64 pixel_revision_ = 0;
    u64 published_revision_ = 0;
    u64 frames_ = 0;
    u64 draws_ = 0;
};
} // namespace clickhle

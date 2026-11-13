#pragma once

#include "clickhle/common/types.h"
#include "clickhle/hle/guest_heap.h"
#include <array>
#include <deque>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

namespace clickhle::classic {

// 7G Audio#17 posts message 0x66000018 with one byte. The original worker
// copies that byte to its context at +0x0d. Exact worker scheduling is unknown.
class AudioService {
public:
    struct Track {
        std::filesystem::path path;
        u32 parameter = 0;
    };
    // SoundEffect#0 creates a firmware-owned object and returns its table index.
    struct Effect {
        std::array<u32, 20> fields{};
        bool playing = false;
        Effect() {
            fields[0x10 / 4] = 44100;
            fields[0x14 / 4] = 1;
            fields[0x18 / 4] = 16;
            fields[0x1c / 4] = fields[0x30 / 4] = fields[0x44 / 4] = 0x7fff;
            fields[0x20 / 4] = 1000;
            fields[0x38 / 4] = 1;
        }
    };
    u32 create_effect() {
        for (u32 i = 0; i < effects_.size(); ++i) {
            if (!effects_[i]) { effects_[i].emplace(); return i; }
        }
        effects_.emplace_back(Effect{});
        return static_cast<u32>(effects_.size() - 1);
    }
    Effect* effect(u32 handle) {
        return handle < effects_.size() && effects_[handle] ? &*effects_[handle] : nullptr;
    }
    bool set_effect_buffer(u32 handle, u32 address) {
        if (!address || handle >= effects_.size() || !effects_[handle]) return false;
        effects_[handle]->fields[1] = address;
        return true;
    }
    bool set_effect_field_08(u32 handle, u32 value) {
        if (handle >= effects_.size() || !effects_[handle]) return false;
        effects_[handle]->fields[2] = value;
        return true;
    }
    bool set_effect_field(u32 handle, u32 offset, u32 value, bool byte) {
        if (handle >= effects_.size() || !effects_[handle] || offset >= 0x50)
            return false;
        u32& field = effects_[handle]->fields[offset / 4];
        if (byte) {
            const u32 shift = (offset & 3) * 8;
            field = (field & ~(0xffu << shift)) | ((value & 0xffu) << shift);
        } else {
            field = value;
        }
        return true;
    }
    std::optional<u32> effect_field(u32 handle, u32 offset, bool byte) const {
        if (handle >= effects_.size() || !effects_[handle] || offset >= 0x50)
            return std::nullopt;
        const u32 value = effects_[handle]->fields[offset / 4];
        return byte ? (value >> ((offset & 3) * 8)) & 0xffu : value;
    }
    void enqueue_control(u32 value) {
        pending_.push_back({Message::Type::control, static_cast<u8>(value), {}, 0});
    }
    void enqueue_stream_enable() {
        // 0x080f5234 updates this server field before queuing the message.
        stream_value_ = 0;
        enqueue_control(1);
    }
    void enqueue_stream_reset() { pending_.push_back({Message::Type::reset, 0, {}, 0}); }
    void enqueue_track(std::filesystem::path path, u32 parameter, GuestAddr name_copy) {
        // The server increments +0x144 synchronously when posting 0x66000014.
        stream_value_ = stream_value_.value_or(static_cast<u32>(tracks_.size())) + 1;
        pending_.push_back({Message::Type::track, 0, Track{std::move(path), parameter}, name_copy});
    }
    void enqueue_track_selection(u32 index) {
        Message message{Message::Type::select, 0, {}, 0};
        message.index = index;
        pending_.push_back(std::move(message));
    }
    void service(GuestHeap* heap = nullptr) {
        while (!pending_.empty()) {
            const auto message = pending_.front();
            if (message.name_copy && !heap) return;
            if (message.type == Message::Type::reset) {
                stream_field_10_ = 0;
                stream_field_14_ = 0;
            } else if (message.type == Message::Type::control) {
                control_ = message.value;
            } else if (message.type == Message::Type::track) {
                control_ = 1;
                // Failed file lookup creates no media item in 0x0817e66c.
                if (message.track && !message.track->path.empty()) tracks_.push_back(*message.track);
                if (message.name_copy) (void)heap->release(message.name_copy);
            } else {
                const u32 index = message.index == 0xffffffffu
                    ? selected_track_.value_or(0xffffffffu) : message.index;
                if (index < tracks_.size() &&
                    (selected_track_ != index || device_state_ != 1)) {
                    selected_track_ = index;
                    device_state_ = 1;
                    pending_playback_ = tracks_[index];
                }
            }
            pending_.pop_front();
        }
    }
    [[nodiscard]] u8 control() const noexcept { return control_; }
    // The fresh device fallback is zero. The tracked count approximates the
    // device playlist until its complete state and worker timing are modeled.
    [[nodiscard]] u32 stream_value() const noexcept {
        return stream_value_.value_or(static_cast<u32>(tracks_.size()));
    }
    [[nodiscard]] const std::vector<Track>& tracks() const noexcept { return tracks_; }
    std::optional<Track> take_pending_playback() noexcept {
        auto result = std::move(pending_playback_);
        pending_playback_.reset();
        return result;
    }
    // 7G device object's +0x93d byte, initialized to zero by 0x08172b88.
    [[nodiscard]] u8 device_state() const noexcept { return device_state_; }
    void set_device_state(u8 value) noexcept { device_state_ = value; }
    // Fresh 7G preferences use half the device maximum and a full limit.
    // The actual device maximum and persisted preferences are not modeled yet.
    [[nodiscard]] u32 current_volume() const noexcept {
        const u32 current = current_volume_ < maximum_volume_ ? current_volume_ : maximum_volume_;
        return current * 255u / maximum_volume_;
    }
    [[nodiscard]] u32 volume_limit() const noexcept {
        return limit_volume_ * 255u / maximum_volume_;
    }
    [[nodiscard]] std::size_t pending_count() const noexcept { return pending_.size(); }

private:
    struct Message {
        enum class Type { control, reset, track, select } type;
        u8 value;
        std::optional<Track> track;
        GuestAddr name_copy;
        u32 index = 0;
    };
    std::deque<Message> pending_;
    std::vector<std::optional<Effect>> effects_;
    std::vector<Track> tracks_;
    std::optional<u32> selected_track_;
    std::optional<Track> pending_playback_;
    u8 control_ = 0;
    u8 device_state_ = 0;
    u32 maximum_volume_ = 256;
    u32 current_volume_ = 128;
    u32 limit_volume_ = 256;
    u32 stream_field_10_ = 0;
    u32 stream_field_14_ = 0;
    std::optional<u32> stream_value_;
};

} // namespace clickhle::classic

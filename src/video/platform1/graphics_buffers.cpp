#include "graphics_api.h"
#include "clickhle/core/emulator.h"
#include "clickhle/hle/call_frame.h"
#include <algorithm>
#include <stdexcept>

namespace clickhle {
u32 GraphicsApi::buffers(u32 ordinal, HleCallFrame &frame, const std::array<u32, 16> &a) {
    const auto invalid = [&](u32 error) -> void {
        error_ = error;
        throw std::runtime_error("Invalid buffer arguments for graphics API #" + std::to_string(ordinal));
    };
    if (ordinal == 75) return a[0] < buffers_.size() && buffers_[a[0]].exists;
    if (ordinal == 44) {
        if (static_cast<i32>(a[0]) < 0) invalid(0x501);
        for (u32 i = 0; i < a[0]; ++i)
            if (!frame.store<u32>(a[1] + i * 4, next_buffer_++))
                throw std::runtime_error("Invalid buffer-name destination");
        return a[0];
    }
    if (ordinal == 10 && static_cast<i32>(a[1]) < 0) invalid(0x501);
    if (ordinal == 11 && (static_cast<i32>(a[1]) < 0 || static_cast<i32>(a[2]) < 0)) invalid(0x501);
    if (a[0] != 0x8892 && a[0] != 0x8893) invalid(0x500);
    u32 &binding = a[0] == 0x8892 ? array_buffer_ : element_buffer_;
    if (ordinal == 3) {
        if (a[1] >= buffers_.size())
            throw std::runtime_error("RetailOS buffer name exceeds its 32-object limit");
        if (a[1]) {
            buffers_[a[1]].exists = true;
            next_buffer_ = std::max(next_buffer_, a[1] + 1);
        }
        binding = a[1];
        return 0;
    }
    // With no binding the original routine dereferences low ARM memory. Do
    // not manufacture an object from those unrelated firmware bytes.
    if (!binding) throw std::runtime_error("Unverified RetailOS operation on an unbound buffer");
    auto &buffer = buffers_[binding];
    const auto allocate = [&] {
        if (buffer.address || !buffer.size) return;
        buffer.address = frame.emulator.heap().allocate(buffer.size);
        if (!buffer.address) invalid(0x505);
    };
    const auto copy = [&](u32 destination, u32 source, u32 size) {
        if (!size) return;
        const auto *bytes = frame.memory.host_ptr(source, size);
        if (!bytes || !frame.memory.write_bytes(destination, bytes, size))
            throw std::runtime_error("Unmapped graphics buffer data");
    };
    switch (ordinal) {
    case 10: {
        const u32 usage = a[3] - 0x88e0;
        if (usage > 10 || usage == 3 || usage == 7) invalid(0x500);
        if (buffer.address && (buffer.size != a[1] || !a[2] || !a[1])) {
            frame.emulator.heap().release(buffer.address);
            buffer.address = 0;
        }
        buffer.size = a[1];
        buffer.usage = a[3];
        buffer.mapped = false;
        buffer.dirty = true;
        if (a[2] && buffer.size) {
            allocate();
            copy(buffer.address, a[2], buffer.size);
        }
        return 0;
    }
    case 11:
        if (buffer.mapped) invalid(0x502);
        if (uint64_t(a[1]) + a[2] > buffer.size) invalid(0x501);
        if (a[2] && !buffer.address)
            throw std::runtime_error("Unverified RetailOS subdata write without buffer storage");
        copy(buffer.address + a[1], a[3], a[2]);
        buffer.dirty = true;
        return 1;
    case 51: case 52: {
        u32 value = 0;
        if (ordinal == 52) {
            if (a[1] != 0x88bd) invalid(0x500);
            value = buffer.mapped ? buffer.address : 0;
        } else switch (a[1]) {
        case 0x8764: value = buffer.size; break;
        case 0x8765: value = buffer.usage; break;
        case 0x88bb: value = buffer.access; break;
        case 0x88bc: value = buffer.mapped; break;
        default: invalid(0x500);
        }
        if (!frame.store<u32>(a[2], value)) throw std::runtime_error("Invalid buffer query destination");
        return value;
    }
    case 83:
        if (a[1] != 0x88b9) invalid(0x500);
        if (buffer.mapped) invalid(0x502);
        buffer.mapped = true;
        allocate();
        return buffer.address;
    case 126:
        if (!buffer.mapped) invalid(0x502);
        buffer.mapped = false;
        buffer.dirty = true;
        return 1;
    }
    throw std::runtime_error("Unknown graphics buffer call");
}
} // namespace clickhle

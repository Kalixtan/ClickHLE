#include "graphics_api.h"
#include "clickhle/common/endian.h"
#include "clickhle/hle/call_frame.h"
#include <stdexcept>

#if __has_include("vc2/program_data.inc")
#include "vc2/program_data.inc"
#endif

namespace clickhle {
u32 GraphicsApi::load_program(HleCallFrame &frame, const std::array<u32, 16> &a) {
    if (a[0] >= custom_programs_.size() || a[1] < 56 || !a[2]) return 0x300c;
    std::array<std::vector<u8>, 2> stages;
    u32 offset = 0;
    for (auto &stage : stages) {
        if (offset > a[1] || a[1] - offset < 28 || uint64_t(a[2]) + offset + 28 > 0x100000000ull)
            throw std::runtime_error("Truncated custom VC2 program header");
        const auto length = frame.memory.read<u32>(a[2] + offset + 24);
        if (!length || *length > a[1] - offset - 28)
            throw std::runtime_error("Truncated custom VC2 program code");
        const u32 size = 28 + *length;
        const auto *source = frame.memory.host_ptr(a[2] + offset, size);
        if (!source) throw std::runtime_error("Unmapped custom VC2 program");
        const auto *bytes = reinterpret_cast<const u8 *>(source);
        stage.assign(bytes, bytes + size);
        offset += size;
    }
    custom_programs_[a[0]] = std::move(stages);
    programs_loaded_[41 + a[0]] = false;
    return 0x3000;
}

u32 GraphicsApi::select_program(u32 selector) {
    if (selector >= 41 && selector <= 48) {
        const auto &stages = custom_programs_[selector - 41];
        if (stages[0].empty() || stages[1].empty())
            throw std::runtime_error("VC2 custom program slot has not been loaded");
        if (!programs_loaded_[selector])
            for (const auto &stage : stages)
                fragments(0, selector, std::span(stage).first(28), std::span(stage).subspan(28));
        programs_loaded_[selector] = true;
        const u32 flags = endian::load_le<u32>(reinterpret_cast<const std::byte *>(stages[1].data() + 4));
        capability(0xb71, (flags & 0x10000) != 0);
        queue({0, 2, 0, selector});
        selected_program_ = selector;
        return 1;
    }
#ifndef CLICKHLE_HAS_VC2_PROGRAM_DATA
    throw std::runtime_error("Built-in VC2 program records are unavailable");
#else
    if (selector >= 50)
        throw std::runtime_error("Invalid VC2 program selector");
    const std::span<const u8> archive(kVc2ProgramData);
    const auto word = [&](std::size_t offset) {
        if (offset > archive.size() || archive.size() - offset < 4)
            throw std::runtime_error("Truncated built-in VC2 program archive");
        return endian::load_le<u32>(reinterpret_cast<const std::byte *>(archive.data() + offset));
    };
    std::size_t offset = 16;
    u32 flags = 0, found = 0;
    for (u32 i = 0; i < word(12); ++i) {
        const auto slot = word(offset), stage = word(offset + 4), size = word(offset + 8);
        offset += 12;
        if (size < 28 || offset > archive.size() || size > archive.size() - offset)
            throw std::runtime_error("Invalid built-in VC2 program record");
        if (slot == selector) {
            if (!programs_loaded_[selector])
                fragments(0, selector, archive.subspan(offset, 28), archive.subspan(offset + 28, size - 28));
            if (stage == 1) flags = word(offset + 4);
            ++found;
        }
        offset += size;
    }
    if (found != 2) throw std::runtime_error("Missing built-in VC2 program stages");
    programs_loaded_[selector] = true;
    capability(0xb71, (flags & 0x10000) != 0);
    queue({0, 2, 0, selector});
    selected_program_ = selector;
    return 1;
#endif
}
} // namespace clickhle

#include "graphics_api.h"
#include "clickhle/common/endian.h"
#include <stdexcept>

#if __has_include("vc2/program_data.inc")
#include "vc2/program_data.inc"
#endif

namespace clickhle {
u32 GraphicsApi::select_program(u32 selector) {
#ifndef CLICKHLE_HAS_VC2_PROGRAM_DATA
    throw std::runtime_error("Built-in VC2 program records are unavailable");
#else
    if (selector >= 50 || (selector >= 41 && selector <= 48))
        throw std::runtime_error("VC2 custom program slot has no verified implementation");
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
    return 1;
#endif
}
} // namespace clickhle

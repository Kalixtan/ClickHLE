#include "clickhle/loader/eapp_image.h"

#include <string>
#include <string_view>

#include <cstdio>

#include <algorithm>
#include <fstream>

#include "clickhle/common/endian.h"
#include "clickhle/common/log.h"
#include "clickhle/core/address_space.h"
#include "clickhle/loader/game_manifest.h"
#include "clickhle/loader/framework_abi.h"

namespace clickhle {
namespace {

constexpr std::string_view kCategory = "eapp";

void fail(std::string* error, std::string message) {
    CLICKHLE_ERROR(kCategory) << message;
    if (error != nullptr) {
        *error = std::move(message);
    }
}

}  // namespace

std::string GuestImport::key() const {
    if (by_ordinal) {
        return module + "#" + std::to_string(ordinal);
    }
    return module + "!" + name;
}

std::optional<EappImage> EappImage::parse(std::span<const std::byte> data,
                                          std::string* error) {
    EappImage image;

    u32 magic = 0;
    if (!endian::read_le<u32>(data, eapp::kOffMagic, magic)) {
        fail(error, "file is too small to hold an eapp header");
        return std::nullopt;
    }
    if (magic != eapp::kMagic) {
        fail(error, "not an eapp image (magic is not 'eapp')");
        return std::nullopt;
    }

    u32 guard = 0;
    GuestAddr library_ptr = 0;
    if (!endian::read_le<u32>(data, eapp::kOffFlags, image.flags_) ||
        !endian::read_le<u32>(data, eapp::kOffVectorCount, image.vector_count_) ||
        !endian::read_le<u32>(data, eapp::kOffLibraryPtr, library_ptr) ||
        !endian::read_le<u32>(data, eapp::kOffGuard, guard)) {
        fail(error, "eapp header is truncated");
        return std::nullopt;
    }
    for (u32 i = 0; i < eapp::kMaxVectors; ++i) {
        if (!endian::read_le<u32>(data, eapp::kOffVectors + 4u * i,
                                  image.vectors_[i])) {
            fail(error, "eapp header is truncated");
            return std::nullopt;
        }
    }

    if (image.vectors_[0] == 0) {
        fail(error, "image has no entry vector");
        return std::nullopt;
    }

    if (guard != eapp::kGuardInstruction) {
        // Not fatal: it is a guard, not a checksum, and a variant build could
        // legitimately hold something else there. Worth saying out loud,
        // though, because every image in hand agrees on it.
        CLICKHLE_WARN(kCategory)
            << "guard word is 0x" << std::hex << guard << std::dec
            << ", expected 0xeafffffe";
    }
    // The firmware rejects a count above 5 outright, so we do too rather than
    // read vectors that are not there.
    if (image.vector_count_ > eapp::kMaxVectors) {
        fail(error, "image declares " + std::to_string(image.vector_count_) +
                        " entry vectors; the firmware accepts at most 5");
        return std::nullopt;
    }
    // The first block's name is reachable two ways: the header pointer at 0x10
    // and the fixed body offset. They agree in every image; disagreeing would
    // mean the header layout is not what we think it is.
    if (library_ptr != eapp::kImageBase + eapp::kOffFirstBody) {
        CLICKHLE_WARN(kCategory)
            << "header name pointer is 0x" << std::hex << library_ptr
            << ", expected 0x"
            << (eapp::kImageBase + eapp::kOffFirstBody) << std::dec;
    }

    // Walk the descriptor chain. The first body sits at a fixed offset; every
    // later one begins with its own magic immediately after the previous
    // block's pointer table.
    u32 body = eapp::kOffFirstBody;
    for (u32 index = 0; index < eapp::kMaxBlocks; ++index) {
        if (index != 0) {
            u32 block_magic = 0;
            if (!endian::read_le<u32>(data, body, block_magic) ||
                block_magic != eapp::kBlockMagic) {
                // The chain has run out. That is the normal way out of this
                // loop, not an error.
                break;
            }
            body += 4;
        }

        Block block;
        for (u32 i = 0; i < eapp::kLibraryNameMax; ++i) {
            const std::size_t at = body + eapp::kBodyName + i;
            if (at >= data.size()) {
                break;
            }
            const auto c = std::to_integer<u8>(data[at]);
            if (c == 0) {
                break;
            }
            block.name.push_back(static_cast<char>(c));
        }

        // The ABI hash, read before the count so a mismatch is reported with
        // the framework it belongs to.
        for (u32 i = 0; i < 16; ++i) {
            const std::size_t at = body + eapp::kBodyHash + i;
            if (at >= data.size()) {
                break;
            }
            char hex[3];
            std::snprintf(hex, sizeof(hex), "%02x",
                          std::to_integer<u8>(data[at]));
            block.hash += hex;
        }

        u32 count = 0;
        u32 next = 0;
        if (!endian::read_le<u32>(data, body + eapp::kBodyCount, count) ||
            !endian::read_le<u32>(data, body + eapp::kBodyNext, next)) {
            fail(error, "import descriptor '" + block.name + "' is truncated");
            return std::nullopt;
        }
        if (count > eapp::kMaxImportsPerBlock) {
            fail(error, "import descriptor '" + block.name +
                            "' declares an implausible count");
            return std::nullopt;
        }

        const u32 stubs = body + eapp::kBodyStubs;
        const u32 table = stubs + 4u * count;
        const u32 tail = table + 4u * count;
        if (tail > data.size()) {
            fail(error, "import descriptor '" + block.name +
                            "' runs past the end of the image");
            return std::nullopt;
        }

        // Parse every platform's descriptors. Only an exact hash AND count
        // match is safe to bind to the existing Platform 1 HLE ordinals.
        block.compatible_abi = has_compatible_framework_abi(block.name, block.hash, count);

        block.count = count;
        block.stub_table = eapp::kImageBase + stubs;
        block.import_table = eapp::kImageBase + table;

        if (count != 0) {
            u32 first = 0;
            if (!endian::read_le<u32>(data, stubs, first) ||
                (first & eapp::kStubMask) != eapp::kStubPattern) {
                fail(error, "first stub of '" + block.name +
                                "' is not `LDR PC, [PC, #imm]`");
                return std::nullopt;
            }
            // This is the same invariant the firmware's own resolver checks,
            // reached from the other direction. Its binding loop reads the
            // group size out of the first thunk --
            // `n = ((*thunks << 20) >> 22) + 2`, i.e. `imm/4 + 2` -- rather
            // than trusting the count field. Since `table == stubs + 4*count`,
            // the equation below rearranges to exactly `imm/4 + 2 == count`.
            // Two independent derivations of one check, and it holds for
            // 140 of 140 records in the corpus.
            //
            // `LDR PC, [PC, #imm]` at address A reads from A + 8 + imm: ARM's
            // pipeline means PC reads as the instruction's address plus 8. So
            // stub 0's immediate fixes where the whole pointer table starts,
            // and stub n reads the n'th word of it. That is the only reason a
            // table of *identical* instructions works at all, and checking it
            // is what stops a plausible-looking file from binding imports into
            // the middle of the code.
            const auto displacement = static_cast<i32>(first & 0x0FFFu) *
                                      ((first & 0x00800000u) ? 1 : -1);
            if (static_cast<i64>(stubs) + 8 + displacement != table) {
                fail(error, "stub table of '" + block.name +
                                "' does not address the pointer table that "
                                "follows it");
                return std::nullopt;
            }
            for (u32 i = 1; i < count; ++i) {
                u32 word = 0;
                if (!endian::read_le<u32>(data, stubs + 4u * i, word) ||
                    word != first) {
                    fail(error, "stub table of '" + block.name +
                                    "' is not uniform");
                    return std::nullopt;
                }
            }

            // The pointer table is zero on disk because the firmware fills it
            // in. If it is not, this image was dumped from a running system and
            // the addresses in it are firmware addresses that mean nothing
            // here -- we would silently run with them instead of our thunks.
            for (u32 i = 0; i < count; ++i) {
                u32 slot = 0;
                if (endian::read_le<u32>(data, table + 4u * i, slot) &&
                    slot != 0) {
                    CLICKHLE_WARN(kCategory)
                        << block.name << " slot " << i
                        << " is pre-filled with 0x" << std::hex << slot
                        << std::dec << "; binding will overwrite it";
                    break;
                }
            }
        }

        image.imports_.reserve(image.imports_.size() + count);
        for (u32 i = 0; i < count; ++i) {
            GuestImport import;
            import.module = block.name;
            import.ordinal = i;
            import.by_ordinal = true;
            import.compatible_abi = block.compatible_abi;
            import.slot_address = block.import_table + 4u * i;
            image.imports_.push_back(std::move(import));
        }

        if (index == 0) {
            image.library_ = block.name;
            image.code_start_ = next;
        }
        image.blocks_.push_back(std::move(block));

        // End of chain. The last descriptor carries no imports and a null
        // `next`; its name is a Fermat's Last Theorem joke. Stopping on the
        // sentinel rather than on "the next magic is absent" matters, because
        // the bytes after it are game code and can hold the magic by
        // coincidence -- which is exactly what happens in Tetris, four bytes
        // past the terminator.
        if (next == 0) {
            break;
        }

        // For every other block, `next` points four bytes past the following
        // magic. Checking it is free and catches a chain walked off course.
        if (next != eapp::kImageBase + tail + 4u) {
            fail(error, "import descriptor '" + image.blocks_.back().name +
                            "' does not chain to the descriptor after it");
            return std::nullopt;
        }
        body = tail;
    }

    if (image.blocks_.empty()) {
        fail(error, "image has no import descriptors");
        return std::nullopt;
    }

    image.raw_.assign(data.begin(), data.end());

    CLICKHLE_INFO(kCategory)
        << "eapp, " << image.vector_count_ << " vectors, "
        << image.blocks_.size()
        << " import blocks, " << image.imports_.size() << " imports, "
        << data.size() << " bytes";
    for (const auto& block : image.blocks_) {
        CLICKHLE_DEBUG(kCategory)
            << "  " << block.name << " x " << block.count << " @0x" << std::hex
            << block.import_table << std::dec;
    }
    return image;
}


std::optional<EappImage> EappImage::load_file(const std::filesystem::path& path,
                                              std::string* error,
                                              u32 preferred_platform) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        fail(error, "cannot open '" + path.string() + "'");
        return std::nullopt;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff length = file.tellg();
    file.seekg(0, std::ios::beg);
    if (length <= 0) {
        fail(error, "'" + path.string() + "' is empty");
        return std::nullopt;
    }
    std::vector<std::byte> data(static_cast<std::size_t>(length));
    // std::byte has no char constructor, so the read goes through a char* view
    // of the same storage rather than an iterator range.
    file.read(reinterpret_cast<char*>(data.data()), length);
    if (!file) {
        fail(error, "failed to read '" + path.string() + "'");
        return std::nullopt;
    }
    auto image = parse(data, error);
    if (!image) return std::nullopt;
    image->available_platform_ids_ = platforms_for_executable(path);
    if (preferred_platform != 0 && !image->available_platform_ids_.empty() &&
        std::find(image->available_platform_ids_.begin(),
                  image->available_platform_ids_.end(), preferred_platform) ==
            image->available_platform_ids_.end()) {
        fail(error, "platform " + std::to_string(preferred_platform) +
                    " does not select this executable in Manifest.plist");
        return std::nullopt;
    }
    if (preferred_platform != 0)
        image->platform_id_ = preferred_platform;
    else if (!image->available_platform_ids_.empty())
        image->platform_id_ = image->available_platform_ids_.front();
    else if (!image->blocks_.empty() && image->blocks_.front().name == "OpenGLES" &&
             image->blocks_.front().compatible_abi)
        image->platform_id_ = 1; // A standalone Platform 1 image without a manifest.
    return image;
}

bool EappImage::map_into(AddressSpace& memory, GuestAddr arena_end,
                         std::string* error) const {
    if (raw_.empty()) {
        fail(error, "no image to map");
        return false;
    }
    // One flat mapping. There are no sections, so there is nothing to place
    // separately and no per-section protection to honour -- and the import
    // pointer table sits inside the image, so the whole thing has to be
    // writable for binding to patch it.
    //
    // The file ends where its initialised data ends; zero-initialised statics
    // continue above it. Keep the image and arena in one host region: a guest
    // read may cross the page boundary between them. Two adjacent regions
    // look contiguous to the ARM CPU but host_ptr() cannot span both buffers.
    //
    // The zero-initialised statics above the file are not a few bytes; they are
    // the whole BSS, and titles reach a long way into it:
    //
    //     Mahjong        image ends 0x18049e70, needs at least +0x5160
    //     Tetris         image ends 0x180256ec, needs at least +0x855c
    //     Texas Hold'em  image ends 0x1805acd4, needs at least +0x83074
    //
    // Texas Hold'em's singleton list head lives 0x83074 bytes past EOF, and
    // every path that inserts into it faulted -- which is why three unrelated
    // completion mechanisms all produced one fault at one instruction. They
    // were not three bugs; they were three routes to the same unmapped load.
    //
    const auto size = static_cast<u32>(raw_.size());
    const u64 minimum_end = eapp::kImageBase +
                            ((static_cast<u64>(size) + 0xFFFu) & ~0xFFFull);
    const u64 end = std::max<u64>(arena_end, minimum_end);
    const u32 mapped = static_cast<u32>(end - eapp::kImageBase);
    if (!memory.map(eapp::kImageBase, mapped,
                    AddressSpace::kRead | AddressSpace::kWrite |
                        AddressSpace::kExec,
                    "image-and-arena")) {
        fail(error, "failed to map the image and arena at 0x18000000");
        return false;
    }
    if (!memory.write_bytes(eapp::kImageBase, raw_.data(), size)) {
        fail(error, "failed to copy the image into guest memory");
        return false;
    }
    return true;
}

std::vector<std::string> EappImage::imported_modules() const {
    std::vector<std::string> modules;
    for (const auto& import : imports_) {
        if (std::find(modules.begin(), modules.end(), import.module) ==
            modules.end()) {
            modules.push_back(import.module);
        }
    }
    return modules;
}

}  // namespace clickhle

#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "clickhle/common/types.h"

namespace clickhle {

class AddressSpace;

/// Layout constants for the iPod click-wheel game executable format.
///
/// The format is undocumented; these fields are checked against the installed
/// game corpus and the 5G/5.5G firmware export tables.
///
/// An image is a **chain of import descriptors** followed by game code. The
/// first descriptor is the file header and carries extra fields; every later
/// one repeats the same shape behind its own magic. Games import from six to
/// more libraries on later iPod platforms.
///
///     first descriptor (magic 'eapp'):
///       0x00  "eapp"                     magic
///       0x04  0x10001000                 constant; version or stack sizing
///       0x08  5                          number of entry vectors (max 5)
///       0x0c  0x28                       offset of the trailing guard word
///       0x10  ptr -> this block's name
///       0x14  vector table: five absolute entry points, 0 = unused
///             [0] entry, [1] frame hook, [2..4] optional
///       0x28  0xeafffffe                 ARM `b .`, a guard
///       0x2c  <common descriptor body>
///
///     later descriptors (magic 0x29061968):
///       0x00  0x29061968                 magic
///       0x04  <common descriptor body>
///
///     common descriptor body:
///       +0x00  char name[0x20]              NUL-padded, e.g. "OpenGLES"
///       +0x20  byte hash[0x10]              the framework's ABI hash
///       +0x30  u32  count                   imports in this block
///       +0x34  u32  next                    address just past the next magic
///       +0x38  count x `LDR PC,[PC,#imm]`   the stubs
///       +      count x u32                  pointer table, ZERO ON DISK
///
/// The hash identifies the *interface version*, not the title. Platform 1's
/// OpenGLES hash is 041f4da520603c37d8ef9b879efbf280; later platforms use
/// another interface. The Platform 1 values match the table RetailOS publishes
/// in its `osos` image.
///
/// This also settles the 16 unexplained bytes at file offset 0x4c that the
/// corpus analysis had listed as an unidentified constant. They are the
/// OpenGLES hash sitting in the first import record.
///
/// The zero-filled pointer tables are the whole reason this project is
/// possible: the firmware patches them at load time, so the game↔firmware
/// boundary is a stable ordinal-indexed interface rather than absolute-address
/// binding. We patch them with SVC thunks instead.
///
/// The chain ends with a zero-count block whose name is a Fermat's Last Theorem
/// joke. It carries no imports and exists only as a terminator.
namespace eapp {

/// All four platform variants in the installed corpus use this base. There
/// is no relocation information in the format.
inline constexpr GuestAddr kImageBase = 0x18000000u;

inline constexpr u32 kMagic = 0x70706165u;  ///< 'eapp' read as a LE word.

/// Magic on every descriptor after the first. 1968-06-29 read as digits;
/// somebody's birthday, presumably.
inline constexpr u32 kBlockMagic = 0x29061968u;

/// First-descriptor header offsets, named so the parser reads as the table
/// above.
inline constexpr u32 kOffMagic       = 0x00;
inline constexpr u32 kOffFlags       = 0x04;
inline constexpr u32 kOffVectorCount = 0x08;
inline constexpr u32 kOffGuardOffset = 0x0C;
inline constexpr u32 kOffLibraryPtr  = 0x10;
inline constexpr u32 kOffVectors     = 0x14;

inline constexpr u32 kOffGuard       = 0x28;

/// Where the first descriptor's common body begins.
inline constexpr u32 kOffFirstBody = 0x2C;

/// Offsets within the common descriptor body.
inline constexpr u32 kBodyName  = 0x00;
inline constexpr u32 kBodyHash  = 0x20;
inline constexpr u32 kBodyCount = 0x30;
inline constexpr u32 kBodyNext  = 0x34;
inline constexpr u32 kBodyStubs = 0x38;

inline constexpr u32 kLibraryNameMax = 0x20;

inline constexpr u32 kGuardInstruction = 0xEAFFFFFEu;  ///< ARM `b .`

/// `LDR PC, [PC, #+/-imm]`. One-import later-platform descriptors use -4.
inline constexpr u32 kStubMask    = 0xFF7FF000u;
inline constexpr u32 kStubPattern = 0xE51FF000u;

/// Sanity bound per block. The largest firmware library exports 179.
inline constexpr u32 kMaxImportsPerBlock = 4096;

/// Sanity bound on the chain, well above the eight blocks any game uses.
inline constexpr u32 kMaxBlocks = 64;

/// The firmware clamps the header's vector count to this and rejects anything
/// larger.
inline constexpr u32 kMaxVectors = 5;

}  // namespace eapp

/// One entry of a guest image's import table.
///
/// The loader is the only thing that knows how imports are encoded; everything
/// downstream -- the registry, the thunk table, the coverage report -- works in
/// terms of these.
struct GuestImport {
    std::string module;         ///< As spelled by the image, e.g. "OpenGLES".
    std::string name;           ///< Empty when imported by ordinal alone.
    u32 ordinal = 0;
    bool by_ordinal = true;
    bool compatible_abi = false; ///< Safe to bind against the current HLE registry.
    GuestAddr slot_address = 0; ///< Pointer-table word this import resolves to.

    /// "OpenGLES#42" -- a stable key for registry lookups and coverage
    /// reports. Named imports use "Module!name".
    ///
    /// Ordinals are per-library and start at zero, so the module name is not
    /// decoration: "OpenGLES#0" and "Audio#0" are different functions.
    [[nodiscard]] std::string key() const;
};

/// A parsed `eapp` image.
///
/// Parsing is done entirely through bounds-checked little-endian reads; no
/// struct is ever cast over the file buffer. That keeps the loader portable to
/// big-endian and strict-alignment hosts, and safe against truncated files.
class EappImage {
public:
    /// One parsed import descriptor.
    struct Block {
        std::string name;
        u32 count = 0;
        /// The framework's ABI hash, as 32 lowercase hex characters.
        std::string hash;
        bool compatible_abi = false;
        GuestAddr stub_table = 0;
        GuestAddr import_table = 0;
    };

    [[nodiscard]] static std::optional<EappImage> parse(
        std::span<const std::byte> data, std::string* error = nullptr);

    [[nodiscard]] static std::optional<EappImage> load_file(
        const std::filesystem::path& path, std::string* error = nullptr,
        u32 preferred_platform = 0);

    /// Maps the image at kImageBase and copies the whole file in.
    ///
    /// The image is one flat blob -- there are no sections and no relocations,
    /// so there is nothing to place or fix up. It is mapped read/write/execute
    /// because the import pointer tables live inside it and have to be writable
    /// for the thunk addresses to be patched in, exactly as the firmware
    /// patches them with real ones.
    [[nodiscard]] bool map_into(AddressSpace& memory, GuestAddr arena_end,
                                std::string* error = nullptr) const;

    /// Vectors the header declares. Five in every image seen, but the format
    /// allows fewer -- the firmware accepts 0 to 5.
    [[nodiscard]] u32 vector_count() const noexcept { return vector_count_; }
    [[nodiscard]] u32 flags() const noexcept { return flags_; }
    /// Zero means the executable could not be associated with a manifest.
    [[nodiscard]] u32 platform_id() const noexcept { return platform_id_; }
    [[nodiscard]] const std::vector<u32>& available_platform_ids() const noexcept {
        return available_platform_ids_;
    }

    /// Every import descriptor, in chain order.
    [[nodiscard]] const std::vector<Block>& blocks() const noexcept {
        return blocks_;
    }

    /// The first block's library (OpenGLES on Platform 1, AsyncFileIO later).
    [[nodiscard]] const std::string& library() const noexcept {
        return library_;
    }

    [[nodiscard]] GuestAddr image_base() const noexcept {
        return eapp::kImageBase;
    }
    [[nodiscard]] u32 size_of_image() const noexcept {
        return static_cast<u32>(raw_.size());
    }

    /// First byte after the first block's tables, from the header word at 0x60.
    ///
    /// Not actually where code starts -- the rest of the descriptor chain sits
    /// between here and the code -- so this is the `next` field of the first
    /// block, kept under its original name because that is what the header
    /// calls it.
    [[nodiscard]] GuestAddr code_start() const noexcept { return code_start_; }

    /// The header's entry vectors, in header order. Zero means unused, and
    /// most titles set three of the five.
    ///
    /// The firmware copies these into its own app structure and uses [2] and
    /// [4] to pick a mode -- so they are not a simple entry/frame-hook pair.
    /// Which one it calls, and with what, depends on the game.
    [[nodiscard]] const std::array<GuestAddr, eapp::kMaxVectors>& vectors()
        const noexcept {
        return vectors_;
    }
    /// Vector 0. The firmware calls this once, with no arguments, right after
    /// binding the import tables -- read off the startup path, which does
    /// `loader(app); ...; (**(code **)(app + 0x250))();` where +0x250 is
    /// vector[0] in its copy of the table.
    [[nodiscard]] GuestAddr entry_point() const noexcept { return vectors_[0]; }

    /// The vector the firmware *registers* as the app's callback.
    ///
    /// Not vector[1], which is what this used to return. The firmware copies
    /// the table into its app structure at +0x250 and then sets a mode byte
    /// from which slots are populated: vector[2] if that is non-zero, else
    /// vector[4]. Only vector[4] is seen being handed to a subsystem --
    /// `slot->vtable[0xc](slot, app->vector4)` -- so it is a registered
    /// callback rather than something called inline.
    ///
    /// Every title in hand leaves [2] and [3] zero and sets [4].
    [[nodiscard]] GuestAddr frame_hook() const noexcept {
        return vectors_[2] != 0 ? vectors_[2] : vectors_[4];
    }

    /// Every import from every block, flattened.
    [[nodiscard]] const std::vector<GuestImport>& imports() const noexcept {
        return imports_;
    }

    /// Distinct imported library names, in chain order.
    [[nodiscard]] std::vector<std::string> imported_modules() const;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return raw_;
    }

private:
    u32 platform_id_ = 0;
    std::vector<u32> available_platform_ids_;
    u32 flags_ = 0;
    u32 vector_count_ = 0;
    std::string library_;
    GuestAddr code_start_ = 0;
    std::vector<Block> blocks_;
    std::array<GuestAddr, eapp::kMaxVectors> vectors_{};
    std::vector<GuestImport> imports_;
    std::vector<std::byte> raw_;
};

}  // namespace clickhle

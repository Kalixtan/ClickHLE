#pragma once

#include <string>
#include <vector>

#include "clickhle/common/types.h"

namespace clickhle {

/// Running the firmware itself, as opposed to emulating its API.
///
/// Exists to inspect the game-library table, which firmware decrypts at boot.
/// Nothing in the HLE path depends on this.
struct BootOptions {
    std::string image;             ///< An extracted `osos` image.
    std::string cpu_backend;       ///< Empty means the build default.

    /// Where the partition directory says the image loads. 0x10000000 for
    /// `osos` on a 5G/5.5G; tools/fw_extract.py prints it.
    GuestAddr load_address = 0x10000000u;
    GuestAddr entry = 0x10727800u;

    /// The device has 64 MB. Mapped in full because firmware will reach for
    /// working memory well above its own image.
    u32 sdram_size = 64u * 1024u * 1024u;

    /// Bounded, because firmware that faults into a loop would otherwise spin
    /// forever.
    u64 max_instructions = 50'000'000;

    /// The region to report entropy on afterwards. Defaults to the descriptor
    /// table, which is the whole reason this mode exists.
    GuestAddr probe_address = 0x1007bd44u;
    u32 probe_size = 0x8e4u;

    /// Report every write covering these addresses, with the pc that did it.
    ///
    /// This is how the decryptor gets found. The table is decrypted in place,
    /// so whatever writes into it *is* the routine we want, and its pc is the
    /// entry to jump at directly rather than booting all the way there.
    std::vector<GuestAddr> watches;
};

/// Returns 0 on a clean stop, 2 on a guest fault, 1 on a setup failure.
int run_firmware(const BootOptions& options);

}  // namespace clickhle

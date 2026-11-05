// Run firmware, rather than emulate its API.
//
// This is the *other* mode, and it exists for one reason: the game-facing
// library table is encrypted in firmware images and is decrypted in place at
// boot. Statically
// it is unreadable. If the firmware runs far enough to decrypt it, it can
// simply be read out of guest memory -- which would turn ordinal identification
// from call-site archaeology into reading a table.
//
// Deliberately separate from the game loader. Nothing in the HLE path depends
// on this working, and this path registers no HLE functions: it is the firmware
// executing its own code, so anything it calls has to be there already.
//
// It is not expected to boot a device. Firmware touches hardware we do not
// model, and it will fault. The question is only whether it faults *before* or
// *after* it decrypts the table, and the entropy report answers that directly.

#include "clickhle/core/boot.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <vector>

#include "clickhle/common/log.h"
#include "clickhle/core/address_space.h"
#include "clickhle/core/cpu.h"

namespace clickhle {
namespace {

constexpr std::string_view kCategory = "boot";

/// Shannon entropy in bits per byte. Ciphertext and compressed data sit near 8;
/// ARM code and tables in this image sit between 5.2 and 6.0, so the gap is
/// wide enough to read as a yes/no.
double entropy(const std::vector<std::byte>& bytes) {
    if (bytes.empty()) {
        return 0.0;
    }
    std::map<u8, std::size_t> counts;
    for (const std::byte b : bytes) {
        ++counts[std::to_integer<u8>(b)];
    }
    double bits = 0.0;
    for (const auto& [value, n] : counts) {
        const double p = static_cast<double>(n) /
                         static_cast<double>(bytes.size());
        bits -= p * std::log2(p);
    }
    return bits;
}

}  // namespace

int run_firmware(const BootOptions& options) {
    std::ifstream file(options.image, std::ios::binary);
    if (!file) {
        std::cerr << "cannot open " << options.image << "\n";
        return 1;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff length = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<std::byte> image(length > 0 ? static_cast<std::size_t>(length)
                                            : 0);
    // std::byte has no char constructor, so the read goes through a char* view
    // of the same storage.
    if (!image.empty()) {
        file.read(reinterpret_cast<char*>(image.data()), length);
    }
    if (image.empty()) {
        std::cerr << options.image << " is empty\n";
        return 1;
    }

    AddressSpace memory;

    // SDRAM. The image is copied in at its load address, and the rest is the
    // working memory the firmware expects to find above it.
    const u32 sdram_size = options.sdram_size;
    if (!memory.map(options.load_address, sdram_size, AddressSpace::kRead |
                        AddressSpace::kWrite | AddressSpace::kExec, "sdram")) {
        std::cerr << "could not map SDRAM\n";
        return 1;
    }
    if (image.size() > sdram_size) {
        std::cerr << "image is larger than the SDRAM window\n";
        return 1;
    }
    if (!memory.write_bytes(options.load_address, image.data(),
                            static_cast<u32>(image.size()))) {
        std::cerr << "could not copy the image into SDRAM\n";
        return 1;
    }

    // The low alias. PP502x answers SDRAM at 0x00000000 as well, and the
    // firmware's own library table is reached through low-address pointers
    // while its code runs from the high alias.
    if (!memory.map_mirror(0, sdram_size, options.load_address, "sdram-low")) {
        std::cerr << "could not mirror SDRAM at zero\n";
        return 1;
    }

    // Watches are installed before the core, so nothing can slip past between
    // mapping and the first instruction.
    std::size_t reported = 0;
    for (const GuestAddr address : options.watches) {
        memory.watch(address);
    }
    if (!options.watches.empty()) {
        memory.set_watch_handler(
            [&reported](GuestAddr address, u32 size, u64 value) {
                // Bounded: a memset over the region would otherwise produce
                // thousands of identical lines and bury the first one, which
                // is the only one that matters.
                if (++reported > 16) {
                    return;
                }
                std::cout << "  write " << size << "B to 0x" << std::hex
                          << address << " = 0x" << value << std::dec << "\n";
            });
    }

    auto cpu = make_cpu_core(options.cpu_backend, memory);
    if (cpu == nullptr) {
        std::cerr << "no usable CPU backend\n";
        return 1;
    }
    cpu->reset();
    cpu->set_pc(options.entry);
    cpu->set_sp(options.load_address + sdram_size - 64);

    std::cout << "booting " << options.image << "\n";
    std::cout << "  sdram   0x" << std::hex << options.load_address << " + 0x"
              << sdram_size << " (mirrored at 0)\n";
    std::cout << "  entry   0x" << options.entry << std::dec << "\n";
    std::cout << "  budget  " << options.max_instructions << " instructions\n\n";

    const RunResult result = cpu->run(options.max_instructions);
    if (!options.watches.empty()) {
        std::cout << reported << " write(s) covered the watched addresses\n";
    }
    std::cout << "ran " << result.instructions_executed << " instructions";
    if (result.status == RunStatus::Fault) {
        std::cout << ", faulted: " << result.fault;
    }
    std::cout << "\n";

    // The point of the exercise: has the region become plaintext?
    if (options.probe_size != 0) {
        std::vector<std::byte> region(options.probe_size);
        if (memory.read_bytes(options.probe_address, region.data(),
                              options.probe_size)) {
            const double bits = entropy(region);
            std::cout << "\nregion 0x" << std::hex << options.probe_address
                      << " + 0x" << options.probe_size << std::dec
                      << ": entropy " << std::fixed << std::setprecision(3)
                      << bits << " bits/byte -- "
                      << (bits > 7.5 ? "still encrypted"
                                     : "PLAINTEXT, dump it")
                      << "\n";
        } else {
            std::cout << "\nprobe region is not mapped\n";
        }
    }
    return result.status == RunStatus::Fault ? 2 : 0;
}

}  // namespace clickhle

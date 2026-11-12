#include "jit.h"
#include "jit_cache.h"
#include "jit_vector.h"
#include "core.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/Orc/IRCompileLayer.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>

namespace clickhle::vc2 {
namespace {

struct Operation {
    u16 word = 0;
    unsigned opcode = 0;
    unsigned destination = 0;
    unsigned left = 0;
    unsigned source = 0;
    unsigned memory_index = 7;
    unsigned memory_slot = 0;
    u32 immediate = 0;
    u32 next_pc = 0;
    bool immediate_source = false;
    bool stack_load = false;
    bool memory_load = false;
    bool nop = false;
    bool branch = false;
    bool register_branch = false;
    bool link = false;
    bool add_compare = false;
    bool add_immediate = false;
    bool compare_register = false;
    unsigned compare_source = 0;
    u32 add_value = 0;
    u32 compare_value = 0;
    u32 guest_pc = 0;
};

bool native_alu(unsigned opcode) {
    switch (opcode) {
    case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9:
    case 10: case 11: case 12: case 13: case 14: case 15: case 16:
    case 18: case 19: case 20: case 21: case 22: case 23: case 24:
    case 26: case 27: case 29: case 30: case 31:
        return true;
    default:
        return false;
    }
}

std::optional<Operation> decode(u16 word, u32 pc) {
    if (word == 0x4430)
        return Operation{.word = word, .source = 26, .branch = true,
                         .register_branch = true};
    if ((word & 0xfff0) == 0x4730)
        return Operation{.word = word,
                         .source = static_cast<unsigned>(word & 15),
                         .next_pc = pc + 2, .branch = true,
                         .register_branch = true, .link = true};
    if ((word & 0xfff0) == 0x4630)
        return Operation{.word = word,
                         .source = static_cast<unsigned>(word & 15),
                         .branch = true, .register_branch = true};
    if (word == 0x4230)
        return Operation{.word = word, .nop = true};
    if (word == 0x4a30 || word == 0x4b30)
        return Operation{.word = word, .opcode = word == 0x4a30 ? 15u : 7u,
                         .destination = 30, .left = 30,
                         .immediate = word == 0x4a30 ? (1u << 30) : ~(1u << 30),
                         .immediate_source = true};
    if ((word & 0xfff0) == 0x5030)
        return Operation{.word = word, .opcode = 16,
                         .destination = 30, .left = static_cast<unsigned>(word & 15),
                         .immediate_source = true};
    if ((word & 0xffc0) == 0x3f00) {
        return Operation{.word = word, .opcode = 12, .destination = 25,
                         .left = 25,
                         .immediate = 4u * static_cast<u32>(sign_extend(word & 63, 6)),
                         .immediate_source = true};
    }
    if ((word & 0xf080) == 0x3000)
        return Operation{.word = word,
                         .immediate = pc + 2u * static_cast<u32>(sign_extend(word & 127, 7)),
                         .next_pc = pc + 2, .branch = true};
    if ((word & 0xe010) == 0x4000)
        return Operation{.word = word, .opcode = 14,
                         .destination = static_cast<unsigned>(word & 15),
                         .immediate = (pc & ~3u) + 4 * ((word >> 5) & 255),
                         .immediate_source = true};
    if ((word & 0xe030) == 0x4010 && ((word >> 6) & 3) == 0)
        return Operation{.word = word,
                         .destination = static_cast<unsigned>(word & 15),
                         .immediate = 4u * static_cast<u32>((word >> 8) & 31),
                         .stack_load = true};
    if ((word & 0xe030) == 0x4010 && ((word >> 6) & 3) == 2)
        return Operation{.word = word, .opcode = 12,
                         .destination = static_cast<unsigned>(word & 15),
                         .left = 25, .immediate = 4u * static_cast<u32>((word >> 8) & 31),
                         .immediate_source = true};
    if ((word & 0xc000) == 0x8000 && ((word >> 8) & 7) == 0)
        return Operation{.word = word, .destination = static_cast<unsigned>(word & 15),
                         .source = static_cast<unsigned>((word >> 4) & 15),
                         .memory_index = static_cast<unsigned>((word >> 11) & 7),
                         .memory_load = true};
    if ((word & 0xf000) == 0x6000) {
        const unsigned opcode = 2 * ((word >> 9) & 15);
        const unsigned destination = word & 15;
        if (!native_alu(opcode)) return std::nullopt;
        u32 immediate = (word >> 4) & 31;
        if (opcode != 4 && opcode != 6 && opcode != 8)
            immediate = static_cast<u32>(sign_extend(immediate, 5));
        return Operation{.word = word, .opcode = opcode,
                         .destination = destination, .left = destination,
                         .immediate = immediate,
                         .immediate_source = true};
    }
    // The 0xe000/0xf000 classes are stack and memory instructions, checked
    // before the register-ALU class in the reference interpreter.
    if ((word & 0xe000) != 0xe000 && (word & 0xc000) == 0xc000) {
        const unsigned opcode = (word >> 8) & 31;
        const unsigned destination = word & 15;
        if (!native_alu(opcode)) return std::nullopt;
        return Operation{.word = word, .opcode = opcode,
                         .destination = destination, .left = destination,
                         .source = static_cast<unsigned>((word >> 4) & 15)};
    }
    return std::nullopt;
}

using NativeVectorRegion = void (*)(u8 *, const u32 *, bool *, bool *, bool *);
using NativeIndexedH16 = void (*)(u8 *, const u8 *, u32, const std::int64_t *, const u32 *, u32, u32);
using NativeBlock = void (*)(u32 *, const u8 *, const u32 *);
using NativeAddress = u32 (*)(u32 *);
using NativeFillLoop = void (*)(u32 *, u8 *, u32);
using NativeMove = void (*)(u8 *, u32, u32);
using NativeLoadBytes = void (*)(u8 *, u32, const u8 *);
using NativeGatherBytes = void (*)(const u8 *, u32, u8 *);
using NativeImmediate = void (*)(u8 *, u32, u32, u32);
using NativeBinary = void (*)(u8 *, u32, u32, u32, u32, std::int64_t *, u32 *, bool *, bool *, bool *, u32 *, u32);
using NativeAddFlags = void (*)(u8 *, u32, u32, u32, bool *, bool *, bool *);

// Decode extended scalar operations into the same region as compact ALU
// instructions. Branches terminate a region after consuming its SSA values.
std::optional<Operation> decode_extended(u16 word, u16 second, u32 pc) {
    if ((word & 0xf000) == 0) {
        const unsigned mode = second >> 14;
        const unsigned destination = word & 15;
        const unsigned compare_source = (second >> 10) & 15;
        if (mode < 2 && compare_source == destination &&
            !(mode == 1 && ((word >> 4) & 15) == 0)) return std::nullopt;
        const unsigned bits = mode < 2 ? 10 : 8;
        return Operation{.word = word, .destination = destination,
            .left = static_cast<unsigned>((word >> 4) & 15),
            .immediate = pc + 2u * static_cast<u32>(sign_extend(second & ((1u << bits) - 1), bits)),
            .next_pc = pc + 4, .branch = true, .add_compare = true,
            .add_immediate = static_cast<bool>(mode & 1),
            .compare_register = mode < 2, .compare_source = compare_source,
            .add_value = static_cast<u32>(sign_extend((word >> 4) & 15, 4)),
            .compare_value = static_cast<u32>((second >> 8) & 63)};
    }
    if ((word & 0xf000) == 0x1000) {
        const bool link = word & 128;
        const u32 encoded = link
            ? (((word >> 8) & 15) << 23) | (static_cast<u32>(second) << 7) | (word & 127)
            : (static_cast<u32>(second) << 7) | (word & 127);
        return Operation{.word = word,
            .immediate = pc + 2u * static_cast<u32>(sign_extend(encoded, link ? 27 : 23)),
            .next_pc = pc + 4, .branch = true, .link = link};
    }
    if ((word & 0xff00) == 0x2c00)
        return Operation{.word = word, .opcode = 12,
            .destination = static_cast<unsigned>(word & 15),
            .left = static_cast<unsigned>((word >> 4) & 15),
            .immediate = static_cast<u32>(sign_extend(second, 16)), .immediate_source = true};
    if ((word & 0xfc00) == 0x2800 && ((word >> 8) & 3) == 0 && !(second & 3))
        return Operation{.word = word, .destination = static_cast<unsigned>(word & 15),
            .source = static_cast<unsigned>((word >> 4) & 15),
            .immediate = static_cast<u32>(sign_extend(second, 16)), .memory_load = true};
    if ((word & 0xfff0) == 0x2d40 && !(second & 3))
        return Operation{.word = word, .destination = static_cast<unsigned>(word & 15),
            .immediate = second, .stack_load = true};
    if ((word & 0xffe0) == 0x2dc0)
        return Operation{.word = word, .opcode = 12,
            .destination = static_cast<unsigned>(word & 15),
            .left = static_cast<unsigned>((word & 16) ? 25 : 24),
            .immediate = second, .immediate_source = true};
    if ((word & 0xfff0) == 0x2de0 && !(second & 0x8000))
        return Operation{.word = word, .opcode = 14,
            .destination = static_cast<unsigned>(word & 15),
            .immediate = pc + second, .immediate_source = true};
    if ((word & 0xffc0) == 0x2e80) {
        const unsigned opcode = (second >> 10) & 31;
        const bool ordinary = word & 32;
        if ((word & 31) == 31 && (!ordinary || opcode != 16)) return std::nullopt;
        if ((ordinary && native_alu(opcode)) || (!ordinary && (opcode < 4 || opcode == 11)))
            return Operation{.word = word, .opcode = ordinary ? opcode : 32 + opcode,
                .destination = static_cast<unsigned>(word & 31),
                .left = static_cast<unsigned>(second & 31),
                .source = static_cast<unsigned>((second >> 5) & 31),
                .immediate = static_cast<u32>((second >> 5) & 31),
                .immediate_source = (second & 0x8000) != 0};
    }
    if ((word & 0xff80) == 0x2e00 && ((word & 31) != 31 || ((word >> 5) & 3) == 0))
        return Operation{.word = word,
            .opcode = std::array<unsigned, 4>{16, 10, 12, 14}[(word >> 5) & 3],
            .destination = static_cast<unsigned>(word & 31), .left = static_cast<unsigned>(word & 31),
            .immediate = static_cast<u32>(sign_extend(second, 16)), .immediate_source = true};
    return std::nullopt;
}

struct Block {
    enum class Kind { Scalar, Memory, Stack, TableBranch, VectorLoad, RasterStore, MoveH16, Broadcast, BinaryVector, AddSetfH16, FillLoop, VectorRegion, IndexedH16 } kind = Kind::Scalar;
    NativeVectorRegion vector_native = nullptr;
    std::vector<u32> vector_pcs;
    u32 pc = 0;
    std::vector<Operation> operations;
    std::array<u16, 64> words{};
    unsigned length = 0;
    unsigned stack_read_bytes = 0;
    struct MemoryRead {
        unsigned base;
        unsigned index;
        u32 offset;
    };
    std::array<MemoryRead, 32> memory_reads{};
    unsigned memory_read_count = 0;
    unsigned repeats = 1;
    unsigned predicate = 0;
    bool dynamic_repeats = false;
    std::array<u64, 2> code_versions{};
    VectorOperand destination, source, right;
    u32 immediate = 0;
    bool scalar_source = false;
    bool broadcast_operand = false;
    unsigned scalar_register = 7;
    unsigned reduction_register = 7;
    NativeFillLoop fill_native = nullptr;
    NativeIndexedH16 indexed_native = nullptr;
    unsigned fill_counter = 0;
    NativeBlock native = nullptr;
    NativeAddress native_address = nullptr;
    unsigned memory_op = 0, memory_register = 0, memory_base = 0, memory_width = 0;
    bool memory_pre = false;
    bool memory_indexed = false;
    unsigned stack_base = 0, stack_count = 0, stack_total = 0;
    bool stack_push = false;
    unsigned load_base = 7, load_stride = 7;
    u32 load_offset = 0;
    NativeMove move_native = nullptr;
    NativeLoadBytes load_bytes_native = nullptr;
    NativeGatherBytes gather_bytes_native = nullptr;
    NativeImmediate immediate_native = nullptr;
    NativeBinary binary_native = nullptr;
    NativeAddFlags add_flags_native = nullptr;
    llvm::orc::ResourceTrackerSP resources;
};

struct Rejection {
    std::array<u16, 5> words{};
    unsigned length = 0;
    std::array<u64, 2> code_versions{};
};

struct Entry {
    Block *block = nullptr;
    Rejection rejection;
    unsigned visits = 0;
};

struct Page {
    std::array<Entry, 2048> entries{};
};

} // namespace

struct Jit::Impl {
    std::unique_ptr<llvm::orc::LLJIT> engine;
    JitObjectCache object_cache;
    struct LoadedObject {
        llvm::orc::ExecutorAddr address;
        llvm::orc::ResourceTrackerSP resources;
    };
    std::unordered_map<std::string, LoadedObject> loaded_objects;
    std::unordered_map<u32, Block> blocks;
    std::vector<std::unique_ptr<Page>> pages =
        std::vector<std::unique_ptr<Page>>(0x4000000 / 4096);
    NativeMove move_kernel = nullptr;
    llvm::orc::ResourceTrackerSP move_resources;
    NativeLoadBytes load_bytes_kernel = nullptr;
    NativeIndexedH16 indexed_h16_kernel = nullptr;
    llvm::orc::ResourceTrackerSP indexed_h16_resources;
    llvm::orc::ResourceTrackerSP load_bytes_resources;
    NativeGatherBytes gather_bytes_kernel = nullptr;
    llvm::orc::ResourceTrackerSP gather_bytes_resources;
    struct ImmediateKernel {
        NativeImmediate native = nullptr;
        llvm::orc::ResourceTrackerSP resources;
    };
    std::unordered_map<unsigned, ImmediateKernel> immediate_kernels;
    struct BinaryKernel {
        NativeBinary native = nullptr;
        llvm::orc::ResourceTrackerSP resources;
    };
    std::unordered_map<unsigned, BinaryKernel> binary_kernels;
    struct AddressKernel {
        NativeAddress native = nullptr;
        llvm::orc::ResourceTrackerSP resources;
    };
    std::unordered_map<u64, AddressKernel> address_kernels;
    std::array<NativeAddFlags, 2> add_flags_kernels{};
    std::vector<llvm::orc::ResourceTrackerSP> add_flags_resources =
        std::vector<llvm::orc::ResourceTrackerSP>(2);
    u64 next_symbol = 0;
    bool compare = false;
    bool profile = false;
    bool sample_time = std::getenv("CLICKHLE_VC2_JIT_TIMING") != nullptr;
    mutable u32 sample_state = 0x91a734b5;
    struct Timing { u64 nanoseconds = 0, samples = 0; };
    mutable std::unordered_map<u32, Timing> native_timing;
    struct SampleTimer {
        Timing *result;
        std::chrono::steady_clock::time_point start;
        explicit SampleTimer(Timing *value) : result(value) {
            if (result) start = std::chrono::steady_clock::now();
        }
        ~SampleTimer() {
            if (!result) return;
            result->nanoseconds += static_cast<u64>(std::chrono::duration_cast<
                std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
            ++result->samples;
        }
    };
    double materialize_cpu = 0, compile_cpu = 0;
    struct CompilationTimer {
        double &total;
        std::clock_t start = std::clock();
        ~CompilationTimer() {
            total += static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
        }
    };
    u64 cache_hits = 0;
    u64 shared_object_hits = 0;
    std::unordered_map<u64, u64> fallback_counts;
    std::unordered_map<u64, u64> native_counts;
    std::unordered_map<u64, u64> native_edges;
    std::optional<u32> previous_native_pc;
    mutable std::array<u64, 13> block_kind_counts{};

    void record(u32 pc, u16 word, unsigned executed) {
        if (!profile) return;
        if (executed && previous_native_pc)
            ++native_edges[(static_cast<u64>(*previous_native_pc) << 32) | pc];
        previous_native_pc = executed ? std::optional<u32>(pc) : std::nullopt;
        const u64 key = (static_cast<u64>(pc) << 16) | word;
        ++(executed ? native_counts : fallback_counts)[key];
    }

    void report() const {
        if (sample_time) {
            std::vector<std::pair<u32, Timing>> times(native_timing.begin(), native_timing.end());
            std::sort(times.begin(), times.end(), [](const auto &a, const auto &b) {
                return a.second.nanoseconds > b.second.nanoseconds;
            });
            std::cerr << "VC2 sampled native execution (approximately 1/1024 calls):\n";
            for (unsigned i = 0; i < std::min<std::size_t>(times.size(), 24); ++i) {
                const auto &item = times[i];
                const auto block = blocks.find(item.first);
                std::cerr << "pc=0x" << std::hex << item.first << std::dec
                          << " sampled_ns=" << item.second.nanoseconds
                          << " samples=" << item.second.samples
                          << " kind=" << (block == blocks.end() ? -1 : static_cast<int>(block->second.kind));
                if (block != blocks.end()) {
                    const auto &compiled = block->second;
                    std::cerr << " words=";
                    if (compiled.kind == Block::Kind::Scalar && !compiled.length) {
                        for (const auto &op : compiled.operations)
                            std::cerr << std::hex << op.word << ',';
                    } else {
                        for (unsigned word = 0; word < compiled.length; ++word)
                            std::cerr << std::hex << compiled.words[word] << ',';
                    }
                    std::cerr << std::dec;
                    if (compiled.kind == Block::Kind::BinaryVector)
                        std::cerr << " predicate=" << compiled.predicate
                                  << " repeats=" << compiled.repeats
                                  << " dynamic=" << compiled.dynamic_repeats
                                  << " widths=" << compiled.destination.width << '/'
                                  << compiled.source.width << '/' << compiled.right.width
                                  << " vertical=" << compiled.destination.vertical << '/'
                                  << compiled.source.vertical << '/' << compiled.right.vertical;
                }
                std::cerr << '\n';
            }
        }
        if (!profile) return;
        std::vector<std::pair<u64, u64>> ranked(fallback_counts.begin(), fallback_counts.end());
        std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
            return a.second > b.second;
        });
        std::cerr << "VC2 JIT blocks: " << blocks.size()
                  << ", object cache hits: " << cache_hits
                  << ", shared native object hits: " << shared_object_hits << "\n"
                  << "VC2 JIT fallback profile: " << ranked.size() << " sites\n";
        for (unsigned i = 0; i < std::min<std::size_t>(ranked.size(), 40); ++i) {
            const u64 key = ranked[i].first;
            const auto hit = native_counts.find(key);
            std::cerr << "pc=0x" << std::hex << (key >> 16)
                      << " word=0x" << (key & 65535) << std::dec
                      << " fallback=" << ranked[i].second
                      << " native=" << (hit == native_counts.end() ? 0 : hit->second) << '\n';
        }
        ranked.assign(native_counts.begin(), native_counts.end());
        std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
            return a.second > b.second;
        });
        std::cerr << "VC2 JIT hottest native sites:\n";
        for (unsigned i = 0; i < std::min<std::size_t>(ranked.size(), 20); ++i) {
            const u64 key = ranked[i].first;
            std::cerr << "pc=0x" << std::hex << (key >> 16)
                      << " word=0x" << (key & 65535) << std::dec
                      << " native=" << ranked[i].second << '\n';
        }
        constexpr std::array<const char *, 13> kinds = {
            "scalar", "memory", "stack", "table branch", "vector load",
            "raster store", "vector move", "broadcast", "binary vector",
            "vector add/flags", "store loop", "vector region", "indexed H16"
        };
        std::cerr << "VC2 JIT executions by block kind:\n";
        for (unsigned i = 0; i < kinds.size(); ++i)
            std::cerr << kinds[i] << '=' << block_kind_counts[i] << '\n';
        ranked.assign(native_edges.begin(), native_edges.end());
        std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
            return a.second > b.second;
        });
        std::cerr << "VC2 JIT hottest consecutive native blocks:\n";
        for (unsigned i = 0; i < std::min<std::size_t>(ranked.size(), 20); ++i) {
            std::cerr << "from=0x" << std::hex << (ranked[i].first >> 32)
                      << " to=0x" << static_cast<u32>(ranked[i].first)
                      << std::dec << " count=" << ranked[i].second;
            // Report the decoded live block: uploaded programs can differ
            // from the firmware image at the same address.
            const auto found = blocks.find(static_cast<u32>(ranked[i].first >> 32));
            if (found != blocks.end()) {
                const auto &block = found->second;
                std::cerr << " kind=" << kinds[static_cast<unsigned>(block.kind)] << " words=";
                if (block.kind == Block::Kind::Scalar && !block.length) {
                    for (const auto &op : block.operations)
                        std::cerr << std::hex << op.word << ',';
                } else {
                    for (unsigned word = 0; word < block.length; ++word)
                        std::cerr << std::hex << block.words[word] << ',';
                }
            }
            std::cerr << std::dec << '\n';
        }
    }


    static std::array<u64, 2> watch_code(Core &core, u32 pc, unsigned length) {
        const u32 first = pc >> 12;
        const u32 last = (pc + 2 * length - 1) >> 12;
        core.code_page_watched_[first] = 1;
        core.code_page_watched_[last] = 1;
        return {core.code_page_generation_[first], core.code_page_generation_[last]};
    }

    static bool code_unchanged(const Core &core, u32 pc, unsigned length,
                               const std::array<u64, 2> &versions) {
        const u32 first = pc >> 12;
        const u32 last = (pc + 2 * length - 1) >> 12;
        return core.code_page_generation_[first] == versions[0] &&
               core.code_page_generation_[last] == versions[1];
    }

    static bool vector_block(Block::Kind kind) {
        switch (kind) {
        case Block::Kind::VectorRegion:
        case Block::Kind::VectorLoad:
        case Block::Kind::RasterStore:
        case Block::Kind::MoveH16:
        case Block::Kind::Broadcast:
        case Block::Kind::BinaryVector:
        case Block::Kind::AddSetfH16:
        case Block::Kind::IndexedH16:
            return true;
        default:
            return false;
        }
    }

    unsigned execute_cached_vector(Core &core, unsigned limit, u32 expected_pc) {
        if (!limit || core.r[31] != expected_pc || expected_pc >= 0x4000000 ||
            (expected_pc & 1)) return 0;
        const auto &page = pages[expected_pc >> 12];
        if (!page) return 0;
        const auto &entry = page->entries[(expected_pc & 4095) >> 1];
        const Block *block = entry.block;
        if (!block || !vector_block(block->kind) ||
            (block->kind == Block::Kind::VectorRegion &&
             block->vector_pcs.size() > limit) ||
            !code_unchanged(core, expected_pc, block->length, block->code_versions))
            return 0;
        const u16 word = profile ? static_cast<u16>(core.read(expected_pc, 2)) : 0;
        const unsigned executed = execute(core, *block, limit);
        if (profile) record(expected_pc, word, executed);
        return executed;
    }

    Impl() {
        static_assert(sizeof(bool) == 1, "VC2 JIT flag storage requires byte bools");
        if (llvm::InitializeNativeTarget() || llvm::InitializeNativeTargetAsmPrinter())
            throw std::runtime_error("VC2 JIT host target initialization failed");
        auto created = llvm::orc::LLJITBuilder().create();
        if (!created)
            throw std::runtime_error("VC2 JIT initialization: " +
                                     llvm::toString(created.takeError()));
        engine = std::move(*created);
        compare = std::getenv("CLICKHLE_VC2_JIT_CHECK") != nullptr;
        profile = std::getenv("CLICKHLE_VC2_JIT_PROFILE") != nullptr;
    }

    static void optimize(llvm::Module &module) {
        llvm::LoopAnalysisManager loops;
        llvm::FunctionAnalysisManager functions;
        llvm::CGSCCAnalysisManager calls;
        llvm::ModuleAnalysisManager modules;
        llvm::PassBuilder passes;
        passes.registerModuleAnalyses(modules);
        passes.registerCGSCCAnalyses(calls);
        passes.registerFunctionAnalyses(functions);
        passes.registerLoopAnalyses(loops);
        passes.crossRegisterProxies(loops, functions, calls, modules);
        auto pipeline = passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
        pipeline.run(module, modules);
        if (llvm::verifyModule(module, &llvm::errs()))
            throw std::runtime_error("VC2 JIT optimization produced invalid IR");
    }

    llvm::orc::ExecutorAddr materialize(std::unique_ptr<llvm::LLVMContext> context_owner,
                                        std::unique_ptr<llvm::Module> module_owner,
                                        llvm::Function *function,
                                        llvm::orc::ResourceTrackerSP &resources,
                                        const std::string &identity) {
        CompilationTimer timer{materialize_cpu};
        // Pair destruction releases the module before its LLVM context.
        auto owned = std::pair(std::move(context_owner), std::move(module_owner));
        auto &module = owned.second;
        // The pre-optimization IR captures the full generated routine, including
        // guest addresses and constants. Normalize its symbol before hashing so
        // compilation order does not affect the cache key.
        function->setName("vc2_cache_template");
        module->setModuleIdentifier("vc2_cache_template");
        std::string ir;
        llvm::raw_string_ostream output(ir);
        module->print(output, nullptr);
        output.flush();

        llvm::SHA256 hash;
        hash.update("clickhle-vc2-object-v1");
        hash.update(identity);
        hash.update(LLVM_VERSION_STRING);
        hash.update(llvm::sys::getHostCPUName());
        llvm::StringMap<bool, llvm::MallocAllocator> features;
        if (llvm::sys::getHostCPUFeatures(features)) {
            std::vector<std::string> enabled;
            for (const auto &feature : features)
                if (feature.second) enabled.push_back(feature.first().str());
            std::sort(enabled.begin(), enabled.end());
            for (const auto &feature : enabled) hash.update(feature);
        }
        hash.update(ir);
        const auto bytes = hash.final();
        constexpr char digits[] = "0123456789abcdef";
        std::string key;
        key.reserve(bytes.size() * 2);
        for (const auto byte : bytes) {
            key.push_back(digits[byte >> 4]);
            key.push_back(digits[byte & 15]);
        }
        // Identical IR may occur at several guest addresses. Load its symbol
        // once per JIT instance and retain it independently of decoded blocks.
        // Invalidating one guest block must not unload another block's code.
        if (const auto found = loaded_objects.find(key); found != loaded_objects.end()) {
            ++shared_object_hits;
            resources.reset();
            return found->second.address;
        }
        const std::string name = "vc2_" + key;
        function->setName(name);
        module->setModuleIdentifier(name);

        auto object = object_cache.load(key);
        if (object) {
            auto parsed = llvm::object::ObjectFile::createObjectFile(
                object->getMemBufferRef());
            if (!parsed) {
                llvm::consumeError(parsed.takeError());
                object.reset();
            } else {
                ++cache_hits;
            }
        }
        if (!object) {
            optimize(*module);
            auto compiled = engine->getIRCompileLayer().getCompiler()(*module);
            if (!compiled)
                throw std::runtime_error("VC2 JIT compilation: " +
                                         llvm::toString(compiled.takeError()));
            object = std::move(*compiled);
            object_cache.store(key, *object);
        }
        if (auto error = engine->addObjectFile(resources, std::move(object)))
            throw std::runtime_error("VC2 JIT object loading: " +
                                     llvm::toString(std::move(error)));
        auto symbol = engine->lookup(name);
        if (!symbol)
            throw std::runtime_error("VC2 JIT lookup: " +
                                     llvm::toString(symbol.takeError()));
        loaded_objects.emplace(key, LoadedObject{*symbol, std::move(resources)});
        return *symbol;
    }

    NativeIndexedH16 compile_indexed_h16() {
        CompilationTimer compile_timer{compile_cpu};
        auto context = std::make_unique<llvm::LLVMContext>();
        auto module = std::make_unique<llvm::Module>("vc2-indexed-h16", *context);
        module->setDataLayout(engine->getDataLayout());
        module->setTargetTriple(engine->getTargetTriple().str());
        auto *i8 = llvm::Type::getInt8Ty(*context);
        auto *i32 = llvm::Type::getInt32Ty(*context);
        auto *i64 = llvm::Type::getInt64Ty(*context);
        auto *ptr = llvm::PointerType::getUnqual(i8);
        auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(*context),
            {ptr, ptr, i32, llvm::PointerType::getUnqual(i64),
             llvm::PointerType::getUnqual(i32), i32, i32}, false);
        auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
            "vc2_indexed_h16_" + std::to_string(next_symbol++), module.get());
        llvm::IRBuilder<> build(llvm::BasicBlock::Create(*context, "entry", function));
        auto argument = [&](unsigned i) { return function->getArg(i); };
        for (unsigned lane = 0; lane < 16; ++lane) {
            auto *lane_index = build.getInt32(lane);
            auto *high = build.CreateLoad(i64, build.CreateGEP(i64, argument(3), lane_index));
            auto *low = build.CreateLoad(i32, build.CreateGEP(i32, argument(4), lane_index));
            auto *index = build.CreateAdd(build.CreateShl(build.CreateTrunc(high, i32), 16), low);
            auto *address = build.CreateAdd(argument(2),
                build.CreateAdd(build.CreateShl(index, 1), argument(6)));
            auto *ram_address = build.CreateSelect(build.CreateICmpUGE(address,
                build.getInt32(0xfe000000)),
                build.CreateAdd(build.CreateSub(address, build.getInt32(0xfe000000)),
                                build.getInt32(0x2000000)), address);
            auto *value = build.CreateAlignedLoad(build.getInt16Ty(),
                build.CreateGEP(i8, argument(1), ram_address), llvm::Align(1));
            auto *row = build.CreateAnd(build.CreateAdd(
                build.CreateLShr(argument(5), 6), lane_index), 63);
            auto *column = build.CreateAnd(argument(5), 63);
            for (unsigned byte = 0; byte < 2; ++byte) {
                auto *offset = build.CreateOr(build.CreateShl(row, 6),
                    build.CreateAnd(build.CreateAdd(column, build.getInt32(16 * byte)), 63));
                auto *part = byte ? build.CreateTrunc(build.CreateLShr(value, 8), i8)
                                  : build.CreateTrunc(value, i8);
                build.CreateStore(part, build.CreateGEP(i8, argument(0), offset));
            }
        }
        build.CreateRetVoid();
        indexed_h16_resources = engine->getMainJITDylib().createResourceTracker();
        return materialize(std::move(context), std::move(module), function,
            indexed_h16_resources, "indexed-h16").toPtr<NativeIndexedH16>();
    }

    static void emit_plain_vector(llvm::IRBuilder<> &build, llvm::Function &function,
                                  const VectorPlan &plan, llvm::Value *destination,
                                  llvm::Value *left, llvm::Value *right) {
        const unsigned opcode = (plan.words[0] & 511) >> 3;
        auto *i8 = build.getInt8Ty();
        auto *i16 = build.getInt16Ty();
        auto *i64 = build.getInt64Ty();
        auto constant = [&](std::int64_t value) { return llvm::ConstantInt::getSigned(i64, value); };
        auto address = [&](llvm::Value *base, unsigned lane, bool vertical,
                           unsigned plane = 0) {
            llvm::Value *row, *column;
            if (vertical) {
                row = build.CreateShl(build.CreateAnd(build.CreateAdd(build.CreateLShr(base, 6),
                    build.getInt32(lane)), build.getInt32(63)), 6);
                column = build.CreateAnd(base, 63);
            } else {
                row = build.CreateAnd(base, ~63u);
                column = build.CreateAnd(build.CreateAdd(build.CreateAnd(base, 63),
                    build.getInt32(lane)), 63);
            }
            column = build.CreateAnd(build.CreateAdd(column, build.getInt32(16 * plane)), 63);
            return build.CreateInBoundsGEP(i8, function.getArg(0), build.CreateOr(row, column));
        };
        auto load = [&](llvm::Value *base, unsigned lane, const VectorOperand &operand) {
            auto *low = build.CreateLoad(i8, address(base, lane, operand.vertical));
            if (operand.width == 1) return static_cast<llvm::Value *>(build.CreateZExt(low, i64));
            auto *high = build.CreateLoad(i8, address(base, lane, operand.vertical, 1));
            auto *word = build.CreateOr(build.CreateZExt(low, i16),
                                        build.CreateShl(build.CreateZExt(high, i16), 8));
            return static_cast<llvm::Value *>(build.CreateSExt(word, i64));
        };
        std::array<llvm::Value *, 16> values{};
        for (unsigned lane = 0; lane < 16; ++lane) {
            auto *a = opcode == 4 ? static_cast<llvm::Value *>(constant(0))
                                  : load(left, lane, plan.a);
            auto *b = load(right, lane, plan.b);
            auto *shift = build.CreateAnd(b, constant(31));
            llvm::Value *value;
            switch (opcode) {
            case 4: value = b; break;
            case 5: value = build.CreateAnd(a, b); break;
            case 6: value = build.CreateOr(a, b); break;
            case 8: case 9: value = build.CreateAdd(a, b); break;
            case 10: case 11: value = build.CreateSub(a, b); break;
            case 12: case 13: value = build.CreateSub(b, a); break;
            case 14: value = build.CreateMul(a, b); break;
            case 16: case 17: value = build.CreateAShr(build.CreateMul(a, b), 8); break;
            case 18: value = build.CreateAShr(build.CreateMul(a, b), 16); break;
            case 19: value = build.CreateAShr(build.CreateMul(a, build.CreateAnd(b, constant(65535))), 16); break;
            case 24: value = build.CreateShl(a, shift); break;
            case 26: value = build.CreateAShr(a, shift); break;
            case 27: value = build.CreateLShr(build.CreateAnd(a,
                constant(plan.a.width == 2 ? 65535 : 255)), shift); break;
            case 28: case 37:
                value = build.CreateSelect(build.CreateICmpSLT(a, b), a, b);
                if (opcode == 37)
                    value = build.CreateSelect(build.CreateICmpSLT(value, constant(0)),
                                               constant(0), value);
                break;
            case 29: value = build.CreateSelect(build.CreateICmpSGT(a, b), a, b); break;
            case 30: value = build.CreateSelect(build.CreateICmpSGE(a, b),
                build.CreateSub(a, b), build.CreateSub(b, a)); break;
            default: throw std::runtime_error("Unsupported plain VC2 vector region operation");
            }
            if (opcode == 9 || opcode == 11 || opcode == 13 || opcode == 17) {
                auto *minimum = constant(plan.d.width == 2 ? -32768 : 0);
                auto *maximum = constant(plan.d.width == 2 ? 32767 : 255);
                value = build.CreateSelect(build.CreateICmpSLT(value, minimum), minimum, value);
                value = build.CreateSelect(build.CreateICmpSGT(value, maximum), maximum, value);
            }
            values[lane] = value;
        }
        for (unsigned lane = 0; lane < 16; ++lane) {
            build.CreateStore(build.CreateTrunc(values[lane], i8),
                address(destination, lane, plan.d.vertical));
            if (plan.d.width == 2)
                build.CreateStore(build.CreateTrunc(build.CreateLShr(values[lane], 8), i8),
                    address(destination, lane, plan.d.vertical, 1));
        }
    }

    NativeVectorRegion compile_vector_region(Block &block, const std::vector<VectorPlan> &plans) {
        CompilationTimer compile_timer{compile_cpu};
        auto context = std::make_unique<llvm::LLVMContext>();
        auto module = std::make_unique<llvm::Module>("vc2-vector-region", *context);
        module->setDataLayout(engine->getDataLayout());
        module->setTargetTriple(engine->getTargetTriple().str());
        auto *i8 = llvm::Type::getInt8Ty(*context);
        auto *i32 = llvm::Type::getInt32Ty(*context);
        auto *pointer = llvm::PointerType::getUnqual(i8);
        auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(*context), {pointer, pointer, pointer, pointer, pointer}, false);
        const std::string name = "vc2_vector_region_" + std::to_string(next_symbol++);
        auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage, name, module.get());
        // These are distinct arrays in Core, never overlapping guest operands.
        for (unsigned arg = 0; arg < 5; ++arg)
            function->addParamAttr(arg, llvm::Attribute::NoAlias);
        llvm::IRBuilder<> build(llvm::BasicBlock::Create(*context, "entry", function));
        auto *status_address = build.CreateGEP(i32, function->getArg(1), build.getInt32(30));
        auto *column = build.CreateAnd(build.CreateLoad(i32, status_address), 48);
        auto offset = [&](const VectorOperand &operand) {
            llvm::Value *base = build.getInt32(operand.base);
            if (operand.reg >= 0) {
                auto *address = build.CreateGEP(i32, function->getArg(1), build.getInt32(static_cast<unsigned>(operand.reg)));
                auto *index = build.CreateLoad(i32, address);
                llvm::Value *normalized;
                if (operand.role == 'a')
                    normalized = build.CreateOr(build.CreateAnd(index, ~63u),
                        build.CreateAnd(build.CreateAdd(index, build.getInt32(16)), 32));
                else
                    normalized = build.CreateAnd(index, ~15u);
                base = build.CreateOr(
                    build.CreateAnd(build.CreateAdd(base, build.CreateAnd(normalized, ~63u)), 0xfc0),
                    build.CreateAnd(build.CreateAdd(base, normalized), 63));
            }
            return build.CreateOr(build.CreateAnd(base, 0xfc0),
                build.CreateAnd(build.CreateAdd(base, column), 63));
        };
        for (const auto &plan : plans) {
            const unsigned opcode = (plan.words[0] & 511) >> 3;
            if (plan.d.vertical || plan.a.vertical || plan.b.vertical) {
                emit_plain_vector(build, *function, plan, offset(plan.d),
                                  offset(plan.a), offset(plan.b));
                continue;
            }
            const bool carry = opcode == 34 || opcode == 35;
            llvm::SmallVector<llvm::Type *, 7> parameters = {pointer, i32, i32, i32};
            if (plan.setf) parameters.append(3, pointer);
            else if (carry) parameters.push_back(pointer);
            auto *kernel_type = llvm::FunctionType::get(build.getVoidTy(), parameters, false);
            auto *kernel = llvm::Function::Create(kernel_type, llvm::Function::InternalLinkage, "lanes", module.get());
            kernel->addFnAttr(llvm::Attribute::AlwaysInline);
            llvm::IRBuilder<> lanes(llvm::BasicBlock::Create(*context, "entry", kernel));
            const unsigned widths = (plan.d.width - 1) | ((plan.a.width - 1) << 1) | ((plan.b.width - 1) << 2);
            if (plan.setf)
                emit_add_flags_simd(lanes, *kernel, plan.modes.lowcarry && plan.modes.carry_operands);
            else
                emit_horizontal_simd(lanes, *kernel, opcode, widths, false,
                    carry ? kernel->getArg(4) : nullptr);
            // Admission requires 16-byte aligned columns. Adding the column
            // bank preserves alignment, so neither byte plane can wrap.
            lanes.CreateUnreachable();
            llvm::SmallVector<llvm::Value *, 7> arguments = {
                function->getArg(0), offset(plan.d), offset(plan.a), offset(plan.b)};
            if (plan.setf)
                for (unsigned arg = 2; arg < 5; ++arg) arguments.push_back(function->getArg(arg));
            else if (carry)
                arguments.push_back(function->getArg(4));
            build.CreateCall(kernel, arguments);
        }
        build.CreateRetVoid();
        block.resources = engine->getMainJITDylib().createResourceTracker();
        return materialize(std::move(context), std::move(module), function, block.resources,
                           "vector-region").toPtr<NativeVectorRegion>();
    }

    bool build_vector_region(Core &core, Block &block, unsigned limit) {
        std::vector<VectorPlan> plans;
        for (unsigned i = 0; i < std::min(limit, 32u); ++i) {
            const u32 pc = block.pc + 2 * block.length;
            if (pc + 6 > 0x4000000) break;
            const u32 first = core.read(pc, 2);
            const unsigned length = (first & 0xfe00) == 0x2000 ? 5 :
                                    (first & 0xfe00) == 0x2200 ? 3 : 0;
            if (!length || block.length + length > block.words.size() || pc + 2 * length > 0x4000000) break;
            const unsigned op = (first & 511) >> 3;
            switch (op) {
            case 4: case 5: case 6: case 8: case 9: case 10: case 11: case 12:
            case 13: case 14: case 16: case 17: case 18: case 19: case 24:
            case 26: case 27: case 28: case 29: case 30:
            case 34: case 35: case 37: break;
            default: return plans.size() >= 2 ? finish_vector_region(core, block, plans) : false;
            }
            std::array<u32, 5> words{};
            for (unsigned j = 0; j < length; ++j) words[j] = core.read(pc + 2 * j, 2);
            VectorPlan plan;
            try { plan = core.decode_vector(words, length); }
            catch (const std::runtime_error &) { break; }
            const auto contiguous_operand = [](const VectorOperand &operand) {
                return !operand.vertical && !operand.discard &&
                    (operand.reg < 0 || operand.width == 2) &&
                    !operand.packed_columns && operand.width <= 2 && (operand.base & 15) == 0;
            };
            const auto plain_operand = [](const VectorOperand &operand) {
                return !operand.discard && operand.reg < 0 &&
                    !operand.packed_columns && operand.width <= 2;
            };
            const bool vertical = plan.d.vertical || plan.a.vertical || plan.b.vertical;
            const auto &mode = plan.modes;
            const bool plain_mode = !mode.nibble && !mode.blend && !mode.byte_mask &&
                !mode.texture_nibble_mask && !mode.alpha_bit && !mode.half_complement &&
                !mode.complement && !mode.byte_or && !mode.halfword_or &&
                !mode.quant_sign && !mode.edge_sign && !mode.nibble_or &&
                !mode.split48 && !mode.split80 && !mode.lowcarry &&
                !mode.lowborrow && !mode.vertical_carry && !mode.highcarry &&
                !mode.highborrow && !mode.carry_operands && !mode.filtered_byte;
            const bool supported_flags = op == 8 && plan.d.width == 2 &&
                plan.a.width == 2 && plan.b.width == 2;
            const bool supported_carry = op == 34 ? !plan.setf && plan.modes.highcarry &&
                    plan.modes.carry_operands
                : op == 35 ? !plan.setf && plan.modes.highborrow &&
                    plan.modes.carry_operands : true;
            // Selectors 5 and 6 have only form-specific mask/OR evidence in
            // the interpreter. The plain vertical region kernel would turn
            // either into a general operation and bypass those guards.
            if (op == 5 || op == 6 ||
                plan.length != length || plan.count != 1 || plan.bkind || !supported_carry ||
                (plan.setf && (!supported_flags || vertical)) ||
                plan.pred || plan.clear || plan.accumulate || plan.x15 || plan.sreg != 7 ||
                (vertical && (!plain_mode || op == 34 || op == 35 ||
                    !plain_operand(plan.d) || !plain_operand(plan.a) ||
                    !plain_operand(plan.b) || plans.size() >= 8)) ||
                (!vertical && ((op == 19 || op == 24) ||
                    !contiguous_operand(plan.d) ||
                    !contiguous_operand(plan.a) || !contiguous_operand(plan.b)))) break;
            plan.words = words;
            block.vector_pcs.push_back(pc);
            for (unsigned j = 0; j < length; ++j) block.words[block.length++] = static_cast<u16>(words[j]);
            plans.push_back(plan);
        }
        return plans.size() >= 2 && finish_vector_region(core, block, plans);
    }

    bool finish_vector_region(Core &core, Block &block, const std::vector<VectorPlan> &plans) {
        const auto start = std::chrono::steady_clock::now();
        block.kind = Block::Kind::VectorRegion;
        block.vector_native = compile_vector_region(block, plans);
        block.code_versions = watch_code(core, block.pc, block.length);
        core.jit_compile_us += static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
        ++core.jit_compilations;
        return true;
    }

    NativeBlock compile(Block &block) {
        CompilationTimer compile_timer{compile_cpu};
        auto context = std::make_unique<llvm::LLVMContext>();
        auto module = std::make_unique<llvm::Module>("vc2-block", *context);
        module->setDataLayout(engine->getDataLayout());
        module->setTargetTriple(engine->getTargetTriple().str());
        auto *i32 = llvm::Type::getInt32Ty(*context);
        auto *pointer = llvm::PointerType::getUnqual(i32);
        auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(*context),
                                              {pointer, pointer, pointer}, false);
        const std::string name = "vc2_block_" + std::to_string(next_symbol++);
        auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                                 name, module.get());
        auto *entry = llvm::BasicBlock::Create(*context, "entry", function);
        llvm::IRBuilder<> build(entry);
        auto *registers = function->getArg(0);
        std::array<llvm::Value *, 32> values{};
        std::array<bool, 32> changed{};
        auto reg_ptr = [&](unsigned index) {
            return build.CreateInBoundsGEP(i32, registers,
                    llvm::ConstantInt::get(i32, index));
        };
        auto load = [&](unsigned index) -> llvm::Value * {
            if (!values[index]) values[index] = build.CreateLoad(i32, reg_ptr(index));
            return values[index];
        };
        auto store = [&](unsigned index, llvm::Value *value) {
            values[index] = value;
            changed[index] = true;
        };
        // Keep the entry address as an input so address-independent blocks
        // can share native code even when uploaded at different guest PCs.
        auto *entry_pc = build.CreateLoad(i32, reg_ptr(31));
        auto condition = [&](unsigned code, llvm::Value *status = nullptr) -> llvm::Value * {
            auto flag = [&](unsigned bit) -> llvm::Value * {
                return build.CreateICmpNE(build.CreateAnd(status ? status : load(30), 1u << bit),
                                          llvm::ConstantInt::get(i32, 0));
            };
            auto *z = flag(3), *n = flag(2), *c = flag(1), *v = flag(0);
            auto not_ = [&](llvm::Value *value) { return build.CreateNot(value); };
            switch (code) {
            case 0: return z;
            case 1: return not_(z);
            case 2: return c;
            case 3: return not_(c);
            case 4: return n;
            case 5: return not_(n);
            case 6: return v;
            case 7: return not_(v);
            case 8: return build.CreateAnd(not_(c), not_(z));
            case 9: return build.CreateOr(c, z);
            case 10: return build.CreateICmpEQ(n, v);
            case 11: return build.CreateICmpNE(n, v);
            case 12: return build.CreateAnd(not_(z), build.CreateICmpEQ(n, v));
            case 13: return build.CreateOr(z, build.CreateICmpNE(n, v));
            case 14: return llvm::ConstantInt::getTrue(*context);
            default: return llvm::ConstantInt::getFalse(*context);
            }
        };
        llvm::Value *branch_pc = nullptr;
        for (const Operation &op : block.operations) {
            values[31] = build.CreateAdd(entry_pc,
                build.getInt32(op.guest_pc ? op.guest_pc - block.pc : 0));
            if (op.nop) continue;
            if (op.memory_load) {
                auto *offset_pointer = build.CreateInBoundsGEP(i32, function->getArg(2),
                    build.getInt32(op.memory_slot));
                auto *offset = build.CreateLoad(i32, offset_pointer);
                auto *data_pointer = build.CreateGEP(build.getInt8Ty(), function->getArg(1), offset);
                store(op.destination, build.CreateAlignedLoad(i32, data_pointer, llvm::Align(1)));
                continue;
            }
            if (op.stack_load) {
                auto *offset = build.CreateAdd(load(25), build.getInt32(op.immediate));
                auto *address = build.CreateGEP(build.getInt8Ty(), function->getArg(1), offset);
                store(op.destination, build.CreateLoad(i32, address));
                continue;
            }
            if (op.add_compare) {
                auto *increment = op.add_immediate
                    ? static_cast<llvm::Value *>(llvm::ConstantInt::get(i32, op.add_value))
                    : load(op.left);
                auto *sum = build.CreateAdd(load(op.destination), increment);
                store(op.destination, sum);
                llvm::Value *comparison = op.compare_register ? load(op.compare_source)
                    : static_cast<llvm::Value *>(llvm::ConstantInt::get(i32, op.compare_value));
                auto *difference = build.CreateSub(sum, comparison);
                auto *zero = build.CreateShl(build.CreateZExt(build.CreateICmpEQ(
                    difference, llvm::ConstantInt::get(i32, 0)), i32), 3);
                auto *negative = build.CreateShl(build.CreateZExt(build.CreateICmpSLT(
                    difference, llvm::ConstantInt::get(i32, 0)), i32), 2);
                auto *borrow = build.CreateShl(build.CreateZExt(build.CreateICmpULT(
                    sum, comparison), i32), 1);
                auto *overflow = build.CreateLShr(build.CreateAnd(build.CreateXor(
                    sum, comparison), build.CreateXor(sum, difference)), 31);
                auto *flags = build.CreateOr(build.CreateOr(zero, negative),
                                             build.CreateOr(borrow, overflow));
                branch_pc = build.CreateSelect(condition((op.word >> 8) & 15, flags),
                    llvm::ConstantInt::get(i32, op.immediate),
                    llvm::ConstantInt::get(i32, op.next_pc));
                break;
            }
            if (op.branch) {
                if (op.link)
                    store(26, llvm::ConstantInt::get(i32, op.next_pc));
                branch_pc = op.register_branch ? load(op.source) :
                    build.CreateSelect(op.link ? llvm::ConstantInt::getTrue(*context) :
                        condition((op.word >> 8) & 15),
                        llvm::ConstantInt::get(i32, op.immediate),
                        llvm::ConstantInt::get(i32, op.next_pc));
                break;
            }
            llvm::Value *a = load(op.left);
            llvm::Value *b = op.immediate_source
                ? static_cast<llvm::Value *>(llvm::ConstantInt::get(i32, op.immediate))
                : load(op.source);
            llvm::Value *result = nullptr;
            switch (op.opcode) {
            case 2: result = build.CreateAnd(
                a, build.CreateSub(build.CreateShl(llvm::ConstantInt::get(i32, 1),
                        build.CreateAnd(b, 31)), llvm::ConstantInt::get(i32, 1))); break;
            case 3: result = build.CreateAnd(a, build.CreateNot(b)); break;
            case 4: result = build.CreateAShr(a, build.CreateAnd(b, 31)); break;
            case 5: result = build.CreateXor(a, b); break;
            case 6: result = build.CreateShl(a, build.CreateAnd(b, 31)); break;
            case 7: result = build.CreateAnd(a, b); break;
            case 8: result = build.CreateLShr(a, build.CreateAnd(b, 31)); break;
            case 9: {
                auto *shift = build.CreateAnd(b, 31);
                result = build.CreateOr(build.CreateLShr(a, shift),
                    build.CreateShl(a, build.CreateAnd(
                        build.CreateSub(llvm::ConstantInt::get(i32, 32), shift), 31)));
                break;
            }
            case 10: result = build.CreateMul(a, b); break;
            case 11: result = build.CreateSub(b, a); break;
            case 12: result = build.CreateAdd(a, b); break;
            case 13: result = build.CreateSub(a, b); break;
            case 14: result = b; break;
            case 15: result = build.CreateOr(a, b); break;
            case 16: {
                auto *difference = build.CreateSub(a, b);
                auto *zero = build.CreateShl(build.CreateZExt(
                    build.CreateICmpEQ(difference, llvm::ConstantInt::get(i32, 0)), i32), 3);
                auto *negative = build.CreateShl(build.CreateZExt(
                    build.CreateICmpSLT(difference, llvm::ConstantInt::get(i32, 0)), i32), 2);
                auto *borrow = build.CreateShl(build.CreateZExt(
                    build.CreateICmpULT(a, b), i32), 1);
                auto *overflow = build.CreateLShr(build.CreateAnd(
                    build.CreateXor(a, b), build.CreateXor(a, difference)), 31);
                auto *flags = build.CreateOr(build.CreateOr(zero, negative),
                                             build.CreateOr(borrow, overflow));
                store(30, build.CreateOr(build.CreateAnd(load(30), ~15u), flags));
                continue;
            }
            case 18: result = build.CreateSelect(build.CreateICmpSGE(a, b), a, b); break;
            case 19: result = build.CreateSelect(build.CreateICmpSLE(a, b), a, b); break;
            case 20: case 21: case 22: case 23:
                result = build.CreateAdd(a, build.CreateShl(b, op.opcode - 19)); break;
            case 24: result = build.CreateNeg(b); break;
            case 26: result = build.CreateSExt(build.CreateTrunc(b, build.getInt8Ty()), i32); break;
            case 27: result = build.CreateSExt(build.CreateTrunc(b, build.getInt16Ty()), i32); break;
            case 29:
                // Match Core::alu's candidate count semantics; full-width
                // result and status flags still lack a device measurement.
                result = build.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, b);
                break;
            case 30: result = build.CreateSelect(build.CreateICmpSLT(
                b, llvm::ConstantInt::get(i32, 0)), build.CreateNeg(b), b); break;
            case 31: result = build.CreateNot(b); break;
            case 32: case 33: case 34: case 35: {
                auto *wide_a = op.opcode == 32 || op.opcode == 33
                    ? build.CreateSExt(a, build.getInt64Ty())
                    : build.CreateZExt(a, build.getInt64Ty());
                auto *wide_b = op.opcode == 32 || op.opcode == 34
                    ? build.CreateSExt(b, build.getInt64Ty())
                    : build.CreateZExt(b, build.getInt64Ty());
                result = build.CreateTrunc(build.CreateLShr(
                    build.CreateMul(wide_a, wide_b), 32), i32);
                break;
            }
            case 43: {
                auto *low = llvm::ConstantInt::getSigned(i32, -32768);
                auto *high = llvm::ConstantInt::getSigned(i32, 32767);
                auto *bounded_low = build.CreateSelect(build.CreateICmpSLT(b, low), low, b);
                result = build.CreateSelect(build.CreateICmpSGT(bounded_low, high), high, bounded_low);
                break;
            }
            default: throw std::runtime_error("VC2 JIT internal opcode error");
            }
            store(op.destination, result);
        }
        store(31, branch_pc ? branch_pc : build.CreateAdd(entry_pc, build.getInt32(
            static_cast<u32>(2 * (block.length ? block.length : block.operations.size())))));
        for (unsigned i = 0; i < changed.size(); ++i)
            if (changed[i]) build.CreateStore(values[i], reg_ptr(i));
        build.CreateRetVoid();
        if (llvm::verifyFunction(*function, &llvm::errs()))
            throw std::runtime_error("VC2 JIT generated invalid IR");
        block.resources = engine->getMainJITDylib().createResourceTracker();
        return materialize(std::move(context), std::move(module), function,
                           block.resources, "block")
            .toPtr<NativeBlock>();
    }

    AddressKernel compile_address(unsigned source, unsigned width, bool pre,
                                  bool indexed, unsigned index, u32 displacement) {
        CompilationTimer compile_timer{compile_cpu};
        auto context = std::make_unique<llvm::LLVMContext>();
        auto module = std::make_unique<llvm::Module>("vc2-memory-address", *context);
        module->setDataLayout(engine->getDataLayout());
        module->setTargetTriple(engine->getTargetTriple().str());
        auto *i32 = llvm::Type::getInt32Ty(*context);
        auto *type = llvm::FunctionType::get(i32, {llvm::PointerType::getUnqual(i32)}, false);
        const std::string name = "vc2_address_" + std::to_string(next_symbol++);
        auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                                 name, module.get());
        llvm::IRBuilder<> build(llvm::BasicBlock::Create(*context, "entry", function));
        llvm::Value *address = build.CreateLoad(i32, build.CreateInBoundsGEP(i32, function->getArg(0),
                                                   llvm::ConstantInt::get(i32, source)));
        if (indexed && index != 7) {
            auto *offset = build.CreateLoad(i32, build.CreateInBoundsGEP(
                i32, function->getArg(0), llvm::ConstantInt::get(i32, index)));
            address = build.CreateAdd(address, build.CreateMul(
                offset, llvm::ConstantInt::get(i32, width)));
        } else if (pre) {
            address = build.CreateSub(address, llvm::ConstantInt::get(i32, width));
        }
        if (displacement)
            address = build.CreateAdd(address, llvm::ConstantInt::get(i32, displacement));
        build.CreateRet(address);
        if (llvm::verifyFunction(*function, &llvm::errs()))
            throw std::runtime_error("VC2 JIT invalid memory address kernel");
        AddressKernel result;
        result.resources = engine->getMainJITDylib().createResourceTracker();
        result.native = materialize(std::move(context), std::move(module), function,
                                    result.resources,
                                    "address:" + std::to_string(source) + ":" +
                                    std::to_string(width) + ":" + std::to_string(pre) +
                                    ":" + std::to_string(indexed) + ":" +
                                    std::to_string(index) + ":" +
                                    std::to_string(displacement)).toPtr<NativeAddress>();
        return result;
    }

    NativeMove compile_move() {
        CompilationTimer compile_timer{compile_cpu};
        auto context = std::make_unique<llvm::LLVMContext>();
        auto module = std::make_unique<llvm::Module>("vc2-move", *context);
        module->setDataLayout(engine->getDataLayout());
        module->setTargetTriple(engine->getTargetTriple().str());
        auto *i8 = llvm::Type::getInt8Ty(*context);
        auto *i32 = llvm::Type::getInt32Ty(*context);
        auto *lanes = llvm::FixedVectorType::get(i8, 16);
        auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(*context),
                                              {llvm::PointerType::getUnqual(i8), i32, i32}, false);
        const std::string name = "vc2_move_" + std::to_string(next_symbol++);
        auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                                 name, module.get());
        auto *entry = llvm::BasicBlock::Create(*context, "entry", function);
        auto *contiguous = llvm::BasicBlock::Create(*context, "contiguous", function);
        auto *wrapped = llvm::BasicBlock::Create(*context, "wrapped", function);
        auto *done = llvm::BasicBlock::Create(*context, "done", function);
        llvm::IRBuilder<> build(entry);
        auto *vrf = function->getArg(0);
        auto *source = function->getArg(1);
        auto *destination = function->getArg(2);
        auto *source_row = build.CreateAnd(source, ~63u);
        auto *source_x = build.CreateAnd(source, 63);
        auto *destination_row = build.CreateAnd(destination, ~63u);
        auto *destination_x = build.CreateAnd(destination, 63);
        auto *simple = build.CreateAnd(
            build.CreateICmpULE(source_x, llvm::ConstantInt::get(i32, 48)),
            build.CreateICmpULE(destination_x, llvm::ConstantInt::get(i32, 48)));
        build.CreateCondBr(simple, contiguous, wrapped);

        auto address = [&](llvm::Value *offset) {
            return build.CreateInBoundsGEP(i8, vrf, offset);
        };
        auto high = [&](llvm::Value *row, llvm::Value *x) {
            return build.CreateOr(row, build.CreateAnd(
                build.CreateAdd(x, llvm::ConstantInt::get(i32, 16)), 63));
        };
        build.SetInsertPoint(contiguous);
        auto *low_bytes = build.CreateAlignedLoad(lanes, address(source), llvm::Align(1));
        auto *high_bytes = build.CreateAlignedLoad(lanes,
            address(high(source_row, source_x)), llvm::Align(1));
        build.CreateAlignedStore(low_bytes, address(destination), llvm::Align(1));
        build.CreateAlignedStore(high_bytes,
            address(high(destination_row, destination_x)), llvm::Align(1));
        build.CreateBr(done);

        build.SetInsertPoint(wrapped);
        std::array<llvm::Value *, 16> low_lanes{}, high_lanes{};
        auto lane_address = [&](llvm::Value *row, llvm::Value *x, unsigned lane,
                                unsigned plane) {
            auto *column = build.CreateAnd(build.CreateAdd(
                x, llvm::ConstantInt::get(i32, lane + plane)), 63);
            return address(build.CreateOr(row, column));
        };
        for (unsigned i = 0; i < 16; ++i) {
            low_lanes[i] = build.CreateLoad(i8, lane_address(source_row, source_x, i, 0));
            high_lanes[i] = build.CreateLoad(i8, lane_address(source_row, source_x, i, 16));
        }
        for (unsigned i = 0; i < 16; ++i) {
            build.CreateStore(low_lanes[i], lane_address(destination_row, destination_x, i, 0));
            build.CreateStore(high_lanes[i], lane_address(destination_row, destination_x, i, 16));
        }
        build.CreateBr(done);
        build.SetInsertPoint(done);
        build.CreateRetVoid();

        if (llvm::verifyFunction(*function, &llvm::errs()))
            throw std::runtime_error("VC2 JIT generated invalid vector move");
        move_resources = engine->getMainJITDylib().createResourceTracker();
        return materialize(std::move(context), std::move(module), function,
                           move_resources, "move").toPtr<NativeMove>();
    }

    NativeLoadBytes compile_load_bytes() {
        CompilationTimer compile_timer{compile_cpu};
        auto context = std::make_unique<llvm::LLVMContext>();
        auto module = std::make_unique<llvm::Module>("vc2-vector-byte-load", *context);
        module->setDataLayout(engine->getDataLayout());
        module->setTargetTriple(engine->getTargetTriple().str());
        auto *i8 = llvm::Type::getInt8Ty(*context);
        auto *i32 = llvm::Type::getInt32Ty(*context);
        auto *bytes = llvm::FixedVectorType::get(i8, 16);
        auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(*context),
            {llvm::PointerType::getUnqual(i8), i32,
             llvm::PointerType::getUnqual(i8)}, false);
        const std::string name = "vc2_load_bytes_" + std::to_string(next_symbol++);
        auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                                 name, module.get());
        llvm::IRBuilder<> build(llvm::BasicBlock::Create(*context, "entry", function));
        auto *vrf = function->getArg(0);
        auto *destination = function->getArg(1);
        auto *source = function->getArg(2);
        auto *x = build.CreateAnd(destination, 63);
        auto *contiguous = llvm::BasicBlock::Create(*context, "contiguous", function);
        auto *wrapped = llvm::BasicBlock::Create(*context, "wrapped", function);
        auto *done = llvm::BasicBlock::Create(*context, "done", function);
        build.CreateCondBr(build.CreateICmpULE(x, llvm::ConstantInt::get(i32, 48)),
                           contiguous, wrapped);
        build.SetInsertPoint(contiguous);
        auto *values = build.CreateAlignedLoad(bytes, source, llvm::Align(1));
        build.CreateAlignedStore(values, build.CreateInBoundsGEP(i8, vrf, destination),
                                 llvm::Align(1));
        build.CreateBr(done);
        build.SetInsertPoint(wrapped);
        for (unsigned lane = 0; lane < 16; ++lane) {
            auto *value = build.CreateLoad(i8, build.CreateInBoundsGEP(
                i8, source, llvm::ConstantInt::get(i32, lane)));
            auto *column = build.CreateAnd(build.CreateAdd(x,
                llvm::ConstantInt::get(i32, lane)), 63);
            auto *address = build.CreateOr(build.CreateAnd(destination, ~63u), column);
            build.CreateStore(value, build.CreateInBoundsGEP(i8, vrf, address));
        }
        build.CreateBr(done);
        build.SetInsertPoint(done);
        build.CreateRetVoid();
        if (llvm::verifyFunction(*function, &llvm::errs()))
            throw std::runtime_error("VC2 JIT invalid vector byte-load kernel");
        load_bytes_resources = engine->getMainJITDylib().createResourceTracker();
        return materialize(std::move(context), std::move(module), function,
                           load_bytes_resources, "load-bytes").toPtr<NativeLoadBytes>();
    }

    NativeGatherBytes compile_gather_bytes() {
        CompilationTimer compile_timer{compile_cpu};
        auto context = std::make_unique<llvm::LLVMContext>();
        auto module = std::make_unique<llvm::Module>("vc2-vector-byte-store", *context);
        module->setDataLayout(engine->getDataLayout());
        module->setTargetTriple(engine->getTargetTriple().str());
        auto *i8 = llvm::Type::getInt8Ty(*context);
        auto *i32 = llvm::Type::getInt32Ty(*context);
        auto *bytes = llvm::FixedVectorType::get(i8, 16);
        auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(*context),
            {llvm::PointerType::getUnqual(i8), i32,
             llvm::PointerType::getUnqual(i8)}, false);
        const std::string name = "vc2_gather_bytes_" + std::to_string(next_symbol++);
        auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                                 name, module.get());
        llvm::IRBuilder<> build(llvm::BasicBlock::Create(*context, "entry", function));
        auto *vrf = function->getArg(0);
        auto *source = function->getArg(1);
        auto *output = function->getArg(2);
        auto *x = build.CreateAnd(source, 63);
        auto *contiguous = llvm::BasicBlock::Create(*context, "contiguous", function);
        auto *wrapped = llvm::BasicBlock::Create(*context, "wrapped", function);
        auto *done = llvm::BasicBlock::Create(*context, "done", function);
        build.CreateCondBr(build.CreateICmpULE(x, llvm::ConstantInt::get(i32, 48)),
                           contiguous, wrapped);
        build.SetInsertPoint(contiguous);
        auto *values = build.CreateAlignedLoad(bytes,
            build.CreateInBoundsGEP(i8, vrf, source), llvm::Align(1));
        build.CreateAlignedStore(values, output, llvm::Align(1));
        build.CreateBr(done);
        build.SetInsertPoint(wrapped);
        for (unsigned lane = 0; lane < 16; ++lane) {
            auto *column = build.CreateAnd(build.CreateAdd(x,
                llvm::ConstantInt::get(i32, lane)), 63);
            auto *address = build.CreateOr(build.CreateAnd(source, ~63u), column);
            auto *value = build.CreateLoad(i8, build.CreateInBoundsGEP(i8, vrf, address));
            build.CreateStore(value, build.CreateInBoundsGEP(
                i8, output, llvm::ConstantInt::get(i32, lane)));
        }
        build.CreateBr(done);
        build.SetInsertPoint(done);
        build.CreateRetVoid();
        if (llvm::verifyFunction(*function, &llvm::errs()))
            throw std::runtime_error("VC2 JIT invalid vector byte-store kernel");
        gather_bytes_resources = engine->getMainJITDylib().createResourceTracker();
        return materialize(std::move(context), std::move(module), function,
                           gather_bytes_resources, "gather-bytes").toPtr<NativeGatherBytes>();
    }

    ImmediateKernel compile_immediate(VectorKernel kernel, unsigned source_width,
                                      unsigned destination_width, bool source_vertical,
                                      bool destination_vertical) {
        CompilationTimer compile_timer{compile_cpu};
        // Plain kernels have no flags, saturation or accumulator effects.
        // Only the low destination byte/halfword survives, so modular i32
        // arithmetic matches the interpreter's wider signed intermediates.
        auto context = std::make_unique<llvm::LLVMContext>();
        auto module = std::make_unique<llvm::Module>("vc2-vertical-immediate", *context);
        module->setDataLayout(engine->getDataLayout());
        module->setTargetTriple(engine->getTargetTriple().str());
        auto *i8 = llvm::Type::getInt8Ty(*context);
        auto *i32 = llvm::Type::getInt32Ty(*context);
        auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(*context),
            {llvm::PointerType::getUnqual(i8), i32, i32, i32}, false);
        const std::string name = "vc2_immediate_" + std::to_string(next_symbol++);
        auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                                 name, module.get());
        llvm::IRBuilder<> build(llvm::BasicBlock::Create(*context, "entry", function));
        auto *vrf = function->getArg(0);
        auto *destination = function->getArg(1);
        auto *source = function->getArg(2);
        auto *immediate = function->getArg(3);
        auto address = [&](llvm::Value *base, unsigned lane, unsigned byte, bool vertical) {
            auto *row = build.CreateAnd(build.CreateAdd(build.CreateLShr(base, 6),
                llvm::ConstantInt::get(i32, vertical ? lane : 0)), llvm::ConstantInt::get(i32, 63));
            auto *column = build.CreateAnd(build.CreateAdd(build.CreateAnd(base, 63),
                llvm::ConstantInt::get(i32, byte * 16 + (vertical ? 0 : lane))), llvm::ConstantInt::get(i32, 63));
            return build.CreateInBoundsGEP(i8, vrf,
                build.CreateOr(build.CreateShl(row, 6), column));
        };
        std::array<llvm::Value *, 16> values{};
        for (unsigned lane = 0; lane < 16; ++lane) {
            auto *low = build.CreateZExt(build.CreateLoad(i8, address(source, lane, 0, source_vertical)), i32);
            llvm::Value *a = low;
            if (source_width == 2) {
                auto *high = build.CreateZExt(build.CreateLoad(i8, address(source, lane, 1, source_vertical)), i32);
                a = build.CreateOr(low, build.CreateShl(high, 8));
            }
            switch (kernel) {
            case VectorKernel::Add: values[lane] = build.CreateAdd(a, immediate); break;
            case VectorKernel::Subtract: values[lane] = build.CreateSub(a, immediate); break;
            case VectorKernel::ReverseSubtract: values[lane] = build.CreateSub(immediate, a); break;
            case VectorKernel::Multiply: values[lane] = build.CreateMul(a, immediate); break;
            case VectorKernel::Move: values[lane] = immediate; break;
            case VectorKernel::And: values[lane] = build.CreateAnd(a, immediate); break;
            case VectorKernel::Or: values[lane] = build.CreateOr(a, immediate); break;
            case VectorKernel::Complement:
                values[lane] = build.CreateXor(a, llvm::ConstantInt::get(i32,
                    destination_width == 1 ? 255 : 65535));
                break;
            default: throw std::runtime_error("VC2 JIT unsupported immediate kernel");
            }
        }
        // Read every source lane before writing any destination lane. VRF
        // operands may overlap after their two-dimensional offsets wrap.
        for (unsigned lane = 0; lane < 16; ++lane) {
            build.CreateStore(build.CreateTrunc(values[lane], i8),
                              address(destination, lane, 0, destination_vertical));
            if (destination_width == 2)
                build.CreateStore(build.CreateTrunc(build.CreateLShr(values[lane], 8), i8),
                                  address(destination, lane, 1, destination_vertical));
        }
        build.CreateRetVoid();
        if (llvm::verifyFunction(*function, &llvm::errs()))
            throw std::runtime_error("VC2 JIT generated invalid immediate vector kernel");
        ImmediateKernel result;
        result.resources = engine->getMainJITDylib().createResourceTracker();
        result.native = materialize(std::move(context), std::move(module), function,
                                    result.resources,
                                    "immediate:" + std::to_string(static_cast<unsigned>(kernel)) +
                                    ":" + std::to_string(source_width) + ":" +
                                    std::to_string(destination_width) + ":" +
                                    std::to_string(source_vertical) + ":" +
                                    std::to_string(destination_vertical)).toPtr<NativeImmediate>();
        return result;
    }

    BinaryKernel compile_binary(unsigned opcode, unsigned layout, unsigned widths, unsigned modifiers) {
        CompilationTimer compile_timer{compile_cpu};
        auto context = std::make_unique<llvm::LLVMContext>();
        auto module = std::make_unique<llvm::Module>("vc2-binary-vector", *context);
        module->setDataLayout(engine->getDataLayout());
        module->setTargetTriple(engine->getTargetTriple().str());
        auto *i8 = llvm::Type::getInt8Ty(*context);
        auto *i32 = llvm::Type::getInt32Ty(*context);
        auto *i16 = llvm::Type::getInt16Ty(*context);
        auto *i64 = llvm::Type::getInt64Ty(*context);
        auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(*context),
            {llvm::PointerType::getUnqual(i8), i32, i32, i32, i32,
             llvm::PointerType::getUnqual(i64), llvm::PointerType::getUnqual(i32),
             llvm::PointerType::getUnqual(i8), llvm::PointerType::getUnqual(i8),
             llvm::PointerType::getUnqual(i8), llvm::PointerType::getUnqual(i32), i32}, false);
        const std::string name = "vc2_binary_vector_" + std::to_string(next_symbol++);
        auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                                 name, module.get());
        llvm::IRBuilder<> build(llvm::BasicBlock::Create(*context, "entry", function));
        auto *vrf = function->getArg(0);
        auto *destination = function->getArg(1);
        auto *left = function->getArg(2);
        auto *right = function->getArg(3);
        auto *selected = function->getArg(4);
        auto address = [&](llvm::Value *base, unsigned lane, bool vertical, unsigned plane = 0) {
            llvm::Value *row, *column;
            if (vertical) {
                row = build.CreateShl(build.CreateAnd(build.CreateAdd(
                    build.CreateLShr(base, 6), llvm::ConstantInt::get(i32, lane)),
                    llvm::ConstantInt::get(i32, 63)), 6);
                column = build.CreateAnd(base, 63);
            } else {
                row = build.CreateAnd(base, ~63u);
                column = build.CreateAnd(build.CreateAdd(build.CreateAnd(base, 63),
                    llvm::ConstantInt::get(i32, lane)), llvm::ConstantInt::get(i32, 63));
            }
            column = build.CreateAnd(build.CreateAdd(column, llvm::ConstantInt::get(i32, 16 * plane)), 63);
            return build.CreateInBoundsGEP(i8, vrf, build.CreateOr(row, column));
        };
        auto load_lane = [&](llvm::Value *base, unsigned lane, bool vertical, bool halfword) -> llvm::Value * {
            auto *low = build.CreateLoad(i8, address(base, lane, vertical));
            if (!halfword) return build.CreateZExt(low, i64);
            auto *high = build.CreateLoad(i8, address(base, lane, vertical, 1));
            auto *word = build.CreateOr(build.CreateZExt(low, i16),
                build.CreateShl(build.CreateZExt(high, i16), 8));
            return (modifiers & 336) ? build.CreateZExt(word, i64) : build.CreateSExt(word, i64);
        };
        const bool carry_simd = (opcode == 34 || opcode == 35) && modifiers == 64;
        if (layout == 0 && ((modifiers & ~128u) == 0 || carry_simd))
            emit_horizontal_simd(build, *function, opcode, widths,
                modifiers & 128, carry_simd ? function->getArg(9) : nullptr);
        std::array<llvm::Value *, 16> values{}, shifted_carry{};
        for (unsigned lane = 0; lane < 16; ++lane) {
            llvm::Value *a = opcode == 4 ? llvm::ConstantInt::get(i64, 0)
                                         : load_lane(left, lane, layout & 2, widths & 2);
            auto *b = (modifiers & 128) ? build.CreateSExt(right, i64)
                                       : load_lane(right, lane, layout & 4, widths & 4);
            if (modifiers & 8192)
                shifted_carry[lane] = build.CreateICmpNE(build.CreateAnd(a, 1), llvm::ConstantInt::get(i64, 0));
            auto *shift = build.CreateAnd(b, 31);
            llvm::Value *value = nullptr;
            switch (opcode) {
            case 4: value = b; break;
            case 5: value = build.CreateAnd(a, b); break;
            case 6: value = build.CreateOr(a, b); break;
            case 8: case 9: value = build.CreateAdd(a, b); break;
            case 10: case 11: value = build.CreateSub(a, b); break;
            case 12: case 13: value = build.CreateSub(b, a); break;
            case 14: value = build.CreateMul(a, b); break;
            case 16: case 17:
                value = build.CreateMul(a, b);
                if (!(modifiers & 16)) value = build.CreateAShr(value, 8);
                break;
            case 18: value = build.CreateAShr(build.CreateMul(a, b), 16); break;
            case 19:
                value = build.CreateAShr(build.CreateMul(a, build.CreateAnd(b, 65535)), 16);
                break;
            case 51:
                value = build.CreateLShr(build.CreateMul(build.CreateAnd(a, 65535),
                                                         build.CreateAnd(b, 65535)), 16);
                break;
            case 20: case 21: case 52: {
                // Preserve the interpreter's signedness model and ties away
                // from zero. JIT agreement does not raise hardware confidence.
                auto *factor = opcode == 20 ? b : build.CreateAnd(b, 65535);
                auto *product = build.CreateMul(a, factor);
                auto *negative = build.CreateICmpSLT(product, llvm::ConstantInt::get(i64, 0));
                auto *magnitude = build.CreateSelect(negative, build.CreateNeg(product), product);
                auto *rounded = build.CreateLShr(build.CreateAdd(magnitude,
                    llvm::ConstantInt::get(i64, 32768)), 16);
                value = build.CreateSelect(negative, build.CreateNeg(rounded), rounded);
                break;
            }
            case 34: case 35: {
                auto *flag = build.CreateInBoundsGEP(i8, function->getArg(9),
                                                    llvm::ConstantInt::get(i32, lane));
                auto *carry = build.CreateZExt(build.CreateLoad(i8, flag), i64);
                value = opcode == 34 ? build.CreateAdd(build.CreateAdd(a, b), carry)
                                     : build.CreateSub(build.CreateSub(a, b), carry);
                break;
            }
            case 24: value = build.CreateShl(a, shift); break;
            case 26: value = build.CreateAShr(a, shift); break;
            case 27: value = (modifiers & 256) ? build.CreateLShr(a, shift)
                : build.CreateLShr(build.CreateAnd(a, (widths & 2) ? 65535 : 255), shift); break;
            case 28:
                value = build.CreateSelect(build.CreateICmpSLT(a, b), a, b); break;
            case 37: {
                auto *minimum = build.CreateSelect(build.CreateICmpSLT(a, b), a, b);
                value = build.CreateSelect(build.CreateICmpSLT(minimum, llvm::ConstantInt::get(i64, 0)),
                    llvm::ConstantInt::get(i64, 0), minimum); break;
            }
            case 29: value = build.CreateSelect(build.CreateICmpSGT(a, b), a, b); break;
            case 30: value = build.CreateSelect(build.CreateICmpSGE(a, b),
                         build.CreateSub(a, b), build.CreateSub(b, a)); break;
            default: throw std::runtime_error("VC2 JIT unsupported binary vector operation");
            }
            values[lane] = value;
        }
        llvm::AllocaInst *reduced = nullptr;
        llvm::AllocaInst *has_lane = nullptr;
        if (modifiers & 2048) {
            reduced = build.CreateAlloca(i64);
            has_lane = build.CreateAlloca(build.getInt1Ty());
            build.CreateStore(llvm::ConstantInt::get(i64, 0), reduced);
            build.CreateStore(build.getFalse(), has_lane);
        }
        for (unsigned lane = 0; lane < 16; ++lane) {
            llvm::BasicBlock *next = nullptr;
            if (modifiers & 32) {
                auto *write = llvm::BasicBlock::Create(*context, "write-lane", function);
                next = llvm::BasicBlock::Create(*context, "next-lane", function);
                build.CreateCondBr(build.CreateICmpNE(build.CreateAnd(selected, 1u << lane),
                    llvm::ConstantInt::get(i32, 0)), write, next);
                build.SetInsertPoint(write);
            }
            llvm::Value *value = values[lane];
            if (modifiers & 279) {
                auto *acc_ptr = build.CreateInBoundsGEP(i64, function->getArg(5), llvm::ConstantInt::get(i32, lane));
                auto *low_ptr = build.CreateInBoundsGEP(i32, function->getArg(6), llvm::ConstantInt::get(i32, lane));
                llvm::Value *acc = (modifiers & 1) ? static_cast<llvm::Value *>(llvm::ConstantInt::get(i64, 0)) : build.CreateLoad(i64, acc_ptr);
                llvm::Value *low = (modifiers & 1) ? static_cast<llvm::Value *>(llvm::ConstantInt::get(i64, 0)) : build.CreateZExt(build.CreateLoad(i32, low_ptr), i64);
                if (modifiers & (16 | 256)) {
                    // These decoder-guarded forms retain full accumulator
                    // precision. Nibble operands contribute their low 16 bits.
                    auto *addend = (modifiers & 256) ? build.CreateAnd(value, 65535) : value;
                    auto *total = build.CreateAdd(build.CreateAdd(build.CreateShl(acc, 16), low), addend);
                    acc = build.CreateAShr(total, 16);
                    low = build.CreateAnd(total, 65535);
                    if (modifiers & 256) {
                        value = total;
                    } else {
                        auto *scaled = build.CreateAShr(total, 8);
                        auto *maximum = llvm::ConstantInt::get(i64, 255);
                        value = build.CreateSelect(build.CreateICmpSGT(scaled, maximum), maximum, scaled);
                    }
                } else if (modifiers & 6) {
                    llvm::Value *total;
                    if (modifiers & 4) {
                        auto *addend = (modifiers & 2) ? value : build.CreateAnd(value, 65535);
                        total = build.CreateAdd(build.CreateAdd(build.CreateShl(acc, 16), low), addend);
                    } else {
                        if (opcode == 24) value = build.CreateSExt(build.CreateTrunc(value, i16), i64);
                        total = build.CreateAdd(build.CreateShl(build.CreateAdd(acc, value), 16), low);
                    }
                    auto *minimum = llvm::ConstantInt::getSigned(i64, -2147483648LL);
                    auto *maximum = llvm::ConstantInt::get(i64, 2147483647);
                    total = build.CreateSelect(build.CreateICmpSLT(total, minimum), minimum, total);
                    total = build.CreateSelect(build.CreateICmpSGT(total, maximum), maximum, total);
                    acc = build.CreateAShr(total, 16);
                    low = build.CreateAnd(total, 65535);
                    value = (modifiers & 4) ? total : acc;
                }
                build.CreateStore(acc, acc_ptr);
                build.CreateStore(build.CreateTrunc(low, i32), low_ptr);
            }
            if (!(modifiers & 16) && (opcode == 9 || opcode == 11 || opcode == 13 || opcode == 17)) {
                value = build.CreateSelect(build.CreateICmpSLT(value,
                    llvm::ConstantInt::getSigned(i64, (widths & 1) ? -32768 : 0)), llvm::ConstantInt::getSigned(i64, (widths & 1) ? -32768 : 0), value);
                value = build.CreateSelect(build.CreateICmpSGT(value,
                    llvm::ConstantInt::get(i64, (widths & 1) ? 32767 : 255)), llvm::ConstantInt::get(i64, (widths & 1) ? 32767 : 255), value);
            }
            if (modifiers & 8) {
                const unsigned mask = (widths & 1) ? 65535 : 255;
                auto *negative = build.CreateICmpSLT(value, llvm::ConstantInt::get(i64, 0));
                auto *zero = build.CreateICmpEQ(build.CreateAnd(value, mask),
                                               llvm::ConstantInt::get(i64, 0));
                auto *carry = (modifiers & 8192) ? shifted_carry[lane] : build.CreateOr(negative, build.CreateICmpSGT(value,
                                                    llvm::ConstantInt::get(i64, mask)));
                // High-half ADC carries unsigned, while IFN consumes the
                // resulting halfword's sign in the firmware colour clamp.
                if (opcode == 34 && (modifiers & 64))
                    negative = build.CreateICmpNE(build.CreateAnd(value, 0x8000),
                                                  llvm::ConstantInt::get(i64, 0));
                const std::array<llvm::Value *, 3> flags{zero, negative, carry};
                for (unsigned flag = 0; flag < flags.size(); ++flag) {
                    auto *target = build.CreateInBoundsGEP(i8, function->getArg(7 + flag),
                                                          llvm::ConstantInt::get(i32, lane));
                    build.CreateStore(build.CreateZExt(flags[flag], i8), target);
                }
            }
            if (modifiers & 2048) {
                auto *previous = build.CreateLoad(i64, reduced);
                llvm::Value *combined;
                switch ((modifiers >> 9) & 3) {
                case 1: combined = build.CreateAdd(previous, value); break;
                case 2: combined = build.CreateSelect(build.CreateICmpSLT(previous, value),
                                                      previous, value); break;
                default: combined = build.CreateSelect(build.CreateICmpSGT(previous, value),
                                                        previous, value); break;
                }
                auto *any = build.CreateLoad(build.getInt1Ty(), has_lane);
                build.CreateStore(build.CreateSelect(any, combined, value), reduced);
                build.CreateStore(build.getTrue(), has_lane);
            }
            if (!(modifiers & 4096)) {
                build.CreateStore(build.CreateTrunc(value, i8),
                                  address(destination, lane, layout & 1));
                if (widths & 1)
                    build.CreateStore(build.CreateTrunc(build.CreateLShr(value, 8), i8),
                                      address(destination, lane, layout & 1, 1));
            }
            if (next) {
                build.CreateBr(next);
                build.SetInsertPoint(next);
            }
        }
        if (modifiers & 2048) {
            auto *target = build.CreateInBoundsGEP(i32, function->getArg(10), function->getArg(11));
            build.CreateStore(build.CreateTrunc(build.CreateLoad(i64, reduced), i32), target);
        }
        build.CreateRetVoid();
        if (llvm::verifyFunction(*function, &llvm::errs()))
            throw std::runtime_error("VC2 JIT generated invalid binary vector kernel");
        BinaryKernel result;
        result.resources = engine->getMainJITDylib().createResourceTracker();
        result.native = materialize(std::move(context), std::move(module), function,
                                    result.resources,
                                    "binary:" + std::to_string(opcode) + ":" +
                                    std::to_string(layout) + ":" +
                                    std::to_string(widths) + ":" +
                                    std::to_string(modifiers)).toPtr<NativeBinary>();
        return result;
    }

    NativeAddFlags compile_add_setf_h16(bool lowcarry) {
        CompilationTimer compile_timer{compile_cpu};
        auto context = std::make_unique<llvm::LLVMContext>();
        auto module = std::make_unique<llvm::Module>("vc2-add-setf-h16", *context);
        module->setDataLayout(engine->getDataLayout());
        module->setTargetTriple(engine->getTargetTriple().str());
        auto *i8 = llvm::Type::getInt8Ty(*context);
        auto *i16 = llvm::Type::getInt16Ty(*context);
        auto *i32 = llvm::Type::getInt32Ty(*context);
        auto *byte_pointer = llvm::PointerType::getUnqual(i8);
        auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(*context),
            {byte_pointer, i32, i32, i32, byte_pointer, byte_pointer, byte_pointer}, false);
        const std::string name = "vc2_add_setf_" + std::to_string(next_symbol++);
        auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                                 name, module.get());
        llvm::IRBuilder<> build(llvm::BasicBlock::Create(*context, "entry", function));
        auto *vrf = function->getArg(0);
        auto *destination = function->getArg(1);
        auto *left = function->getArg(2);
        auto *right = function->getArg(3);
        auto *zero_flags = function->getArg(4);
        auto *negative_flags = function->getArg(5);
        auto *carry_flags = function->getArg(6);
        emit_add_flags_simd(build, *function, lowcarry);
        auto address = [&](llvm::Value *base, unsigned lane, unsigned byte) {
            auto *column = build.CreateAnd(build.CreateAdd(build.CreateAnd(base, 63),
                llvm::ConstantInt::get(i32, lane + 16 * byte)), llvm::ConstantInt::get(i32, 63));
            return build.CreateInBoundsGEP(i8, vrf,
                build.CreateOr(build.CreateAnd(base, ~63u), column));
        };
        auto read_halfword = [&](llvm::Value *base, unsigned lane) {
            auto *low = build.CreateZExt(build.CreateLoad(i8, address(base, lane, 0)), i16);
            auto *high = build.CreateZExt(build.CreateLoad(i8, address(base, lane, 1)), i16);
            return build.CreateSExt(build.CreateOr(low, build.CreateShl(high, 8)), i32);
        };
        std::array<llvm::Value *, 16> sums{}, carry_values{};
        for (unsigned lane = 0; lane < 16; ++lane) {
            auto *a = read_halfword(left, lane);
            auto *b = read_halfword(right, lane);
            sums[lane] = build.CreateAdd(a, b);
            if (lowcarry)
                carry_values[lane] = build.CreateICmpUGT(build.CreateAdd(
                    build.CreateAnd(a, 65535), build.CreateAnd(b, 65535)),
                    llvm::ConstantInt::get(i32, 65535));
        }
        auto flag_address = [&](llvm::Value *flags, unsigned lane) {
            return build.CreateInBoundsGEP(i8, flags, llvm::ConstantInt::get(i32, lane));
        };
        for (unsigned lane = 0; lane < 16; ++lane) {
            auto *sum = sums[lane];
            build.CreateStore(build.CreateTrunc(sum, i8), address(destination, lane, 0));
            build.CreateStore(build.CreateTrunc(build.CreateLShr(sum, 8), i8),
                              address(destination, lane, 1));
            auto *zero = build.CreateICmpEQ(build.CreateAnd(sum, 65535),
                                            llvm::ConstantInt::get(i32, 0));
            auto *negative = build.CreateICmpSLT(sum, llvm::ConstantInt::get(i32, 0));
            auto *carry = lowcarry ? carry_values[lane] : build.CreateOr(negative,
                build.CreateICmpSGT(sum, llvm::ConstantInt::get(i32, 65535)));
            build.CreateStore(build.CreateZExt(zero, i8), flag_address(zero_flags, lane));
            build.CreateStore(build.CreateZExt(negative, i8), flag_address(negative_flags, lane));
            build.CreateStore(build.CreateZExt(carry, i8), flag_address(carry_flags, lane));
        }
        build.CreateRetVoid();
        if (llvm::verifyFunction(*function, &llvm::errs()))
            throw std::runtime_error("VC2 JIT generated invalid vector add/flags kernel");
        auto &resources = add_flags_resources[lowcarry];
        resources = engine->getMainJITDylib().createResourceTracker();
        return materialize(std::move(context), std::move(module), function,
                           resources, "add-flags:" + std::to_string(lowcarry))
            .toPtr<NativeAddFlags>();
    }

    NativeFillLoop compile_fill_loop(Block &block) {
        CompilationTimer compile_timer{compile_cpu};
        auto context = std::make_unique<llvm::LLVMContext>();
        auto module = std::make_unique<llvm::Module>("vc2-store-loop", *context);
        module->setDataLayout(engine->getDataLayout());
        module->setTargetTriple(engine->getTargetTriple().str());
        auto *i32 = llvm::Type::getInt32Ty(*context);
        auto *pointer = llvm::PointerType::getUnqual(i32);
        auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(*context),
                                            {pointer, pointer, i32}, false);
        auto *function = llvm::Function::Create(type, llvm::Function::ExternalLinkage,
                                                "vc2_store_loop", module.get());
        llvm::IRBuilder<> build(llvm::BasicBlock::Create(*context, "entry", function));
        auto reg = [&](unsigned n) { return build.CreateGEP(i32, function->getArg(0), build.getInt32(n)); };
        auto *address = build.CreateLoad(i32, reg(block.memory_base));
        auto *value = build.CreateLoad(i32, reg(block.memory_register));
        auto *counter = build.CreateLoad(i32, reg(block.fill_counter));
        auto *count = function->getArg(2);
        auto *done = llvm::BasicBlock::Create(*context, "done", function);
        // The caller bounds this trace at the next 32-instruction service point.
        for (unsigned i = 0; i < 16; ++i) {
            auto *store = llvm::BasicBlock::Create(*context, "store", function);
            build.CreateCondBr(build.CreateICmpUGT(count, build.getInt32(i)), store, done);
            build.SetInsertPoint(store);
            build.CreateStore(value, build.CreateGEP(i32, function->getArg(1), build.getInt32(i)));
        }
        build.CreateBr(done);
        build.SetInsertPoint(done);
        build.CreateStore(build.CreateAdd(address, build.CreateMul(count, build.getInt32(4))), reg(block.memory_base));
        auto *remaining = build.CreateSub(counter, count);
        build.CreateStore(remaining, reg(block.fill_counter));
        build.CreateStore(build.CreateSelect(build.CreateICmpEQ(remaining, build.getInt32(0)),
            build.getInt32(block.pc + 6), build.getInt32(block.pc)), reg(31));
        build.CreateRetVoid();
        if (llvm::verifyFunction(*function, &llvm::errs()))
            throw std::runtime_error("Invalid VC2 store-loop IR");
        block.resources = engine->getMainJITDylib().createResourceTracker();
        return materialize(std::move(context), std::move(module), function,
                           block.resources, "store-loop").toPtr<NativeFillLoop>();
    }

    unsigned try_execute(Core &core, unsigned limit, u16 first_word) {
        if (limit == 0) return 0;
        const u32 pc = core.r[31];
        if (pc >= 0x4000000 || (pc & 1)) return 0;
        auto &page = pages[pc >> 12];
        if (!page) page = std::make_unique<Page>();
        Entry &entry = page->entries[(pc & 4095) >> 1];
        if (entry.block) {
            Block &block = *entry.block;
            if (block.kind == Block::Kind::VectorRegion && block.vector_pcs.size() > limit) return 0;
            if (block.kind == Block::Kind::Scalar && block.operations.size() > limit)
                return 0;
            const unsigned length = block.kind == Block::Kind::Scalar && !block.length
                ? static_cast<unsigned>(block.operations.size()) : block.length;
            if (code_unchanged(core, pc, length, block.code_versions))
                return execute(core, block, limit);
            bool matches = true;
            for (unsigned i = 0; i < length; ++i) {
                const u16 expected = block.kind == Block::Kind::Scalar && !block.length
                    ? block.operations[i].word : block.words[i];
                matches &= (i ? core.read(pc + 2 * i, 2) : first_word) == expected;
            }
            if (matches) {
                block.code_versions = watch_code(core, pc, length);
                return execute(core, block, limit);
            }
            // Guest stores and DMA invalidate this decoded block. Shared native
            // objects remain cached by content for other blocks and later reuse.
            if (block.resources)
                if (auto error = block.resources->remove())
                    throw std::runtime_error("VC2 JIT invalidation: " +
                                             llvm::toString(std::move(error)));
            blocks.erase(pc);
            entry.block = nullptr;
        }
        if (entry.rejection.length) {
            if (code_unchanged(core, pc, entry.rejection.length,
                               entry.rejection.code_versions))
                return 0;
            bool unchanged = true;
            for (unsigned i = 0; i < entry.rejection.length; ++i)
                unchanged &= (i ? core.read(pc + 2 * i, 2) : first_word) ==
                             entry.rejection.words[i];
            if (unchanged) {
                entry.rejection.code_versions = watch_code(core, pc,
                                                           entry.rejection.length);
                return 0;
            }
            entry.rejection = {};
        }
        if (entry.visits < 64) ++entry.visits;
        if (entry.visits < 64 || blocks.size() >= 8192) return 0;
        if ((first_word & 0xfc00) == 0x2000 && limit >= 2) {
            Block region;
            region.pc = pc;
            if (build_vector_region(core, region, limit)) {
                auto [inserted, fresh] = blocks.emplace(pc, std::move(region));
                (void)fresh;
                entry.block = &inserted->second;
                return execute(core, inserted->second, limit);
            }
        }
        // Observed palette-shader halfword texture reads. Keep unsupported
        // indexed forms on the interpreter path.
        if (first_word == 0x2156 && pc + 10 <= 0x4000000) {
            const u16 destination = static_cast<u16>(core.read(pc + 2, 2));
            const u16 index = static_cast<u16>(core.read(pc + 4, 2));
            const u16 offset = static_cast<u16>(core.read(pc + 6, 2));
            const u16 modifier = static_cast<u16>(core.read(pc + 8, 2));
            const unsigned bank = destination >> 8;
            if ((destination & 255) == 0 && ((bank & 0xf0) == 0x60 || (bank & 0xf0) == 0xa0) &&
                !(bank & 1) && index == 0x1f && modifier == 0x80e7 &&
                (offset == 0 || (offset == 0x2000 && (bank & 3) == 2))) {
                if (!indexed_h16_kernel) {
                    const auto start = std::chrono::steady_clock::now();
                    indexed_h16_kernel = compile_indexed_h16();
                    core.jit_compile_us += static_cast<u64>(std::chrono::duration_cast<
                        std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
                    ++core.jit_compilations;
                }
                Block indexed;
                indexed.kind = Block::Kind::IndexedH16;
                indexed.pc = pc; indexed.length = 5;
                indexed.words = {first_word, destination, index, offset, modifier};
                indexed.destination.base = (((bank >> 6) * 16) << 6) | (32 + (bank & 15));
                indexed.destination.vertical = true;
                indexed.destination.width = 2;
                indexed.load_offset = offset == 0x2000 ? 2 : 0;
                indexed.indexed_native = indexed_h16_kernel;
                indexed.code_versions = watch_code(core, pc, 5);
                auto [inserted, fresh] = blocks.emplace(pc, std::move(indexed));
                (void)fresh;
                entry.block = &inserted->second;
                return execute(core, inserted->second, limit);
            }
        }
        // Postincrement word store followed by decrement/compare-zero/backedge.
        // Register separation makes every iteration independent of the stored value.
        if ((first_word & 0xff00) == 0x7900 && pc + 6 <= 0x4000000) {
            const u16 branch = static_cast<u16>(core.read(pc + 2, 2));
            const u16 operand = static_cast<u16>(core.read(pc + 4, 2));
            const unsigned base = (first_word >> 4) & 15, source = first_word & 15;
            const unsigned counter = branch & 15;
            if ((branch & 0xfff0) == 0x01f0 && operand == 0xc0ff &&
                base != source && base != counter && source != counter) {
                Block loop;
                loop.kind = Block::Kind::FillLoop;
                loop.pc = pc; loop.length = 3;
                loop.words = {first_word, branch, operand, 0, 0};
                loop.memory_base = base; loop.memory_register = source;
                loop.fill_counter = counter;
                const auto start = std::chrono::steady_clock::now();
                loop.fill_native = compile_fill_loop(loop);
                core.jit_compile_us += static_cast<u64>(std::chrono::duration_cast<
                    std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
                ++core.jit_compilations;
                loop.code_versions = watch_code(core, pc, 3);
                auto [inserted, fresh] = blocks.emplace(pc, std::move(loop));
                (void)fresh;
                entry.block = &inserted->second;
                return execute(core, inserted->second, limit);
            }
        }
        if ((first_word & 0xff00) == 0xe000 ||
            (first_word & 0xff00) == 0xf000) {
            const unsigned base = std::array<unsigned, 4>{0, 6, 16, 24}[
                (first_word >> 5) & 3];
            const unsigned m = first_word & 31;
            const unsigned count = (base == 24 && m == 15) ? 0 : m + 1;
            const unsigned total = count + ((first_word >> 7) & 1);
            const bool push = (first_word & 0xff00) == 0xf000;
            const u32 displacement = push ? 0u - 4u * total : 0;
            const u64 key = 25u | (4u << 5) | (7u << 10) |
                (static_cast<u64>(displacement) << 13);
            auto found = address_kernels.find(key);
            if (found == address_kernels.end()) {
                const auto start = std::chrono::steady_clock::now();
                found = address_kernels.emplace(key,
                    compile_address(25, 4, false, false, 7, displacement)).first;
                core.jit_compile_us += static_cast<u64>(std::chrono::duration_cast<
                    std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
                ++core.jit_compilations;
            }
            Block stack;
            stack.kind = Block::Kind::Stack;
            stack.pc = pc;
            stack.length = 1;
            stack.words[0] = first_word;
            stack.stack_base = base;
            stack.stack_count = count;
            stack.stack_total = total;
            stack.stack_push = push;
            stack.native_address = found->second.native;
            stack.code_versions = watch_code(core, pc, 1);
            auto [inserted, fresh] = blocks.emplace(pc, std::move(stack));
            (void)fresh;
            entry.block = &inserted->second;
            return execute(core, inserted->second, limit);
        }
        if ((first_word & 0xfff0) == 0x4c30 ||
            (first_word & 0xfff0) == 0x4d30) {
            const unsigned source = first_word & 15;
            const bool halfword = (first_word & 0xfff0) == 0x4d30;
            const u32 displacement = pc + 2;
            const u64 key = source | (1u << 5) |
                (static_cast<u64>(halfword) << 9) |
                (static_cast<u64>(halfword ? source : 7) << 10) |
                (static_cast<u64>(displacement) << 13);
            auto found = address_kernels.find(key);
            if (found == address_kernels.end()) {
                const auto start = std::chrono::steady_clock::now();
                found = address_kernels.emplace(key,
                    compile_address(source, 1, false, halfword,
                                    halfword ? source : 7, displacement)).first;
                core.jit_compile_us += static_cast<u64>(std::chrono::duration_cast<
                    std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
                ++core.jit_compilations;
            }
            Block table;
            table.kind = Block::Kind::TableBranch;
            table.pc = pc;
            table.length = 1;
            table.words[0] = first_word;
            table.memory_width = halfword ? 2 : 1;
            table.native_address = found->second.native;
            table.code_versions = watch_code(core, pc, 1);
            auto [inserted, fresh] = blocks.emplace(pc, std::move(table));
            (void)fresh;
            entry.block = &inserted->second;
            return execute(core, inserted->second, limit);
        }
        if ((first_word & 0xfc00) == 0x2800 && pc + 4 <= 0x4000000 &&
            !(((first_word >> 8) & 3) == 0 && !(core.read(pc + 2, 2) & 3))) {
            static constexpr unsigned widths[] = {4, 4, 2, 2, 1, 1, 2, 1};
            const u16 second = static_cast<u16>(core.read(pc + 2, 2));
            unsigned op = (first_word >> 8) & 3;
            u32 displacement = static_cast<u32>(sign_extend(second, 16));
            if (op < 2 && (second & 1)) {
                op = op == 0 ? 2 : 6;
                displacement &= ~1u;
            } else if (op == 0) {
                op = second & 2 ? 1 : 0;
                displacement &= ~3u;
            } else if (op == 1) {
                op = 3;
                displacement &= ~1u;
            } else {
                op = op == 2 ? 4 : 5;
            }
            const unsigned source = (first_word >> 4) & 15;
            const u64 key = source | (widths[op] << 5) | (7u << 10) |
                (static_cast<u64>(displacement) << 13);
            auto found = address_kernels.find(key);
            if (found == address_kernels.end()) {
                const auto start = std::chrono::steady_clock::now();
                found = address_kernels.emplace(key,
                    compile_address(source, widths[op], false, false, 7,
                                    displacement)).first;
                core.jit_compile_us += static_cast<u64>(std::chrono::duration_cast<
                    std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
                ++core.jit_compilations;
            }
            Block memory;
            memory.kind = Block::Kind::Memory;
            memory.pc = pc;
            memory.length = 2;
            memory.words[0] = first_word;
            memory.words[1] = second;
            memory.memory_op = op;
            memory.memory_register = first_word & 15;
            memory.memory_base = source;
            memory.memory_width = widths[op];
            memory.memory_indexed = true;
            memory.native_address = found->second.native;
            memory.code_versions = watch_code(core, pc, 2);
            auto [inserted, fresh] = blocks.emplace(pc, std::move(memory));
            (void)fresh;
            entry.block = &inserted->second;
            return execute(core, inserted->second, limit);
        }
        if ((first_word & 0xffe0) == 0x2f60 &&
            (first_word & 31) != 31 && pc + 6 <= 0x4000000) {
            const u16 low = static_cast<u16>(core.read(pc + 2, 2));
            const u16 high = static_cast<u16>(core.read(pc + 4, 2));
            Block move;
            move.pc = pc;
            move.length = 3;
            move.words[0] = first_word;
            move.words[1] = low;
            move.words[2] = high;
            move.operations.push_back(Operation{.word = first_word, .opcode = 14,
                .destination = static_cast<unsigned>(first_word & 31),
                .immediate = static_cast<u32>(low) | (static_cast<u32>(high) << 16),
                .immediate_source = true});
            const auto start = std::chrono::steady_clock::now();
            move.native = compile(move);
            move.code_versions = watch_code(core, pc, 3);
            core.jit_compile_us += static_cast<u64>(std::chrono::duration_cast<
                std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
            ++core.jit_compilations;
            auto [inserted, fresh] = blocks.emplace(pc, std::move(move));
            (void)fresh;
            entry.block = &inserted->second;
            return execute(core, inserted->second, limit);
        }
        // All scalar memory forms use the same bus-backed path. Some update
        // their base register; offset and indexed forms leave it alone.
        if ((first_word & 0xf800) == 0x7800 ||
             (first_word & 0xf800) == 0x7000 ||
             ((first_word & 0xc000) == 0x8000 &&
              ((first_word >> 8) & 7) != 0) ||
             ((first_word & 0xe000) == 0xe000 &&
              (first_word & 0xff00) != 0xe000 &&
              (first_word & 0xff00) != 0xf000) ||
             ((first_word & 0xe030) == 0x4010 &&
              ((first_word >> 6) & 3) == 1)) {
            static constexpr unsigned widths[] = {4, 4, 2, 2, 1, 1, 2, 1};
            const bool offset_word = (first_word & 0xe000) == 0xe000;
            const bool stack_word = (first_word & 0xe030) == 0x4010;
            const unsigned op = offset_word ? ((first_word >> 12) & 1)
                : stack_word ? ((first_word >> 6) & 1) : ((first_word >> 8) & 7);
            const unsigned source = stack_word ? 25 : ((first_word >> 4) & 15);
            const bool indexed = (first_word & 0xc000) == 0x8000;
            const unsigned index = indexed ? (first_word >> 11) & 7 : 7;
            const bool pre = (first_word & 0xf800) == 0x7000;
            const u32 displacement = offset_word ? 4 * ((first_word >> 8) & 15)
                : stack_word ? 4 * ((first_word >> 8) & 31) : 0;
            const u64 key = source | (widths[op] << 5) |
                (static_cast<unsigned>(pre) << 8) |
                (static_cast<unsigned>(indexed) << 9) | (index << 10) |
                (static_cast<u64>(displacement) << 13);
            auto found = address_kernels.find(key);
            if (found == address_kernels.end()) {
                const auto start = std::chrono::steady_clock::now();
                found = address_kernels.emplace(key,
                    compile_address(source, widths[op], pre, indexed, index,
                                    displacement)).first;
                core.jit_compile_us += static_cast<u64>(std::chrono::duration_cast<
                    std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
                ++core.jit_compilations;
            }
            Block memory;
            memory.kind = Block::Kind::Memory;
            memory.pc = pc;
            memory.length = 1;
            memory.words[0] = first_word;
            memory.memory_op = op;
            memory.memory_register = first_word & 15;
            memory.memory_base = source;
            memory.memory_width = widths[op];
            memory.memory_pre = pre;
            memory.memory_indexed = indexed || offset_word || stack_word;
            memory.native_address = found->second.native;
            memory.code_versions = watch_code(core, pc, 1);
            auto [inserted, fresh] = blocks.emplace(pc, std::move(memory));
            (void)fresh;
            entry.block = &inserted->second;
            return execute(core, inserted->second, limit);
        }
        if ((first_word == 0x2208 || first_word == 0x2209 ||
             first_word == 0x220c || first_word == 0x220d) &&
            pc + 6 <= 0x4000000) {
            const u16 second = static_cast<u16>(core.read(pc + 2, 2));
            const u16 third = static_cast<u16>(core.read(pc + 4, 2));
            if (((second & 31) == 1 || (second & 31) == 3) &&
                (third & 0x7ff) == 7) {
                const u32 bits = ((second & 0xe0u) << 10) |
                    ((first_word & 4u) << 12) | ((first_word & 1u) << 13) |
                    ((second >> 8) << 5);
                const unsigned row = ((second & 2) ? 16 : 0) +
                                     ((third >> 11) & 15);
                const unsigned column = 32u + ((static_cast<unsigned>(third) >> 15) * 16u);
                Block raster;
                raster.kind = Block::Kind::RasterStore;
                raster.pc = pc;
                raster.length = 3;
                raster.words[0] = first_word;
                raster.words[1] = second;
                raster.words[2] = third;
                raster.source.base = ((row & 63) << 6) | (column & 63);
                raster.load_offset = static_cast<u32>(sign_extend(bits, 18));
                if (!gather_bytes_kernel) {
                    const auto start = std::chrono::steady_clock::now();
                    gather_bytes_kernel = compile_gather_bytes();
                    core.jit_compile_us += static_cast<u64>(std::chrono::duration_cast<
                        std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
                    ++core.jit_compilations;
                }
                raster.gather_bytes_native = gather_bytes_kernel;
                raster.code_versions = watch_code(core, pc, 3);
                auto [inserted, fresh] = blocks.emplace(pc, std::move(raster));
                (void)fresh;
                entry.block = &inserted->second;
                return execute(core, inserted->second, limit);
            }
        }
        if (first_word == 0x2200 && pc + 6 <= 0x4000000) {
            const u16 second = static_cast<u16>(core.read(pc + 2, 2));
            const u16 third = static_cast<u16>(core.read(pc + 4, 2));
            const unsigned dest_code = second >> 8;
            const unsigned row = (dest_code >> 6) * 16 + (dest_code & 15);
            const unsigned column = ((dest_code >> 4) & 3) * 16;
            Block load;
            load.kind = Block::Kind::VectorLoad;
            load.pc = pc;
            load.length = 3;
            load.words[0] = first_word;
            load.words[1] = second;
            load.words[2] = third;
            load.destination.base = ((row & 63) << 6) | column;
            load.destination.step = true;
            load.load_base = (third >> 3) & 7;
            load.load_stride = third & 7;
            load.load_offset = static_cast<u32>(
                8 * sign_extend(((second & 255) << 7) | (third >> 9), 15));
            const unsigned repeat_bits = (third >> 6) & 7;
            load.dynamic_repeats = repeat_bits == 7;
            load.repeats = 1u << repeat_bits;
            if (!load_bytes_kernel) {
                const auto start = std::chrono::steady_clock::now();
                load_bytes_kernel = compile_load_bytes();
                core.jit_compile_us += static_cast<u64>(std::chrono::duration_cast<
                    std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
                ++core.jit_compilations;
            }
            load.load_bytes_native = load_bytes_kernel;
            load.code_versions = watch_code(core, pc, 3);
            auto [inserted, fresh] = blocks.emplace(pc, std::move(load));
            (void)fresh;
            entry.block = &inserted->second;
            return execute(core, inserted->second, limit);
        }
        if ((first_word & 0xfc00) == 0x2000) {
            const unsigned length = (first_word & 0x200) ? 3 : 5;
            if (pc + 2 * length <= 0x4000000) {
                std::array<u32, 5> words{};
                words[0] = first_word;
                for (unsigned i = 1; i < length; ++i)
                    words[i] = core.read(pc + 2 * i, 2);
                std::optional<VectorPlan> decoded;
                try {
                    decoded = core.decode_vector(words, length);
                } catch (const std::runtime_error &) {
                    // The interpreter reports unsupported vector forms.
                }
                if (decoded && (decoded->length == length || decoded->length == 0) &&
                    decoded->modes.kernel == VectorKernel::Move &&
                    decoded->bkind == 0 && !decoded->d.vertical &&
                    !decoded->b.vertical && decoded->d.width == 2 &&
                    decoded->b.width == 2 && !decoded->d.discard) {
                        Block move;
                        move.kind = Block::Kind::MoveH16;
                        move.pc = pc;
                        move.length = length;
                        move.repeats = decoded->count;
                        move.dynamic_repeats = decoded->length == 0;
                        move.destination = decoded->d;
                        move.source = decoded->b;
                        for (unsigned i = 0; i < length; ++i)
                            move.words[i] = static_cast<u16>(words[i]);
                        move.code_versions = watch_code(core, pc, length);
                        if (!move_kernel) {
                            const auto start = std::chrono::steady_clock::now();
                            move_kernel = compile_move();
                            core.jit_compile_us += static_cast<u64>(std::chrono::duration_cast<
                                std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
                            ++core.jit_compilations;
                        }
                        move.move_native = move_kernel;
                        auto [inserted, fresh] = blocks.emplace(pc, std::move(move));
                        (void)fresh;
                        entry.block = &inserted->second;
                        return execute(core, inserted->second, limit);
                }
                if (decoded && (decoded->length == length || decoded->length == 0) &&
                    (decoded->bkind == 1 || decoded->bkind == 2) &&
                    !decoded->d.discard && decoded->d.width <= 2 &&
                    decoded->a.width <= 2 &&
                    decoded->modes.kernel != VectorKernel::General) {
                    const unsigned key = (static_cast<unsigned>(decoded->modes.kernel) << 8) |
                                         (decoded->a.width << 4) | decoded->d.width |
                                         (static_cast<unsigned>(decoded->a.vertical) << 6) |
                                         (static_cast<unsigned>(decoded->d.vertical) << 7);
                    auto found = immediate_kernels.find(key);
                    if (found == immediate_kernels.end()) {
                        const auto start = std::chrono::steady_clock::now();
                        found = immediate_kernels.emplace(key,
                            compile_immediate(decoded->modes.kernel,
                                decoded->a.width, decoded->d.width,
                                decoded->a.vertical, decoded->d.vertical)).first;
                        core.jit_compile_us += static_cast<u64>(std::chrono::duration_cast<
                            std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
                        ++core.jit_compilations;
                    }
                    Block arithmetic;
                    arithmetic.kind = Block::Kind::Broadcast;
                    arithmetic.pc = pc;
                    arithmetic.length = length;
                    arithmetic.repeats = decoded->count;
                    arithmetic.dynamic_repeats = decoded->length == 0;
                    arithmetic.destination = decoded->d;
                    arithmetic.source = decoded->a;
                    arithmetic.immediate = static_cast<u32>(decoded->immediate);
                    arithmetic.scalar_source = decoded->bkind == 2;
                    arithmetic.scalar_register = decoded->scalar;
                    arithmetic.immediate_native = found->second.native;
                    for (unsigned i = 0; i < length; ++i)
                        arithmetic.words[i] = static_cast<u16>(words[i]);
                    arithmetic.code_versions = watch_code(core, pc, length);
                    auto [inserted, fresh] = blocks.emplace(pc, std::move(arithmetic));
                    (void)fresh;
                    entry.block = &inserted->second;
                    return execute(core, inserted->second, limit);
                }
                const unsigned vector_op = (first_word & 511) >> 3;
                const bool plain_math = vector_op == 4 || vector_op == 8 || vector_op == 9 ||
                    vector_op == 10 || vector_op == 11 || vector_op == 12 ||
                    vector_op == 13 || vector_op == 14 || vector_op == 16 ||
                    vector_op == 17 || vector_op == 24 || vector_op == 26 ||
                    vector_op == 27 || vector_op == 28 || vector_op == 29 ||
                    vector_op == 30 || vector_op == 37 || vector_op == 18 ||
                    vector_op == 19 || vector_op == 20 || vector_op == 21 ||
                    vector_op == 51 || vector_op == 52;
                if (decoded && (decoded->length == length || decoded->length == 0) &&
                    (plain_math || decoded->modes.nibble ||
                     (vector_op == 6 && (decoded->modes.byte_or ||
                         decoded->modes.halfword_or || decoded->modes.nibble_or)) ||
                     (vector_op == 34 && decoded->modes.highcarry) ||
                     (vector_op == 35 && decoded->modes.highborrow)) &&
                    (decoded->sreg == 7 || length == 3 || decoded->sru != 0) &&
                    // The interpreter resolves D after writing a scalar result.
                    // The native wrapper currently resolves D before the call.
                    (decoded->sreg == 7 || decoded->d.reg != static_cast<int>(decoded->sreg)) &&
                    (!decoded->setf || (!decoded->modes.lowcarry && !decoded->modes.lowborrow)) &&
                    (!decoded->x15 || decoded->d.width == 2 ||
                     decoded->modes.blend || decoded->modes.nibble) &&
                    // Immediate SETF shifts include exact shifted-out carry
                    // rules; keep those on the reference path for now.
                    !(decoded->bkind != 0 && decoded->setf && vector_op == 27) &&
                    decoded->d.width <= 2 && decoded->a.width <= 2 &&
                    decoded->b.width <= 2) {
                    const unsigned layout = static_cast<unsigned>(decoded->d.vertical) |
                        (static_cast<unsigned>(decoded->a.vertical) << 1) |
                        (static_cast<unsigned>(decoded->b.vertical) << 2);
                    const unsigned widths = (decoded->d.width - 1) |
                        ((decoded->a.width - 1) << 1) | ((decoded->b.width - 1) << 2);
                    const unsigned modifiers = static_cast<unsigned>(decoded->clear) |
                        (static_cast<unsigned>(decoded->accumulate) << 1) |
                        (static_cast<unsigned>(decoded->x15) << 2) |
                        (static_cast<unsigned>(decoded->setf) << 3) |
                        (static_cast<unsigned>(decoded->modes.blend) << 4) |
                        (static_cast<unsigned>(decoded->pred != 0) << 5) |
                        (static_cast<unsigned>(decoded->modes.highcarry || decoded->modes.highborrow) << 6) |
                        (static_cast<unsigned>(decoded->bkind != 0) << 7) |
                        (static_cast<unsigned>(decoded->modes.nibble) << 8) |
                        (static_cast<unsigned>(decoded->sru) << 9) |
                        (static_cast<unsigned>(decoded->sreg != 7) << 11) |
                        (static_cast<unsigned>(decoded->d.discard) << 12) |
                        (static_cast<unsigned>(length == 3 && words[0] == 0x22d2 &&
                            words[1] == 0x10e0 && words[2] == 0x0601) << 13);
                    const unsigned key = vector_op * 1048576 + modifiers * 64 + widths * 8 + layout;
                    auto found = binary_kernels.find(key);
                    if (found == binary_kernels.end()) {
                        const auto start = std::chrono::steady_clock::now();
                        found = binary_kernels.emplace(key,
                            compile_binary(vector_op, layout, widths, modifiers)).first;
                        core.jit_compile_us += static_cast<u64>(std::chrono::duration_cast<
                            std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
                        ++core.jit_compilations;
                    }
                    Block binary;
                    binary.kind = Block::Kind::BinaryVector;
                    binary.pc = pc;
                    binary.length = length;
                    binary.repeats = decoded->count;
                    binary.dynamic_repeats = decoded->length == 0;
                    binary.predicate = decoded->pred;
                    binary.destination = decoded->d;
                    binary.source = decoded->a;
                    binary.right = decoded->b;
                    binary.broadcast_operand = decoded->bkind != 0;
                    binary.scalar_source = decoded->bkind == 2;
                    binary.scalar_register = decoded->scalar;
                    binary.reduction_register = decoded->sreg;
                    binary.immediate = static_cast<u32>(decoded->immediate);
                    binary.binary_native = found->second.native;
                    for (unsigned i = 0; i < length; ++i)
                        binary.words[i] = static_cast<u16>(words[i]);
                    binary.code_versions = watch_code(core, pc, length);
                    auto [inserted, fresh] = blocks.emplace(pc, std::move(binary));
                    (void)fresh;
                    entry.block = &inserted->second;
                    return execute(core, inserted->second, limit);
                }
                if (decoded && decoded->length == length && decoded->count == 1 &&
                    vector_op == 8 && decoded->setf && !decoded->pred &&
                    !decoded->clear && !decoded->accumulate && !decoded->x15 &&
                    decoded->sreg == 7 && decoded->bkind == 0 &&
                    !decoded->d.discard && !decoded->d.vertical &&
                    !decoded->a.vertical && !decoded->b.vertical &&
                    decoded->d.width == 2 && decoded->a.width == 2 &&
                    decoded->b.width == 2) {
                    const bool lowcarry = decoded->modes.lowcarry &&
                                          decoded->modes.carry_operands;
                    auto &kernel = add_flags_kernels[lowcarry];
                    if (!kernel) {
                        const auto start = std::chrono::steady_clock::now();
                        kernel = compile_add_setf_h16(lowcarry);
                        core.jit_compile_us += static_cast<u64>(std::chrono::duration_cast<
                            std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
                        ++core.jit_compilations;
                    }
                    Block add;
                    add.kind = Block::Kind::AddSetfH16;
                    add.pc = pc;
                    add.length = length;
                    add.destination = decoded->d;
                    add.source = decoded->a;
                    add.right = decoded->b;
                    add.add_flags_native = kernel;
                    for (unsigned i = 0; i < length; ++i)
                        add.words[i] = static_cast<u16>(words[i]);
                    add.code_versions = watch_code(core, pc, length);
                    auto [inserted, fresh] = blocks.emplace(pc, std::move(add));
                    (void)fresh;
                    entry.block = &inserted->second;
                    return execute(core, inserted->second, limit);
                }
            }
            entry.rejection.words[0] = first_word;
            entry.rejection.length = std::min(length, (0x4000000u - pc) / 2);
            for (unsigned i = 1; i < entry.rejection.length; ++i)
                entry.rejection.words[i] = static_cast<u16>(core.read(pc + 2 * i, 2));
            entry.rejection.code_versions = watch_code(core, pc, entry.rejection.length);
            return 0;
        }
        Block block;
        block.pc = pc;
        Rejection rejected_words;
        bool changes_stack = false;
        std::array<bool, 32> written_registers{};
        for (unsigned i = 0; i < std::min(limit, 32u); ++i) {
            const u32 address = pc + 2 * block.length;
            if (address >= 0x4000000) break;
            const u16 word = i ? static_cast<u16>(core.read(address, 2)) : first_word;
            unsigned words = 1;
            auto op = decode(word, address);
            if (!op && address + 4 <= 0x4000000) {
                op = decode_extended(word, static_cast<u16>(core.read(address + 2, 2)), address);
                if (op) words = 2;
            }
            if (!op) {
                if (!i) { rejected_words.words[0] = word; rejected_words.length = 1; }
                break;
            }
            if (block.length + words > block.words.size()) break;
            if (op->memory_load) {
                // Preflight every address against the entry register state.
                // A base changed earlier in the region needs an in-code guard.
                if (written_registers[op->source] ||
                    (op->memory_index != 7 && written_registers[op->memory_index])) break;
                op->memory_slot = block.memory_read_count;
                block.memory_reads[block.memory_read_count++] =
                    {op->source, op->memory_index, op->immediate};
            }
            if (op->stack_load) {
                if (changes_stack) break;
                block.stack_read_bytes = std::max(block.stack_read_bytes, op->immediate + 4);
            } else if (!op->branch && !op->nop && op->destination == 25) {
                if (block.stack_read_bytes) break;
                changes_stack = true;
            }
            op->guest_pc = address;
            block.words[block.length++] = word;
            if (words == 2) block.words[block.length++] = static_cast<u16>(core.read(address + 2, 2));
            block.operations.push_back(*op);
            if (!op->branch && !op->nop)
                written_registers[op->destination] = true;
            if (op->branch) break;
        }
        if (block.operations.empty()) {
            if (rejected_words.length) {
                rejected_words.code_versions = watch_code(core, pc, rejected_words.length);
                entry.rejection = rejected_words;
            }
            return 0;
        }
        const auto compile_start = std::chrono::steady_clock::now();
        block.native = compile(block);
        block.code_versions = watch_code(core, pc, block.length);
        core.jit_compile_us += static_cast<u64>(std::chrono::duration_cast<
            std::chrono::microseconds>(std::chrono::steady_clock::now() - compile_start).count());
        ++core.jit_compilations;
        auto [inserted, fresh] = blocks.emplace(pc, std::move(block));
        (void)fresh;
        entry.block = &inserted->second;
        return execute(core, inserted->second, limit);
    }

    unsigned execute(Core &core, const Block &block, unsigned limit) const {
        Timing *sample = nullptr;
        if (sample_time) {
            sample_state ^= sample_state << 13;
            sample_state ^= sample_state >> 17;
            sample_state ^= sample_state << 5;
            if ((sample_state & 1023) == 0) sample = &native_timing[block.pc];
        }
        SampleTimer timer(sample);
        if (profile) ++block_kind_counts[static_cast<unsigned>(block.kind)];
        if (block.kind == Block::Kind::VectorRegion) {
            if (compare) {
                const auto before = core.vrf;
                const auto regs = core.r;
                const auto acc = core.acc;
                const auto acclo = core.acclo;
                const auto vz = core.vz, vn = core.vn, vc = core.vc;
                for (unsigned i = 0; i < block.vector_pcs.size(); ++i) core.step();
                auto expected_regs = regs;
                expected_regs[31] = block.pc + 2 * block.length;
                if (core.r != expected_regs || core.acc != acc || core.acclo != acclo)
                    throw std::runtime_error("VC2 vector region has unexpected side effects");
                const auto expected = core.vrf;
                const auto expected_vz = core.vz, expected_vn = core.vn, expected_vc = core.vc;
                core.vrf = before;
                core.vz = vz; core.vn = vn; core.vc = vc;
                block.vector_native(core.vrf.data(), regs.data(), core.vz.data(), core.vn.data(), core.vc.data());
                if (core.vrf != expected || core.vz != expected_vz || core.vn != expected_vn ||
                    core.vc != expected_vc || core.r[31] != block.pc + 2 * block.length)
                    throw std::runtime_error("VC2 native vector region mismatch at PC " + std::to_string(block.pc));
            } else {
                block.vector_native(core.vrf.data(), core.r.data(), core.vz.data(), core.vn.data(), core.vc.data());
                for (u32 pc : block.vector_pcs)
                    core.recent_pcs[core.recent_pc_count++ % core.recent_pcs.size()] = pc;
                core.r[31] = block.pc + 2 * block.length;
            }
            return static_cast<unsigned>(block.vector_pcs.size());
        }
        if (block.kind == Block::Kind::IndexedH16) {
            // The native kernel reads ordinary RAM or its 0xfe000000 mirror.
            // MMIO and misaligned reads retain the interpreter's bus behavior.
            for (unsigned lane = 0; lane < 16; ++lane) {
                const u32 index = static_cast<u32>(core.acc[lane]) * 65536u + core.acclo[lane];
                const u32 address = core.r[3] + index * 2u + block.load_offset;
                if (address & 1) return 0;
                if (address > core.ram_.size() - 2 &&
                    (address < 0xfe000000 || address > 0xfe400000 - 2)) return 0;
            }
            const u32 destination = block.destination.offset(core, 0);
            if (compare) {
                const auto original = core.vrf;
                const auto before = core.r;
                core.step();
                const auto expected = core.vrf;
                core.vrf = original;
                block.indexed_native(core.vrf.data(), core.ram_.data(), before[3],
                    core.acc.data(), core.acclo.data(), destination, block.load_offset);
                if (core.vrf != expected || core.r[31] != block.pc + 10)
                    throw std::runtime_error("VC2 indexed H16 native mismatch");
            } else {
                block.indexed_native(core.vrf.data(), core.ram_.data(), core.r[3],
                    core.acc.data(), core.acclo.data(), destination, block.load_offset);
                core.recent_pcs[core.recent_pc_count++ % core.recent_pcs.size()] = block.pc;
                core.r[31] = block.pc + 10;
            }
            return 1;
        }
        if (block.kind == Block::Kind::FillLoop) {
            const u32 address = core.r[block.memory_base];
            const u32 counter = core.r[block.fill_counter];
            const unsigned count = std::min({limit / 2, 16u, counter ? counter : 16u});
            if (!count || (address & 3) || address >= core.ram_.size() ||
                count * 4 > core.ram_.size() - address) return 0;
            for (u32 page = address >> 12; page <= (address + count * 4 - 1) >> 12; ++page)
                if (core.code_page_watched_[page]) return 0;
            if (compare) {
                // Reference execution also validates memory effects and exact PCs.
                const auto before = core.r;
                std::array<u8, 64> expected{}, original{};
                std::copy_n(core.ram_.data() + address, count * 4, original.data());
                for (unsigned i = 0; i < count * 2; ++i) core.step();
                const auto registers = core.r;
                std::copy_n(core.ram_.data() + address, count * 4, expected.data());
                core.r = before;
                std::copy_n(original.data(), count * 4, core.ram_.data() + address);
                block.fill_native(core.r.data(), core.ram_.data() + address, count);
                if (core.r != registers || !std::equal(expected.begin(), expected.begin() + count * 4,
                                                       core.ram_.data() + address))
                    throw std::runtime_error("VC2 native store-loop mismatch");
            } else {
                block.fill_native(core.r.data(), core.ram_.data() + address, count);
                for (unsigned i = 0; i < count; ++i) {
                    core.recent_pcs[core.recent_pc_count++ % core.recent_pcs.size()] = block.pc;
                    core.recent_pcs[core.recent_pc_count++ % core.recent_pcs.size()] = block.pc + 2;
                }
            }
            return count * 2;
        }
        if (block.kind == Block::Kind::TableBranch) {
            core.recent_pcs[core.recent_pc_count++ % core.recent_pcs.size()] = block.pc;
            const u32 address = block.native_address(core.r.data());
            const u32 value = core.read(address, block.memory_width);
            core.r[31] = block.pc + 2 + 2u * static_cast<u32>(
                sign_extend(value, block.memory_width * 8));
            return 1;
        }
        if (block.kind == Block::Kind::RasterStore) {
            core.recent_pcs[core.recent_pc_count++ % core.recent_pcs.size()] = block.pc;
            std::array<u8, 16> bytes{};
            block.gather_bytes_native(core.vrf.data(),
                block.source.offset(core, 0), bytes.data());
            const u32 address = core.r[0] + block.load_offset;
            if (!core.write_ram16(address, bytes))
                for (unsigned lane = 0; lane < 16; ++lane)
                    core.write(address + lane, bytes[lane], 1);
            core.r[31] = block.pc + 6;
            return 1;
        }
        if (block.kind == Block::Kind::VectorLoad) {
            core.recent_pcs[core.recent_pc_count++ % core.recent_pcs.size()] = block.pc;
            const unsigned count = block.dynamic_repeats ? core.r[0] : block.repeats;
            if (count > 65536)
                throw std::runtime_error("VC2 vector repeat limit");
            u32 address = (block.load_base == 7 ? 0 : core.r[block.load_base]) +
                          block.load_offset;
            for (unsigned repeat = 0; repeat < count; ++repeat) {
                std::array<u8, 16> bytes{};
                const u32 aligned = address & ~15u;
                if (!core.read_ram16(aligned, bytes))
                    for (unsigned lane = 0; lane < 16; ++lane)
                        bytes[lane] = static_cast<u8>(core.read(aligned + lane, 1));
                block.load_bytes_native(core.vrf.data(),
                    block.destination.offset(core, repeat), bytes.data());
                if (block.load_stride != 7)
                    address += core.r[block.load_stride];
            }
            core.r[31] = block.pc + 6;
            return 1;
        }
        if (block.kind == Block::Kind::Stack) {
            core.recent_pcs[core.recent_pc_count++ % core.recent_pcs.size()] = block.pc;
            const u32 sp = block.native_address(core.r.data());
            u32 next = block.pc + 2;
            for (unsigned i = 0; i < block.stack_total; ++i) {
                const unsigned reg = i < block.stack_count
                    ? (block.stack_base + block.stack_count - 1 - i) & 31
                    : (block.stack_push ? 26 : 31);
                if (block.stack_push)
                    core.write(sp + 4 * i, core.r[reg]);
                else {
                    const u32 value = core.read(sp + 4 * i);
                    if (reg == 31) next = value;
                    else core.r[reg] = value;
                }
            }
            core.r[25] = block.stack_push ? sp : sp + 4 * block.stack_total;
            core.r[31] = next;
            return 1;
        }
        if (block.kind == Block::Kind::Memory) {
            const u32 address = block.native_address(core.r.data());
            const unsigned width = block.memory_width;
            if (address & (width - 1))
                return 0; // The interpreter handles measured delayed faults.
            core.recent_pcs[core.recent_pc_count++ % core.recent_pcs.size()] = block.pc;
            const unsigned op = block.memory_op, d = block.memory_register;
            if (op == 1 || op == 3 || op == 5)
                core.write(address, core.r[d], width);
            else {
                u32 value = core.read(address, width);
                if (op >= 6)
                    value = static_cast<u32>(sign_extend(value, width * 8));
                core.r[d] = value;
            }
            const unsigned source = block.memory_base;
            if (!block.memory_indexed)
                core.r[source] = block.memory_pre ? address : core.r[source] + width;
            core.r[31] = block.pc + 2 * block.length;
            return 1;
        }
        if (block.kind != Block::Kind::Scalar) {
            auto run_vector = [&] {
                core.recent_pcs[core.recent_pc_count++ % core.recent_pcs.size()] = block.pc;
                const unsigned repeats = block.dynamic_repeats ? core.r[0] : block.repeats;
                if (repeats > 65536)
                    throw std::runtime_error("VC2 vector repeat limit");
                for (unsigned repeat = 0; repeat < repeats; ++repeat) {
                    // Selection uses the flags at the start of this repeat.
                    // SETF can change which lanes the next repeat selects.
                    u32 selected = block.predicate ? 0 : 65535;
                    if (block.kind == Block::Kind::BinaryVector && block.predicate > 1) {
                        for (unsigned lane = 0; lane < 16; ++lane) {
                            const unsigned pred = block.predicate;
                            const bool flag = pred < 4 ? core.vz[lane] :
                                              pred < 6 ? core.vn[lane] : core.vc[lane];
                            if (flag == ((pred & 1) == 0)) selected |= 1u << lane;
                        }
                    }
                    const u32 source = block.source.offset(core, repeat);
                    const u32 destination = block.destination.offset(core, repeat);
                    if (block.kind == Block::Kind::MoveH16)
                        block.move_native(core.vrf.data(), source, destination);
                    else if (block.kind == Block::Kind::Broadcast) {
                        u32 value = block.immediate;
                        if (block.scalar_source) {
                            const u32 scalar = block.scalar_register == 7
                                ? 0 : core.r[block.scalar_register];
                            value = static_cast<u32>(sign_extend(scalar + value, 16));
                        }
                        block.immediate_native(core.vrf.data(), destination, source, value);
                    }
                    else if (block.kind == Block::Kind::BinaryVector) {
                        u32 right;
                        if (block.broadcast_operand) {
                            right = block.immediate;
                            if (block.scalar_source) {
                                const u32 scalar = block.scalar_register == 7
                                    ? 0 : core.r[block.scalar_register];
                                right = static_cast<u32>(sign_extend(scalar + right, 16));
                            }
                        } else {
                            right = block.right.offset(core, repeat);
                        }
                        block.binary_native(core.vrf.data(), destination, source,
                                            right, selected, core.acc.data(), core.acclo.data(),
                                            core.vz.data(), core.vn.data(), core.vc.data(),
                                            core.r.data(), block.reduction_register);
                    }
                    else
                        block.add_flags_native(core.vrf.data(), destination, source,
                            block.right.offset(core, repeat), core.vz.data(), core.vn.data(), core.vc.data());
                }
                core.r[31] = block.pc + 2 * block.length;
            };
            if (compare) {
                const auto before_regs = core.r;
                const auto before_pcs = core.recent_pcs;
                const u64 before_count = core.recent_pc_count;
                const auto before_vrf = core.vrf;
                const auto before_acc = core.acc;
                const auto before_acclo = core.acclo;
                const auto before_vz = core.vz, before_vn = core.vn, before_vc = core.vc;
                core.step();
                const auto expected_regs = core.r;
                const auto expected_pcs = core.recent_pcs;
                const auto expected_vrf = core.vrf;
                const auto expected_acc = core.acc;
                const auto expected_acclo = core.acclo;
                const auto expected_vz = core.vz, expected_vn = core.vn, expected_vc = core.vc;
                const u64 expected_count = core.recent_pc_count;
                core.r = before_regs;
                core.recent_pcs = before_pcs;
                core.recent_pc_count = before_count;
                core.vrf = before_vrf;
                core.acc = before_acc; core.acclo = before_acclo;
                core.vz = before_vz; core.vn = before_vn; core.vc = before_vc;
                run_vector();
                if (core.r != expected_regs || core.recent_pcs != expected_pcs ||
                    core.recent_pc_count != expected_count || core.vrf != expected_vrf ||
                    core.acc != expected_acc || core.acclo != expected_acclo ||
                    core.vz != expected_vz || core.vn != expected_vn || core.vc != expected_vc) {
                    for (unsigned i = 0; i < core.vrf.size(); ++i)
                        if (core.vrf[i] != expected_vrf[i])
                            throw std::runtime_error("VC2 JIT vector VRF mismatch at PC " +
                                std::to_string(block.pc) + " byte " + std::to_string(i) +
                                " expected " + std::to_string(expected_vrf[i]) +
                                " got " + std::to_string(core.vrf[i]));
                    for (unsigned i = 0; i < 16; ++i)
                        if (core.vz[i] != expected_vz[i] || core.vn[i] != expected_vn[i] ||
                            core.vc[i] != expected_vc[i])
                            throw std::runtime_error("VC2 JIT vector flag mismatch at PC " +
                                std::to_string(block.pc) + " words " + std::to_string(block.words[0]) + "/" +
                                std::to_string(block.words[1]) + "/" + std::to_string(block.words[2]) +
                                " lane " + std::to_string(i) +
                                " expected " + std::to_string(expected_vz[i]) + "/" +
                                std::to_string(expected_vn[i]) + "/" +
                                std::to_string(expected_vc[i]) + " got " +
                                std::to_string(core.vz[i]) + "/" +
                                std::to_string(core.vn[i]) + "/" +
                                std::to_string(core.vc[i]));
                    throw std::runtime_error("VC2 JIT vector state mismatch at PC " +
                                             std::to_string(block.pc));
                }
            } else {
                run_vector();
            }
            return 1;
        }
        std::array<u32, 32> memory_offsets;
        for (unsigned i = 0; i < block.memory_read_count; ++i) {
            const auto &read = block.memory_reads[i];
            const u32 address = core.r[read.base] + read.offset +
                (read.index == 7 ? 0 : 4 * core.r[read.index]);
            if ((address & 3) || (address > core.ram_.size() - 4 &&
                (address < 0xfe000000 || address > 0xfe400000 - 4))) return 0;
            memory_offsets[i] = address >= 0xfe000000
                ? address - 0xfe000000 + 0x2000000 : address;
        }
        if (block.stack_read_bytes) {
            const u32 sp = core.r[25];
            if ((sp & 3) || sp >= core.ram_.size() ||
                block.stack_read_bytes > core.ram_.size() - sp) return 0;
        }
        if (compare) {
            const auto before_regs = core.r;
            const auto before_pcs = core.recent_pcs;
            const u64 before_count = core.recent_pc_count;
            for (unsigned i = 0; i < block.operations.size(); ++i)
                core.step();
            const auto expected_regs = core.r;
            const auto expected_pcs = core.recent_pcs;
            const u64 expected_count = core.recent_pc_count;
            core.r = before_regs;
            core.recent_pcs = before_pcs;
            core.recent_pc_count = before_count;
            for (unsigned i = 0; i < block.operations.size(); ++i)
                core.recent_pcs[core.recent_pc_count++ % core.recent_pcs.size()] =
                    block.operations[i].guest_pc ? block.operations[i].guest_pc : block.pc + 2 * i;
            block.native(core.r.data(), core.ram_.data(), memory_offsets.data());
            if (core.r != expected_regs || core.recent_pcs != expected_pcs ||
                core.recent_pc_count != expected_count)
                throw std::runtime_error("VC2 JIT/interpreter mismatch at PC " +
                                         std::to_string(block.pc));
        } else {
            for (unsigned i = 0; i < block.operations.size(); ++i)
                core.recent_pcs[core.recent_pc_count++ % core.recent_pcs.size()] =
                    block.operations[i].guest_pc ? block.operations[i].guest_pc : block.pc + 2 * i;
            block.native(core.r.data(), core.ram_.data(), memory_offsets.data());
        }
        return static_cast<unsigned>(block.operations.size());
    }
};

Jit::Jit() : impl_(std::make_unique<Impl>()) {}
Jit::~Jit() { impl_->report(); }
std::size_t Jit::loaded_object_count() const { return impl_->loaded_objects.size(); }
unsigned Jit::try_execute_cached(Core &core, unsigned limit) {
    const u32 pc = core.r[31];
    if (!limit || pc >= 0x4000000 || (pc & 1)) return 0;
    const auto &page = impl_->pages[pc >> 12];
    if (!page) return 0;
    const Block *block = page->entries[(pc & 4095) >> 1].block;
    if (!block || (block->kind == Block::Kind::VectorRegion && block->vector_pcs.size() > limit) ||
        (block->kind == Block::Kind::Scalar && block->operations.size() > limit)) return 0;
    const unsigned length = block->kind == Block::Kind::Scalar && !block->length
        ? static_cast<unsigned>(block->operations.size()) : block->length;
    if (!Impl::code_unchanged(core, pc, length, block->code_versions)) return 0;
    const unsigned executed = impl_->execute(core, *block, limit);
    if (!executed) return 0;
    if (impl_->profile) {
        const u16 first = block->kind == Block::Kind::Scalar && !block->length
            ? block->operations.front().word : block->words[0];
        impl_->record(pc, first, executed);
    }
    return continue_vectors(core, limit, pc, executed);
}
unsigned Jit::try_execute(Core &core, unsigned limit, u16 first_word) {
    const u32 pc = core.r[31];
    unsigned executed = impl_->try_execute(core, limit, first_word);
    core.jit_compile_cpu = impl_->compile_cpu;
    impl_->record(pc, first_word, executed);
    return continue_vectors(core, limit, pc, executed);
}
unsigned Jit::continue_vectors(Core &core, unsigned limit, u32 pc, unsigned executed) {
    if (!executed || pc >= 0x4000000) return executed;
    const auto &page = impl_->pages[pc >> 12];
    const Block *block = page->entries[(pc & 4095) >> 1].block;
    if (!block || !Impl::vector_block(block->kind)) return executed;
    u32 expected_pc = pc + 2 * block->length;
    while (executed < limit) {
        const unsigned next = impl_->execute_cached_vector(core, limit - executed,
                                                            expected_pc);
        if (!next) break;
        executed += next;
        const auto &next_page = impl_->pages[expected_pc >> 12];
        block = next_page->entries[(expected_pc & 4095) >> 1].block;
        expected_pc += 2 * block->length;
    }
    return executed;
}

} // namespace clickhle::vc2

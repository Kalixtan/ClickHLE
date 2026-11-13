
#pragma once
#include "clickhle/common/types.h"
#include "vector_plan.h"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace clickhle::vc2 {
struct DmaTransfer {
    u32 block = 0, next = 0, source = 0, destination = 0, unit = 0;
    u32 rows = 0, columns = 0, row = 0, column = 0, line = 0;
    i32 source_skip = 0, destination_skip = 0;
    bool paced = false, increment = false, active = false;
    bool operator==(const DmaTransfer &) const = default;
};
struct DmaDescriptor {
    std::array<u32, 6> words;
    bool operator==(const DmaDescriptor &) const = default;
};
class Jit;

class Core {
  public:
    Core();
    // Offline replacement validation; copies device state without sharing JIT code.
    std::unique_ptr<Core> fork_reference() const;
    std::string state_difference(const Core &other) const;
    void load(std::span<const u8> image);
    // The instruction budget is a soft limit checked at function returns.
    // Sleep and unsupported instructions stop immediately; hardware service
    // points remain independent of the budget.
    bool run(u64 budget = 2'000'000);
    void set_jit_enabled(bool enabled) noexcept { jit_enabled_ = enabled; }
    void set_g5_enabled(bool enabled) noexcept { g5_enabled_ = enabled; }
    unsigned run_replacement(unsigned budget);
    void step(std::optional<u16> prefetched = std::nullopt);
    void advance(u32 microseconds);
    void set_gpio_input(unsigned pin, bool high);
    void progress_dma();
    bool dma_busy() const;
    bool interrupt(u32 return_pc);
    u32 read(u32 address, unsigned width = 4);
    void write(u32 address, u32 value, unsigned width = 4);
    void copy_in(u32 address, std::span<const u8> bytes);
    std::vector<u8> copy_out(u32 address, u32 size) const;
    bool read_ram16(u32 address, std::array<u8, 16> &bytes) const noexcept;
    bool write_ram16(u32 address, const std::array<u8, 16> &bytes) noexcept;
    std::array<u32, 128> recent_pcs{};
    u64 recent_pc_count = 0;
    std::array<u32, 32> r{};
    std::array<u8, 4096> vrf{};
    std::array<std::int64_t, 16> acc{};
    std::array<u32, 16> acclo{};
    std::array<bool, 16> vz{}, vn{}, vc{};
    std::vector<DmaDescriptor> descriptors;
    bool sleeping = false;
    std::string error;
    u64 instructions = 0, dma_words = 0;
    u64 replacement_instructions = 0;
    u64 jit_instructions = 0;
    u64 jit_executions = 0;
    u64 jit_compilations = 0, jit_compile_us = 0;
    double jit_compile_cpu = 0;
    double dma_cpu_seconds = 0;
    u64 dma_passes = 0;
    u32 timer = 0;

  private:
    friend class Jit;
    static constexpr unsigned vector_cache_size = 1024;
    std::vector<VectorPlan> vector_cache_ = std::vector<VectorPlan>(vector_cache_size);
    std::shared_ptr<Jit> jit_;
    bool jit_enabled_ = false;
    bool g5_enabled_ = false;
    u32 memcpy_entry_ = ~0u;
    u64 memcpy_first_generation_ = 0, memcpy_last_generation_ = 0;
    std::vector<u8> ram_;
    // A compiled instruction is valid until a write touches one of its pages.
    // Unwatched data pages do not pay for generation updates.
    std::vector<u64> code_page_generation_ = std::vector<u64>(0x4000000 / 4096);
    std::vector<u8> code_page_watched_ = std::vector<u8>(0x4000000 / 4096);
    std::map<u32, u32> registers_, reads_;
    // Measured misaligned scalar accesses finish, then enter vector 1 after
    // the instruction window. Counts include the faulting instruction.
    unsigned alignment_exception_steps_ = 0;
    std::array<u32, 4> compare_{};
    std::array<bool, 4> armed_{};
    u32 timer_status_ = 0, level_ = 0, dma_time_ = 0, time_fraction_ = 0;
    std::array<u32, 32> priorities_{};
    std::array<u32, 8> dma_config_{}, dma_status_{};
    std::array<bool, 8> dma_finished_{};
    std::array<std::array<DmaTransfer, 3>, 8> dma_{};
    bool progressing_ = false;
    u32 ram_address(u32 address, u32 length) const;
    u32 alu(unsigned op, u32 a, u32 b);
    void vector(const std::array<u32, 5> &words, unsigned length);
    VectorPlan decode_vector(const std::array<u32, 5> &words, unsigned length) const;
    void execute_vector(const std::array<u32, 5> &words, unsigned length,
                        const VectorPlan &decoded);
    void load_dma(unsigned channel, unsigned sub, u32 address);
    unsigned pending_source();
};
i32 sign_extend(u32 value, unsigned bits);
u32 subtract_flags(u32 a, u32 b);
bool condition(unsigned code, u32 flags);
} // namespace clickhle::vc2

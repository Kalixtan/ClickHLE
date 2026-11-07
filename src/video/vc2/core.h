
#pragma once
#include "clickhle/common/types.h"
#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace clickhle::vc2 {
struct DmaTransfer {
    u32 block = 0, next = 0, source = 0, destination = 0, unit = 0;
    u32 rows = 0, columns = 0, row = 0, column = 0, line = 0;
    i32 source_skip = 0, destination_skip = 0;
    bool paced = false, increment = false, active = false;
};
struct DmaDescriptor {
    std::array<u32, 6> words;
};
class Core {
  public:
    Core();
    void load(std::span<const u8> image);
    // Stops at sleep, an unsupported instruction, or the instruction budget.
    bool run(u64 budget = 2'000'000);
    void step();
    void advance(u32 microseconds);
    void set_gpio_input(unsigned pin, bool high);
    void progress_dma();
    bool dma_busy() const;
    bool interrupt(u32 return_pc);
    u32 read(u32 address, unsigned width = 4);
    void write(u32 address, u32 value, unsigned width = 4);
    void copy_in(u32 address, std::span<const u8> bytes);
    std::vector<u8> copy_out(u32 address, u32 size) const;
    std::array<u32, 128> recent_pcs{};
    u64 recent_pc_count = 0;
    std::array<u32, 32> r{};
    std::array<u8, 4096> vrf{};
    std::array<std::int64_t, 16> acc{};
    std::array<u32, 16> acclo{};
    std::array<bool, 16> vz{}, vn{}, vc{};
    // The halfword SETF load has established Z/N consumers, but no C rule.
    // Keep that uncertainty until an instruction replaces the lane flags.
    std::array<bool, 16> unknown_carry{};
    std::vector<DmaDescriptor> descriptors;
    bool sleeping = false;
    std::string error;
    u64 instructions = 0, dma_words = 0;
    u32 timer = 0;

  private:
    std::vector<u8> ram_;
    std::map<u32, u32> registers_, reads_;
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
    void load_dma(unsigned channel, unsigned sub, u32 address);
    unsigned pending_source();
};
i32 sign_extend(u32 value, unsigned bits);
u32 subtract_flags(u32 a, u32 b);
bool condition(unsigned code, u32 flags);
} // namespace clickhle::vc2

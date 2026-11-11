
#include "core.h"
#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <stdexcept>

namespace clickhle::vc2 {
u32 Core::read(u32 address, unsigned width) {
    if (width != 1 && width != 2 && width != 4)
        throw std::runtime_error("Invalid VC2 bus width");
    if (address & (width - 1))
        throw std::runtime_error("Unaligned VC2 bus read");
    if (address < ram_.size() || (address >= 0xfe000000 && address < 0xfe400000)) {
        const u32 a = ram_address(address, width);
        u32 value = ram_[a];
        if (width >= 2)
            value |= static_cast<u32>(ram_[a + 1]) << 8;
        if (width == 4)
            value |= (static_cast<u32>(ram_[a + 2]) << 16) |
                     (static_cast<u32>(ram_[a + 3]) << 24);
        return value;
    }
    u32 v = registers_[address], seen = reads_[address]++;
    // Explicit ready-bit assumptions inherited from the standalone testbed.
    if (address == 0x10000c00) {
        if (seen >= 1)
            v |= 1;
    } else if (address == 0x10003420) {
        if (seen >= 1)
            v |= 0x8000;
    } else if (address == 0x10001c14) {
        if (seen >= 1)
            v |= 0x1000;
    } else if (address >= 0x10001800 && address < 0x10001820) {
        unsigned off = address - 0x10001800;
        if (off == 4) {
            // Poll-progress fixture; the 1 MHz software clock is firmware-backed,
            // but elapsed time per read is not a measured hardware behavior.
            advance(1);
            v = timer;
        } else if (!off)
            v = timer_status_;
        else if (off >= 8 && off <= 20)
            v = compare_[(off - 8) / 4];
    } else if (address == 0x10001004) {
        unsigned source = pending_source();
        v = source < 32 ? (priorities_[source] << 8) | (32 + source) : 0;
    } else if (address >= 0x10000800 && address < 0x10000c00) {
        progress_dma();
        unsigned ch = (address - 0x10000800) >> 7, off = address & 127;
        if (!off) {
            v = dma_config_[ch] & 0x1fffffff;
            bool busy = false;
            for (const auto &transfer : dma_[ch])
                busy |= transfer.active;
            if (busy)
                v |= 1u << 29;
            else if (dma_finished_[ch] && (v & 3))
                v |= 2u << 29;
            else
                v = dma_config_[ch];
        } else if (off == 4)
            v = dma_status_[ch];
        else if (off >= 0x38 && (off - 0x38) % 0x20 == 0) {
            const auto &t = dma_[ch][(off - 0x38) / 0x20];
            v = t.active ? t.block : 0;
        }
    }
    return width == 4 ? v : v & ((1u << (8 * width)) - 1);
}
void Core::write(u32 address, u32 value, unsigned width) {
    if (width != 1 && width != 2 && width != 4)
        throw std::runtime_error("Invalid VC2 bus width");
    if (address & (width - 1))
        throw std::runtime_error("Unaligned VC2 bus write");
    if (address < ram_.size() || (address >= 0xfe000000 && address < 0xfe400000)) {
        const u32 a = ram_address(address, width);
        if (code_page_watched_[a >> 12])
            ++code_page_generation_[a >> 12];
        ram_[a] = static_cast<u8>(value);
        if (width >= 2)
            ram_[a + 1] = static_cast<u8>(value >> 8);
        if (width == 4) {
            ram_[a + 2] = static_cast<u8>(value >> 16);
            ram_[a + 3] = static_cast<u8>(value >> 24);
        }
        return;
    }
    if (width < 4)
        value &= (1u << (8 * width)) - 1;
    // GPIO event status is acknowledged by writing ones, not a latch value.
    if (address == 0x10002828 || address == 0x1000282c) {
        registers_[address] &= ~value;
        return;
    }
    registers_[address] = value;
    reads_[address] = 0;
    if (address >= 0x10001800 && address < 0x10001820) {
        unsigned off = address - 0x10001800;
        if (!off)
            timer_status_ &= ~value;
        else if (off >= 8 && off <= 20) {
            unsigned k = (off - 8) / 4;
            compare_[k] = value;
            armed_[k] = true;
        }
    } else if (address == 0x10001000)
        level_ = value & 7;
    else if (address >= 0x10001010 && address <= 0x1000101c) {
        unsigned start = (address - 0x10001010) * 2;
        for (unsigned i = 0; i < 8; ++i)
            priorities_[start + i] = (value >> (4 * i)) & 15;
    } else if (address >= 0x10000800 && address < 0x10000c00) {
        progress_dma();
        unsigned ch = (address - 0x10000800) >> 7, off = address & 127;
        if (!off) {
            dma_config_[ch] = value;
            dma_finished_[ch] = false;
            if (!(value & 3))
                for (auto &t : dma_[ch])
                    t.active = false;
        } else if (off == 4)
            dma_status_[ch] &= ~value;
        else if (off >= 0x38 && (off - 0x38) % 0x20 == 0) {
            if (!(dma_config_[ch] & 3))
                throw std::runtime_error("DMA start on disabled channel");
            load_dma(ch, (off - 0x38) / 0x20, value);
        }
    }
}
void Core::set_gpio_input(unsigned pin, bool high) {
    if (pin >= 52)
        throw std::runtime_error("Invalid VC2 GPIO input");
    const unsigned shift = 2 * (pin % 16);
    if ((registers_[0x10002800 + 4 * (pin / 16)] >> shift) & 3)
        return; // An external input does not drive an output/alternate pin.
    const u32 bank = 4 * (pin / 32), mask = 1u << (pin % 32);
    auto &level = registers_[0x10002820 + bank];
    const bool previous = (level & mask) != 0;
    if (high)
        level |= mask;
    else
        level &= ~mask;
    if (previous != high && (registers_[(high ? 0x10002830 : 0x10002838) + bank] & mask))
        registers_[0x10002828 + bank] |= mask;
}
void Core::advance(u32 us) {
    timer += us;
    for (unsigned k = 0; k < 4; ++k)
        if (armed_[k] && timer - compare_[k] < 0x80000000) {
            timer_status_ |= 1u << k;
            armed_[k] = false;
        }
}
unsigned Core::pending_source() {
    progress_dma();
    unsigned best = 32;
    for (unsigned k = 0; k < 32; ++k) {
        bool active =
            k < 8 ? dma_status_[k] != 0
                  : k == 11 ? (registers_[0x1000282c] & (1u << 17)) != 0
                            : k >= 16 && k < 20 && (timer_status_ & (1u << (k - 16)));
        if (active && priorities_[k] > level_ && (best == 32 || priorities_[k] > priorities_[best]))
            best = k;
    }
    return best;
}
bool Core::interrupt(u32 return_pc) {
    if (!(r[30] & (1u << 30)))
        return false;
    unsigned source = pending_source();
    if (source == 32)
        return false;
    u32 sp = r[25] - 8;
    write(sp, r[30]);
    write(sp + 4, return_pc);
    r[25] = sp;
    r[30] &= ~(1u << 30);
    r[31] = read(4 * (32 + source));
    return true;
}
bool Core::dma_busy() const {
    for (const auto &channel : dma_)
        for (const auto &transfer : channel)
            if (transfer.active)
                return true;
    return false;
}
void Core::load_dma(unsigned channel, unsigned sub, u32 address) {
    if (address & 31)
        throw std::runtime_error("Unaligned DMA descriptor");
    DmaDescriptor descriptor{};
    for (unsigned i = 0; i < 6; ++i)
        descriptor.words[i] = read(address + 4 * i);
    descriptors.push_back(descriptor);
    const auto &f = descriptor.words;
    auto &t = dma_[channel][sub];
    t = {};
    t.active = true;
    t.block = address;
    t.next = f[0];
    t.source = f[1];
    t.destination = f[2];
    unsigned sw = (f[4] >> 4) & 7, dw = (f[4] >> 12) & 7;
    t.unit = 1u << std::min(sw, dw);
    if (t.unit == 8 || t.unit == 16) {
        // Firmware-derived: RenderServer uses these widths for aligned RAM
        // uploads. Hardware fixtures do not yet cover their alignment quirks,
        // 2-D strides, or peripheral behavior; keep those forms explicit.
        const auto ram = [](u32 a) { return a < 0x4000000 || (a >= 0xfe000000 && a < 0xfe400000); };
        if (sw > 5 || dw > 5 || !ram(t.source) || !ram(t.destination) ||
            (t.source & ((1u << sw) - 1)) || (t.destination & ((1u << dw) - 1)) ||
            (f[4] & 0x18000) || !(f[4] & 0x20000))
            throw std::runtime_error("Unverified wide DMA transfer form");
    } else if (t.unit != 1 && t.unit != 2 && t.unit != 4 && t.unit != 32) {
        throw std::runtime_error("Unsupported DMA transfer width");
    }
    t.rows = (f[4] & 0x10000) ? f[3] >> 16 : 1;
    t.columns = (f[4] & 0x10000) ? f[3] & 65535 : f[3];
    if (!t.rows || !t.columns)
        throw std::runtime_error("Empty DMA descriptor");
    t.source_skip = sign_extend(f[5] & 65535, 16) * static_cast<i32>(t.unit);
    t.destination_skip = sign_extend(f[5] >> 16, 16) * static_cast<i32>(t.unit);
    t.paced = f[4] & 0x8000;
    t.increment = f[4] & 0x20000;
    t.line = (dma_config_[channel] >> 16) & 31;
    if (t.unit == 32)
        t.destination &= ~31u;
}
void Core::progress_dma() {
    if (progressing_)
        return;
    const u32 delta = timer - dma_time_;
    dma_time_ = timer;
    if (!delta || !dma_busy())
        return;
    static const bool timing = std::getenv("CLICKHLE_VC2_TIMING") != nullptr;
    const std::clock_t started = timing ? std::clock() : 0;
    ++dma_passes;
    progressing_ = true;
    try {
        for (unsigned ch = 0; ch < 8; ++ch)
            for (unsigned k = 0; k < 3; ++k) {
                auto &t = dma_[ch][k];
                // Testbed throughput, not measured bus rates or LCD pacing.
                u64 budget = static_cast<u64>(delta) * (t.paced ? 5 : 64);
                unsigned cost = std::max(1u, t.unit / 4);
                while (t.active && budget >= cost) {
                    unsigned width = std::min(t.unit, 4u);
                    for (unsigned i = 0; i < t.unit; i += width) {
                        u32 v = read(t.source + i, width);
                        write(t.destination + (t.increment ? i : 0), v, width);
                        ++dma_words;
                    }
                    t.source += t.unit;
                    if (t.increment)
                        t.destination += t.unit;
                    ++t.column;
                    budget -= cost;
                    if (t.column == t.columns) {
                        t.column = 0;
                        ++t.row;
                        t.source += static_cast<u32>(t.source_skip);
                        if (t.increment)
                            t.destination += static_cast<u32>(t.destination_skip);
                        if (t.row >= t.rows) {
                            if (t.next)
                                load_dma(ch, k, t.next);
                            else {
                                t.active = false;
                                dma_finished_[ch] = true;
                                dma_status_[ch] |= 1u << (13 + k);
                                if (t.paced && t.line == 3)
                                    dma_status_[ch] |= 0x300;
                            }
                        }
                    }
                }
            }
    } catch (...) {
        progressing_ = false;
        throw;
    }
    progressing_ = false;
    if (timing)
        dma_cpu_seconds += static_cast<double>(std::clock() - started) / CLOCKS_PER_SEC;
}
} // namespace clickhle::vc2

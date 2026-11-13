#include "../vc2/core.h"
#include <algorithm>
#include <array>

namespace clickhle::vc2 {
namespace {
// Complete position-independent memcpy routine shared by RenderServer and
// the named VMCS export. Match instructions, never a game or firmware address.
constexpr std::array<u16, 33> copy_code{
    0xf0a0, 0x2ea4, 0x3c01, 0xce43, 0x6223, 0x0103, 0xc00a,
    0xce06, 0x0b02, 0xc40a, 0x7813, 0x7963, 0x0ac2, 0xc4fe,
    0x3e04, 0x6004, 0x3009, 0xce06, 0x0d02, 0xc006, 0x7c15,
    0x7d65, 0x0cf2, 0xc0fe, 0xe0a0, 0xce06, 0x0b02, 0xc2f8,
    0x7e14, 0x7b64, 0x0ae2, 0xc2fe, 0x3e72};
constexpr u32 code_size = copy_code.size() * 2;
}

unsigned Core::run_replacement(unsigned budget) {
    if (!budget || alignment_exception_steps_) return 0;
    auto matches = [&](u32 address) {
        if (address > ram_.size() - code_size || (address & 1)) return false;
        for (unsigned i = 0; i < copy_code.size(); ++i)
            if ((ram_[address + 2 * i] | (u16(ram_[address + 2 * i + 1]) << 8)) != copy_code[i])
                return false;
        return true;
    };
    if (memcpy_entry_ == ~0u || r[31] - memcpy_entry_ >= code_size) {
        if (!matches(r[31])) return 0;
        memcpy_entry_ = r[31];
        code_page_watched_[memcpy_entry_ >> 12] = 1;
        code_page_watched_[(memcpy_entry_ + code_size - 1) >> 12] = 1;
        memcpy_first_generation_ = code_page_generation_[memcpy_entry_ >> 12];
        memcpy_last_generation_ = code_page_generation_[(memcpy_entry_ + code_size - 1) >> 12];
    } else {
        const auto first = code_page_generation_[memcpy_entry_ >> 12];
        const auto last = code_page_generation_[(memcpy_entry_ + code_size - 1) >> 12];
        if (first != memcpy_first_generation_ || last != memcpy_last_generation_) {
            if (!matches(memcpy_entry_)) {
                memcpy_entry_ = ~0u;
                return 0;
            }
            memcpy_first_generation_ = first;
            memcpy_last_generation_ = last;
        }
    }
    const u32 entry = memcpy_entry_;
    auto readable = [&](u32 address, u32 size) {
        return address <= ram_.size() && size <= ram_.size() - address;
    };
    auto writable = [&](u32 address, u32 size) {
        return readable(address, size) &&
            (u64(address) + size <= entry || address >= entry + code_size);
    };
    auto record = [&](u32 pc) { recent_pcs[recent_pc_count++ % recent_pcs.size()] = pc; };
    unsigned executed = 0;
    while (executed < budget) {
        const u32 pc = r[31], offset = pc - entry;
        // Collapse whole load/store/backedge iterations. Preserve the final
        // loaded value and the firmware's forward-copy overlap behaviour.
        if ((offset == 20 || offset == 40 || offset == 56) && budget - executed >= 3) {
            const unsigned width = offset == 20 ? 4 : offset == 40 ? 1 : 2;
            const unsigned reg = offset == 20 ? 3 : offset == 40 ? 5 : 4;
            const u32 count = std::min((budget - executed) / 3,
                static_cast<i32>(r[2]) > 0 ? r[2] / width : 0u);
            const u32 bytes = count * width, source = r[1], destination = r[6];
            if (count && !((source | destination) & (width - 1)) &&
                readable(source, bytes) && writable(destination, bytes)) {
                u32 last = 0;
                if (u64(source) + bytes <= destination || u64(destination) + bytes <= source) {
                    last = read(source + bytes - width, width);
                    copy_in(destination, std::span<const u8>(ram_.data() + source, bytes));
                } else {
                    for (u32 i = 0; i < bytes; i += width) {
                        last = read(source + i, width);
                        write(destination + i, last, width);
                    }
                }
                r[reg] = width == 2 ? static_cast<u32>(sign_extend(last, 16)) : last;
                r[1] += bytes;
                r[6] += bytes;
                r[2] -= bytes;
                for (u32 i = 0; i < count; ++i) {
                    record(pc); record(pc + 2); record(pc + 4);
                }
                r[31] = static_cast<i32>(r[2]) >= static_cast<i32>(width) ? pc : pc + 8;
                executed += count * 3;
                continue;
            }
        }
        u32 next = pc + 2;
        switch (offset) {
        case 0:
            if ((r[25] & 3) || !writable(r[25] - 8, 8)) return executed;
            write(r[25] - 8, r[6]); write(r[25] - 4, r[26]); r[25] -= 8;
            break;
        case 2: r[4] = r[1] | r[0]; next = pc + 4; break;
        case 6: r[3] = r[4]; break;
        case 8: r[3] &= 3; break;
        case 10: next = entry + (r[3] ? 30 : 14); break;
        case 14: case 34: case 50: r[6] = r[0]; break;
        case 16: next = entry + (static_cast<i32>(r[2]) < 4 ? 36 : 20); break;
        case 20: case 40: case 56: {
            const unsigned width = offset == 20 ? 4 : offset == 40 ? 1 : 2;
            if ((r[1] & (width - 1)) || !readable(r[1], width)) return executed;
            const u32 value = read(r[1], width);
            r[offset == 20 ? 3 : offset == 40 ? 5 : 4] =
                width == 2 ? static_cast<u32>(sign_extend(value, 16)) : value;
            r[1] += width;
            break;
        }
        case 22: case 42: case 58: {
            const unsigned width = offset == 22 ? 4 : offset == 42 ? 1 : 2;
            if ((r[6] & (width - 1)) || !writable(r[6], width)) return executed;
            write(r[6], r[offset == 22 ? 3 : offset == 42 ? 5 : 4], width);
            r[6] += width;
            break;
        }
        case 24: case 44: case 60: {
            const unsigned width = offset == 24 ? 4 : offset == 44 ? 1 : 2;
            r[2] -= width;
            next = static_cast<i32>(r[2]) >= static_cast<i32>(width) ? pc - 4 : pc + 4;
            break;
        }
        case 28: case 64: next = entry + 36; break;
        case 30: r[30] = (r[30] & ~8u) | (r[4] & 1 ? 0 : 8); break;
        case 32: next = entry + (r[30] & 8 ? 50 : 34); break;
        case 36: next = entry + (static_cast<i32>(r[2]) <= 0 ? 48 : 40); break;
        case 48:
            if ((r[25] & 3) || !readable(r[25], 8)) return executed;
            r[6] = read(r[25]); next = read(r[25] + 4); r[25] += 8;
            break;
        case 52: next = entry + (static_cast<i32>(r[2]) < 2 ? 36 : 56); break;
        default: return executed;
        }
        record(pc);
        r[31] = next;
        ++executed;
        // Return to the scheduler at a function return, even with budget left.
        if (offset == 48) break;
    }
    return executed;
}
} // namespace clickhle::vc2

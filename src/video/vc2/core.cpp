
#include "core.h"
#include <algorithm>
#include <bit>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace clickhle::vc2 {
i32 sign_extend(u32 value, unsigned bits) {
    const u32 mask = bits == 32 ? ~0u : (1u << bits) - 1;
    value &= mask;
    if (bits == 32)
        return std::bit_cast<i32>(value);
    return static_cast<i32>(value & ((1u << (bits - 1)) - 1)) -
           static_cast<i32>(value & (1u << (bits - 1)));
}
u32 subtract_flags(u32 a, u32 b) {
    const u32 v = a - b;
    return (v == 0 ? 8u : 0u) | (v >> 31 ? 4u : 0u) | (a < b ? 2u : 0u) |
           (((a ^ b) & (a ^ v)) >> 31);
}
bool condition(unsigned c, u32 f) {
    const bool z = f & 8, n = f & 4, cy = f & 2, v = f & 1;
    const bool results[] = {z,    !z,        cy,      !cy,    n,      !n,           v,
                            !v,   !cy && !z, cy || z, n == v, n != v, !z && n == v, z || n != v,
                            true, false};
    return results[c & 15];
}
// Testbed backing, not a verified 64 MiB physical RAM map. In particular the
// FE aperture below uses a private backing range, not a proven low-memory alias.
Core::Core() : ram_(0x4000000) {}
void Core::load(std::span<const u8> image) {
    if (image.size() < 0x206 || image.size() > 0x140000)
        throw std::runtime_error("Invalid VC2 firmware size");
    *this = Core();
    copy_in(0, image);
    r[31] = 0x200;
}
u32 Core::ram_address(u32 a, u32 n) const {
    if (a < ram_.size() && n <= ram_.size() - a)
        return a;
    if (a >= 0xfe000000 && a <= 0xfe400000 && n <= 0xfe400000 - a)
        return a - 0xfe000000 + 0x2000000;
    throw std::runtime_error("VC2 block access outside modeled RAM");
}
void Core::copy_in(u32 a, std::span<const u8> b) {
    if (b.empty())
        return;
    const auto offset = ram_address(a, static_cast<u32>(b.size()));
    std::copy(b.begin(), b.end(), ram_.begin() + offset);
}
std::vector<u8> Core::copy_out(u32 a, u32 n) const {
    const auto offset = ram_address(a, n);
    return {ram_.begin() + offset, ram_.begin() + offset + n};
}
u32 Core::alu(unsigned op, u32 a, u32 b) {
    const unsigned shift = b & 31;
    switch (op) {
    case 0:
        r[30] = (r[30] & ~8u) | (((a >> shift) & 1) ? 0u : 8u);
        return a;
    case 1: {
        const u32 v = a + b;
        r[30] = (r[30] & ~15u) | (v == 0 ? 8u : 0u) | (v >> 31 ? 4u : 0u) |
                (static_cast<u64>(a) + b > 0xffffffff ? 2u : 0u) | ((~(a ^ b) & (a ^ v)) >> 31);
        return a;
    }
    case 2:
        return a & ((1u << shift) - 1);
    case 3:
        return a & ~b;
    case 4:
        return static_cast<u32>(sign_extend(a, 32) >> shift);
    case 5:
        return a ^ b;
    case 6:
        return a << shift;
    case 7:
        return a & b;
    case 8:
        return a >> shift;
    case 10:
        return a * b;
    case 11:
        return b - a;
    case 12:
        return a + b;
    case 13:
        return a - b;
    case 14:
        return b;
    case 15:
        return a | b;
    case 16:
        r[30] = (r[30] & ~15u) | subtract_flags(a, b);
        return a;
    case 18:
        return sign_extend(a, 32) >= sign_extend(b, 32) ? a : b;
    case 19:
        return sign_extend(a, 32) <= sign_extend(b, 32) ? a : b;
    case 20:
    case 21:
    case 22:
    case 23:
        return a + (b << (op - 19));
    case 24:
        return 0u - b;
    case 25:
        return b ? 31u - static_cast<u32>(std::countl_zero(b)) : ~0u;
    case 26:
        return static_cast<u32>(sign_extend(b, 8));
    case 27:
        return static_cast<u32>(sign_extend(b, 16));
    case 28: {
        u32 out = 0;
        for (unsigned i = 0; i < 32; ++i)
            out = (out << 1) | ((b >> i) & 1);
        return out;
    }
    case 29:
        // Experimental: population count is a candidate, not a verified VC2 opcode.
        // Firmware uses this on a free-record mask and only tests zero/nonzero;
        // the patent names COUNT but does not establish this encoding. Revisit
        // the result and flag semantics when numeric-result or hardware evidence exists.
        return static_cast<u32>(std::popcount(b));
    case 30:
        return b >> 31 ? 0u - b : b;
    case 31:
        return ~b;
    default:
        throw std::runtime_error("Unsupported VC2 scalar ALU operation " + std::to_string(op));
    }
}
void Core::step() {
    const u32 pc = r[31];
    recent_pcs[recent_pc_count++ % recent_pcs.size()] = pc;
    const u32 h = read(pc, 2);
    u32 next = pc + 2;
    auto word = [&](unsigned index) { return read(pc + 2 * index, 2); };
    auto target = [&](u32 v, unsigned bits, unsigned scale = 2) {
        return pc + static_cast<u32>(sign_extend(v, bits)) * scale;
    };
    auto set = [&](unsigned d, u32 v) {
        if (d == 31)
            next = v;
        else
            r[d] = v;
    };
    auto arithmetic = [&](unsigned op, unsigned d, u32 a, u32 b) {
        const u32 v = alu(op, a, b);
        if (op != 0 && op != 1 && op != 16)
            set(d, v);
    };
    auto memory = [&](unsigned op, unsigned d, u32 address) {
        const unsigned widths[] = {4, 4, 2, 2, 1, 1, 2, 1};
        const unsigned width = widths[op];
        // Matches the testbed's data result for aligned accesses. Exception
        // timing for misalignment remains unsupported and must not be hidden.
        if (address & (width - 1))
            throw std::runtime_error("Misaligned VC2 scalar access");
        if (op == 1 || op == 3 || op == 5)
            write(address, r[d], width);
        else {
            u32 value = read(address, width);
            if (op >= 6)
                value = static_cast<u32>(sign_extend(value, width * 8));
            set(d, value);
        }
    };
    if (h == 0x4430)
        next = r[26];
    else if (h == 0x4230) {
    } else if (h == 0x5430 || h == 0x5530 || h == 0x5630 || h == 0x5730) {
        // Provisional column-base controls: firmware uses these around vector
        // blocks advancing 16/32/48 bytes. The analogous VC4 controls place CB
        // in SR[5:4]; that placement is not independently verified on VC2.
        const u32 increment = ((h >> 8) - 0x54) * 16;
        const u32 column = increment ? (r[30] + increment) & 0x30 : 0;
        r[30] = (r[30] & ~0x30u) | column;
    } else if (h == 0x4a30)
        r[30] |= 1u << 30;
    else if (h == 0x4b30)
        r[30] &= ~(1u << 30);
    else if (h == 0x5830) {
        const u32 sp = r[25];
        r[30] = read(sp);
        next = read(sp + 4);
        r[25] = sp + 8;
    } else if (h == 0x4330) {
        u32 delta = ~0u;
        for (unsigned i = 0; i < 4; ++i)
            if (armed_[i])
                delta = std::min(delta, compare_[i] - timer);
        if (delta != ~0u)
            advance(delta);
        if (interrupt(next))
            return;
        sleeping = true;
        return;
    } else if ((h & 0xfff0) == 0x4730) {
        r[26] = next;
        next = r[h & 15];
    } else if ((h & 0xfff0) == 0x4c30)
        // RenderServer's control dispatcher uses negative byte offsets to
        // share its return path (1328: dc branches back to 12e2).
        next += static_cast<u32>(2 * sign_extend(read(next + r[h & 15], 1), 8));
    else if ((h & 0xfff0) == 0x4d30)
        next += static_cast<u32>(2 * sign_extend(read(next + 2 * r[h & 15], 2), 16));
    else if ((h & 0xfff0) == 0x5030)
        r[30] = (r[30] & ~15u) | subtract_flags(r[h & 15], 0);
    else if ((h & 0xe0f0) == 0x4030) {
        if (((h >> 8) & 31) != 19)
            throw std::runtime_error("Unsupported VC2 system selector");
        r[h & 15] = 0;
    } else if ((h & 0xe010) == 0x4000)
        r[h & 15] = (pc & ~3u) + 4 * ((h >> 5) & 255);
    else if ((h & 0xe030) == 0x4010) {
        unsigned op = (h >> 6) & 3;
        u32 addr = r[25] + 4 * ((h >> 8) & 31);
        if (op == 2)
            r[h & 15] = addr;
        else if (op < 2)
            memory(op, h & 15, addr);
        else
            throw std::runtime_error("Unknown SP instruction");
    } else if ((h & 0xffc0) == 0x3f00)
        r[25] += static_cast<u32>(4 * sign_extend(h & 63, 6));
    else if ((h & 0xf0f0) == 0x30f0)
        r[h & 15] = condition((h >> 8) & 15, r[30]);
    else if ((h & 0xf080) == 0x3000) {
        if (condition((h >> 8) & 15, r[30]))
            next = target(h & 127, 7);
    } else if ((h & 0xf800) == 0x7800 || (h & 0xf800) == 0x7000) {
        unsigned op = (h >> 8) & 7, s = (h >> 4) & 15;
        const unsigned widths[] = {4, 4, 2, 2, 1, 1, 2, 1};
        const bool pre = (h & 0xf800) == 0x7000;
        u32 a = r[s] - (pre ? widths[op] : 0);
        memory(op, h & 15, a);
        r[s] = pre ? a : r[s] + widths[op];
    } else if ((h & 0xf000) == 0x6000) {
        unsigned op = 2 * ((h >> 9) & 15);
        u32 v = (h >> 4) & 31;
        if (op != 0 && op != 2 && op != 4 && op != 6 && op != 7 && op != 8)
            v = static_cast<u32>(sign_extend(v, 5));
        arithmetic(op, h & 15, r[h & 15], v);
    } else if ((h & 0xc000) == 0x8000) {
        unsigned op = (h >> 8) & 7, x = (h >> 11) & 7;
        const unsigned widths[] = {4, 4, 2, 2, 1, 1, 2, 1};
        memory(op, h & 15, r[(h >> 4) & 15] + (x == 7 ? 0 : r[x] * widths[op]));
    } else if ((h & 0xff00) == 0xe000 || (h & 0xff00) == 0xf000) {
        const unsigned base = std::array<unsigned, 4>{0, 6, 16, 24}[(h >> 5) & 3], m = h & 31;
        const unsigned count = (base == 24 && m == 15) ? 0 : m + 1, total = count + ((h >> 7) & 1);
        const bool push = (h & 0xff00) == 0xf000;
        u32 sp = push ? r[25] - 4 * total : r[25];
        for (unsigned i = 0; i < total; ++i) {
            unsigned reg = i < count ? (base + count - 1 - i) & 31 : (push ? 26 : 31);
            if (push)
                write(sp + 4 * i, r[reg]);
            else
                set(reg, read(sp + 4 * i));
        }
        r[25] = push ? sp : sp + 4 * total;
    } else if ((h & 0xe000) == 0xe000)
        memory((h >> 12) & 1, h & 15, r[(h >> 4) & 15] + 4 * ((h >> 8) & 15));
    else if ((h & 0xc000) == 0xc000)
        arithmetic((h >> 8) & 31, h & 15, r[h & 15], r[(h >> 4) & 15]);
    else if ((h & 0xf000) == 0) {
        u32 w = word(1);
        next = pc + 4;
        unsigned mode = w >> 14;
        u32 a = (mode & 1) ? static_cast<u32>(sign_extend((h >> 4) & 15, 4)) : r[(h >> 4) & 15];
        u32 b = mode < 2 ? r[(w >> 10) & 15] : (w >> 8) & 63;
        // The reference reads the comparison register after writeback, while
        // the former native decoder read it before. A zero increment agrees
        // under either interpretation; other aliasing cases remain unproven.
        if (mode < 2 && a != 0 && ((w >> 10) & 15) == (h & 15))
            throw std::runtime_error("Unverified VC2 addcmpb comparison-register alias");
        r[h & 15] += a;
        if (condition((h >> 8) & 15, subtract_flags(r[h & 15], b)))
            next = target(w & (mode < 2 ? 1023 : 255), mode < 2 ? 10 : 8);
    } else if ((h & 0xf000) == 0x1000) {
        u32 w = word(1);
        next = pc + 4;
        if (h & 128) {
            r[26] = next;
            next = target(((h >> 8) & 15) << 23 | w << 7 | (h & 127), 27);
        } else if (condition((h >> 8) & 15, r[30]))
            next = target(w << 7 | (h & 127), 23);
    } else if ((h & 0xfc00) == 0x2000) {
        unsigned length = (h & 0x200) ? 3 : 5;
        std::array<u32, 5> words{};
        words[0] = h;
        for (unsigned i = 1; i < length; ++i)
            words[i] = word(i);
        vector(words, length);
        next = pc + 2 * length;
    } else if ((h & 0xfc00) == 0x2800) {
        u32 w = word(1), op = (h >> 8) & 3, off = static_cast<u32>(sign_extend(w, 16));
        if (op < 2 && (w & 1)) {
            op = op == 0 ? 2 : 6;
            off &= ~1u;
        } else if (op == 0) {
            op = w & 2 ? 1 : 0;
            off &= ~3u;
        } else if (op == 1) {
            op = 3;
            off &= ~1u;
        } else
            op = op == 2 ? 4 : 5;
        memory(op, h & 15, r[(h >> 4) & 15] + off);
        next = pc + 4;
    } else if ((h & 0xff00) == 0x2c00) {
        r[h & 15] = r[(h >> 4) & 15] + static_cast<u32>(sign_extend(word(1), 16));
        next = pc + 4;
    } else if ((h & 0xff00) == 0x2d00) {
        u32 group = (h >> 4) & 15, off = word(1), base = group < 4 || group == 12 ? 24 : 25;
        next = pc + 4;
        if (group == 12 || group == 13)
            r[h & 15] = r[base] + off;
        else if (group < 8) {
            unsigned op = group & 3;
            if (op < 2 && (off & 1)) {
                op = op == 0 ? 2 : 6;
                off &= ~1u;
            } else if (op == 0) {
                op = off & 2 ? 1 : 0;
                off &= ~3u;
            } else if (op == 1) {
                op = 3;
                off &= ~1u;
            } else
                op = op == 2 ? 4 : 5;
            memory(op, h & 15, r[base] + off);
        } else
            throw std::runtime_error("Unknown GP memory class");
    } else if ((h & 0xff80) == 0x2e00) {
        arithmetic(std::array<unsigned, 4>{16, 10, 12, 14}[(h >> 5) & 3], h & 31, r[h & 31],
                   static_cast<u32>(sign_extend(word(1), 16)));
        if ((h & 31) != 31 || ((h >> 5) & 3) == 0)
            next = pc + 4;
    } else if ((h & 0xffc0) == 0x2e80) {
        u32 w = word(1), op = (w >> 10) & 31, d = h & 31, a = r[w & 31],
            b = (w & 0x8000) ? (w >> 5) & 31 : r[(w >> 5) & 31];
        next = pc + 4;
        if (h & 32)
            arithmetic(op, d, a, b);
        else {
            // Reference closures discard this PC write; branching is not
            // independently established either. Keep the ambiguity visible.
            if (d == 31)
                throw std::runtime_error("Unverified VC2 extended ALU destination pc");
            std::int64_t x = (op == 0 || op == 1 || op == 4 || op == 5)
                                 ? sign_extend(a, 32)
                                 : static_cast<std::int64_t>(a),
                         y = (op == 0 || op == 2 || op == 4 || op == 6)
                                 ? sign_extend(b, 32)
                                 : static_cast<std::int64_t>(b);
            u32 value = 0;
            if (op < 4)
                value = static_cast<u32>((static_cast<u64>(x) * static_cast<u64>(y)) >> 32);
            else if (op < 8) {
                if (!y)
                    throw std::runtime_error("VC2 divide by zero");
                value = static_cast<u32>(x / y);
            } else if (op == 8 || op == 9) {
                auto v = static_cast<std::int64_t>(sign_extend(a, 32)) +
                         (op == 8 ? static_cast<std::int64_t>(sign_extend(b, 32))
                                  : -static_cast<std::int64_t>(sign_extend(b, 32)));
                value = static_cast<u32>(std::clamp<std::int64_t>(v, -2147483648LL, 2147483647LL));
            } else if (op >= 12 && op <= 15)
                value = a + (b << (op - 7));
            else
                throw std::runtime_error("Unknown extended VC2 ALU");
            set(d, value);
        }
    } else if ((h & 0xff00) == 0x2f00) {
        u32 w1 = word(1), w2 = word(2), d = h & 31, base = w2 >> 11;
        next = pc + 6;
        if ((h & 0xffe0) == 0x2f60)
            set(d, w1 | (w2 << 16));
        else if ((h & 0xffe0) == 0x2f20) {
            if (d == 31)
                throw std::runtime_error("Unverified VC2 address destination pc");
            set(d, (base == 31 ? pc : r[base]) +
                       static_cast<u32>(sign_extend((w2 & 2047) << 16 | w1, 27)));
        } else {
            u32 off = static_cast<u32>(sign_extend((w2 & 2047) << 16 | w1, 27)) & ~3u;
            off |= ((h >> 6) & 1) << 1;
            if (h & 128)
                off |= (h >> 5) & 1;
            unsigned op = ((h >> 7) & 1) * 4 + (w1 & 3);
            if (d == 31 && op != 1 && op != 3 && op != 5)
                throw std::runtime_error("Unverified VC2 memory destination pc");
            memory(op, d, (base == 31 ? pc & ~3u : r[base]) + off);
        }
    } else {
        std::ostringstream out;
        out << "Unsupported VC2 instruction 0x" << std::hex << h;
        throw std::runtime_error(out.str());
    }
    r[31] = next;
}
bool Core::run(u64 budget) {
    sleeping = false;
    error.clear();
    try {
        for (u64 n = 0; n < budget; ++n) {
            step();
            ++instructions;
            if (sleeping)
                return true;
            if ((n & 31) == 31) {
                time_fraction_ += 32;
                if (time_fraction_ >= 100) {
                    advance(time_fraction_ / 100);
                    time_fraction_ %= 100;
                }
                interrupt(r[31]);
            }
        }
        throw std::runtime_error("VC2 instruction budget exhausted");
    } catch (const std::exception &e) {
        std::ostringstream out;
        out << e.what() << " at 0x" << std::hex << r[31];
        error = out.str();
        return false;
    }
}
} // namespace clickhle::vc2

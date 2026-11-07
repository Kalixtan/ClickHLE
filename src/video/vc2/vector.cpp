
#include "core.h"
#include <algorithm>
#include <stdexcept>
#include <iomanip>
#include <sstream>

namespace clickhle::vc2 {
namespace {
using Value = std::int64_t;
using Lanes = std::array<Value, 16>;
u32 add2d(u32 a, u32 b) { return ((a + (b & ~63u)) & 0xfc0) | ((a + b) & 63); }
unsigned repeats(unsigned bits, const Core &c) { return bits == 7 ? c.r[0] : 1u << bits; }
struct Operand {
    u32 base = 0;
    bool vertical = false;
    unsigned width = 1;
    int reg = -1;
    bool step = false, discard = false;
    char role = 0;
    bool packed_columns = false;
    u32 offset(const Core &c, unsigned k) const {
        u32 o = step ? add2d(base, k * (vertical ? 1 : 64)) : base;
        if (reg >= 0) {
            u32 v = c.r[static_cast<unsigned>(reg)];
            if ((role == 'b' && !vertical) || (role == 'd' && !vertical && width == 2))
                v = (v & ~63u) | (v & 48);
            else if (role == 'a' && !vertical && width == 2)
                v = (v & ~63u) | (((v & 63) + 16) & 32);
            else if (role == 'd' && vertical)
                v = (v & ~0xfc0u) | ((v & 0xc00));
            o = add2d(o, v);
        }
        // Provisional packed-halfword indexing: logical columns 16..31
        // select the second H16 bank, skipping the first bank's high bytes.
        // Firmware flip/copy contracts support this bit permutation; silicon
        // behaviour and the full modifier encoding remain unverified.
        if (packed_columns)
            o = (o & ~48u) | ((o & 16u) << 1) | ((o & 32u) >> 1);
        // Provisional column-base model, paired with scalar 5430..5730.
        // Wrap columns independently of rows; never carry into the next row.
        return add2d(o, c.r[30] & 0x30);
    }
};
int reg3(unsigned v) { return v == 7 ? -1 : static_cast<int>(v); }
Operand operand(unsigned y, unsigned x, bool vertical, unsigned width, int reg = -1,
                bool step = false) {
    return {((y & 63) << 6) | (x & 63), vertical, width, reg, step, false, 0};
}
Operand coordinate(unsigned d, bool vertical, unsigned width, int reg = -1, bool step = false) {
    unsigned y = (d >> 6) * 16, x = ((d >> 4) & 3) * 16;
    if (vertical)
        x += d & 15;
    else
        y += d & 15;
    return operand(y, x, vertical, width, reg, step);
}
Operand destination(u32 w, unsigned select, bool step) {
    auto d = coordinate(w >> 8, (select & 2) != 0, (select & 1) + 1, reg3((w >> 5) & 7), step);
    d.role = 'd';
    d.discard = d.width == 2 && (w & 0x1000) && d.reg < 0;
    return d;
}
Operand source_a(unsigned a, int reg, unsigned t) {
    unsigned y = 16 * ((t >> 1) & 1) + 32 * ((t >> 2) & 1), x = 32 * (t & 1) + 16 * ((t >> 3) & 1);
    bool v = t & 16;
    Operand out;
    if (a & 0x400) {
        if (a & 32)
            throw std::runtime_error("Unsupported vector A mode4");
        out = operand(y + ((a >> 6) & 15), x + (a & 31), false, 1, reg, v);
    } else {
        unsigned mode = (a >> 8) & 3;
        if (!mode) {
            if (!v && (a & 255))
                throw std::runtime_error("Unsupported vector A mode0");
            out = operand(y, x + (v ? (a & 31) : 0), v, 1, reg, v && (a & 32));
        } else if (mode == 1)
            throw std::runtime_error("Unsupported vector A mode1");
        else if (v) {
            if (a & 0xd0)
                throw std::runtime_error("Unsupported vector A vertical coordinate");
            out = operand(y, x + (a & 15), true, 2, reg, a & 32);
        } else {
            if (a & 0x33)
                throw std::runtime_error("Unsupported vector A horizontal coordinate");
            out = operand(y + ((a >> 2) & 3) * 4 + ((a >> 6) & 3), x, false, 2, reg, mode == 3);
        }
    }
    out.role = 'a';
    return out;
}
Lanes get(const Core &c, const Operand &o, unsigned repeat, bool sign = true) {
    u32 base = o.offset(c, repeat), y = base >> 6, x = base & 63;
    Lanes values{};
    for (unsigned i = 0; i < 16; ++i) {
        u32 value = 0;
        for (unsigned b = 0; b < o.width; ++b) {
            u32 a = o.vertical ? (((y + i) & 63) << 6) | ((x + 16 * b) & 63)
                               : (y << 6) | ((x + i + 16 * b) & 63);
            value |= static_cast<u32>(c.vrf[a]) << (8 * b);
        }
        values[i] =
            sign && o.width > 1 ? sign_extend(value, 8 * o.width) : static_cast<Value>(value);
    }
    return values;
}
bool enabled(const Core &c, unsigned pred, unsigned lane) {
    if (!pred)
        return true;
    if (pred == 1)
        return false;
    bool v = pred < 4 ? c.vz[lane] : pred < 6 ? c.vn[lane] : c.vc[lane];
    return v == ((pred & 1) == 0);
}
void put(Core &c, const Operand &o, unsigned repeat, const Lanes &values, unsigned pred = 0) {
    if (o.discard)
        return;
    u32 base = o.offset(c, repeat), y = base >> 6, x = base & 63;
    for (unsigned i = 0; i < 16; ++i)
        if (enabled(c, pred, i))
            for (unsigned b = 0; b < o.width; ++b) {
                u32 a = o.vertical ? (((y + i) & 63) << 6) | ((x + 16 * b) & 63)
                                   : (y << 6) | ((x + i + 16 * b) & 63);
                c.vrf[a] = static_cast<u8>(values[i] >> (8 * b));
            }
}
Value saturate(Value v, unsigned width) {
    return width == 1   ? std::clamp<Value>(v, 0, 255)
           : width == 2 ? std::clamp<Value>(v, -32768, 32767)
                        : std::clamp<Value>(v, -2147483648LL, 2147483647LL);
}
} // namespace
void Core::vector(const std::array<u32, 5> &w, unsigned length) {
    const unsigned op9 = w[0] & 511, op = op9 >> 3, select = (op9 >> 1) & 3;
    // Fixed-row stores used by firmware fills and mask builders. Width comes
    // from the A operand, not the load-style opcode bits.
    // EXPERIMENTAL: the 0x01c2 form is modelled as 64 rows because the two
    // clear loops advance 2048 bytes and fill the recorded allocation sizes.
    // Its repeat-field decoding is NOT established on hardware and may be
    // wrong. Keep this hypothesis limited to the exact observed encoding;
    // do not reinterpret repeat field 7 in other instructions.
    const bool fill_halfwords = length == 5 && w[0] == 0x2008 && w[1] == 0 &&
                                w[2] == 0x0101 && w[3] == 0x010e && w[4] == 0;
    const bool fill_bytes = length == 5 && w[0] == 0x2008 && w[1] == 0 &&
                            w[2] == 0x0102 && w[3] == 0x020e && w[4] == 0;
    const bool clear_halfwords = length == 5 && w[0] == 0x2008 && w[1] == 0 &&
                                 w[2] == 0x01c2 && w[3] == 0x010e && w[4] == 0;
    if (fill_halfwords || fill_bytes || clear_halfwords) {
        const unsigned width = fill_bytes ? 1 : 2;
        const unsigned rows = clear_halfwords ? 64 : 16;
        const u32 stride = r[fill_halfwords ? 1 : 2];
        if ((r[0] | stride) & (16 * width - 1))
            throw std::runtime_error("Unverified unaligned 80-bit vector store");
        const auto values = get(*this, operand(0, 0, false, width), 0, false);
        for (unsigned row = 0; row < rows; ++row) {
            const u32 address = r[0] + row * stride;
            for (unsigned lane = 0; lane < 16; ++lane)
                for (unsigned byte = 0; byte < width; ++byte)
                    write(address + lane * width + byte,
                          static_cast<u32>(values[lane] >> (8 * byte)), 1);
        }
        return;
    }
    // Experimental split stores: verified against original memcpy and guarded
    // buffers. Encodings, never game identities or firmware PCs, select them.
    const bool partial_negative =
        (w[0] == 0x2375 || w[0] == 0x237d) && w[1] == 0xffe0 && w[2] == 0x407;
    const bool partial_zero =
        (w[0] == 0x2370 || w[0] == 0x2378 || w[0] == 0x2372 || w[0] == 0x237a) && w[1] == 0 &&
        w[2] == 7;
    if (length == 3 && (partial_negative || partial_zero)) {
        unsigned width = (w[0] == 0x2372 || w[0] == 0x237a) ? 2 : 1;
        bool second = w[0] & 8;
        u32 address = r[0] - (w[1] ? 16 : 0);
        auto values = get(*this, operand(0, 0, false, width), 0, false);
        for (unsigned i = 0; i < 16; ++i) {
            u32 a = address + i * width;
            if (((a & ~15u) != (address & ~15u)) == second)
                for (unsigned k = 0; k < width; ++k)
                    write(a + k, static_cast<u32>(values[i] >> (8 * k)), 1);
        }
        return;
    }
    // Provisional H8 raster-store layout. The split signed offset follows
    // the observed 1280-byte row spacing; the retained H16 device records
    // do not establish this byte-store encoding. Keep other t5 forms guarded.
    if (length == 3 && (w[0] == 0x2208 || w[0] == 0x2209 || w[0] == 0x220c || w[0] == 0x220d) &&
        ((w[1] & 31) == 1 || (w[1] & 31) == 3) && (w[2] & 0x7ff) == 7) {
        u32 offset =
            ((w[1] & 0xe0u) << 10) | ((w[0] & 4u) << 12) | ((w[0] & 1u) << 13) | ((w[1] >> 8) << 5);
        u32 address = r[0] + static_cast<u32>(sign_extend(offset, 18));
        // t5 bit 1 selects the row block, as in the measured H16 stores.
        // Uploaded raster tails encode rows 16..31 both as t5=3,row=0..15
        // and as t5=1,row=16..31. In particular, the latter's first 2208
        // must not fall through to the unrelated load-style field decoder.
        const unsigned row = ((w[1] & 2) ? 16 : 0) + (w[2] >> 11);
        auto values = get(*this, operand(row, 32, false, 1), 0, false);
        for (unsigned i = 0; i < 16; ++i)
            write(address + i, static_cast<u32>(values[i]), 1);
        return;
    }
    if (length == 5 && (w[0] == 0x2150 || w[0] == 0x2152 || w[0] == 0x2156) && w[3] == 0) {
        unsigned reg = 0, width = 0, y = 16, x = 0;
        bool vertical = false, low = false;
        if (w[0] == 0x2150 && w[1] == 0x4000 && w[2] == 0x2f && w[4] == 0x87) {
            reg = 5;
            width = 1;
        } else if (w[0] == 0x2152 && w[1] == 0x4100 && w[2] == 0x17 && w[4] == 0x87) {
            reg = 2;
            width = 2;
            y = 17;
        } else if (w[0] == 0x2150 && w[1] == 0x5000 && w[2] == 0x37 && w[4] == 0x87) {
            reg = 6;
            width = 1;
            x = 16;
        } else if (w[0] == 0x2156 &&
                   (w[1] == 0x6000 || w[1] == 0x6400 || w[1] == 0x6800 || w[1] == 0x6c00) &&
                   w[2] == 0x1f && w[4] == 0x80e7) {
            reg = 3;
            width = 2;
            x = 32 + ((w[1] >> 8) & 15);
            vertical = low = true;
        }
        if (width) {
            Lanes values{};
            for (unsigned i = 0; i < 16; ++i)
                values[i] = read(r[reg] + static_cast<u32>(low ? acclo[i] : acc[i]) * width, width);
            put(*this, operand(y, x, vertical, width), 0, values);
            return;
        }
    }
    if (op9 < 32) {
        const unsigned kind = op9 >> 3;
        unsigned count = repeats((w[2] >> 6) & 7, *this), base = (w[2] >> 3) & 7, stride = w[2] & 7;
        // Provisional context-save/restore forms. Firmware allocates 4192 bytes:
        // two 2048-byte VRF blocks, then three 32-byte state blocks. These four
        // instructions use r0 as that buffer's address, so REP=r0 cannot fit
        // the routine's contract. 64 rows of 32 bytes fit both halves exactly.
        // This does not establish the meaning of repeat field 7 in other forms.
        const bool context_store = length == 3 && w[0] == 0x220a &&
                                   (w[1] == 0 || w[1] == 0x4001) && w[2] == 0x01c1;
        const bool context_load = length == 3 && w[0] == 0x2202 &&
                                  (w[1] == 0 || w[1] == 0x2002) && w[2] == 0x01c1;
        if (context_store || context_load) {
            if ((r[0] & 31) || r[1] != 32)
                throw std::runtime_error("Unverified VC2 vector context layout");
            count = 64;
        }
        bool inc = true;
        // Provisional 2203: the observed REP16 H16 loads fit the ordinary
        // aligned-load data path in both firmware copy and flip routines.
        // This does not establish what distinguishes it from 2202 (for
        // example flags, masking or timing). Reject untested repeat forms.
        const bool provisional_load = length == 3 && op9 == 3 && count == 16;
        if (((op9 & 1) && !provisional_load) || select == 3) {
            std::ostringstream message;
            message << "Unsupported vector memory opcode [" << std::hex << std::setfill('0');
            for (unsigned i = 0; i < length; ++i)
                message << (i ? " " : "") << std::setw(4) << w[i];
            message << "]";
            throw std::runtime_error(message.str());
        }
        Operand d = coordinate(w[1] >> 8, select & 2, (select & 1) + 1);
        i32 offset = 0;
        if (length == 3) {
            offset = sign_extend(((w[1] & 255) << 7) | (w[2] >> 9), 15) * 8;
            if (kind == 1 && select == 1) {
                unsigned t = w[1] & 31;
                // Provisional: t5 bit 3 is a modifier, not an x=16 origin.
                // Original copy routines preserve guarded buffers with this
                // layout. Any lane-mask or bus-write effects are unresolved.
                d = operand(16 * ((t >> 1) & 1) + 32 * ((t >> 2) & 1) + (w[2] >> 11),
                            32 * (t & 1), false, 2);
                offset = static_cast<i32>(32 * (w[1] >> 8));
            }
        } else {
            if ((w[1] & 255) != 255 || (w[3] & 0xeffe) != 0xeffe || w[4] != 0xe7 || kind == 1) {
                std::ostringstream message;
                message << "Unsupported 80-bit vector memory form [" << std::hex << std::setfill('0');
                for (unsigned i = 0; i < w.size(); ++i)
                    message << (i ? " " : "") << std::setw(4) << w[i];
                message << "]";
                throw std::runtime_error(message.str());
            }
            offset = sign_extend(w[2] >> 9, 7) + static_cast<i32>((w[3] >> 12) & 1);
            inc = w[3] & 1;
        }
        if (count > 65536)
            throw std::runtime_error("VC2 vector repeat limit");
        u32 address = (base == 7 ? 0 : r[base]) + static_cast<u32>(offset);
        for (unsigned k = 0; k < count; ++k) {
            Operand current = d;
            if (inc)
                current.base = add2d(d.base, k * (d.vertical ? 1 : 64));
            u32 a = kind < 2 ? address & ~(16 * d.width - 1) : address;
            if (kind == 1) {
                auto values = get(*this, current, 0, false);
                for (unsigned i = 0; i < 16; ++i)
                    for (unsigned b = 0; b < d.width; ++b)
                        write(a + i * d.width + b, static_cast<u32>(values[i] >> (8 * b)), 1);
            } else {
                auto values = get(*this, current, 0, false);
                for (unsigned i = 0; i < 16; ++i) {
                    bool same = ((a + i * d.width) & ~15u) == (a & ~15u);
                    if (kind == 0 || same == (kind == 2)) {
                        u32 value = 0;
                        for (unsigned b = 0; b < d.width; ++b)
                            value |= read(a + i * d.width + b, 1) << (8 * b);
                        values[i] = value;
                    }
                }
                put(*this, current, 0, values);
            }
            if (stride != 7)
                address += r[stride];
        }
        return;
    }
    if (op9 == 0x170 && length == 5) {
        if ((w[2] & 0x200) || (w[3] >> 10) || (w[4] & 0xc7ff))
            throw std::runtime_error("Unsupported vstr form");
        auto a = source_a(((w[3] >> 4) & 63) * 32 + (w[2] >> 11), reg3((w[3] >> 1) & 7), w[1] & 31);
        unsigned base = (w[2] >> 3) & 7, stride = w[2] & 7, pred = (w[4] >> 11) & 7,
                 count = repeats((w[2] >> 6) & 7, *this);
        if (count > 65536)
            throw std::runtime_error("VC2 vector repeat limit");
        u32 address = base == 7 ? 0 : r[base];
        for (unsigned k = 0; k < count; ++k) {
            auto values = get(*this, a, k, false);
            for (unsigned i = 0; i < 16; ++i)
                if (enabled(*this, pred, i) && ((address + i * a.width) & ~15u) == (address & ~15u))
                    for (unsigned b = 0; b < a.width; ++b)
                        write(address + i * a.width + b, static_cast<u32>(values[i] >> (8 * b)), 1);
            if (stride != 7)
                address += r[stride];
        }
        return;
    }
    Operand d, a, b;
    unsigned count = 1, pred = 0, sru = 0, sreg = 7;
    bool setf = false, clear = false, accumulate = false, x15 = false;
    int bkind = 0;
    Value immediate = 0;
    unsigned scalar = 7;
    if (length == 5) {
        setf = w[4] & 0x4000;
        pred = (w[4] >> 11) & 7;
        clear = w[4] & 0x400;
        accumulate = w[4] & 0x200;
        x15 = w[4] & 0x8000;
        if (w[4] & 0x100)
            throw std::runtime_error("Unsupported vector x8 modifier");
        count = repeats((w[4] >> 5) & 7, *this);
        sru = (w[4] >> 3) & 3;
        sreg = w[4] & 7;
        d = destination(w[1], select, w[3] & 1);
        // Provisional packed destination form used by firmware block copies.
        d.packed_columns = op9 == 0x27;
        a = source_a(((w[3] >> 4) & 63) * 32 + (w[2] >> 11), reg3((w[3] >> 1) & 7), w[1] & 31);
        if (w[2] & 0x400) {
            bkind = 1;
            immediate = sign_extend(((w[3] >> 10) << 10) | (w[2] & 1023), 16);
        } else if (w[3] & 0x8000) {
            bkind = 2;
            scalar = (w[3] >> 12) & 7;
            immediate = sign_extend(((w[3] >> 10) & 3) * 1024 + (w[2] & 1023), 12);
        } else {
            const bool packed_b = (w[2] & 3) == 3 && (w[3] & 0x800) && op == 4;
            if ((w[2] & 2) && !packed_b)
                throw std::runtime_error("Unsupported vector B type");
            b = coordinate((w[2] >> 2) & 255, w[2] & 1, w[3] & 0x800 ? 2 : 1,
                           reg3((w[3] >> 12) & 7), w[3] & 0x400);
            b.role = 'b';
            // Provisional B type: checked by original VMCS horizontal flips
            // and RenderServer copies, including crossing an H16 bank edge.
            b.packed_columns = packed_b;
        }
    } else {
        if (w[2] & 0x400) {
            setf = w[2] & 0x200;
            count = repeats((w[2] >> 6) & 7, *this);
            // Provisional flag packing in the paired firmware context routines:
            // start at 2, add 1 if C, subtract 2 if Z, subtract 4 if N.
            // Treating these fields as REP loses the flags and changes unrelated
            // VRF rows. Limit the correction to the observed encodings until
            // the general 48-bit immediate modifier layout is established.
            if (w[1] == 0x00e0 &&
                ((w[0] == 0x2242 && w[2] == 0x0581) ||
                 (w[0] == 0x2252 && (w[2] == 0x0482 || w[2] == 0x0504)))) {
                pred = (w[2] >> 6) & 7;
                count = 1;
            }
            bkind = 1;
            immediate = sign_extend(w[2] & 63, 6);
        }
        d = destination(w[1], select, count > 1);
        unsigned t = w[1] & 31;
        int ar = -1, br = -1;
        if (!(op9 & 1)) {
            if (t & 8) {
                t &= ~8u;
                ar = d.reg;
            }
            if (!bkind && (w[2] & 2))
                br = d.reg;
            if (d.reg >= 0 && (ar >= 0 || br >= 0)) {
                d.reg = -1;
                if (d.width == 2 && (w[1] & 0x1000))
                    d.discard = true;
            }
        } else if (!bkind && (w[2] & 2))
            throw std::runtime_error("Unsupported 48-bit B type");
        unsigned y = 16 * ((t >> 1) & 1) + 32 * ((t >> 2) & 1),
                 x = 32 * (t & 1) + 16 * ((t >> 3) & 1), c = w[2] >> 11;
        a = operand(y + (d.vertical ? 0 : c), x + (d.vertical ? c : 0), d.vertical, d.width, ar);
        a.role = 'a';
        if (!bkind) {
            b = coordinate((w[2] >> 2) & 255, w[2] & 1, d.width, br);
            if (br >= 0)
                b.role = 'b';
        }
    }
    if (count > 65536)
        throw std::runtime_error("VC2 vector repeat limit");
    const bool nibble = length == 5 && (w[0] == 0x20c4 || w[0] == 0x20dc || w[0] == 0x202c) &&
                        (w[1] & 255) == 0xf3 && w[3] == 14 && (w[4] == 0x8007 || w[4] == 0x8407);
    // The same blend pair occurs with its first source in either VRF row
    // block (t5=0x11 or 0x13); that coordinate does not change the arithmetic.
    const bool blend = length == 5 && (w[0] == 0x208a || w[0] == 0x208c) &&
                       ((w[1] & 255) == 0xf1 || (w[1] & 255) == 0xf3) &&
                       (w[3] == 0x700e || w[3] == 0x780e) && (w[4] == 0x8407 || w[4] == 0x8007);
    // Firmware-derived byte operations: texture unpacking clears the low
    // nibble (RGBA4444) or three low bits (RGB565); blend weights use a byte
    // complement followed by +1. These contracts establish only the forms
    // below, not arbitrary selectors 5/7 or their flag/accumulator modifiers.
    const bool byte_mask = length == 5 && w[0] == 0x202c && bkind == 1 &&
                           (immediate == 0xf0 || immediate == 0xf8) && w[4] == 7 &&
                           count == 1 && a.width == 1 && d.width == 1 &&
                           a.reg < 0 && d.reg < 0;
    const bool complement = length == 3 && w[0] == 0x223c && bkind == 1 &&
                            immediate == -1 && count == 1 && !setf &&
                            a.reg < 0 && d.reg < 0;
    const bool split48 = length == 3 && (w[0] == 0x23f2 || w[0] == 0x23ea) &&
                         (w[1] == 0xe0 || w[1] == 0x20e0) && (w[2] == 0x804 || w[2] == 0x884);
    const bool split80 = length == 5 && (w[0] == 0x21f2 || w[0] == 0x21ea) && w[2] == 0x100 &&
                         w[3] == 0x7d8f && w[4] == 0x87 && (w[1] == 0xe2 || w[1] == 0x20e2);
    const bool lowcarry = length == 5 && (w[0] == 0x2042 || w[0] == 0x2046) && w[4] == 0x4007;
    const bool lowborrow = length == 5 && w[0] == 0x2052 && w[4] == 0x4007;
    // Firmware-derived add-with-carry candidate. Both forms pair with a
    // low-half vadd/setf in 32-bit planar arithmetic. Silicon confirmation
    // and general modifiers remain outstanding.
    const bool vertical_carry = length == 3 && w[0] == 0x2316 && (w[2] & 0x7ff) == 0x400;
    const bool highcarry = vertical_carry || (length == 3 && w[0] == 0x2312) ||
                           (length == 5 && w[0] == 0x2112 && w[4] == 0x4007);
    // Firmware-derived subtract-with-borrow: RenderServer's planar 32-bit
    // row update pairs 2052/setf on the low halves with 231a on the highs.
    // No independent silicon result establishes other forms or flag writes.
    const bool highborrow = length == 3 && w[0] == 0x231a && bkind == 0;
    const bool carry_operands = count == 1 && d.vertical == a.vertical &&
                                d.width == 2 && a.width == 2 && d.reg < 0 && a.reg < 0 &&
                                ((bkind == 0 && !d.vertical && !b.vertical && b.width == 2 && b.reg < 0) ||
                                 (bkind == 1 && d.vertical));
    if (highcarry && !carry_operands)
        throw std::runtime_error("Unsupported vector carry operands");
    if (highborrow && (!carry_operands || d.vertical))
        throw std::runtime_error("Unsupported vector borrow operands");
    if (x15 && !nibble && !blend && (d.width != 2 || op == 9 || op == 11 || op == 13 || op == 17))
        throw std::runtime_error("Unsupported vector accumulator form");

    for (unsigned k = 0; k < count; ++k) {
        auto av = get(*this, a, k, !(nibble || blend || complement || highcarry || highborrow)), bv = Lanes{};
        if (bkind)
            bv.fill(
                bkind == 1
                    ? immediate
                    : sign_extend((scalar == 7 ? 0 : r[scalar]) + static_cast<u32>(immediate), 16));
        else
            bv = get(*this, b, k, !(blend || highcarry || highborrow));
        if (split48 || split80) {
            if (split48) {
                av = get(*this, operand(1, 0, false, 2), 0, false);
                bv = get(*this, operand(1, w[2] == 0x884 ? 32 : 0, false, 2), 0, false);
            }
            Lanes values{};
            unsigned odd = (op == 62) ? 1 : 0;
            for (unsigned i = 0; i < 8; ++i) {
                values[i] = av[2 * i + odd];
                values[i + 8] = bv[2 * i + odd];
            }
            put(*this, d, k, values);
            continue;
        }
        Lanes values{};
        std::array<bool, 16> selected{};
        for (unsigned i = 0; i < 16; ++i)
            selected[i] = enabled(*this, pred, i);
        for (unsigned i = 0; i < 16; ++i) {
            Value x = av[i], y = bv[i], value = 0;
            unsigned sh = static_cast<unsigned>(y) & 31;
            if (complement)
                value = x ^ 255;
            else if (byte_mask)
                value = x & y;
            else if (nibble)
                value = op == 24 ? x * (1LL << sh) : op == 27 ? x >> sh : x & y;
            else if (blend)
                value = x * y;
            else
                switch (op) {
                case 4:
                    value = y;
                    break;
                case 8:
                case 9:
                    value = x + y;
                    break;
                case 10:
                case 11:
                    value = x - y;
                    break;
                case 12:
                case 13:
                    value = y - x;
                    break;
                case 14:
                    value = x * y;
                    break;
                case 16:
                case 17:
                    value = (x * y) >> 8;
                    break;
                case 18:
                    value = (x * y) >> 16;
                    break;
                case 19:
                    value = (x * (y & 65535)) >> 16;
                    break;
                case 51:
                    value = ((x & 65535) * (y & 65535)) >> 16;
                    break;
                case 20:
                case 21:
                case 52: {
                    if (op != 20)
                        y &= 65535;
                    Value p = x * y;
                    value = p < 0 ? -((-p + 32768) >> 16) : (p + 32768) >> 16;
                    break;
                }
                case 24:
                    value = x * (1LL << sh);
                    break;
                case 26:
                    value = x >> sh;
                    break;
                case 27:
                    value = (static_cast<u32>(x) & ((1u << (8 * a.width)) - 1)) >> sh;
                    break;
                case 28:
                    value = std::min(x, y);
                    break;
                case 29:
                    value = std::max(x, y);
                    break;
                case 30:
                    value = x >= y ? x - y : y - x;
                    break;
                case 34:
                    if (!highcarry)
                        throw std::runtime_error("Unsupported vector carry form");
                    value = x + y + vc[i];
                    break;
                case 35:
                    if (!highborrow)
                        throw std::runtime_error("Unsupported vector borrow form");
                    value = x - y - vc[i];
                    break;
                case 37:
                    value = std::max<Value>(0, std::min(x, y));
                    break;
                case 44:
                    value = (y >> i) & 1;
                    break;
                default:
                    throw std::runtime_error("Unsupported vector ALU operation " +
                                             std::to_string(op));
                }
            if (selected[i]) {
                if (clear) {
                    acc[i] = 0;
                    acclo[i] = 0;
                }
                if (nibble || blend) {
                    Value total = acc[i] * 65536 + acclo[i] + (nibble ? value & 65535 : value);
                    acc[i] = total >> 16;
                    acclo[i] = static_cast<u32>(total) & 65535;
                    value = blend ? std::min<Value>(255, total >> 8) : total;
                } else if (x15) {
                    if (d.width != 2)
                        throw std::runtime_error("Unsupported byte accumulator modifier");
                    Value total = saturate(
                        acc[i] * 65536 + acclo[i] + (accumulate ? value : value & 65535), 4);
                    acc[i] = total >> 16;
                    acclo[i] = static_cast<u32>(total) & 65535;
                    value = total;
                } else if (accumulate) {
                    Value total = saturate((acc[i] + value) * 65536 + acclo[i], 4);
                    acc[i] = total >> 16;
                    acclo[i] = static_cast<u32>(total) & 65535;
                    value = acc[i];
                }
            }
            if (!blend && (op == 9 || op == 11 || op == 13 || op == 17))
                value = saturate(value, d.width);
            values[i] = value;
        }
        if (sreg != 7) {
            if (!sru && length == 5)
                throw std::runtime_error("Unsupported vector scalar result");
            bool any = false;
            Value value = 0;
            for (unsigned i = 0; i < 16; ++i)
                if (selected[i]) {
                    value = !any       ? values[i]
                            : sru == 1 ? value + values[i]
                            : sru == 2 ? std::min(value, values[i])
                                       : std::max(value, values[i]);
                    any = true;
                }
            r[sreg] = static_cast<u32>(value);
        }
        // Capture lane selection before setf: newly written flags must not
        // change which destination lanes this instruction writes.
        for (unsigned i = 0; i < 16; ++i)
            if (selected[i] && !d.discard) {
                Lanes lane = values;
                Operand single = d;
                u32 base = single.offset(*this, k), yy = base >> 6, xx = base & 63;
                for (unsigned byte = 0; byte < d.width; ++byte) {
                    u32 address = d.vertical ? (((yy + i) & 63) << 6) | ((xx + 16 * byte) & 63)
                                             : (yy << 6) | ((xx + i + 16 * byte) & 63);
                    vrf[address] = static_cast<u8>(lane[i] >> (8 * byte));
                }
            }
        if (setf)
            for (unsigned i = 0; i < 16; ++i)
                if (selected[i]) {
                    u32 mask = (1u << (8 * d.width)) - 1;
                    vz[i] = (values[i] & mask) == 0;
                    vn[i] = values[i] < 0;
                    vc[i] = values[i] < 0 || values[i] > mask;
                    // The matching context restore shifts the packed flag word
                    // right by one: its low bit restores C. Other shift forms
                    // still need independent carry/zero-shift evidence.
                    if (length == 3 && w[0] == 0x22d2 && w[1] == 0x10e0 && w[2] == 0x0601)
                        vc[i] = (static_cast<u32>(av[i]) & 1) != 0;
                    if (lowcarry && carry_operands)
                        vc[i] =
                            (static_cast<u32>(av[i]) & 65535) + (static_cast<u32>(bv[i]) & 65535) >
                            65535;
                    if (lowborrow && carry_operands)
                        vc[i] = (static_cast<u32>(av[i]) & 65535) <
                                (static_cast<u32>(bv[i]) & 65535);
                }
    }
}
} // namespace clickhle::vc2

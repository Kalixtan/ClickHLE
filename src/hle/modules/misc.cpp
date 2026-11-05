// The "miscTBD" library: 15 ordinals, imported by every game in hand.
//
// Apple's own name for it, typo and all. Ordinals 0 and 1 are the heap, which
// is why every title stopped here -- a game cannot get far before it allocates.
//
// Ordinal behavior is based on the games' call sites and firmware behavior.

#include "clickhle/hle/modules.h"

#include <string>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>

#include "clickhle/common/log.h"
#include "clickhle/hle/format.h"
#include "clickhle/core/emulator.h"
#include "clickhle/hle/call_frame.h"
#include "clickhle/hle/guest_heap.h"
#include "clickhle/hle/registry.h"

namespace clickhle {
namespace {

constexpr std::string_view kCategory = "misc";
constexpr const char* kModule = "miscTBD";




}  // namespace

namespace {

/// Variadic arguments as the ARM procedure standard passes them: r1, r2, r3,
/// then the stack.
///
/// r0 is excluded because it holds the format string -- the one fixed argument.
class RegisterThenStackArgs final : public FormatArgs {
public:
    RegisterThenStackArgs(ICpuCore& cpu, const AddressSpace& memory)
        : cpu_(cpu), memory_(memory) {}

    u32 next_word() override {
        if (index_ < 3) {
            return cpu_.reg(reg::kR1 + index_++);
        }
        const GuestAddr at = cpu_.sp() + 4u * (index_++ - 3u);
        return memory_.read<u32>(at).value_or(0);
    }

    u64 next_double_word() override {
        // Eight-byte arguments are 8-byte aligned in the ARM standard.
        if (index_ % 2 == 1) {
            ++index_;
        }
        const u64 low = next_word();
        return low | (static_cast<u64>(next_word()) << 32);
    }

private:
    ICpuCore& cpu_;
    const AddressSpace& memory_;
    u32 index_ = 0;
};

}  // namespace

void register_misc(HleRegistry& registry) {
    const auto fn = [&registry](u32 ordinal, std::string name, u8 arity,
                                HleHandler handler) {
        registry.add(kModule, ordinal, std::move(name), arity,
                     std::move(handler));
    };

    // alloc(size) -> pointer, null on failure.
    //
    // Evidence, from four titles that share no build:
    //   Tetris / Pac-Man / Zuma reach it through an adapter pair that is
    //   byte-identical in all three --
    //       mov r0, r1 ; b <miscTBD#0>      the wrapper's size argument
    //       movs r0, r1 ; bne <miscTBD#1>   free, skipped when null
    //   Lost calls it directly with a constant 0x7ff80 (512 KB) in r0, then
    //   `cmp r0, #0` and stores the result -- one argument in, one pointer out,
    //   null-checked.
    fn(0, "alloc", 1, [](HleCallFrame& f) {
        const u32 size = f.args.next_u32();
        // Who is allocating, at the framework boundary. The CPU-side
        // CLICKHLE_DUMP_ALLOC_STACK probe needs each title's own thunk
        // address; this works for any title without one, which is what makes
        // an A/B between a working and a stalled title cheap. Logged with the
        // size because "10 allocations total" (Sudoku) versus "3335"
        // (Mahjong) is the clearest signal found so far that a title never
        // enters its construction phase.
        if (std::getenv("CLICKHLE_DUMP_ALLOC_LR") != nullptr) {
            CLICKHLE_INFO(kCategory)
                << "alloc(" << std::dec << size << ") from lr=0x" << std::hex
                << f.cpu.lr() << std::dec;
        }
        const GuestAddr block = f.emulator.heap().allocate(size);
        if (block == kNullGuest) {
            CLICKHLE_WARN(kCategory)
                << "alloc(" << size << ") failed; heap holds "
                << f.emulator.heap().bytes_in_use() << " of "
                << f.emulator.heap().capacity() << " bytes";
        }
        // Returning null on exhaustion rather than faulting is correct: every
        // call site seen checks for it, so the guest has a path for this and
        // taking it is more honest than pretending the heap is infinite.
        f.ret(block);
    });

    // free(pointer). Callers already skip it for null, but tolerate one
    // anyway -- a free(NULL) that faults would be a worse bug than the one it
    // is guarding against.
    fn(1, "free", 1, [](HleCallFrame& f) {
        const GuestAddr block = f.args.next_ptr();
        if (block == kNullGuest) {
            f.ret(0);
            return;
        }
        if (!f.emulator.heap().release(block)) {
            CLICKHLE_WARN(kCategory)
                << "free(0x" << std::hex << block << std::dec
                << ") does not name a live block";
        }
        f.ret(0);
    });

    // read_time(u32* out) -- writes one word and returns nothing useful.
    //
    // Evidence: every one of the 88 call sites across all 16 titles is the same
    // three instructions, `mov r0, sp; bl <#9>; ldr r0, [sp]`. Several zero the
    // slot first and then compare the result against a value held in a
    // structure, which is what polling a clock against a deadline looks like.
    // It is also the second-busiest ordinal in miscTBD and the first thing
    // vector[4] asks for in 15 of 16 titles, both of which fit a clock and very
    // little else.
    //
    // Backed by the emulated tick, which is derived from work the guest has
    // done rather than from the host clock, so runs stay reproducible.
    //
    // Microseconds, not milliseconds, and that is evidence rather than taste.
    // Reporting milliseconds made Sims Bowling and Sims Pool die in the armcc
    // runtime with "Arithmetic exception: Divide By Zero" -- their own debug
    // channel said so. Startup is a few thousand instructions, which rounds to
    // zero milliseconds, so two reads returned the same value and something
    // divided by the elapsed time.
    //
    // A finer unit also fits how the ordinal is used: it is polled in tight
    // loops and differenced, which milliseconds would quantise into
    // uselessness. The absolute scale is still unconfirmed -- a wrong one makes
    // a game run fast or slow rather than fail.
    fn(9, "read_time?", 1, [](HleCallFrame& f) {
        const GuestAddr out = f.args.next_ptr();
        const u32 value = static_cast<u32>(f.emulator.microsecond_count());
        f.store<u32>(out, value);
        // LLE's disassembly and oracle's read of the call chain agree: real
        // firmware returns the value in r0 AS WELL AS writing it to *out --
        // both, not either. Every known call site reads the out-parameter,
        // so this was latent rather than live, but it's a one-word fix for a
        // confirmed fact rather than something to leave silently wrong.
        f.ret(value);
    });

    // --- InputEvents ---------------------------------------------------------
    //
    // Eight of the sixteen titles stop here the moment their main callback
    // starts, which makes this the smallest library with the largest effect.
    //
    // Identified from the call sites because the firmware's export address
    // for #0 lands mid-function.
    // The evidence is unusually clear: Zuma and Tetris share a byte-identical
    //
    //     mov r0, sp
    //     bl  <InputEvents#0>
    //     ldr r0, [sp, #4]
    //     tst r0, #0x40000000
    //
    // One out-pointer; the caller reads the word at +4 and tests one bit. That
    // is a polled button mask, which is also why a library this small needs
    // only two entries.
    //
    // Reporting nothing held is a real answer, not a stub: it is what the
    // hardware reports with nobody touching the wheel, and it is what lets a
    // menu loop run. Wiring the wheel to it -- spin direction and velocity,
    // taps at the cardinal points, the centre button -- needs the bit
    // assignments, of which only 0x40000000 is so far known.

    // InputEvents#1 is unidentified and deliberately unregistered.

    // --- Settings ------------------------------------------------------------
    //
    // get_setting(const char* name, void* out_value, void* out_kind) -> status.
    //
    // From the call sites, which agree in shape across titles:
    //
    //     add r0, pc, #152        a literal string -- the setting's name
    //     add r1, sp, #8          out
    //     add r2, sp, #4          out
    //     bl  <Settings#0>
    //     cmp r0, #0
    //     ldrbeq r0, [sp, #8]     the value is read ONLY when it returned 0
    //
    // So a non-zero return means "no such setting", and the caller falls
    // through to whatever default it carries. Reporting that is a real answer
    // rather than a stub: an emulator with no persisted preferences genuinely
    // has none of these, and the games are written to cope. It is also the
    // safe direction to be wrong in -- inventing a value would silently pick a
    // setting on the player's behalf.
    //
    // The out-parameters are left untouched, exactly as a failing lookup would.

    // Named by the DogParty import map, which is why these arrive in a batch
    // rather than one call site at a time.

    fn(2, "realloc", 2, [](HleCallFrame& f) {
        const GuestAddr block = f.args.next_ptr();
        const u32 size = f.args.next_u32();
        // GuestHeap has no resize, so this is allocate-copy-release. The old
        // size bounds the copy; without it there is no way to know how much of
        // the new block is meaningful.
        const GuestAddr fresh = f.emulator.heap().allocate(size);
        if (fresh == kNullGuest) {
            f.ret(kNullGuest);
            return;
        }
        if (block != kNullGuest) {
            const auto old = f.emulator.heap().size_of(block);
            const u32 copy = old.has_value() ? std::min(*old, size) : 0;
            for (u32 i = 0; i < copy; ++i) {
                const auto byte = f.memory.read<u8>(block + i);
                if (!byte.has_value()) {
                    break;
                }
                (void)f.memory.write<u8>(fresh + i, *byte);
            }
            (void)f.emulator.heap().release(block);
        }
        f.ret(fresh);
    });

    // The device "level" -- a capability tier or hardware generation. Set and
    // get are a matched pair over one value the firmware would own; nothing
    // reads it but the game itself, so remembering what it was told is the
    // whole contract.
    static u32 device_level = 0;
    fn(5, "device_level_set", 1, [](HleCallFrame& f) {
        // LLE's decompile: `if (level > 100) level = 100; else if (level < 0)
        // level = 0;` before storing. A title writing 150 and reading back
        // through #6 gets 100 on real hardware, not 150.
        const i32 requested = static_cast<i32>(f.args.next_u32());
        const i32 clamped = std::clamp(requested, 0, 100);
        device_level = static_cast<u32>(clamped);
        f.ret(0);
    });
    // #3(format, ...) -- the games' own printf.
    //
    // Variadic, which matters: a fixed arity here would read the wrong words
    // off the stack and produce something that looks like it works. The
    // firmware prologue at `0x1025cc50` is the textbook ARM varargs pattern
    // (LLE track):
    //
    //     push {r0,r1,r2,r3}     ; spill the argument registers
    //     push {r4,lr}
    //     ldr  r0,[sp,#8]        ; = the original r0, the format string
    //     add  r1,sp,#0xc        ; = &r1, where the varargs begin
    //     ...
    //     ldr  pc,[sp],#0x14
    //
    // So one fixed argument and the rest variadic, starting at r1.
    //
    // Worth having for its own sake rather than to unblock anything: it is the
    // game telling us what it thinks is happening, in its own words. The
    // semihosting channel did exactly this once and handed us the guest's own
    // "Arithmetic exception: Divide By Zero", which is what identified the
    // clock unit.
    registry.add(kModule, 3, "trace", 1, [](HleCallFrame& f) {
        const GuestAddr format_address = f.args.next_ptr();
        std::string format;
        for (u32 i = 0; i < 512; ++i) {
            const auto c = f.memory.read<u8>(format_address + i);
            if (!c.has_value() || *c == 0) {
                break;
            }
            format.push_back(static_cast<char>(*c));
        }
        if (format.empty()) {
            f.ret(0);
            return;
        }
        // r1-r3 then the stack, which is where the callee's own va_list points.
        RegisterThenStackArgs args(f.cpu, f.memory);
        const std::string text =
            format_guest(f.memory, format, args, /*wide=*/false);
        std::string trimmed = text;
        while (!trimmed.empty() &&
               (trimmed.back() == '\n' || trimmed.back() == '\r')) {
            trimmed.pop_back();
        }
        CLICKHLE_INFO(kCategory) << "guest: " << trimmed;
        f.ret(static_cast<u32>(text.size()));
    });

    fn(6, "device_level_get", 0, [](HleCallFrame& f) { f.ret(device_level); });

    // Constants. Named for what they return because that is all that was ever
    // established about them -- and among the busiest ordinals in the API,
    // #10 at 105 call sites and #11 at 90, so answering them is close to free
    // and unblocks a great deal.
    fn(7, "zero", 0, [](HleCallFrame& f) { f.ret(0); });
    fn(10, "thousand", 0, [](HleCallFrame& f) { f.ret(1000); });
    fn(11, "zero", 0, [](HleCallFrame& f) { f.ret(0); });

    // Wall-clock time, as a Unix timestamp.
    //
    // This used to write `microsecond_count() / 1e6` -- seconds since the
    // emulator STARTED, not a time of day. A title drawing a clock from it
    // therefore rendered nonsense: musika's status bar read **95:95**, an
    // impossible time, static across the whole run (identical at frame 7000
    // and 8000) because it reads this once.
    //
    // PAC-MAN was briefly a counter-example -- it calls #12 and looked fine --
    // but that was a misread on my part: its top-left shows "TOT 17", a game
    // counter, not a clock. Zoomed and confirmed. It has no clock to be wrong,
    // so it is no evidence that elapsed-seconds was right.
    //
    // A Unix timestamp, base + emulated elapsed -- the right KIND of value
    // for an ordinal named host_time, in place of the old "seconds since the
    // emulator started" (which made musika's status bar read the impossible
    // **95:95**, and would misrender any title's clock the same way).
    //
    // NOT the real host clock, deliberately. Two reasons:
    //   1. Reproducibility is a standing property here (see --clock-hz: guest
    //      time comes from work the guest has done, never the wall), and this
    //      project compares screenshots -- a host-clock base makes any frame
    //      containing a clock differ between runs.
    //   2. #12 turned out NOT to be musika's clock source after all --
    //      tested, returning a real timestamp left its status bar unchanged
    //      at 95:95 -- so there was nothing to gain from real time yet, only
    //      determinism to lose.
    //
    // So the default is a fixed, PLAUSIBLE, recognisable time: deliberately
    // 12:34:56 on a fixed date, chosen to be obviously a placeholder (nobody
    // mistakes 12:34 for a real capture) while still being a legitimate time
    // a title can format and do arithmetic on. Swap kPlaceholderEpoch for
    // std::time(nullptr) if/when a title is confirmed to actually read this
    // for display and real time is wanted -- CLICKHLE_EPOCH below already
    // supports that per-run without a code change.
    //
    //   CLICKHLE_EPOCH=<unix-seconds>  pin to a specific time
    //   CLICKHLE_EPOCH=now             use the real host clock
    fn(12, "host_time", 1, [](HleCallFrame& f) {
        static const u64 base = [] {
            // 2006-09-12 12:34:56 UTC.
            constexpr u64 kPlaceholderEpoch = 1158064496ull;
            if (const char* pin = std::getenv("CLICKHLE_EPOCH")) {
                if (std::strcmp(pin, "now") == 0) {
                    return static_cast<u64>(std::time(nullptr));
                }
                const long long v = std::strtoll(pin, nullptr, 10);
                if (v > 0) {
                    return static_cast<u64>(v);
                }
            }
            return kPlaceholderEpoch;
        }();
        const GuestAddr out = f.args.next_ptr();
        const u64 now = base + f.emulator.microsecond_count() / 1000000u;
        f.store<u32>(out, static_cast<u32>(now));
        CLICKHLE_DEBUG(kCategory) << "host_time -> " << now;
        f.ret(0);
    });

    // Battery. Reported full: an emulator is mains-powered, and a game that
    // dims itself or refuses to start on a flat battery would be wrong here.
    // Scale is 0..20, not 0..100 -- confirmed by both tracks independently.
    // LLE's disassembly: firmware negates anything > 20 (`cmp r0,#0x14` /
    // `rsbgt r0,r0,#0`). Oracle called directly into the tail at 0x0025e364
    // and measured the exact boundary: 20 passes through unchanged, 21
    // returns -21, 100 returns -100. So `f.ret(100)` -- this handler's
    // previous value -- delivered -100 to every title, below any low-battery
    // threshold. An emulator is mains-powered and should report full, which
    // this scale's maximum (20) means, not literal 100.
    fn(13, "host_battery", 0, [](HleCallFrame& f) { f.ret(20); });

    // resolve_name(...) -- four arguments, one of which is a name to look up.
    //
    // Answers "not found". What it resolves against is not established, and a
    // fabricated result would be handed straight back to the caller as though
    // it meant something. A game told the name is unknown can take its own
    // path; a game given a made-up handle cannot.
    //
    // "Not found" is -1, not 0 -- confirmed independently by both tracks:
    // LLE's return-convention sweep read it as a constant -1 in the
    // disassembly, and oracle's execution measurement agreed exactly, on
    // both firmware images. 0 is this interface's success shape elsewhere in
    // the corpus, so the previous f.ret(0) told every caller the opposite of
    // what the device says.
    fn(14, "resolve_name", 4, [](HleCallFrame& f) {
        (void)f.args.next_u32();
        const GuestAddr name = f.args.next_ptr();
        std::string text;
        for (u32 i = 0; i < 64; ++i) {
            const auto c = f.memory.read<u8>(name + i);
            if (!c.has_value() || *c == 0) {
                break;
            }
            text.push_back(static_cast<char>(*c));
        }
        CLICKHLE_DEBUG(kCategory)
            << "resolve_name(\"" << text << "\") -> not found";
        f.ret(static_cast<u32>(-1));
    });

    // The remaining miscTBD ordinals are unidentified. Deliberately not registered: an
    // unregistered ordinal stops the run and names itself, which is how the
    // next one gets found. See opengles.cpp for why stubs that return zero
    // would be worse than nothing.
}

}  // namespace clickhle

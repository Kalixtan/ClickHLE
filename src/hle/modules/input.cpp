// The "InputEvents" library: two ordinals, the whole input API.
//
// Two exports for an entire input system looks impossibly small until you see
// what the hardware is: a wheel and five buttons. There is no event queue and
// nothing to enumerate, so a single poll covers it.
//
// ## How the wheel encoding was established
//
// From Ms. Pac-Man's call site, which is the same in Sims Bowling and reads
// identically in the others:
//
//     mov  r1, sp ; add r0, sp, #4 ; bl <InputEvents#0>
//     ldr  r0, [sp]
//     and  r2, r0, #0xff          ; low byte
//     and  r1, r0, #0x40000000    ; bit 30
//     ldr  r0, [r8, #32]          ; the previous reading, kept in game state
//     lsr  r4, r1, #30            ; bit 30 as 0 or 1
//     sub  r1, r0, r2             ; previous - current
//     lsl  r5, r1, #24 ; asr r5, r5, #24    ; sign-extend to 8 bits
//
// Subtracting the previous reading and sign-extending the difference to eight
// bits is how you turn a 256-step *absolute* angular encoder into a signed
// step count, and it is only correct if the low byte wraps -- which is what
// makes this a wheel position rather than a counter. Bit 30 gates it: the game
// masks the delta with it, so it is the "finger is on the wheel" flag, and
// without it a release would read as an enormous jump.
//
// That is also why a d-pad cannot stand in for this. The games do not read a
// direction, they read a position and difference it, so what reaches gameplay
// is *velocity*. Anything driving this has to produce a plausible sequence of
// positions, not a pair of booleans.

#include "clickhle/hle/modules.h"

#include <cstdlib>
#include <string_view>
#include <utility>

#include "clickhle/common/log.h"
#include "clickhle/core/emulator.h"
#include "clickhle/hle/call_frame.h"
#include "clickhle/hle/registry.h"
#include "clickhle/video/display.h"

namespace clickhle {
namespace {

constexpr std::string_view kCategory = "input";
constexpr const char* kModule = "InputEvents";

/// Set while a finger is on the wheel. The games mask the computed delta with
/// this, so leaving it clear means "not touching" and suppresses motion.
constexpr u32 kWheelTouched = 1u << 30;

/// The wheel reports an absolute position in the low byte, so a full turn is
/// 256 steps and the value wraps.
constexpr u32 kWheelPositionMask = 0xFFu;

/// Steps the wheel advances per frame while a spin key is held.
///
/// A real thumb sweeps the ring at some rate; 6 of 256 steps per frame is about
/// a full turn every two seconds, which is a comfortable menu speed. Too fast
/// and a game's own acceleration makes it uncontrollable; too slow and long
/// lists take forever.
/// One encoder detent, in the units games actually read.
///
/// The hardware wheel has **96 positions** -- the firmware wraps the raw value
/// with a modulus of 0x60 -- and the transform onto the 256-step value games
/// difference scales by 8/3, which is exactly 256/96. So one physical detent
/// moves the read value by 256/96, and 3 is the nearest whole step.
///
/// The previous value of 6 was arbitrary and moved the wheel at roughly twice
/// detent speed. Nothing observably depended on it, but a title that scales a
/// menu by wheel delta would have scrolled twice as fast as on hardware.
constexpr i32 kSpinStep = 1;  ///< one detent; the transform does the rest

/// Polls between wheel event records. DEFAULT 1, i.e. one per poll.
///
/// Rate-limiting was the obvious fix for Zuma scrolling too fast, and it does
/// not work: measured over a 120-poll hold, div 1 traverses the whole menu and
/// opens the exit dialog, while div 4 and div 12 produce frames BYTE-IDENTICAL
/// to no input at all. Fewer events does not mean slower scrolling, it means no
/// scrolling -- so Zuma is not counting one event per menu entry and something
/// about the sequence matters that I do not yet understand.
///
/// Left at 1 because that is the only value observed to work. Do not raise it
/// as a "fix" for scroll speed without re-measuring; see task #59.
const u32 kWheelEventEveryNPolls = [] {
    const char* v = std::getenv("CLICKHLE_WHEEL_EVENT_DIV");
    const int n = v != nullptr ? std::atoi(v) : 1;
    return n > 0 ? static_cast<u32>(n) : 1u;
}();

/// The encoder's raw byte, and the transform onto what games read.
///
/// MEASURED, not guessed. The oracle swept every raw byte through the real
/// `InputEvents#0` on both RetailOS images and got 256/256 agreement with LLE's
/// closed form, so the transform below is firmware behaviour:
///
///     position = ((0x77 - raw) << 3) / 3 & 0xff
///
/// Three things fall out of it that our old model had wrong:
///
///  - **The position runs BACKWARDS.** raw 0x00 -> 0x3d, 0x01 -> 0x3a, 0x02 ->
///    0x38: increasing raw DECREASES the reported position. We were counting
///    up, so every title differencing successive readings got its scroll
///    velocity sign-inverted.
///  - **At rest the reading is 0x3d, not 0.** A title that reads a rest
///    position and differences against it started 61 steps out with our zero.
///  - **The wheel has 96 detents**, and the table repeats with that period:
///    raw 0x60 maps to 0x3d, the same as raw 0x00. Stepping raw by one detent
///    and running it through the transform reproduces that for free, where our
///    old fixed +3 on the position did not.
///
/// `0x77 -> 0` is the anchor that pins the constant, and it is measured rather
/// than fitted.
///
/// The firmware's own 256-entry table is a local research artifact and stays
/// out of this tree; this is the algorithm, which is not.
[[nodiscard]] u32 wheel_position_for_raw(u8 raw) {
    return (static_cast<u32>((0x77 - static_cast<i32>(raw)) << 3) / 3u) & 0xFFu;
}

/// Current wheel state.
///
/// Holds the RAW encoder byte. Everything a game sees is derived, because the
/// raw value is the thing the hardware actually has and the transform above is
/// what sits between it and the guest.
struct WheelState {
    u8 raw = 0;
    bool touched = false;

    [[nodiscard]] u32 position() const { return wheel_position_for_raw(raw); }

    [[nodiscard]] u32 raw_word() const {
        return static_cast<u32>(raw) | (touched ? kWheelTouched : 0u);
    }

    [[nodiscard]] u32 packed() const {
        // Contact is bit 30, passed through untouched and NOT part of the
        // transform -- the oracle measured 0x40000000 and 0x00 giving the same
        // position 61, and 0x40000040 and 0x40 the same 146.
        return position() | (touched ? kWheelTouched : 0u);
    }

    /// One physical detent. The ring has 96 of them and the raw byte wraps
    /// with that period.
    void spin(i32 detents) {
        i32 next = (static_cast<i32>(raw) + detents) % 96;
        if (next < 0) {
            next += 96;
        }
        raw = static_cast<u8>(next);
    }
};

/// Mirror the wheel worker that feeds InputEvents#0's self-consuming counter.
void update_wheel_counter(Emulator::InputState& state, u32 current_word) {
    const u32 previous_word = state.previous_raw_wheel;
    state.previous_raw_wheel = current_word;
    if ((current_word & kWheelTouched) == 0 ||
        (previous_word & kWheelTouched) == 0) {
        return;
    }

    i32 delta = static_cast<i32>(current_word & kWheelPositionMask) -
                static_cast<i32>(previous_word & kWheelPositionMask);
    if (delta > 48) {
        delta -= 96;
    } else if (delta < -48) {
        delta += 96;
    }

    if (std::abs(delta) < static_cast<i32>(state.wheel_threshold)) {
        state.wheel_accumulator += delta;
        if (state.wheel_accumulator < -3 || state.wheel_accumulator > 3) {
            state.wheel_threshold = 0;
            state.wheel_accumulator = 0;
        }
    } else if (delta >= -35 && delta <= 35) {
        state.wheel_event_counter += static_cast<u32>(delta);
        state.wheel_accumulator = 0;
    }
}

/// Which controls are currently held, indexed by Button.
///
/// The display hands out *transitions*; the wheel needs *state*, because it
/// advances for as long as a spin is held rather than once per press. Nothing
/// else in the tree was draining the event queue, so this is also where guest
/// input begins to exist at all.
/// Added to the record type when sweeping for the id->type mapping.
u32 g_event_type_base = 0;

/// The guest event list carries both transitions from the host input queue:
/// phase 2 for press and phase 1 for release. `edge` and `held` remain useful
/// probe modes, but `both` is the default because RetailOS queues transitions.
std::string_view button_record_mode() {
    static const std::string_view mode = [] {
        const char* value = std::getenv("CLICKHLE_RECORD_MODE");
        return std::string_view(value != nullptr ? value : "both");
    }();
    return mode;
}

/// Which 12-byte record type each control posts.
///
/// PLACEHOLDER, and deliberately marked as one. The firmware's chain is
/// hardware bit -> button id -> OS code -> record type, and the id->code step
/// reads a byte table at `0x1081dac1` which is **populated at run time** (LLE,
/// INPUT.md), with a second remap when a flag byte is set. It is not
/// determinable statically, and LLE's recommendation is to find it empirically,
/// one control at a time.
///
/// The types themselves are firmware fact: the pump emits 1, 2, 3, 4 or 5 (from
/// OS codes 0x6e, 0x6f, 0x5a, 0x59, 0x10), and anything else is 0, which
/// `InputEvents#1` ignores. So these five values are the whole space; only the
/// assignment to physical controls is open.
/// The five physical controls, and only those. ActionEast/ActionWest are the
/// keyboard's stand-in for rotating the ring -- they are not buttons on the
/// device and must not post a button record, or a spin would read as a press.
u8 button_event_id(Button button) {
    // Values in {1,3,4,5}. NOT 0..4, which is what this used to emit.
    //
    // Measured by the oracle against `InputEvents#1`, the dispatcher, with the
    // instruction count beside each because a return value alone cannot tell a
    // handled call from a rejected one:
    //
    //     +0x00 = 0    Returned r0=0     16 instr   REJECTED
    //     +0x00 = 1    Lost              2466       handled
    //     +0x00 = 2    Returned r0=2     16         REJECTED
    //     +0x00 = 3    Returned r0=0     2765       handled
    //     +0x00 = 4    Returned r0=0     2768       handled
    //     +0x00 = 5    Returned r0=0     2771       handled
    //     +0x00 = 6+   Returned r0=id    16         REJECTED
    //
    // A rejected id is returned to the caller unchanged, so a non-zero return
    // from #1 means "I did not handle this", not a status.
    //
    // This also matches LLE's producer-side table, where the pump emits 1..5
    // from OS codes 0x6e/0x6f/0x5a/0x59/0x10 and #1 has no case for 2. Two
    // independent routes to the same four values.
    //
    // WHICH control maps to WHICH id is still open: the id->code step reads a
    // RAM table at 0x1081dac1 with a flag-gated remap on top, so it is not
    // determinable statically. CLICKHLE_SELECT_ID overrides Select's id for
    // sweeping, which is now a four-value search rather than open-ended.
    switch (button) {
        // FIVE DISTINCT IDS. This table previously gave JoypadNorth id 1 and
        // let Select default to 1 as well -- a collision, so "tap up" and
        // Select were indistinguishable to the guest and the five controls only
        // ever spanned four ids. Every tap sweep run before this is invalid for
        // JoypadNorth.
        //
        // SUPERSEDES the "Select = 1" claim below (and its 2026-09-19 sweep):
        // that sweep ran through the dead duplicate-record loop
        // (`queue_input_event(2 + (i % 4), ...)`, see the "BUG THAT COST DAYS"
        // comment further down in this file), which injected a phantom
        // `[id=2 type=1]` record alongside every real Select press. With that
        // loop removed, a clean single-tap-per-id re-sweep from the actual main
        // menu gives the opposite mapping:
        //
        //     id 1   exit dialog directly            Menu/Back
        //     id 2   menu -> level select -> instructions, one clean tap each   Select
        //     id 3,4,5   no visible effect from the menu   unverified
        //
        // Select = 2 is now the verified value. JoypadNorth = 1 follows the
        // same re-sweep (id 1's exit-dialog behaviour matches its "Menu" role).
        // JoypadEast/South/West (3,4,5) were never re-verified under clean
        // delivery and remain placeholders assigned around the two verified
        // values.
        case Button::JoypadNorth: return 1;   // Menu, top tap
        case Button::JoypadEast: return 3;    // Forward, right tap
        case Button::JoypadSouth: return 4;   // Play, bottom tap
        case Button::JoypadWest: return 5;    // Back, left tap
        case Button::ActionNorth: {           // Select, the centre button
            if (const char* v = std::getenv("CLICKHLE_SELECT_ID")) {
                return static_cast<u8>(std::atoi(v));
            }
            return 2;
        }
        default: return 0xff;
    }
}

/// Re-posts a press record for every control currently held.
///
/// oracle measured (2026-09-19, after retracting the toggle claim) that genuine
/// repeated delivery of the same press is **idempotent**, not a toggle: chaining
/// two real id=2 type=1 records in one call leaves FLAGS=1, the second does not
/// flip it back. So re-posting while held is safe, which their earlier toggle
/// reading had ruled out.
///
/// Only under CLICKHLE_RECORD_MODE=held. Real hardware posts on transitions, so
/// this is a deliberate deviation kept for screens that appear to want a level
/// rather than an edge.
void repost_held(Emulator& emulator, Emulator::InputState& state,
                 std::string_view mode,
                 u8 (*id_of)(Button)) {
    if (mode != "held") {
        return;
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(Button::Count); ++i) {
        if (!state.held_buttons[i]) {
            continue;
        }
        const u8 id = id_of(static_cast<Button>(i));
        if (id == 0xff) {
            continue;
        }
        emulator.queue_input_event(id, emulator.tick_count(), 2u);
    }
}

void drain_input(Emulator& emulator) {
    auto& input_state = emulator.input_state();
    Display& display = emulator.display();
    InputEvent event;
    while (display.next_input(event)) {
        if (event.button == Button::Count) {
            continue;
        }
        input_state.held_buttons[static_cast<std::size_t>(event.button)] =
            event.pressed;
        CLICKHLE_DEBUG(kCategory)
            << "button " << static_cast<int>(event.button)
            << (event.pressed ? " down" : " up");

        // Post a real 12-byte record as well as setting the poll bit.
        //
        // This was the gap: nothing in the button path ever called
        // queue_input_event, so a press produced ONLY a poll bit and never a
        // record on app+0x30. Hardware produces both -- the ISR posts into the
        // OS event system and the pump turns that into a record -- so a title
        // that reads the event list rather than polling saw nothing at all. It
        // is why --press and every --force-buttons mask moved no title: there
        // was no event to find, not a wrong one.
        //
        // Press only. The pump emits a record per transition it recognises, and
        // a release carries no type of its own in the five-code table; the
        // held state is what the poll reports.
        //
        // The payload is a millisecond timestamp, not a keycode (LLE: the pump
        // passes `to_millis(ts, 1000)`), and the gate must be 1 -- gate 2 is
        // source kind 4 and `InputEvents#1` requires `event[1] == 1`.
        // record[+0x00] = BUTTON ID (0-5), record[+0x01] = 1 release / 2 press.
        //
        // CORRECTED 2026-09-19 from oracle relaying LLE: these fields are not
        // "type" and "gate". I had been writing the TYPE where the id belongs,
        // and gate 1 always -- which in the real encoding means RELEASE. So
        // every button press ClickHLE has ever delivered arrived as a release,
        // and an id out of range is dropped by the bounds check at 5 anyway.
        //
        // Both transitions are posted now, because release is a real event here
        // rather than something to suppress.
        const u8 id = button_event_id(event.button);
        if (id == 0xff) {
            continue;  // not a device control; the ring is polled, not posted
        }
        // ONE record per press, on the edge. NOT one per transition.
        //
        // oracle called Zuma's own `handle_events` (0x18011d3c) directly with a
        // synthesized record and found the bit is a **TOGGLE on delivery**, not
        // a press/release encoding:
        //
        //     FLAGS=0, id=2, type=1  -> bit 0x01 SET
        //     FLAGS=0, id=2, type=2  -> bit 0x01 SET
        //     FLAGS=1, id=2, type=1  -> bit 0x01 CLEARED
        //     FLAGS=1, id=2, type=2  -> bit 0x01 CLEARED
        //
        // Payload made no difference (0, 1, 0xffffffff identical). Each record
        // flips the bit from whatever it already was.
        //
        // So posting a record on press AND on release flipped the bit twice per
        // button press and left it exactly where it started -- records arriving,
        // being read every frame, and having zero net effect. That is precisely
        // the symptom Zuma's stage screen showed.
        //
        // I introduced this when applying LLE's "1 = release / 2 = press"
        // correction: I made both transitions post, reasoning that release was
        // "a real event rather than something to suppress". Under toggle
        // semantics that reasoning is exactly backwards.
        // TWO COMPETING MODELS, selectable, because oracle and LLE disagree and
        // I can test where neither of them can.
        //
        //   oracle, by calling handle_events with a synthesized record:
        //     the bit TOGGLES on delivery. FLAGS=0 + id=2 sets it for type 1
        //     AND type 2; FLAGS=1 + id=2 clears it for both. So one record per
        //     press edge, and posting on release too would cancel it.
        //
        //   LLE, by re-deriving the per-record logic independently (106/106):
        //     it is UNCONDITIONAL -- press sets bit 0x01, release clears it,
        //     neither depending on the starting value. They believe oracle's
        //     test exercised a separate value-dependent XOR over a pending
        //     bitmask at input_object+0xc, not the record path.
        //
        // CLICKHLE_RECORD_MODE=edge  -> one record on press only        (probe)
        // CLICKHLE_RECORD_MODE=both  -> press phase 2, release phase 1  (RetailOS)
        //
        // Default is `both`: the frame driver publishes the transitions it
        // receives from the OS event queue. `edge` is available for probing
        // guests that appear to consume only the press transition.
        static const std::string_view record_mode = [] {
            return button_record_mode();
        }();
        const bool post_release = record_mode == "both" || record_mode == "held";
        if (!event.pressed && !post_release) {
            continue;
        }
        // +0x01 must be 1. Measured: the dispatcher loads +0x01, compares, and
        // returns in FIVE instructions for any value other than 1 -- including
        // 2. A record with +0x01 = 2 is discarded, unread beyond that byte.
        //
        // This does not flatly contradict LLE's "1 = release, 2 = press": that
        // came from Mahjong's CONSUMER walking app+0x30, and this is the
        // outbound dispatcher. Both can hold -- a consumer may accept 1 and 2
        // where the dispatcher accepts only 1. But 2 is measured as dropped,
        // and 0..4 ids were measured as half-rejected, so the combination I was
        // emitting could not work either way.
        u8 phase = post_release ? (event.pressed ? 2u : 1u) : 1u;
        if (const char* g = std::getenv("CLICKHLE_EVENT_GATE")) {
            phase = static_cast<u8>(std::atoi(g));
        }
        u32 payload = emulator.tick_count();
        if (const char* pl = std::getenv("CLICKHLE_EVENT_PAYLOAD")) {
            payload = static_cast<u32>(std::strtoul(pl, nullptr, 0));
        }
        emulator.queue_input_event(
            static_cast<u8>(g_event_type_base != 0
                                ? (g_event_type_base + id) & 0xff
                                : id),
            payload, phase);
    }
}

[[nodiscard]] bool held(const Emulator::InputState& state, Button button) {
    return state.held_buttons[static_cast<std::size_t>(button)];
}

}  // namespace

namespace {
/// A button mask forced into every poll, pulsed on and off.
///
/// The click wheel's five controls are Select, Menu, Play/Pause, Next and
/// Previous, and which bit carries which is still unestablished -- the five
/// host keys are wired to bits 0..4 as a placeholder. Guessing a mapping and
/// testing it a title at a time is slow; this drives the word directly so the
/// bit can be found by bisection. Pulsed rather than held because a menu reacts
/// to the press, not to the level.
u32 g_forced_buttons = 0;
u32 g_forced_wheel = 0;
/// Base added to the firmware's button id to form the event type byte.
///
/// The firmware's dispatcher maps hardware bit -> id (0..5). What the *game*
/// receives as `record[0]` is one translation further on, through a pump the
/// LLE track has not read yet, so the offset is unknown and swept rather than
/// guessed.
u32 g_forced_period = 0;
}  // namespace

void set_forced_buttons(u32 mask, u32 period) {
    g_forced_buttons = mask;
    g_forced_period = period;
}

void set_forced_wheel(u32 mask) { g_forced_wheel = mask; }

void set_event_type_base(u32 base) { g_event_type_base = base; }

void register_input(HleRegistry& registry) {
    const auto fn = [&registry](u32 ordinal, std::string name, u8 arity,
                                HleHandler handler) {
        registry.add(kModule, ordinal, std::move(name), arity,
                     std::move(handler));
    };

    // poll(u32* out_a, u32* out_b)
    fn(0, "poll_input", 2, [](HleCallFrame& f) {
        const GuestAddr out_a = f.args.next_ptr();
        const GuestAddr out_b = f.args.next_ptr();
        // The wheel word goes to the SECOND argument. Both call shapes agree
        // once the whole prologue is read: iQuiz sets `r1 = sp + 4`, passes
        // `r0 = sp`, and reads `[sp + 4]`; Ms. Pac-Man passes `(sp + 4, sp)`
        // and reads `[sp]`. Different registers, same conclusion -- each reads
        // whatever it put in r1.
        // Advance the wheel before answering. A game reads position and
        // differences it, so motion has to happen between polls -- a wheel that
        // only moves when a key is pressed and released would deliver a single
        // step no matter how long it was held.
        drain_input(f.emulator);
        auto& input_state = f.emulator.input_state();
        {
            static const std::string_view mode = button_record_mode();
            repost_held(f.emulator, input_state, mode, &button_event_id);
        }
        WheelState wheel{input_state.wheel_raw, input_state.wheel_touched};
        const bool clockwise = held(input_state, Button::ActionEast);
        const bool anticlockwise = held(input_state, Button::ActionWest);
        const bool was_touched = wheel.touched;
        wheel.touched = clockwise || anticlockwise;
        if (clockwise != anticlockwise) {
            // kSpinStep is one physical detent, which is correct per poll ONLY
            // if polls happen at hardware rate. Headless we poll as fast as the
            // host allows; interactively the interpreter runs well below 60fps,
            // so the same hold produces far fewer detents and the menu barely
            // moves. That is a rate error, not a mapping error -- scrolling
            // works, it is just too slow to use.
            //
            // CLICKHLE_WHEEL_STEP multiplies the detents per poll. Default 1 so
            // headless runs and the screenshot baselines are unchanged.
            static const i32 multiplier = [] {
                const char* v = std::getenv("CLICKHLE_WHEEL_STEP");
                const i32 n = v != nullptr ? std::atoi(v) : 1;
                return n > 0 ? n : 1;
            }();
            wheel.spin((clockwise ? kSpinStep : -kSpinStep) * multiplier);
        }
        // A finger leaving the wheel ZEROES the raw word, it does not merely
        // clear bit 30.
        //
        // LLE, 2026-09-18: the hardware path `0x10277510` sets bit 30 from the
        // register and writes `block[0x24] = 0` outright on release. We were
        // clearing the touch flag and keeping the last position, so after a
        // release the guest kept reading a stale non-zero position with the
        // contact bit clear -- a state the hardware never produces.
        //
        // This module's own header already said why that matters: "releasing
        // must clear it, or the next press reads as one enormous jump". The
        // rule was written down and only half implemented.
        if (was_touched && !wheel.touched) {
            wheel.raw = 0;
            input_state.wheel_threshold = 3;
            input_state.wheel_accumulator = 0;
            CLICKHLE_DEBUG(kCategory) << "wheel released, raw word zeroed";
        }

        if (wheel.touched) {
            CLICKHLE_DEBUG(kCategory)
                << "wheel " << wheel.position() << (wheel.touched ? " touched" : "");
        }
        input_state.wheel_raw = wheel.raw;
        input_state.wheel_touched = wheel.touched;
        const u32 raw_wheel_word = wheel.raw_word();
        u32 wheel_word = wheel.packed();
        u32 counter_source = raw_wheel_word;
        if (g_forced_wheel != 0 && g_forced_period != 0 &&
            (input_state.poll_count / g_forced_period) % 2 == 0) {
            wheel_word |= g_forced_wheel;
            counter_source |= g_forced_wheel;
        }
        update_wheel_counter(input_state, counter_source);
        // THE BUG THAT COST DAYS ON ZUMA'S STAGE/INSTRUCTIONS SCREENS.
        //
        // This whole block used to be here: a stale, pre-id/type-model loop
        // over the five taps, posting `queue_input_event(2 + (i % 4), ...)` on
        // every held-state CHANGE, plus a second stale post of
        // `queue_input_event(1, g_wheel.position())` while the wheel was
        // touched. Both predate `button_event_id`/`drain_input`/`repost_held`
        // above, which is the actual, correct delivery path -- this ran IN
        // PARALLEL with it and was never removed.
        //
        // Its computed output (`events`) never reached the guest: `out_a` is
        // written from the firmware counter below, not from a button mask.
        // So the loop was doing
        // nothing useful and, on every Select press, injecting an UNINTENDED
        // second record -- `taps[4] = ActionNorth`, `2 + (4 % 4) = 2` -- right
        // alongside the real one.
        //
        // Found by walking handle_events's own record list on the exact call
        // that produced an unexplained FLAGS bit (LLE + oracle, 2026-09-19):
        // every single Select press carried TWO records, `[id=1 ...]` (mine)
        // and `[id=2 type=1]` (this dead loop's). `touched` therefore always
        // included bit 0x01 alongside bit 0x10, and `handle_events`'s stage-3
        // edge compensation treated that spurious touch as "didn't move this
        // call" and forced a flip -- corrupting `c->buttons` to `0x11` on
        // every Select press, menu included. If the Instructions screen reads
        // `c->buttons` as an EXACT match against the confirm value, a
        // permanently-polluted low bit would fail that check on every single
        // press, which is exactly the symptom: Select worked well enough for
        // the menu's presumably looser check and never once satisfied
        // whatever the Instructions screen tests.
        //
        // `g_forced_buttons`/`--force-buttons` remains inert: this API has no
        // button-mask output. It should be removed from the CLI or redirected
        // to the real event path in a separate change.

        // The first word is a SIGNED EVENT COUNT, not a button mask.
        //
        // Measured by the oracle against real firmware (2026-09-18), by setting
        // the state block and calling the actual poll:
        //
        //     *out_a = block[0x10] - block[0x14]     counter - snapshot
        //     block[0x14] = block[0x10]              consumed on every poll
        //
        //     counter 7, snapshot 3 -> 4
        //     counter 5, snapshot 5 -> 0
        //     counter 2, snapshot 9 -> 0xfffffff9, i.e. -7
        //
        // The subtract is SIGNED and the result can be negative -- it does not
        // saturate and does not wrap to a large positive. A consumer reading it
        // as unsigned, or as a bitmask, sees 4294967289: simultaneously "every
        // bit set" and an enormous event count. That is the exact shape of a
        // title appearing to have every button held at once.
        //
        // The stale comment here said "the first word carries the buttons",
        // which is what this code was originally built on and is wrong.
        ++input_state.poll_count;
        // out_a is the self-consuming firmware wheel-event counter delta,
        // not a button mask or a direct wheel-position delta.
        //
        // LLE, 2026-09-18: `InputEvents#0` consumes block[0x10] - block[0x14].
        // The wheel worker feeds that counter after checking contact, wrapping
        // raw deltas across 96 positions, applying a small deadband, and
        // discarding steps beyond +/-35. Buttons do not come through here.
        //
        //     *out_delta = block[0x10] - block[0x14];   movement since last poll
        //     block[0x14] = block[0x10];                consumed by the call
        //
        // Self-consuming, so a second poll in the same frame reports zero, and
        // signed, so it can be negative -- the oracle measured 2 - 9 giving
        // 0xfffffff9 rather than a saturate or a wrap.
        //
        const u32 counter_delta = input_state.wheel_event_counter -
                                  input_state.wheel_counter_snapshot;
        // Keep the firmware's store order: it writes the counter result before
        // transforming the wheel word, then consumes the snapshot and clears
        // the pending byte only after both output stores succeed.
        if (!f.store<u32>(out_a, counter_delta)) {
            f.cpu.request_stop();
            return;
        }
        if (!f.store<u32>(out_b, wheel_word)) {
            f.cpu.request_stop();
            return;
        }
        input_state.wheel_counter_snapshot = input_state.wheel_event_counter;
        input_state.pending_event_flag = 0;
    });

    // #1(event) -- OUTBOUND. The game posting an event to the OS, not the OS
    // delivering one to the game.
    //
    // Fully decompiled by the LLE track:
    //
    //     void InputEvents_1(char *event) {
    //         if (event[1] != 1) return;
    //         switch (event[0]) {
    //           case 1:  o = alloc(ctx(), 0xc);
    //                    o = event_init(o, 0x66000002, 0, 0);
    //                    post(o, target(), 1, 0, 0);  return;
    //           case 3:  notify(0x66000009); return;
    //           case 4:  notify(0x6600000a); return;
    //           case 5:  notify(0x6600000b); return;
    //         }
    //     }
    //
    // This is the only gap all 20 titles reach. It is *not* the button path --
    // that reading was retracted, and buttons travel through neither
    // InputEvents export.
    //
    // We have no OS event bus for these to be posted to, and inventing one
    // would be fabricating a subscriber. What is reproduced is the part that is
    // observable to the guest: the gate, the type dispatch, and returning. The
    // message ID is logged, because a title posting one is telling us something
    // about its state and that is worth not throwing away.
    fn(1, "post_event", 1, [](HleCallFrame& f) {
        const GuestAddr event = f.args.next_ptr();
        const auto type = f.memory.read<u8>(event);
        const auto gate = f.memory.read<u8>(event + 1);
        if (!type.has_value() || !gate.has_value()) {
            f.cpu.request_stop();
            return;
        }
        if (*gate != 1) {
            f.ret_void();
            return;
        }
        u32 message = 0;
        switch (*type) {
            case 1: message = 0x66000002; break;
            case 3: message = 0x66000009; break;
            case 4: message = 0x6600000a; break;
            case 5: message = 0x6600000b; break;
            default:
                // The firmware falls through and does nothing. A type outside
                // 1/3/4/5 is not an error, so this is silent rather than a
                // warning -- but it is worth knowing if one ever appears.
                CLICKHLE_DEBUG(kCategory)
                    << "post_event: unhandled type " << static_cast<u32>(*type);
                f.ret_void();
                return;
        }
        CLICKHLE_DEBUG(kCategory)
            << "post_event type " << static_cast<u32>(*type) << " -> message 0x"
            << std::hex << message << std::dec;
        f.ret_void();
    });

    CLICKHLE_DEBUG(kCategory) << kModule << ": wheel poll and event post registered";
}

}  // namespace clickhle

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

#include <array>
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

/// Raw wheel detents per input poll for the legacy scripted spin controls.
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
    const i32 scaled = (0x77 - static_cast<i32>(raw)) * 8;
    return static_cast<u32>(scaled / 3) & 0xffu;
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
    // RetailOS 0x100dc0b4 uses 0x48 as the half-range cutoff.
    if (delta > 72) {
        delta -= 96;
    } else if (delta < -72) {
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
/// ActionEast/ActionWest in legacy scripts rotate the ring; they are not
/// physical buttons and must not post button records.
// Rockbox's PP502x register read names bits 8..12: Select, Right, Left,
// Play/Pause, Menu. RetailOS shifts them down to bits 0..4 and maps them to
// internal IDs at 0x100c9204. Bit 5 has no confirmed physical label.
constexpr std::array<u8, 6> kHardwareBitToId{3, 1, 2, 4, 0, 5};
// The RetailOS ID-to-OS-code table is populated in RAM. These game-facing
// types preserve the observed Select/Menu behavior and Fliwheel's directional
// receipts, but remain provisional until that table is measured on hardware.
constexpr std::array<u8, 6> kIdToEventType{1, 4, 3, 2, 5, 0xff};

u8 hardware_bit_for_button(Button button) {
    switch (button) {
        case Button::ActionNorth: return 0; // Select
        case Button::JoypadEast: return 1;  // Next / Right
        case Button::JoypadWest: return 2;  // Previous / Left
        case Button::JoypadSouth: return 3; // Play / Pause
        case Button::JoypadNorth: return 4; // Menu
        default: return 0xff;
    }
}

u8 event_type_for_button(Button button) {
    const u8 bit = hardware_bit_for_button(button);
    if (bit == 0xff) return 0xff;
    if (button == Button::ActionNorth) {
        if (const char* override = std::getenv("CLICKHLE_SELECT_ID"))
            return static_cast<u8>(std::atoi(override));
    }
    return kIdToEventType[kHardwareBitToId[bit]];
}

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

// The ISR's six button bits map to these IDs at 0x100c9204. The later
// ID-to-OS-code table is populated at runtime and remains unverified here.
void post_button_record(Emulator& emulator, u8 event_type, bool pressed) {
    if (event_type == 0xff) return;
    const std::string_view mode = button_record_mode();
    if (!pressed && mode != "both" && mode != "held") return;
    u8 phase = (mode == "both" || mode == "held") ? (pressed ? 2u : 1u) : 1u;
    if (const char* override = std::getenv("CLICKHLE_EVENT_GATE"))
        phase = static_cast<u8>(std::atoi(override));
    u32 payload = emulator.tick_count();
    if (const char* override = std::getenv("CLICKHLE_EVENT_PAYLOAD"))
        payload = static_cast<u32>(std::strtoul(override, nullptr, 0));
    // The frame driver's 12-byte list node carries type, phase and timestamp.
    // The physical bit to internal ID is proven; the later OS-code translation
    // into this game-facing type is still provisional.
    emulator.queue_input_event(
        static_cast<u8>(g_event_type_base != 0
                            ? (g_event_type_base + event_type) & 0xff
                            : event_type),
        payload, phase);
}

void apply_hardware_bit_internal(Emulator& emulator, u8 bit, bool pressed) {
    if (bit >= kHardwareBitToId.size()) return;
    auto& state = emulator.input_state();
    const u32 mask = 1u << bit;
    if (((state.hardware_button_mask & mask) != 0) == pressed) return;
    if (pressed) state.hardware_button_mask |= mask;
    else state.hardware_button_mask &= ~mask;
    post_button_record(emulator, kIdToEventType[kHardwareBitToId[bit]], pressed);
}

void apply_button_transition(Emulator& emulator, InputEvent event) {
    if (event.button == Button::Count) return;
    emulator.input_state().held_buttons[static_cast<std::size_t>(event.button)] =
        event.pressed;
    const u8 bit = hardware_bit_for_button(event.button);
    if (bit != 0xff) emulator.set_hardware_button(bit, event.pressed);
}

void drain_input(Emulator& emulator) {
    InputEvent event;
    while (emulator.display().next_input(event))
        apply_button_transition(emulator, event);
}

[[nodiscard]] bool held(const Emulator::InputState& state, Button button) {
    return state.held_buttons[static_cast<std::size_t>(button)];
}

}  // namespace

void apply_hardware_button(Emulator& emulator, Button button, bool pressed) {
    apply_button_transition(emulator, {button, pressed});
}

void apply_hardware_bit(Emulator& emulator, u8 bit, bool pressed) {
    apply_hardware_bit_internal(emulator, bit, pressed);
}

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

void poll_hardware_input(Emulator& emulator) {
    drain_input(emulator);
    auto& state = emulator.input_state();
    const bool clockwise = held(state, Button::ActionEast);
    const bool anticlockwise = held(state, Button::ActionWest);
    if (clockwise != anticlockwise) {
        static const i32 multiplier = [] {
            const char* value = std::getenv("CLICKHLE_WHEEL_STEP");
            const i32 n = value != nullptr ? std::atoi(value) : 1;
            return n > 0 ? n : 1;
        }();
        emulator.move_hardware_wheel(
            (clockwise ? kSpinStep : -kSpinStep) * multiplier);
        state.synthetic_wheel_active = true;
    } else if (state.synthetic_wheel_active) {
        emulator.release_hardware_wheel();
        state.synthetic_wheel_active = false;
    }

    const u32 port = emulator.sample_hardware_port();
    const bool locked = emulator.device_settings().hold_switch;
    // Rockbox validates this mask; RetailOS checks the same signature
    // through a mask loaded at runtime. Other packets leave this state.
    if (locked || (port & 0x800000ffu) == 0x8000001au) {
        const u8 buttons = locked ? 0 : static_cast<u8>((port >> 8) & 0x3f);
        for (u8 bit = 0; bit < 6; ++bit)
            apply_hardware_bit_internal(emulator, bit,
                                        (buttons & (1u << bit)) != 0);
        state.hardware_wheel_raw = !locked && (port & kWheelTouched)
            ? std::optional<u8>{static_cast<u8>((port >> 16) & 0x7f)}
            : std::nullopt;
    }
    static const std::string_view mode = button_record_mode();
    if (!locked) repost_held(emulator, state, mode, &event_type_for_button);

    WheelState wheel{state.wheel_raw, state.wheel_touched};
    const bool was_touched = wheel.touched;
    wheel.touched = state.hardware_wheel_raw.has_value();
    if (wheel.touched) wheel.raw = *state.hardware_wheel_raw;
    if (was_touched && !wheel.touched) {
        wheel.raw = 0;
        state.wheel_threshold = 3;
        state.wheel_accumulator = 0;
    }
    state.wheel_raw = wheel.raw;
    state.wheel_touched = wheel.touched;
    update_wheel_counter(state, wheel.raw_word());
}

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
        // RetailOS's input worker has already sampled the port and updated
        // the button list and wheel counter for this callback. This API reads
        // that cached state; it does not read the physical register again.
        auto& input_state = f.emulator.input_state();
        u32 wheel_word = WheelState{input_state.wheel_raw,
                                    input_state.wheel_touched}.packed();
        if (g_forced_wheel != 0 && g_forced_period != 0 &&
            (input_state.poll_count / g_forced_period) % 2 == 0)
            wheel_word |= g_forced_wheel;
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

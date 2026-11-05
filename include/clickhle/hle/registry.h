#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "clickhle/common/types.h"
#include "clickhle/hle/call_frame.h"
#include "clickhle/loader/eapp_image.h"

namespace clickhle {

using HleHandler = std::function<void(HleCallFrame&)>;

struct HleFunction {
    std::string module;  ///< As the image spells it, e.g. "OpenGLES".
    std::string name;    ///< Always set, even for ordinal-only exports.
    u32 ordinal = 0;     ///< Meaningful only when has_ordinal is set.
    bool has_ordinal = false;  ///< Ordinal 0 is a real ordinal; see add().
    u8 arity = 0;        ///< Argument words; used for tracing, not dispatch.
    HleHandler handler;

    [[nodiscard]] std::string key() const;
};

/// The table of implemented guest APIs. This is the thing that gets filled out
/// over the life of the project: 179 entries is the whole surface a
/// click-wheel game can reach.
///
/// Ordinals live in our own source rather than in a table shipped with the
/// emulator, deliberately. Games import by ordinal only, and the map from
/// ordinal to meaning is recoverable only from Apple's firmware -- which we
/// must not redistribute. Each function we implement declares the one ordinal
/// it answers to, which is our code, not theirs. (The optional name file loaded
/// by the CLI is only used to make reports about *unimplemented* ordinals
/// readable, and the user generates it from their own firmware copy.)
class HleRegistry {
public:
    void add(std::string module, u32 ordinal, std::string name, u8 arity,
             HleHandler handler);

    /// For modules imported by name rather than ordinal.
    /// Registers a function that owns its storage here but is deliberately
    /// *not* reachable by ordinal or name.
    ///
    /// This is what --probe needs. Going through add() cannot work: the
    /// duplicate guard keeps the first registration, so the probe was silently
    /// dropped and the run reported it installed. Going through an overwriting
    /// add() would be worse -- it would make the probed ordinal count as
    /// implemented, which is exactly the lie the probe exists to avoid.
    ///
    /// The caller rebinds the thunk to the returned pointer.
    [[nodiscard]] const HleFunction* add_shadow(std::string module, u32 ordinal,
                                                std::string name, u8 arity,
                                                HleHandler handler);

    void add_named(std::string module, std::string name, u8 arity,
                   HleHandler handler);

    [[nodiscard]] const HleFunction* find(std::string_view module,
                                          u32 ordinal) const;
    [[nodiscard]] const HleFunction* find(std::string_view module,
                                          std::string_view name) const;
    [[nodiscard]] const HleFunction* resolve(const GuestImport& import) const;

    [[nodiscard]] std::size_t size() const noexcept { return functions_.size(); }
    [[nodiscard]] std::vector<const HleFunction*> all() const;

private:
    std::vector<std::unique_ptr<HleFunction>> functions_;
    std::map<std::string, const HleFunction*, std::less<>> by_ordinal_;
    std::map<std::string, const HleFunction*, std::less<>> by_name_;
};

/// The synthetic page of `SVC` instructions that guest imports branch to.
///
/// Each imported function gets four bytes containing `SVC #n`, and the image's
/// IAT slot is pointed at it. When the guest calls the import it lands on the
/// SVC, the core traps to our handler, and we dispatch on `n`. One trap
/// instruction, no per-instruction execution hooks, and the same mechanism
/// works on any ARM core -- mGBA exposes it as ARMInterruptHandler::swi32 and
/// dynarmic as UserCallbacks::CallSVC. That is what keeps the cores
/// interchangeable.
class ThunkTable {
public:
    static constexpr GuestAddr kBase = 0xF0000000u;
    static constexpr u32 kSize = 64u * 1024u;
    static constexpr u32 kSlotSize = 4u;

    /// SVC immediate reserved to mean "a host-to-guest call has returned".
    /// Host code sets lr to kReturnSentinel before entering guest code and
    /// stops when it is reached.
    static constexpr u32 kReturnSvc = 0xFFFFFFu;

    /// Bytes kept free at the end of the page.
    ///
    /// The sentinel must not be the last instruction in the mapping: ARM's
    /// three-stage pipeline prefetches up to two instructions past the one it
    /// is executing, so a sentinel in the final slot faults on a prefetch of
    /// the word after it -- an "unmapped read at 0xF0010000" that has nothing
    /// to do with anything the guest did.
    static constexpr u32 kTailGuard = 16;
    static constexpr GuestAddr kReturnSentinel = kBase + kSize - kTailGuard;

    struct Entry {
        std::string import_key;      ///< "OpenGLES#42"
        const HleFunction* function; ///< Null when unimplemented.
        GuestAddr address = 0;
    };

    [[nodiscard]] bool initialise(AddressSpace& memory);

    /// Allocates a slot, writes the SVC instruction, and returns its address.
    /// Repeated binds of the same import key reuse the same slot, so a game
    /// importing memcpy twice costs one thunk.
    [[nodiscard]] std::optional<GuestAddr> bind(const std::string& import_key,
                                                const HleFunction* function,
                                                AddressSpace& memory);

    /// Points an already-bound thunk at a different implementation.
    ///
    /// bind() is deliberately idempotent -- a repeated bind of the same import
    /// reuses its slot -- which also means it will not *change* one. Anything
    /// installed after load (a probe, a hot-swapped implementation) needs this
    /// instead, or it registers into the table and never gets called.
    ///
    /// Returns false if the key was never bound.
    bool rebind(const std::string& import_key, const HleFunction* function);

    [[nodiscard]] const Entry* lookup(u32 svc_immediate) const;
    [[nodiscard]] std::size_t bound_count() const noexcept {
        return entries_.size();
    }

private:
    std::vector<Entry> entries_;
    std::map<std::string, u32, std::less<>> index_by_key_;
    bool initialised_ = false;
};

/// Encodes an unconditional `SVC #immediate` in the ARM instruction set.
[[nodiscard]] constexpr u32 encode_svc(u32 immediate) noexcept {
    return 0xEF000000u | (immediate & 0x00FFFFFFu);
}

}  // namespace clickhle

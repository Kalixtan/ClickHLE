#pragma once

// A self-contained subset of Unicorn Engine's public ABI (GPLv2 upstream),
// hand-declared here so this project has ZERO build-time or link-time
// dependency on Unicorn -- no <unicorn/unicorn.h>, no -lunicorn, nothing in
// CMakeLists.txt. Unicorn's C ABI is small and stable (a flat extern "C"
// function table plus plain integer enums), so re-declaring exactly the
// pieces this adapter calls is both correct and the whole point: the
// resulting binary can be built and shipped with no GPLv2 code anywhere in
// it, and only reaches out to libunicorn.so at RUNTIME, via dlopen, only if
// and when a user asks for the "unicorn" CPU backend AND has it installed.
// See UnicornLibrary below and cpu_unicorn.cpp's own header comment for the
// full reasoning.
//
// Every value here is copied from Unicorn's own public headers
// (unicorn/unicorn.h, unicorn/arm.h), which are themselves a stable ABI
// contract Unicorn documents and versions -- these are not guesses.

#include <cstddef>
#include <cstdint>

namespace clickhle::unicorn_shim {

using uc_engine = void;  // opaque
using uc_hook = std::size_t;

enum uc_arch : int {
    kArchArm = 1,
};

enum uc_mode : int {
    kModeArm = 0,
};

enum uc_err : int {
    kErrOk = 0,
};

enum uc_prot : uint32_t {
    kProtNone = 0,
    kProtRead = 1,
    kProtWrite = 2,
    kProtExec = 4,
};

enum uc_mem_type : int {
    kMemRead = 16,           // UC_MEM_READ
    kMemReadUnmapped = 19,   // UC_MEM_READ_UNMAPPED
    kMemWriteUnmapped = 20,  // UC_MEM_WRITE_UNMAPPED
    kMemWriteProt = 22,      // UC_MEM_WRITE_PROT
};

enum uc_hook_type : uint64_t {
    kHookIntr = 1ull << 0,
    kHookCode = 1ull << 2,
    kHookMemRead = 1ull << 10,
    // UC_HOOK_MEM_UNMAPPED, from unicorn.h's own macro expansion:
    // UC_HOOK_MEM_READ_UNMAPPED(1<<4) + UC_HOOK_MEM_WRITE_UNMAPPED(1<<5) +
    // UC_HOOK_MEM_FETCH_UNMAPPED(1<<6) -- verified against the real macro
    // in unicorn.h, not the position originally (wrongly) assumed here.
    kHookMemUnmapped = (1ull << 4) | (1ull << 5) | (1ull << 6),
};

// ARM register indices (unicorn/arm.h's `uc_arm_reg` enum), copied by
// literal position count against the real header -- NOT assumed
// contiguous with R0 (SP/LR/PC/CPSR are declared much earlier in Unicorn's
// own enum, at positions 3/10/11/12; R13/R14/R15 are aliases back onto
// them). Verified 2026-09-18 by counting every preceding entry in
// /usr/include/unicorn/arm.h's real `uc_arm_reg` definition:
//   0 INVALID, 1 APSR, 2 APSR_NZCV, 3 CPSR, 4 FPEXC, 5 FPINST, 6 FPSCR,
//   7 FPSCR_NZCV, 8 FPSID, 9 ITSTATE, 10 LR, 11 PC, 12 SP, 13 SPSR,
//   14..45 D0..D31 (32 entries), 46 FPINST2, 47..49 MVFR0..MVFR2,
//   50..65 Q0..Q15 (16 entries), 66..78 R0..R12 (13 entries).
// An earlier draft of this table guessed CPSR=10/LR=15/PC=16/SP=17 by
// pattern-matching a DIFFERENT enum's shape from memory instead of
// counting this one -- caught and corrected before ever being run,
// exactly the kind of silent-wrong-value mistake hand-declaring an ABI
// invites if each entry isn't checked against the real source.
enum uc_arm_reg : int {
    kRegCpsr = 3,
    kRegLr = 10,
    kRegPc = 11,
    kRegSp = 12,
    kRegR0 = 66,
    kRegR1 = 67,
    kRegR2 = 68,
    kRegR3 = 69,
    kRegR4 = 70,
    kRegR5 = 71,
    kRegR6 = 72,
    kRegR7 = 73,
    kRegR8 = 74,
    kRegR9 = 75,
    kRegR10 = 76,
    kRegR11 = 77,
    kRegR12 = 78,
};

using uc_cb_hookcode_t = void (*)(uc_engine* uc, uint64_t address, uint32_t size,
                                  void* user_data);
using uc_cb_hookintr_t = void (*)(uc_engine* uc, uint32_t intno, void* user_data);
using uc_cb_hookmem_t = void (*)(uc_engine* uc, uc_mem_type type, uint64_t address,
                                 int size, int64_t value, void* user_data);
using uc_cb_eventmem_t = bool (*)(uc_engine* uc, uc_mem_type type, uint64_t address,
                                  int size, int64_t value, void* user_data);

// Function pointer types matching each libunicorn entry point this adapter
// calls, resolved via dlsym -- see UnicornLibrary.
using fn_uc_open = uc_err (*)(uc_arch, uc_mode, uc_engine**);
using fn_uc_close = uc_err (*)(uc_engine*);
using fn_uc_errno = uc_err (*)(uc_engine*);
using fn_uc_strerror = const char* (*)(uc_err);
using fn_uc_reg_read = uc_err (*)(uc_engine*, int, void*);
using fn_uc_reg_write = uc_err (*)(uc_engine*, int, const void*);
using fn_uc_mem_map_ptr = uc_err (*)(uc_engine*, uint64_t, size_t, uint32_t, void*);
using fn_uc_emu_start = uc_err (*)(uc_engine*, uint64_t, uint64_t, uint64_t, size_t);
using fn_uc_emu_stop = uc_err (*)(uc_engine*);
// uc_hook_add is variadic in the real API (extra args depend on hook type);
// every call site here needs exactly (uc, hook*, type, callback, user_data,
// begin, end), which is the fixed-arity subset the real function accepts
// with no trailing hook-specific arguments.
using fn_uc_hook_add = uc_err (*)(uc_engine*, uc_hook*, int, void*, void*,
                                  uint64_t, uint64_t);
// uc_ctl is variadic in the real API (arg count/types depend on the control
// selector); every call site here needs exactly one trailing `int` arg (the
// CPU model, per unicorn.h's own doc comment on UC_CTL_CPU_MODEL: "Write:
// @args = (int)"), so that's the fixed signature declared here rather than
// reproducing full variadic forwarding.
using fn_uc_ctl_set_cpu_model = uc_err (*)(uc_engine*, int /*control*/, int /*model*/);

// uc_control_type (unicorn.h). Only the entry this adapter needs -- counted
// by literal position against the real enum (UC_CTL_UC_MODE=0 through
// UC_CTL_UC_EXITS=6, then UC_CTL_CPU_MODEL=7), verified 2026-09-18.
enum uc_control_type : int {
    kCtlCpuModel = 7,
};

// UC_CTL()/UC_CTL_WRITE() from unicorn.h's own macros: a control value packs
// R/W(bit31..30) | NR(bits29..26) | reserved(bits25..16) | Type(bits15..0).
constexpr uint32_t kCtlIoWrite = 1;
constexpr uint32_t uc_ctl_write(uc_control_type type, uint32_t nr) {
    return static_cast<uint32_t>(type) | (nr << 26) | (kCtlIoWrite << 30);
}

// uc_cpu_arm (unicorn/arm.h). The real iPod (PortalPlayer PP502x) core is
// ARM7TDMI (ARMv4T) -- Unicorn's model list has NO exact match for it (the
// list starts at ARM926EJ-S, already ARMv5TEJ). UC_CPU_ARM_926 is the
// closest available and is used here, but this is a genuine, acknowledged
// approximation, unlike DangerVM's oracle where UC_CPU_ARM_1136 is an exact
// match for the real Hiptop hardware. ares' own vendored arm7tdmi core
// remains the accurate reference backend for this project; --cpu unicorn is
// a fallback option, not a claim of bit-exact ARM7TDMI behavior.
enum uc_cpu_arm : int {
    kCpuArm926 = 0,  // closest available match to the real ARM7TDMI core.
};

}  // namespace clickhle::unicorn_shim

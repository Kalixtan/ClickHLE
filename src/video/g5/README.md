# G5 graphics replacements

G5 handles float and fixed-point matrix identity (165–166), translation
(169–170), scaling (171–172), and multiplication (175–176). Other API calls
retain the shared graphics state and RetailOS packet builder, with JIT fallback.
Those matrix operations already ran on the host; they do not remove rendering
cost by themselves.

The firmware replacement in `firmware.cpp` handles the complete memcpy routine
shared by RenderServer and VMCS. It matches all 66 instruction bytes, regardless
of address, and rechecks after code-page writes. Native copies preserve scratch
registers, stack writes, forward overlap behaviour, signed length tests and
instruction history. Execution yields at the existing 32-instruction hardware
service boundary. MMIO, alignment exceptions and writes into the matched routine
fall back to normal execution. This replaces a shared firmware helper, not a
whole texture-upload or drawing API.

`g5_api` compares returns and mapped guest memory over 4,096 matrix cases.
`g5_firmware` compares the copy routine with the interpreter across alignment,
overlap, length, relocation, execution-slice and code-invalidation cases. A
scheduled run also compares timer and device state. These establish parity with
the emulator reference, not independent hardware verification.

For offline game validation, set `CLICKHLE_G5_VERIFY=1` with the headless
`--capture-videocore --vc2-engine g5` options. Each firmware run is repeated on an
independent interpreter copy; a state mismatch stops execution. The comparison
covers all RAM, registers, VRF, accumulators, flags, interrupts, DMA and timers.
It excludes code caches and profiling counters, while instruction counts are
checked separately. This is deliberately expensive and disabled during normal
play. The API path and packet queue remain shared; the check validates firmware
replacement execution, not independently the packet builder.

Drawing, texture decoding and shader replacements remain unfinished. Each new
replacement needs an input-dependent state contract and comparison against the
firmware before bypassing it. Captured register or memory values are not valid
constant substitutes for those contracts.

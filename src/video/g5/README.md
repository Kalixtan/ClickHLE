# G5 graphics replacements

G5 handles only calls with no VideoCore-visible state. `g5_graphics_call`
returns a value for a replaced ordinal and `std::nullopt` for every other call.
The caller then uses the existing RetailOS packet builder and VideoCore JIT.
This allows one call at a time to move here without changing draw order or
firmware state.

Current replacements are float and fixed-point matrix identity (165–166), translation
(169–170), scaling (171–172), and multiplication (175–176). They write guest memory and return the same values as the
original ARM-backed graphics API. New replacements need evidence for memory
writes, return values, errors, packet side effects, and interaction with pending
VC2 work. Stateful drawing calls need a shared renderer state before they can
move here safely.

`g5_api` compares replacement returns and the complete mapped guest memory
against the current graphics API over 4,096 cases, including overlapping
matrices and fixed-point wraparound. This verifies parity with that reference,
not independent retail hardware. Matrix operations already run on the host in
the reference backend, so this coverage alone does not remove rendering cost.

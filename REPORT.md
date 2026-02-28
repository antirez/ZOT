# Correctness Review and Fix Report

Date: 2026-02-28

## Scope

Reviewed and fixed correctness issues in:

- `z80.c`, `z80.h`
- `spectrum.c`, `spectrum.h`
- `z80_test.c`, `spectrum_test.c`
- `z80-specs/z80_reference.md`
- `spectrum-specs/io_ports.md`
- `spectrum-specs/beeper.md`

Cross-checked Z80/Spectrum hardware behavior with primary references listed in the sources section.

## Findings fixed in code

1. `R` register accounting for ignored `DD`/`FD` prefixes
- Problem: Ignored-prefix flow over-counted `R` due to re-fetch behavior.
- Fix: Reworked DD/FD ignored-prefix handling so the instruction is re-decoded in-place, and collapsed long DD/FD chains iteratively.
- Practical implication if unfixed: Timing-sensitive software and diagnostic tests that inspect `R` could misbehave.

2. Missing `EI` one-instruction delay
- Problem: Interrupt could be accepted immediately after `EI`.
- Fix: Added `ei_delay` state; interrupts are blocked until one subsequent instruction boundary.
- Practical implication if unfixed: Incorrect interrupt acceptance timing, especially around `EI; RET` style critical sequences.

3. IM2 vector low byte forced even
- Problem: IM2 used `(I << 8) | (data & 0xFE)`.
- Fix: IM2 now uses full data byte `(I << 8) | data`.
- Practical implication if unfixed: Wrong ISR vector when odd low-byte vectors are used.

4. IM0 restricted to RST-only behavior
- Problem: IM0 path converted bus data into RST only.
- Fix: IM0 now executes the bus opcode via the normal opcode executor, with acknowledge overhead accounted.
- Practical implication if unfixed: Non-RST IM0 devices/tests run incorrectly.

5. Frame timing dropped accepted INT T-states
- Problem: `z80_interrupt()` return value was ignored by frame integration.
- Fix: Accepted interrupt cycles are added to both `cpu.clocks` and `frame_tstates`.
- Practical implication if unfixed: Small but cumulative CPU/ULA timing drift per frame.

6. 48K I/O contention missing
- Problem: ULA contention was applied to memory accesses but not I/O cycles.
- Fix: Implemented 48K I/O contention patterns in read/write I/O callbacks.
- Practical implication if unfixed: Border effects and timing-sensitive routines/loaders can be off.

7. EAR read behavior too simplified
- Problem: EAR bit read used only external EAR level.
- Fix: EAR read bit now models Issue 3 practical behavior: speaker-high forces EAR bit high.
- Practical implication if unfixed: Port-read behavior differs from real boards in known edge cases.

8. `.z80` decompression accepted partial output
- Problem: Decompressor could return success without filling destination length.
- Fix: Decompressor now fails if output is not exactly complete.
- Practical implication if unfixed: Truncated/corrupt snapshots could be silently accepted with stale RAM.

9. 48K snapshot loader accepted non-48K v2/v3 modes
- Problem: Hardware mode byte was not validated.
- Fix: Loader now accepts only 48K modes (`0` and `1`) and rejects others.
- Practical implication if unfixed: 128K snapshots could be misloaded as invalid 48K state.

10. Misleading init-time interrupt comment/path
- Problem: Startup comment implied first interrupt delivery at init despite reset IFF state.
- Fix: Removed misleading init-time interrupt call/comment; frame interrupt handling remains at frame boundary.
- Practical implication if unfixed: Misleading code comments and startup timing assumptions.

## Specification fixes

1. `RETI`/`RETN` semantics in `z80-specs/z80_reference.md`
- Fixed incorrect claim that RETI does not restore IFF like RETN.
- Spec now matches Z80 CPU-level behavior (`IFF1 <- IFF2` for both).

2. IM2 vector note in `z80-specs/z80_reference.md`
- Removed misleading even-only framing.
- Spec now states that IM2 low byte comes from full bus data byte.

3. EAR/Speaker wording alignment in Spectrum specs
- Updated `spectrum-specs/io_ports.md` and `spectrum-specs/beeper.md` so Issue 3 coupling behavior is consistent and not contradictory.

4. IM0 timing note clarity
- Added practical note: flat `+2` acknowledge overhead is exact for common RST IM0 flow; exotic multi-byte IM0 flows depend on external device bus behavior.

## Minor-note follow-ups requested and applied

1. MIC state comment only
- `mic` is explicitly documented as retained for future Issue 2 board-accurate EAR/MIC coupling work.

2. DD/FD recursion risk
- Replaced recursive chain behavior with iterative collapse to avoid unbounded recursion under pathological prefix streams.

3. IM0 timing approximation note
- Added explicit code and spec comments documenting the exact/approximate boundary.

## Validation

- `make test` passes:
  - `z80_test`: 158/158
  - `spectrum_test`: 56/56
  - `cpm_test`: 78/78
- `make fulltest` was also run during this review cycle and passed (including `zexdoc` and `zexall`).

## Sources used for hardware verification

- Zilog Z80 CPU User Manual:
  - https://zany80.github.io/documentation/Z80/UserManual.html
- World of Spectrum 48K reference:
  - https://worldofspectrum.org/faq/reference/48kreference.htm
- World of Spectrum Z80 reference:
  - https://worldofspectrum.org/faq/reference/z80reference.htm
- Z80 interrupt notes:
  - https://www.z80.info/interrup.htm
- TZX format reference:
  - https://worldofspectrum.net/TZXformat.html

# ZX Spectrum Cassette Tape Format

## Overview

The ZX Spectrum stores programs on standard audio cassettes using a simple
FSK (Frequency Shift Keying) encoding scheme. The signal is a square wave
read through the EAR socket (bit 6 of port $FE). The ROM contains routines
to save and load data at approximately 1500 bits per second.

---

## 1. Signal Encoding

All timing is expressed in **T-states** (CPU clock cycles at 3.5 MHz).
One T-state = ~0.286 microseconds.

### Pilot Tone

Each pilot pulse is one half-cycle of a square wave:

| Parameter | Value |
|-----------|-------|
| Half-cycle duration | **2168 T-states** |
| Full cycle | 4336 T (~807 Hz) |
| Header block pulses | **8063** half-cycles (~4.8 seconds) |
| Data block pulses | **3223** half-cycles (~1.9 seconds) |

The long pilot on header blocks gives the user time to press PLAY and for
the cassette motor to stabilise.

### Sync Pulses

Immediately following the pilot tone are two sync pulses (half-cycles):

| Pulse | Duration |
|-------|----------|
| Sync 1 | **667 T-states** (~190 us) |
| Sync 2 | **735 T-states** (~210 us) |

These are deliberately asymmetric and much shorter than pilot pulses,
which is how the ROM distinguishes them from the ongoing pilot tone.

### Data Bits

Each data bit is encoded as **two identical half-cycles** (one complete
square-wave cycle):

| Bit | Half-cycle | Full cycle | Frequency |
|-----|-----------|------------|-----------|
| 0 | **855 T** | 1710 T | ~2047 Hz |
| 1 | **1710 T** | 3420 T | ~1024 Hz |

Bits are transmitted **MSB first** (bit 7 first, bit 0 last).

The signal is edge-triggered, not level-triggered, so the initial
polarity does not matter.

---

## 2. Block Structure

A complete tape block on the wire:

```
[Pilot tone] [Sync1] [Sync2] [Flag byte] [Data bytes...] [Parity byte]
```

- **Flag byte**: First data byte. `$00` for header blocks, `$FF` for data blocks.
- **Data bytes**: The payload.
- **Parity byte**: XOR of all preceding bytes (flag + all data bytes).
  If loading is correct, the running XOR accumulator ends up at zero.

### Header Block Format (17 bytes after flag)

| Offset | Size | Description |
|--------|------|-------------|
| 0 | 1 | Type: 0=Program, 1=Number array, 2=Character array, 3=Code |
| 1 | 10 | Filename (padded with spaces) |
| 11 | 2 | Data length (little-endian) |
| 13 | 2 | Parameter 1 (autostart line for Program, start address for Code) |
| 15 | 2 | Parameter 2 (variable area offset for Program, 32768 for Code) |

Total header block on tape: 1 (flag) + 17 (data) + 1 (parity) = **19 bytes**.

### Header vs Data Blocks

| Property | Header block | Data block |
|----------|-------------|------------|
| Flag byte | `$00` | `$FF` |
| Pilot pulses | 8063 | 3223 |
| Pilot duration | ~4.8 sec | ~1.9 sec |
| Data length | 17 bytes fixed | Variable |

The ROM determines pilot length based on `flag byte < 128` (long pilot)
vs `flag byte >= 128` (short pilot).

---

## 3. Tape File Formats

### TAP Format

Simple container with no timing information:

```
For each block:
  [length: 2 bytes LE] [flag] [data...] [parity]
```

The length field includes flag and parity bytes.

### TZX Format

Full-featured tape image format that can represent turbo loaders,
pure tones, custom pulse sequences, and direct recordings. Key block
types:

| ID | Name | Description |
|----|------|-------------|
| $10 | Standard Speed Data | ROM-compatible block with pause |
| $11 | Turbo Speed Data | Custom pilot/sync/data timing |
| $12 | Pure Tone | Single repeated pulse |
| $13 | Pulse Sequence | Arbitrary sequence of pulses |
| $14 | Pure Data | Data with custom bit encoding, no pilot/sync |
| $15 | Direct Recording | Raw sample data |
| $20 | Pause/Stop | Silence between blocks |

---

## 4. The ROM Loading Routine

### Entry Point: LD-BYTES ($0556)

**Entry conditions:**
- `A` = expected flag byte
- `F carry` = set for LOAD, reset for VERIFY
- `DE` = expected block length
- `IX` = destination address

### Complete Disassembly

#### Initialisation ($0556)

```
0556  INC D           ;  4T  Reset zero flag
0557  EX AF,AF'       ;  4T  Save flag byte and LOAD/VERIFY flag
0558  DEC D           ;  4T  Restore D
0559  DI              ;  4T  Disable interrupts
055A  LD A,$0F        ;  7T  White border, MIC off
055C  OUT ($FE),A     ; 11T  Set border colour
055E  LD HL,$053F     ; 10T  Address of SA/LD-RET
0561  PUSH HL         ; 11T  Push as return address on stack
0562  IN A,($FE)      ; 11T  Read initial EAR state
0564  RRA             ;  4T  Shift bit 6 (EAR) to bit 5
0565  AND $20         ;  7T  Isolate bit 5
0567  OR $02          ;  7T  Combine with RED border
0569  LD C,A          ;  4T  C = edge-state + border colour
056A  CP A            ;  4T  Set zero flag
```

#### LD-BREAK ($056B)

```
056B  RET NZ          ;  5T  Return failure if SPACE pressed
```

#### LD-START ($056C) — Wait for any edge

```
056C  CALL $05E7      ; 17T  LD-EDGE-1: find a single edge
056F  JR NC,$056B     ;  7T  Timeout -> LD-BREAK
```

#### LD-WAIT ($0571) — Stabilisation delay (~1 second)

```
0571  LD HL,$0415     ; 10T  Outer counter = 1045
0574  DJNZ $0574      ; 13/8T Self-loop 256 times (inner loop)
0576  DEC HL          ;  6T
0577  LD A,H          ;  4T
0578  OR L            ;  4T
0579  JR NZ,$0574     ; 12T  Continue outer loop
057B  CALL $05E3      ; 17T  LD-EDGE-2: find first complete pulse
057E  JR NC,$056B     ;  7T  Timeout -> LD-BREAK
```

#### LD-LEADER ($0580) — Count 256 valid pilot cycles

```
0580  LD B,$9C        ;  7T  B = 156 (timing start)
0582  CALL $05E3      ; 17T  LD-EDGE-2: measure one full cycle
0585  JR NC,$056B     ;  7T  Timeout -> LD-BREAK
0587  LD A,$C6        ;  7T  Threshold = 198
0589  CP B            ;  4T  Carry set if $C6 < B (i.e., B > 198)
058A  JR NC,$056C     ; 12T  B <= 198: too short, not pilot -> LD-START
058C  INC H           ;  4T  Count valid pilot cycle
058D  JR NZ,$0580     ; 12T  Loop until H wraps 255->0 (256 pulses)
```

**Note:** When B <= 198, the pulse is too short to be a pilot half-cycle.
The code jumps back to LD-START (not a full restart), preserving H but
looking for a new edge. When B > 198, it's accepted as a valid pilot.

#### LD-SYNC ($058F) — Detect sync pulses

```
058F  LD B,$C9        ;  7T  B = 201 (timing start)
0591  CALL $05E7      ; 17T  LD-EDGE-1: measure ONE half-cycle
0594  JR NC,$056B     ;  7T  Timeout -> LD-BREAK
0596  LD A,B          ;  4T
0597  CP $D4          ;  7T  Threshold = 212
0599  JR NC,$058F     ; 12T  B >= 212: still pilot -> keep looking
059B  CALL $05E7      ; 17T  LD-EDGE-1: get second sync edge
059E  RET NC          ;  5T  Timeout -> return failure
```

**Sync detection logic:**
- B starts at 201. If B >= 212 after LD-EDGE-1, the half-cycle was
  still pilot-length (long). Keep waiting.
- If B < 212, the half-cycle was short enough to be sync1.
- Maximum sync1 duration: 358 + (212-201-1) * 59 = 358 + 590 = **948 T**

#### Post-sync setup ($059F)

```
059F  LD A,C          ;  4T  Current edge state
05A0  XOR $03         ;  7T  Switch to blue/yellow border stripes
05A2  LD C,A          ;  4T  Store new border colours
05A3  LD H,$00        ;  7T  Initialise parity accumulator = 0
05A5  LD B,$B0        ;  7T  Initial timing constant = 176
05A7  JR $05C8        ; 12T  Jump to LD-MARKER
```

#### LD-8-BITS ($05CA) — Load one bit per iteration

```
05CA  CALL $05E3      ; 17T  LD-EDGE-2: measure one full signal cycle
05CD  RET NC          ;  5T  Timeout -> return failure
05CE  LD A,$CB        ;  7T  Threshold = 203
05D0  CP B            ;  4T  Carry set if $CB < B (i.e., B > 203)
05D1  RL L            ;  8T  Rotate carry into L from right
05D3  LD B,$B0        ;  7T  Reset B = 176 for next bit
05D5  JP NC,$05CA     ; 10T  Loop if sentinel bit hasn't shifted out
```

**Sentinel bit trick:** L starts as `$01` (binary 00000001). Each `RL L`
shifts bits left, inserting the new data bit at bit 0. After 8 bits,
the original sentinel `1` shifts out of bit 7, setting carry, and the
`JP NC` falls through.

#### Byte complete ($05D8) and parity check

```
05D8  LD A,H          ;  4T  Get running parity
05D9  XOR L           ;  4T  XOR with completed byte
05DA  LD H,A          ;  4T  Store updated parity
05DB  LD A,D          ;  4T  Check remaining byte count
05DC  OR E            ;  4T
05DD  JR NZ,$05A9     ; 12T  More bytes -> LD-LOOP
05DF  LD A,H          ;  4T  Final parity
05E0  CP $01          ;  7T  Sets carry if H = 0 (good parity)
05E2  RET             ; 10T  Return with carry = success/failure
```

---

## 5. Edge Detection: LD-EDGE-1 and LD-EDGE-2

### LD-EDGE-2 ($05E3)

```
05E3  CALL $05E7      ; 17T  Call LD-EDGE-1 (find first edge)
05E6  RET NC          ;  5T  Return failure if no edge found
      ; Falls through to LD-EDGE-1 (find second edge)
```

LD-EDGE-2 detects **two consecutive edges** spanning one full cycle of
the signal. B accumulates across both LD-EDGE-1 calls and is NOT reset
between them.

### LD-EDGE-1 ($05E7)

```
; --- Initial delay (358 T-states) ---
05E7  LD A,$16        ;  7T  A = 22
05E9  DEC A           ;  4T
05EA  JR NZ,$05E9     ; 12/7T  Loop: 21*(4+12) + (4+7) = 347T
05EC  AND A           ;  4T  Clear carry flag
                      ;      Total: 7 + 347 + 4 = 358T

; --- Sampling loop (59 T per iteration) ---
05ED  INC B           ;  4T  Increment timing counter
05EE  RET Z           ; 5/11T Return if B wrapped to 0 (timeout)
05EF  LD A,$7F        ;  7T  High byte of port address
05F1  IN A,($FE)      ; 11T  Read port $7FFE (EAR=bit6, SPACE=bit0)
05F3  RRA             ;  4T  Rotate: bit6->bit5, bit0->carry
05F4  RET NC          ; 5/11T Return if SPACE pressed (carry=0 from bit0)
05F5  XOR C           ;  4T  Compare EAR (bit5) with stored state
05F6  AND $20         ;  7T  Isolate bit 5
05F8  JR Z,$05ED      ; 12/7T No edge -> loop back

; --- Edge found ---
05FA  LD A,C          ;  4T  Get current edge state
05FB  CPL             ;  4T  Toggle all bits
05FC  LD C,A          ;  4T  Store new edge state
05FD  AND $07         ;  7T  Extract border colour bits
05FF  OR $08          ;  7T  Set MIC off (bit 3)
0601  OUT ($FE),A     ; 11T  Update border (loading stripes!)
0603  SCF             ;  4T  Set carry = success
0604  RET             ; 10T  Return
```

### Timing per sampling iteration

| Path | Timing |
|------|--------|
| No edge (loop back) | 4+5+7+11+4+5+4+7+12 = **59 T** |
| SPACE pressed | 4+5+7+11+4+11 = **42 T** (RET NC taken) |
| Edge found + cleanup | 4+5+7+11+4+5+4+7+7 + 4+4+4+7+7+11+4+10 = **109 T** |

**Important:** The `RET NC` at $05F4 checks the SPACE key (bit 0 of
port $FE becomes carry after RRA). This adds **5 T** to each normal
sampling iteration, making the loop **59 T** total.

### Threshold Summary

| Routine | B start | Threshold | Accept condition | Meaning |
|---------|---------|-----------|-----------------|---------|
| LD-LEADER | $9C (156) | $C6 (198) | B > 198 | Valid pilot (long pulse) |
| LD-SYNC | $C9 (201) | $D4 (212) | B < 212 | Sync found (short pulse) |
| LD-8-BITS (1st byte) | $B0 (176) | $CB (203) | B > 203 = bit 1 | Data bit discrimination |
| LD-8-BITS (2nd+ byte) | $B2 (178) | $CB (203) | B > 203 = bit 1 | Slightly less headroom |

### Numerical Verification

For LD-8-BITS with B starting at $B0 = 176 and 59T per loop:

**Bit 0** (half-cycles of 855 T):
- Per half-cycle: (855 - 358) / 59 = ~8.4 iterations
- Total B increments across both edges: ~17
- B final = 176 + 17 = **~193** (< 203: carry clear = bit 0)

**Bit 1** (half-cycles of 1710 T):
- Per half-cycle: (1710 - 358) / 59 = ~22.9 iterations
- Total B increments across both edges: ~46
- B final = 176 + 46 = **~222** (> 203: carry set = bit 1)

The threshold of 203 sits between ~193 and ~222, giving comfortable
margin for tape speed variation.

For LD-LEADER with B starting at $9C = 156 and pilot half-cycle 2168T:
- Per half-cycle: (2168 - 358) / 59 = ~30.7 iterations
- Total across both edges: ~61
- B final = 156 + 61 = **~217** (> 198: accepted as pilot)

---

## 6. Turbo Loaders

### Overview

Almost all turbo loaders use the same basic approach as the ROM: pulse-
width modulation with two half-cycles per bit. They achieve higher speed
by reducing the T-state count for each pulse. The ROM's sampling loop at
59 T/iteration limits how short pulses can be (a pulse must last at least
2-3 sampling iterations to be reliably detected).

**Standard ROM speed:** ~1365 bits/sec average (855/1710 T encoding)

**General turbo approach:** Halve the pulse widths (or similar) to reach
~2000-3000 baud. Some schemes go further with tighter sampling loops.

**Common modifications to the ROM loader core:**
- Remove the `RET NC` SPACE key check (saves 5T/iteration, tightens loop)
- Remove the initial 358T delay in LD-EDGE-1 (faster response)
- Reduce B-register timing constants (shorter thresholds)
- Change pilot/sync structure (fewer pilot pulses, custom sync)
- Add encryption/obfuscation of loaded data
- Remove header blocks (headerless loading)
- Add multi-block loading with on-screen progress

### Loader Core Variants

The Sinclair Wiki documents several "core" families based on how the
sampling loop differs from the ROM. Most commercial loaders are minor
variants of the ROM's LD-EDGE-1 loop:

| Core | Key change | Loop timing | Used by |
|------|-----------|-------------|---------|
| **ROM** | Standard | 59T | Cybernoid, Dan Dare |
| **Speedlock** | Remove `RET NC` (SPACE check) | 54T | Daley Thompson's Decathlon, Head over Heels, many Ocean games |
| **Alkatraz** | Invert edge logic, `JP` instead of `JR`, `RET Z` replaces `RET NC` | ~54T | 720°, Cobra, Gauntlet III, Out Run Europa |
| **Bleepload** | Replace `RET NC` with `NOP` | 56T | Bubble Bobble, Starglider 2 (Firebird) |
| **Microsphere** | Replace `RET NC` with `AND A` | 54T | Skool Daze, Contact Sam Cruise |
| **Paul Owens** | `RET NC` → `RET Z` | 54T | Chase H.Q. (Ocean) |
| **Dinaload** | Set port high byte to $FF (disables keyboard) | 59T | Freddy Hardest, Astro Marine Corps (Dinamic) |
| **Search Loader** | Remove `RRA`, use `AND $40` directly, read all keyboard rows | ~59T | Blood Brothers, City Slicker (Hewson) |
| **Digital Integration** | Minimal loop with `DEC B`, no `LD A,$7F` | ~42T | ATF, Tomahawk |

### Major Commercial Loaders

#### Speedlock (1983-1990s, by David Aubrey-Jones & David Looker)

The most widely-used protection/turbo system. 7+ versions, each adding
more anti-copy measures. Used by Ocean Software and many others.

**Common features across versions:**
- Removes SPACE check from sampling loop (54T/iteration)
- "Clicking leader" pilot tone in early versions: short tone bursts
  followed by clicks, defeating tape copiers that need continuous pilot
- Multi-block structure: KLRBC (Clicking leader, Long sync, short
  Marker, Bytes, Checksum)
- R-register and parity-flag tricks for copy protection
- Data encryption (XOR/ADD based)

**Version differences:**

| Version | Approx date | Baud rate | Notable features |
|---------|------------|-----------|------------------|
| Speedlock 1 | 1984 | ~1900 | Clicking leader (4-bit patterns), ~150% ROM speed |
| Speedlock 2 | 1985 | ~1800 | |
| Speedlock 3 | 1985-86 | ~1800 | |
| Speedlock 4 | 1986-87 | ~1900 | Counter showing time remaining |
| Speedlock 5 | 1987 | ~1700 | |
| Speedlock 6 | 1988 | ~1675 | |
| Speedlock 7 | 1989+ | ~1700 | Most complex protection |

**Speedlock 1 details (from Ramsoft Loaders Guide):**
- Clicking leader: repeated groups of 4 pilot segments with varying
  lengths (e.g. Jetset Willy uses 228, 220, 224, 222 half-periods),
  each followed by a click, repeated 8 times
- Long sync: ~3000 T (580 Hz), mixed with pilot frequency (2168T)
- Data at ~3100 Hz for bit-0, ~1550 Hz for bit-1
- Pause of >= 1500 ms required between blocks (copy protection check
  examines signal during pause)

**Speedlock Associates variant (Dan Dare 2):**
- Bit 0: DP 555, Bit 1: DP 1110
- Pilot: P 2168 (1591-1599 pulses per block)
- Complex sync: DP 693, 693, 1420, 2100, 1420, 693
- Data encryption: XOR + ADD with varying keys per block
- Mid-block sync transitions

#### Alkatraz (Ocean/Imagine)

Used primarily by Ocean Software and Imagine for their later titles.

- Sampling loop removes SPACE check, inverts edge detection logic
- Uses `JP` for loop-back instead of `JR` (same timing)
- Standard pilot/sync/data structure but with turbo pulse widths
- Multi-load support for large games
- Games: 720°, Cobra, Fairlight, Gauntlet III, Out Run Europa,
  Shadow Dancer

#### Bleepload (Firebird/Telecomm Soft)

Named for its distinctive bleeping sounds during loading.

- Replaces `RET NC` with `NOP` in sampling loop
- Loads in small blocks (~250 bytes each) with very short pilot tones
- Displays block number in hex on screen during loading
- Includes decryption routine for loaded code
- Error recovery: can rewind tape and reload individual blocks
- Games: Bubble Bobble, Starglider 2, and many Firebird titles

#### Gremlin Loader

Developed as competition to Novaload and Speedlock.

- Bit 0: DP 465, Bit 1: DP 930 (nearly 2x ROM speed)
- Standard pilot (2168T) and sync (667/735T) structure
- Flag byte and parity byte present
- Uses ROM-like routine with modified timing constants
- Games: numerous Gremlin Graphics titles

#### Flash Load

- Pilot: 8063 pulses at P 2168 (standard)
- Sync: standard P 667, P 735
- Bit 0: DP 543, Bit 1: DP 839 (~1.6x ROM speed)
- Bytes saved in reverse order (DEC IX)
- Flag byte and parity byte present

#### Elite Systems Loader

- Pilot: 3223 pulses at P 2168 (standard data pilot)
- Sync: standard P 667, P 735
- Bit 0: DP 452, Bit 1: DP 878 (~1.9x ROM speed)
- Copy of ROM routine with changed timing constants:
  $B0→$5B, $B2→$5C, $CB→$65, $16→$10
- Flag byte $FF, parity present

#### Players 1 Loader

- Pilot: standard (3223 or 8063 depending on flag)
- Sync: standard P 667, P 735
- Bit 0: DP 582, Bit 1: DP 1151 (~1.5x ROM speed)
- Version 1.5 adds data encryption: (nth byte) XOR (3*n) XOR
  ((block_length+1-n) mod 256)

#### Microprose Loader

- Pilot: 3223 pulses at P 2168
- Sync: standard P 667, P 735
- Bit 0: DP 699, Bit 1: DP 1398 (~1.2x ROM speed)
- Xenophobe variant: headerless, dual checksum (XOR + ADD)
- Timing quirks: first pulse of each datablock 95T longer,
  first pulse of each byte 30T shorter

#### LazerLoad 48

- Dummy pulses: 21 groups of 289×P2168 + P900
- Pilot: 699 pulses at P 2168
- Sync: standard P 667, P 735
- Bit 0: DP 660, Bit 1: DP 1320 (~1.3x ROM speed)
- Flag byte $07
- 4000 ms pause between blocks

#### Haxpoc-Lock

- Pilot: 3223 pulses at P 2168
- Sync: standard P 667, P 735
- Normal mode: Bit 0: DP 816, Bit 1: DP 1632 (~1.05x ROM speed)
- Turbo mode: Bit 0: DP 621, Bit 1: DP 1242 (~1.4x ROM speed)
- No parity byte
- Bytes saved in reverse order (DEC IX)

#### Software Projects (SoftLock)

Custom protection used by Software Projects (Jet Set Willy, Manic Miner).
Speedlock 1 was actually first used commercially by Software Projects.

#### Dinaload (Dinamic Software)

- Sets port $FE high byte to $FF, preventing keyboard reads
- Otherwise uses standard ROM-like timing
- Used on Spanish games: Freddy Hardest, Astro Marine Corps,
  Army Moves, Camelot Warriors

#### Search Loader (Hewson Consultants)

- Fundamentally different sampling loop: removes RRA shift,
  reads all keyboard rows (A=$00), checks bit 6 directly via AND $40
- Includes dummy `RET C` instruction
- Designed to match ROM timing despite different instructions
- Games: Blood Brothers, City Slicker, Cybernoid II
- Gremlin variant used for Lotus Esprit Turbo Challenge, Space Crusade

#### Digital Integration Loader

- "Probably the minimal loop possible"
- Uses `DEC B` (inverted counter direction)
- Omits `LD A,$7F` (reads all keyboard rows)
- ~42T per iteration (fastest sampling loop of any common loader)
- Games: ATF, Tomahawk, Fighter Bomber

### Speed Comparison Table

| Loader | Bit 0 (T) | Bit 1 (T) | Approx speed | vs ROM |
|--------|----------|----------|-------------|--------|
| **Standard ROM** | 855 | 1710 | ~1365 bps | 1.0x |
| **Microprose** | 699 | 1398 | ~1700 bps | 1.2x |
| **LazerLoad** | 660 | 1320 | ~1800 bps | 1.3x |
| **Haxpoc (turbo)** | 621 | 1242 | ~1900 bps | 1.4x |
| **Players 1** | 582 | 1151 | ~2000 bps | 1.5x |
| **Speedlock 1** | ~570 | ~1140 | ~2050 bps | 1.5x |
| **Flash Load** | 543 | 839 | ~2200 bps | 1.6x |
| **47loader** | 543 | 1086 | ~2200 bps | 1.6x |
| **Gremlin** | 465 | 930 | ~2600 bps | 1.9x |
| **Elite** | 452 | 878 | ~2700 bps | 2.0x |
| **47loader (max)** | 475 | 950 | ~2500 bps | 1.85x |

Note: "DP" values represent one half-cycle in T-states. A full bit uses
two identical half-cycles. The 2:1 ratio between bit-1 and bit-0 is
nearly universal across all loaders.

### Encoding Methods

Almost all ZX Spectrum turbo loaders use the same fundamental encoding:

**Pulse-Width Modulation (PWM):** Each bit is two identical half-cycles.
A "0" bit uses short pulses, a "1" bit uses long pulses (typically 2x).
This is the same scheme as the ROM, just with shorter pulse widths.

No major Spectrum loader used alternative encoding schemes like:
- Manchester encoding
- Frequency Shift Keying (FSK) with distinct carriers
- Phase encoding
- Multi-level amplitude encoding

The reason: the EAR port provides only a 1-bit digital input (bit 6 of
port $FE), so the only measurable quantity is time between edges. PWM
with a 2:1 ratio is optimal for this constraint because the bit-0 and
bit-1 timings are maximally separated.

### Anti-Copy Protection Techniques

Beyond speed changes, commercial loaders used various protection methods:

1. **Clicking leaders**: Interrupted pilot tones that confuse tape
   copier hardware expecting continuous pilot (Speedlock 1-3)
2. **Headerless blocks**: No standard header, so `LOAD ""` won't find it
3. **Custom sync patterns**: Non-standard sync pulse sequences
4. **Data encryption**: XOR, ADD, or R-register-based decryption
5. **Timing-dependent decryption**: Using CPU R register (Speedlock)
6. **Pause analysis**: Examining signal during inter-block pauses to
   detect copies (Speedlock 1)
7. **Multi-stage loading**: Loader loads another loader, which loads
   the actual data
8. **Custom pilot tones**: Non-standard frequencies for the leader

---

## 7. TZX Turbo Block Format (ID $11)

The TZX format's block $11 can represent any turbo loader that uses
the standard PWM encoding with custom timing:

| Offset | Size | Field | Default |
|--------|------|-------|---------|
| $00 | 2 | Pilot pulse length | 2168 T |
| $02 | 2 | Sync first pulse | 667 T |
| $04 | 2 | Sync second pulse | 735 T |
| $06 | 2 | Zero bit pulse | 855 T |
| $08 | 2 | One bit pulse | 1710 T |
| $0A | 2 | Pilot tone pulses | 8063/3223 |
| $0C | 1 | Used bits in last byte | 8 |
| $0D | 2 | Pause after block (ms) | 1000 |
| $0F | 3 | Data length | - |
| $12 | N | Data | - |

For loaders that don't fit this model, TZX provides:

- **Block $12 (Pure Tone)**: Repeat one pulse N times
- **Block $13 (Pulse Sequence)**: Up to 255 arbitrary-length pulses
- **Block $14 (Pure Data)**: Data with custom bit timing, no pilot/sync
- **Block $15 (Direct Recording)**: Raw 1-bit samples at configurable
  rate (recommended 79 T/sample = 44100 Hz, or 158 T/sample = 22050 Hz)

---

## References

- The Complete Spectrum ROM Disassembly — Ian Logan & Frank O'Hara
- SkoolKit ROM Disassembly — https://skoolkid.github.io/rom/
- TZX Format Specification — https://worldofspectrum.net/TZXformat.html
- Sinclair Wiki — https://sinclair.wiki.zxnet.co.uk/wiki/Spectrum_tape_interface

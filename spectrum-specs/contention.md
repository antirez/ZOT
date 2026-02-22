# ZX Spectrum 48K Memory Contention

## Overview

On the 48K Spectrum, addresses 0x4000-0x7FFF (lower 16KB of RAM) are shared
between the ULA and CPU. The ULA has priority -- when both try to access this
memory simultaneously, the Z80 clock is literally stopped until the ULA finishes.

## The 6,5,4,3,2,1,0,0 Pattern

The ULA reads memory in 8 T-state cycles. When the CPU accesses contended
memory during the display period, it's delayed depending on position in the cycle:

| Position in 8-cycle | Delay (T-states) |
|---------------------|-------------------|
| 0                   | 6                 |
| 1                   | 5                 |
| 2                   | 4                 |
| 3                   | 3                 |
| 4                   | 2                 |
| 5                   | 1                 |
| 6                   | 0                 |
| 7                   | 0                 |

Positions 0-5: ULA is fetching, CPU must wait.
Positions 6-7: ULA is idle, CPU proceeds immediately.

## When Contention Applies

Contention occurs ONLY:
- During the 128 T-states of pixel data per scanline (not border/retrace)
- During the 192 display scanlines (not top/bottom border or vsync)
- When accessing addresses 0x4000-0x7FFF

## First Contended T-state

T-state 14,335 after the interrupt (counting from 0).

```
64 top scanlines * 224 T-states/line = 14,336
First ULA fetch begins at T-state 14,335
```

The pattern repeats:
```
T-state 14335: delay 6
T-state 14336: delay 5
T-state 14337: delay 4
T-state 14338: delay 3
T-state 14339: delay 2
T-state 14340: delay 1
T-state 14341: delay 0
T-state 14342: delay 0
T-state 14343: delay 6  (next group)
...
```

128 T-states of contention, then 96 T-states non-contended (border + retrace),
then next scanline starts.

## Contention Check Points

At hardware level, contention is checked at the T1 state of any:
- Instruction fetch (opcode read from PC)
- Memory read (operand fetch, indirect reads)
- Memory write (stores, stack pushes)

For multi-byte instructions, each individual memory access to the contended
region is independently subject to contention.

## I/O Contention

I/O contention depends on two factors:
1. Whether the HIGH BYTE of the port address is in 0x40-0x7F (contended range)
2. Whether the LOW BIT (bit 0) is 0 (ULA port) or 1

| High byte contended? | Low bit | Pattern           |
|----------------------|---------|-------------------|
| No                   | 0 (ULA) | N:1, C:3         |
| No                   | 1       | N:4              |
| Yes                  | 0 (ULA) | C:1, C:3         |
| Yes                  | 1       | C:1, C:1, C:1, C:1 |

Where:
- N:x = x uncontended T-states
- C:x = apply contention delay, then x T-states

## Implementation: Precomputed Table

```c
#define TSTATES_PER_FRAME   69888
#define FIRST_CONTENDED     14335
#define DISPLAY_LINES       192
#define CONTENDED_PER_LINE  128
#define TSTATES_PER_LINE    224

static uint8_t contention_table[TSTATES_PER_FRAME];

void init_contention_table(void) {
    static const uint8_t pattern[] = {6,5,4,3,2,1,0,0};
    memset(contention_table, 0, sizeof(contention_table));

    for (int line = 0; line < DISPLAY_LINES; line++) {
        int line_start = FIRST_CONTENDED + (line * TSTATES_PER_LINE);
        for (int col = 0; col < CONTENDED_PER_LINE; col++) {
            int t = line_start + col;
            if (t >= 0 && t < TSTATES_PER_FRAME)
                contention_table[t] = pattern[col & 7];
        }
    }
}
```

## Instruction-Level Approximation

Since we execute one full instruction per step (not cycle-by-cycle), we
approximate contention by:

1. Checking contention on opcode fetch (if PC in 0x4000-0x7FFF)
2. Checking contention on data memory access (if effective address in 0x4000-0x7FFF)
3. Using the precomputed table for O(1) delay lookup

This gives ~95%+ accuracy for most software. Only racing-the-beam border
effects and some multicolor engines need cycle-exact timing.

## Games Sensitive to Contention

- Aquaplane: border horizon split relies on contention delays
- Arkanoid: uses floating bus (reads from unattached ports during ULA fetches)
- Dark Star: border graphics via precise scanline timing
- Nirvana/Bifrost engine games: multicolor attribute effects

## Sources

- Sinclair Wiki: Contended memory, Contended I/O
- World of Spectrum FAQ: 48K reference
- FUSE emulator source code (spectrum.c, ula.c)
- Spectrum for Everyone: Memory contention and floating bus

# ZX Spectrum 48K ULA Video Generation

## Overview

The ULA (Uncommitted Logic Array) generates a PAL TV signal at 50.08 Hz.
Pixel clock: 7 MHz (2x CPU clock of 3.5 MHz).

## Frame Timing

- CPU clock: 3,500,000 Hz
- T-states per scanline: 224
- Scanlines per frame: 312
- T-states per frame: 69,888
- Frame rate: 3,500,000 / 69,888 = 50.08 Hz

### Scanline Breakdown (312 total)

| Region            | Scanlines | T-states | Notes                    |
|-------------------|-----------|----------|--------------------------|
| Vertical sync     | 8         | 1,792    | TV sync pulses           |
| Top border        | 56        | 12,544   | Border color rendered    |
| Display area      | 192       | 43,008   | Active pixel area        |
| Bottom border     | 56        | 12,544   | Border color rendered    |
| **Total**         | **312**   | **69,888** |                        |

Top 64 scanlines (8 vsync + 56 top border) = 64 * 224 = 14,336 T-states before
the first display byte appears.

### Per-Scanline Structure (224 T-states)

| Phase                        | T-states | Range    |
|------------------------------|----------|----------|
| Pixel data (256 pixels)      | 128      | 0-127    |
| Right border                 | 24       | 128-151  |
| Horizontal blanking/retrace  | 48       | 152-199  |
| Left border                  | 24       | 200-223  |

## ULA Fetch Cycle

During the 128 T-states of active display per scanline, the ULA fetches video
data in repeating 8 T-state cycles:

```
T-state 0: Read bitmap byte         (from 0x4000-0x57FF)
T-state 1: Read attribute byte      (from 0x5800-0x5AFF)
T-state 2: Read bitmap byte + 1     (next column)
T-state 3: Read attribute byte + 1  (next column)
T-state 4: Idle (no ULA memory access)
T-state 5: Idle
T-state 6: Idle
T-state 7: Idle
```

This gives 16 byte-pairs (32 bytes bitmap + 32 attribute reads) per scanline.
Each bitmap byte = 8 pixels, so 32 bytes = 256 pixels per line.

## Screen Memory Layout

- Bitmap area: 0x4000-0x57FF (6144 bytes) -- 256x192 pixels, 1 bit per pixel
- Attribute area: 0x5800-0x5AFF (768 bytes) -- 32x24 color cells, 8x8 pixels each

## Interleaved Line Addressing

The bitmap memory uses an interleaved addressing scheme. The screen is divided
into 3 thirds of 64 pixel lines (8 character rows) each.

For pixel at position (x, y) where x=0..255, y=0..191:

```
Address = 0x4000
        | ((y & 0xC0) << 5)   // Y7,Y6 -> bits 12-11 (which third: 0,1,2)
        | ((y & 0x07) << 8)   // Y2,Y1,Y0 -> bits 10-8 (pixel line in cell)
        | ((y & 0x38) << 2)   // Y5,Y4,Y3 -> bits 7-5 (char row in third)
        | ((x >> 3) & 0x1F)   // X4..X0 -> bits 4-0 (column byte)
```

In binary, the address bits:
```
Bit:  15 14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
       0  1  0 Y7 Y6 Y2 Y1 Y0 Y5 Y4 Y3 X4 X3 X2 X1 X0
```

The pixel bit within the byte: `7 - (x & 7)` (bit 7 = leftmost pixel).

### Why interleaved?

Incrementing the high byte moves down one pixel line within the same character
cell (addr += 256). Moving down one full character row: addr += 32. Moving to
the next third: addr += 0x800.

## Attribute Address

For character cell at column c (0-31), row r (0-23):
```
attr_addr = 0x5800 + (r * 32) + c
```

Or from pixel coordinates:
```
attr_addr = 0x5800 + ((y >> 3) * 32) + (x >> 3)
```

## Attribute Byte Format

```
Bit 7:    FLASH (toggle INK/PAPER every 16 frames)
Bit 6:    BRIGHT (increase intensity)
Bits 5-3: PAPER color (0-7)
Bits 2-0: INK color (0-7)
```

## FLASH Behavior

FLASH toggles every 16 frames (32-frame full cycle, ~0.64s at 50Hz).
When FLASH is set and the flash state is inverted, swap INK and PAPER colors.

## Color Palette

Normal (BRIGHT=0):
| Index | Color   | RGB       |
|-------|---------|-----------|
| 0     | Black   | #000000   |
| 1     | Blue    | #0000D7   |
| 2     | Red     | #D70000   |
| 3     | Magenta | #D700D7   |
| 4     | Green   | #00D700   |
| 5     | Cyan    | #00D7D7   |
| 6     | Yellow  | #D7D700   |
| 7     | White   | #D7D7D7   |

Bright (BRIGHT=1):
| Index | Color          | RGB       |
|-------|----------------|-----------|
| 0     | Black          | #000000   |
| 1     | Bright Blue    | #0000FF   |
| 2     | Bright Red     | #FF0000   |
| 3     | Bright Magenta | #FF00FF   |
| 4     | Bright Green   | #00FF00   |
| 5     | Bright Cyan    | #00FFFF   |
| 6     | Bright Yellow  | #FFFF00   |
| 7     | Bright White   | #FFFFFF   |

Normal intensity is ~0xD7 (215/255 = ~84%). BRIGHT applies to BOTH INK and
PAPER in the character cell simultaneously.

## Border Rendering

Border color set by bits 0-2 of port 0xFE output.
Border is rendered during:
- 24 T-states left border per scanline
- 24 T-states right border per scanline
- Full 224 T-states of each top/bottom border scanline

The border color change takes effect immediately (racing-the-beam effects).

## Interrupt Timing

- INT fires at T-state 0 of each frame (start of vsync)
- INT held LOW for 32 T-states (longest Z80 instruction is 23 T-states,
  so interrupt is always recognized)
- In IM1 mode: RST 0x38 (13 T-states for the acknowledge/call)
- 14,336 T-states from INT to first display pixel

## Sources

- World of Spectrum FAQ: 48K reference
- Sinclair Wiki: ZX Spectrum ULA
- Break Into Program: Screen Memory Layout
- ZX Design Info: Video Parameters

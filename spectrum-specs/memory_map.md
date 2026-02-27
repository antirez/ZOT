# ZX Spectrum 48K Memory Map

## Full 64KB Layout

| Address Range | Size     | Contents                         | Contended? |
|---------------|----------|----------------------------------|------------|
| 0x0000-0x3FFF | 16 KB    | ROM (BASIC + character set)      | No (ROM)   |
| 0x4000-0x57FF | 6 KB     | Screen bitmap (256x192 pixels)   | Yes        |
| 0x5800-0x5AFF | 768 B    | Screen attributes (32x24 cells)  | Yes        |
| 0x5B00-0x5BFF | 256 B    | Printer buffer                   | Yes        |
| 0x5C00-0x5CBF | 192 B    | System variables                 | Yes        |
| 0x5CC0-0x7FFF | ~9.5 KB  | BASIC program area (lower)       | Yes        |
| 0x8000-0xFFFF | 32 KB    | General RAM (uncontended)        | No         |

## Key Points

- ROM (0x0000-0x3FFF): Read-only. Writes silently ignored.
- Contended range (0x4000-0x7FFF): Shared with ULA, subject to wait states.
- Uncontended range (0x8000-0xFFFF): Full-speed CPU access.

## ROM Contents

The 16KB ROM contains:
- Sinclair BASIC interpreter
- Tape load/save routines (LOAD at ~0x0556, SAVE at ~0x04C2)
- Character set at 0x3D00-0x3FFF (96 chars * 8 bytes = 768 bytes, ASCII 32-127)
- RST handlers:
  - RST 0x00: Reset
  - RST 0x08: Error handler
  - RST 0x10: Print character
  - RST 0x18: Collect character
  - RST 0x20: Collect next character
  - RST 0x28: Calculator
  - RST 0x30: Make room
  - RST 0x38: Interrupt handler (IM1)
- Keyboard scanning routines at 0x028E

## System Variables (0x5C00-0x5CBF)

Key system variables:
- CHARS (0x5C36, 2 bytes): Points to character set - 256
- LAST_K (0x5C08): Last key pressed
- FLAGS (0x5C3B): System flags
- ERR_SP (0x5C3D, 2 bytes): Error stack pointer
- RAMTOP (0x5CB2, 2 bytes): Top of available RAM

## Stack

The stack typically starts at RAMTOP and grows downward. Default RAMTOP for
48K is 0xFF57 (65367).

## ROM Availability

The Spectrum ROM is still copyrighted, but redistribution for emulator use
has historically relied on Amstrad's published permission statement (with
copyright notice preserved and no separate ROM sales). The ROM file is
exactly 16,384 bytes, commonly named "48.rom" or "spectrum.rom".

## Sources

- World of Spectrum FAQ: Memory map
- Sinclair Wiki: System variables

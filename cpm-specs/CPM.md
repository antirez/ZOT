# CP/M 2.2 Reference for Emulator Development

Factual hardware and software documentation for implementing a CP/M 2.2
emulator. Compiled from the Digital Research CP/M 2.2 manual, the CCP
source code, and community references.

## Files

- **bdos.md** -- All 39 BDOS functions (0-37, 40) with calling convention,
  parameters, return values, error codes. FCB format, directory entries,
  disk parameter blocks.
- **bios.md** -- All 17 BIOS entry points, DPH/DPB structures, disk
  geometry, sector translation, memory map.
- **ccp.md** -- CCP built-in commands (DIR, ERA, TYPE, SAVE, REN, USER),
  command parsing, .COM loading, FCB filling, SUBMIT mechanism.

## Architecture Overview

CP/M 2.2 is a three-layer system:

```
+------------------------------------------+
| CCP  - Console Command Processor  ~2KB   |
|   Command line, built-in commands,        |
|   .COM loader                             |
+------------------------------------------+
| BDOS - Basic Disk Operating System ~3.5KB |
|   39 system calls (0-37, 40) via CALL 0005h |
|   File I/O, console I/O, disk management  |
+------------------------------------------+
| BIOS - Basic I/O System           ~1.5KB  |
|   17 entry points (jump table)            |
|   Hardware abstraction layer              |
+------------------------------------------+
```

Programs load at 0100h and call BDOS via `CALL 0005h` with function
number in C and parameter in DE.

## Memory Map (64K system)

```
0000-00FF   Page Zero (vectors, FCBs, DMA)
0100-DBFF   TPA (Transient Program Area)
DC00-E3FF   CCP  (0x800 bytes)
E400-F1FF   BDOS (0xE00 bytes)
F200-FFFF   BIOS (variable)
```

## Emulator Strategy

The simplest approach: trap CALL 0005h and implement BDOS functions in
native C code. No need for actual CP/M BDOS/BIOS binaries. Map CP/M
files to host filesystem (one host directory per CP/M drive). Console
I/O passes through to the host terminal.

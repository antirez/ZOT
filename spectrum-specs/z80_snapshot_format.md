# .z80 Snapshot File Format

## Version Detection

Check bytes 6-7 (PC) in the 30-byte header:
- **Version 1**: PC != 0 (actual program counter value)
- **Version 2/3**: PC == 0, then read extended header length at bytes 30-31:
  - Length 23 = Version 2
  - Length 54 (or 55) = Version 3

## Version 1 Header (30 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0  | 1 | A |
| 1  | 1 | F |
| 2  | 2 | BC (little-endian: C at offset 2, B at offset 3) |
| 4  | 2 | HL (little-endian: L at offset 4, H at offset 5) |
| 6  | 2 | PC (non-zero = version 1) |
| 8  | 2 | SP |
| 10 | 1 | I |
| 11 | 1 | R (bit 7 is bit 0 of byte 12) |
| 12 | 1 | Flags byte 1 (see below) |
| 13 | 2 | DE |
| 15 | 2 | BC' (shadow) |
| 17 | 2 | DE' (shadow) |
| 19 | 2 | HL' (shadow) |
| 21 | 1 | A' |
| 22 | 1 | F' |
| 23 | 2 | IY |
| 25 | 2 | IX |
| 27 | 1 | IFF1 (0=disabled, nonzero=enabled) |
| 28 | 1 | IFF2 |
| 29 | 1 | Flags byte 2 (see below) |

### Flags byte 1 (offset 12)

- Bit 0: Bit 7 of R register
- Bits 1-3: Border color (0-7)
- Bit 4: 1 = SamRom basic (ignore)
- Bit 5: 1 = data is compressed (version 1 only)
- Bits 6-7: unused

If byte 12 == 0xFF, treat it as 1 (for compatibility).

### Flags byte 2 (offset 29)

- Bits 0-1: Interrupt mode (0, 1, or 2)
- Bit 2: Issue 2 emulation (0=issue 3)
- Bit 3: Double interrupt frequency (ignore)
- Bits 4-5: Video synchronisation (ignore)
- Bits 6-7: Joystick type (ignore)

## Version 2/3 Extended Header

After the 30-byte header:

| Offset | Size | Field |
|--------|------|-------|
| 30 | 2 | Extended header length (23 or 54+) |
| 32 | 2 | PC (the real PC for v2/v3) |
| 34 | 1 | Hardware mode (0=48K, 3=128K, ...) |
| 35 | 1 | 128K page register (ignore for 48K) |
| 36 | 1 | Interface 1 paged (ignore) |
| 37 | 1 | R register emulation flags (ignore) |
| 38 | 1 | Last OUT to port 0xFFFD (AY, ignore for 48K) |
| 39 | 16 | AY register contents (ignore for 48K) |

For 48K: hardware mode byte should be 0 or 1 (48K or 48K+IF1).

## Version 1 Memory Data

After the 30-byte header, a single compressed block of 48KB:
- Addresses 0x4000-0xFFFF (the RAM portion)
- Compressed with ED ED RLE scheme
- Terminated by 00 ED ED 00

## Version 2/3 Memory Pages

After the extended header, memory is stored as page blocks:

Each block:
```
Bytes 0-1: Compressed data length (little-endian). 0xFFFF = uncompressed (16384 bytes raw)
Byte 2:    Page number
```
Followed by the compressed (or raw) data.

### 48K Page-to-Address Mapping

| Page | Address Range | Description |
|------|---------------|-------------|
| 4    | 0x8000-0xBFFF | Upper RAM bank 1 |
| 5    | 0xC000-0xFFFF | Upper RAM bank 2 |
| 8    | 0x4000-0x7FFF | Screen + contended RAM |

## Compression Scheme (ED ED RLE)

```
while (input remaining):
    byte = read()
    if byte != 0xED:
        output(byte)
    else:
        next = read()
        if next != 0xED:
            output(0xED)
            output(next)
        else:
            count = read()
            value = read()
            output value repeated count times
```

End-of-block marker (version 1 only): 00 ED ED 00

## Sources

- World of Spectrum: z80format.htm
- Flock of Spectrums: Reading Z80 Snapshots

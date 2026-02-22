# CP/M 2.2 BIOS (Basic I/O System) Reference

## Overview

The BIOS is the hardware-dependent layer of CP/M. All physical I/O goes through
the BIOS. The BDOS calls BIOS routines for disk and character I/O. User programs
never call BIOS directly -- they go through BDOS via CALL 0005h.

## Memory Map (64K system)

```
0000-00FF   Page Zero (vectors, FCBs, DMA buffer)
0100-DBFF   TPA (Transient Program Area)
DC00-E3FF   CCP  (~2KB, 0x800 bytes)
E400-F1FF   BDOS (~3.5KB, 0xE00 bytes)
F200-FFFF   BIOS (~1.5KB, variable)
```

CCP can be overwritten by large programs. Warm boot reloads CCP+BDOS from
system tracks of drive A:.

---

## BIOS Jump Table

17 entries, each a 3-byte JP instruction (51 bytes total):

```
BIOS+00:  JP BOOT       ;  0 - Cold start
BIOS+03:  JP WBOOT      ;  1 - Warm boot
BIOS+06:  JP CONST      ;  2 - Console status
BIOS+09:  JP CONIN      ;  3 - Console input
BIOS+0C:  JP CONOUT     ;  4 - Console output
BIOS+0F:  JP LIST       ;  5 - Printer output
BIOS+12:  JP PUNCH      ;  6 - Aux output
BIOS+15:  JP READER     ;  7 - Aux input
BIOS+18:  JP HOME       ;  8 - Home disk head
BIOS+1B:  JP SELDSK     ;  9 - Select disk
BIOS+1E:  JP SETTRK     ; 10 - Set track
BIOS+21:  JP SETSEC     ; 11 - Set sector
BIOS+24:  JP SETDMA     ; 12 - Set DMA address
BIOS+27:  JP READ       ; 13 - Read sector
BIOS+2A:  JP WRITE      ; 14 - Write sector
BIOS+2D:  JP LISTST     ; 15 - Printer status
BIOS+30:  JP SECTRAN    ; 16 - Sector translation
```

The warm boot JP at 0000h points to BIOS+03 (WBOOT). The BDOS calls these
entry points by offset from the known BIOS base address.

---

## BIOS Entry Points

### 0: BOOT (Cold Start) -- BIOS+00h

**Params:** None. **Returns:** Does not return.

Called once at power-on. Initializes hardware, sets up page zero (JP at 0000h
to WBOOT, JP at 0005h to BDOS), sets IOBYTE to default, sets drive to A:,
optionally prints sign-on message, transfers control to CCP.

---

### 1: WBOOT (Warm Boot) -- BIOS+03h

**Params:** None. **Returns:** Does not return.

Called on JP 0000h, Ctrl-C, or program exit. Reloads CCP+BDOS from system
tracks of drive A:. Re-initializes page zero (preserving IOBYTE and drive/user
at 0003-0004h). Resets DMA to 0080h. Transfers control to CCP.

---

### 2: CONST (Console Status) -- BIOS+06h

**Params:** None. **Returns:** A=00h (no char) or A=FFh (char ready).

Non-blocking check for console input.

---

### 3: CONIN (Console Input) -- BIOS+09h

**Params:** None. **Returns:** A=character.

Blocks until character available. Does NOT echo (BDOS handles echoing).
Should strip bit 7 to 7-bit ASCII.

---

### 4: CONOUT (Console Output) -- BIOS+0Ch

**Params:** C=character. **Returns:** None.

Sends character to console. Waits if device busy. BDOS handles tab expansion,
not CONOUT.

---

### 5: LIST (Printer Output) -- BIOS+0Fh

**Params:** C=character. **Returns:** None.

Sends character to printer. If no printer, should return without hanging.

---

### 6: PUNCH (Aux Output) -- BIOS+12h

**Params:** C=byte. **Returns:** None.

Sends byte to auxiliary output (serial port). Waits until ready.

---

### 7: READER (Aux Input) -- BIOS+15h

**Params:** None. **Returns:** A=byte.

Reads byte from auxiliary input. Blocks until available. If not implemented,
return 1Ah (Ctrl-Z / EOF).

---

### 8: HOME (Home Disk) -- BIOS+18h

**Params:** None. **Returns:** None.

Moves disk head to track 0. Often just calls SETTRK(0). Called before reading
system tracks during warm boot.

---

### 9: SELDSK (Select Disk) -- BIOS+1Bh

**Params:** C=drive (0=A, 1=B, ...), E=login flag (bit 0: 0=first select, 1=re-select).
**Returns:** HL=DPH address, or HL=0000h if drive invalid.

Selects drive for subsequent disk operations. Returns pointer to 16-byte Disk
Parameter Header. Each drive has its own DPH.

---

### 10: SETTRK (Set Track) -- BIOS+1Eh

**Params:** BC=track number (16-bit, 0-based). **Returns:** None.

Sets track for next READ/WRITE.

---

### 11: SETSEC (Set Sector) -- BIOS+21h

**Params:** BC=sector number (translated, from SECTRAN). **Returns:** None.

Sets sector for next READ/WRITE. This is the physical sector after translation.

---

### 12: SETDMA (Set DMA Address) -- BIOS+24h

**Params:** BC=address. **Returns:** None.

Sets memory address for next 128-byte disk transfer. Default 0080h.

---

### 13: READ (Read Sector) -- BIOS+27h

**Params:** None (uses disk/track/sector/DMA set previously).
**Returns:** A=00h (ok), 01h (error), FFh (media changed).

Reads one 128-byte sector into DMA address. If physical sectors are larger,
BIOS must deblock (read full sector, extract 128-byte portion).

---

### 14: WRITE (Write Sector) -- BIOS+2Ah

**Params:** C=write type. **Returns:** A=00h (ok), 01h (error), 02h (read-only).

Writes 128 bytes from DMA to disk.

Write type codes (for deblocking):
- **C=0:** Normal write, can be deferred
- **C=1:** Directory write, must be immediate
- **C=2:** First write to unallocated block, no pre-read needed

Simple BIOS without deblocking can ignore C and always write immediately.

---

### 15: LISTST (Printer Status) -- BIOS+2Dh

**Params:** None. **Returns:** A=00h (not ready) or FFh (ready).

If not implemented, return FFh (always ready).

---

### 16: SECTRAN (Sector Translation) -- BIOS+30h

**Params:** BC=logical sector, DE=translation table address. **Returns:** HL=physical sector.

If DE=0000h, no translation: return HL=BC.
Otherwise, index into table: `HL = table[BC]` (byte entries if SPT < 256).

Standard 8" disk skew-6 table (26 sectors):
```
1,7,13,19,25,5,11,17,23,3,9,15,21,2,8,14,20,26,6,12,18,24,4,10,16,22
```

---

## Disk Parameter Header (DPH) -- 16 bytes

Returned by SELDSK. One per drive.

```
Offset  Size  Field    Description
  +0      2    XLT     Sector translation table address (0=none)
  +2      6    scratch  Three 16-bit scratch words (BDOS workspace)
  +8      2    DIRBUF  128-byte directory buffer (shared by ALL drives)
 +10      2    DPB     Disk Parameter Block address
 +12      2    CSV     Checksum vector (per-drive, CKS bytes)
 +14      2    ALV     Allocation vector (per-drive, (DSM/8)+1 bytes)
```

Multiple drives can share the same DPB if they have the same format. DIRBUF
is shared across all drives. CSV and ALV must be unique per drive.

---

## Disk Parameter Block (DPB) -- 15 bytes

```
Offset  Size  Field  Description
  +0      2    SPT   128-byte sectors per track
  +2      1    BSH   Block shift: log2(BLS/128)
  +3      1    BLM   Block mask: BLS/128 - 1
  +4      1    EXM   Extent mask
  +5      2    DSM   Total blocks - 1
  +7      2    DRM   Directory entries - 1
  +9      1    AL0   Directory allocation bitmap (high byte)
 +10      1    AL1   Directory allocation bitmap (low byte)
 +11      2    CKS   Checksum vector size
 +13      2    OFF   Reserved system tracks
```

Block size = 128 * 2^BSH. Disk capacity = BLS * (DSM+1).

---

## Standard 8" SSSD Disk (IBM 3740)

77 tracks, 26 sectors/track, 128 bytes/sector, 256KB raw.

```
SPT=26  BSH=3  BLM=7  EXM=0  DSM=242  DRM=63
AL0=0xC0  AL1=0x00  CKS=16  OFF=2
```

242KB usable, 64 directory entries, 1KB blocks, 2 system tracks.

---

## BDOS-to-BIOS Call Sequence for Disk I/O

1. SELDSK(drive, login_flag) -- get DPH
2. Calculate track and logical sector from record number
3. SECTRAN(logical_sector, XLT) -- translate to physical sector
4. SETTRK(track)
5. SETSEC(physical_sector)
6. SETDMA(buffer_address)
7. READ() or WRITE(type)

Each call transfers exactly one 128-byte record.

---

## Register Convention Summary

| Function | Entry | Exit |
|----------|-------|------|
| BOOT     | --    | no return |
| WBOOT    | --    | no return |
| CONST    | --    | A=00/FF |
| CONIN    | --    | A=char |
| CONOUT   | C=char | -- |
| LIST     | C=char | -- |
| PUNCH    | C=char | -- |
| READER   | --    | A=char |
| HOME     | --    | -- |
| SELDSK   | C=drive, E=login | HL=DPH/0 |
| SETTRK   | BC=track | -- |
| SETSEC   | BC=sector | -- |
| SETDMA   | BC=addr | -- |
| READ     | --    | A=err |
| WRITE    | C=type | A=err |
| LISTST   | --    | A=00/FF |
| SECTRAN  | BC=log, DE=tbl | HL=phys |

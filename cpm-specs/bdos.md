# CP/M 2.2 BDOS (Basic Disk Operating System) Reference

## Calling Convention

All BDOS functions are invoked via `CALL 0005h`:

```asm
LD   C, function_number    ; Function number in C (0-40)
LD   DE, parameter         ; 16-bit param (address), or E = 8-bit param
CALL 0005h                 ; BDOS entry point
; Return: A=L for 8-bit results, HL=BA for 16-bit results
```

- **C** = BDOS function number (0-40)
- **DE** = 16-bit parameter (FCB address, buffer address, string address)
- **E** = 8-bit parameter (character, drive number, user number)
- 8-bit return in **both A and L**
- 16-bit return in **both HL and BA** (B=H, A=L)
- BDOS does NOT preserve BC, DE, HL, or AF across calls
- Default DMA address is 0080h (reset by function 13)

---

## Page Zero Layout

```
0000h       JMP BIOS+3 (warm boot vector)
0003h       IOBYTE
0004h       Current drive/user (high nibble=user, low nibble=drive)
0005h       JMP BDOS (entry point)
0006h-07h   BDOS address (high byte = top of TPA)
005Ch       Default FCB #1 (parsed from command line arg 1)
006Ch       Default FCB #2 (parsed from command line arg 2)
0080h       Default DMA buffer / command tail (length byte + text)
0100h       TPA start (program load address)
```

---

## File Control Block (FCB) -- 36 bytes

```
Offset  Size  Field  Description
  +0      1    DR    Drive: 0=default, 1=A:, 2=B:, ... 16=P:
  +1      8    F1-F8 Filename, uppercase ASCII, space-padded (20h)
  +9      3    T1-T3 Extension, space-padded. High bits are attributes:
                     T1 bit 7 = Read-Only
                     T2 bit 7 = System (hidden from DIR)
                     T3 bit 7 = Archive
 +12      1    EX    Extent low byte. Set to 0 before OPEN/MAKE
 +13      1    S1    Reserved. Set to 0
 +14      1    S2    Extent high byte (bits 0-5). Set to 0
 +15      1    RC    Record count in current extent (0-128)
 +16     16    AL    Allocation map (block pointers for this extent)
                     DSM < 256: 16 single-byte block numbers
                     DSM >= 256: 8 double-byte (16-bit LE) block numbers
 +32      1    CR    Current record within extent (0-127). Set to 0 for sequential I/O
 +33      1    R0    Random record number, low byte
 +34      1    R1    Random record number, middle byte
 +35      1    R2    Random record number, high byte (must be 0)
```

### Wildcards

`?` (3Fh) matches any character in search operations (functions 17, 18, 19).
`*` is expanded by the CCP to fill remaining positions with `?`.

### Extent/Record Mapping

Each extent holds up to 16K (128 records of 128 bytes):

```
CR = (file_position % 16384) / 128
EX = (file_position % 524288) / 16384
S2 = file_position / 524288
extent_number = 32 * S2 + EX
```

### Random Record Mapping

```
random_record = R0 + (R1 << 8) + (R2 << 16)
CR = random_record & 0x7F
EX = (random_record >> 7) & 0x1F
S2 = random_record >> 12
```

Reverse: `random_record = S2 * 4096 + EX * 128 + CR`

---

## Directory Entry -- 32 bytes (on disk)

Identical to FCB bytes 0-31. Four entries per 128-byte sector.

```
+0    ST    Status: 0-15 = user number, E5h = deleted
+1    F1-F8 Filename (high bits = attributes)
+9    T1-T3 Extension (high bits = R/O, SYS, Archive)
+12   EX    Extent low byte
+13   S1    Reserved (0)
+14   S2    Extent high byte
+15   RC    Record count (0-128)
+16   AL    Allocation block pointers (16 bytes)
```

---

## Disk Parameter Block (DPB) -- 15 bytes

```
Offset  Size  Field  Description
  +0      2    SPT   Sectors (128-byte records) per track
  +2      1    BSH   Block shift: log2(block_size / 128)
  +3      1    BLM   Block mask: (block_size / 128) - 1
  +4      1    EXM   Extent mask
  +5      2    DSM   Disk size - 1 (highest block number)
  +7      2    DRM   Directory entries - 1
  +9      1    AL0   Allocation bitmap byte 0 (directory blocks, MSB first)
 +10      1    AL1   Allocation bitmap byte 1
 +11      2    CKS   Checksum vector size: (DRM+1)/4 removable, 0 fixed
 +13      2    OFF   Track offset (reserved system tracks)
```

### Block Size Table

| Block Size | BSH | BLM | EXM (DSM<256) | EXM (DSM>=256) |
|-----------|-----|-----|---------------|----------------|
| 1,024     | 3   | 7   | 0             | N/A            |
| 2,048     | 4   | 15  | 1             | 0              |
| 4,096     | 5   | 31  | 3             | 1              |
| 8,192     | 6   | 63  | 7             | 3              |
| 16,384    | 7   | 127 | 15            | 7              |

### AL0/AL1 Directory Allocation

16-bit bitmap (AL0 is high byte). Bit 7 of AL0 = block 0. Set bits = directory blocks.
- 2 blocks: AL0=0xC0, AL1=0x00
- 4 blocks: AL0=0xF0, AL1=0x00

---

## BDOS Functions

### Function 0: System Reset (P_TERMCPM)

**Entry:** C=0. **Return:** Does not return.

Warm boot. Terminates program, reloads CCP, returns to command prompt. All drives
except A: are deactivated. Close files before calling (data loss otherwise).

---

### Function 1: Console Input (C_READ)

**Entry:** C=1. **Return:** A=character.

Waits for keyboard input, echoes to console. Handles Ctrl-S (pause), Ctrl-P
(printer echo toggle), Ctrl-C (warm boot). Tabs expanded to 8-column stops.

---

### Function 2: Console Output (C_WRITE)

**Entry:** C=2, E=character. **Return:** None.

Sends character to console. Tabs expanded. Checks Ctrl-S/Ctrl-P during output.

---

### Function 3: Auxiliary Input (A_READ)

**Entry:** C=3. **Return:** A=character.

Reads from auxiliary/reader device. Blocks until available.

---

### Function 4: Auxiliary Output (A_WRITE)

**Entry:** C=4, E=character. **Return:** None.

Sends character to auxiliary/punch device.

---

### Function 5: Printer Output (L_WRITE)

**Entry:** C=5, E=character. **Return:** None.

Sends character to list (printer) device.

---

### Function 6: Direct Console I/O (C_RAWIO)

**Entry:** C=6, E=code. **Return:** A=result.

Raw console I/O, bypasses Ctrl-S/Ctrl-P/Ctrl-C processing.

| E value | Operation |
|---------|-----------|
| 00h-FEh | Output character E |
| FFh     | Input: return char in A, or 00h if none ready |

---

### Function 7: Get I/O Byte (A_STATIN)

**Entry:** C=7. **Return:** A=IOBYTE from address 0003h.

IOBYTE format:

| Bits | Device  | 0=   | 1=   | 2=   | 3=   |
|------|---------|------|------|------|------|
| 1-0  | CONSOLE | TTY  | CRT  | BAT  | UC1  |
| 3-2  | READER  | TTY  | PTR  | UR1  | UR2  |
| 5-4  | PUNCH   | TTY  | PTP  | UP1  | UP2  |
| 7-6  | LIST    | TTY  | CRT  | LPT  | UL1  |

---

### Function 8: Set I/O Byte (A_STATOUT)

**Entry:** C=8, E=IOBYTE. **Return:** None.

Stores E into IOBYTE at 0003h.

---

### Function 9: Print String (C_WRITESTR)

**Entry:** C=9, DE=string address. **Return:** None.

Prints ASCII string terminated by '$' (24h). Tab expansion and Ctrl-S/Ctrl-P active.

---

### Function 10: Read Console Buffer (C_READSTR)

**Entry:** C=10, DE=buffer address. **Return:** Characters in buffer.

Buffered line input with editing:

```
Buffer+0: max chars (set by caller, 1-255)
Buffer+1: actual chars read (set by BDOS, excludes CR)
Buffer+2: character data
```

Editing keys: Ctrl-C (abort), Ctrl-H/DEL (backspace), Ctrl-U (cancel line),
Ctrl-X (erase line), Ctrl-R (retype), Ctrl-E (newline, continue input).

---

### Function 11: Console Status (C_STAT)

**Entry:** C=11. **Return:** A=00h (nothing) or FFh/01h (char ready).

Non-blocking check for pending console input.

---

### Function 12: Version Number (S_BDOSVER)

**Entry:** C=12. **Return:** HL: H=system type, L=version.

For CP/M 2.2: L=22h, H=00h (CP/M for 8080/Z80).

---

### Function 13: Reset Disk System (DRV_ALLRESET)

**Entry:** C=13. **Return:** None.

Resets all drives to read-write, deactivates all except A:, reselects A:,
resets DMA to 0080h. User number unaffected.

---

### Function 14: Select Disk (DRV_SET)

**Entry:** C=14, E=drive (0=A, 1=B, ... 15=P). **Return:** None.

Selects default drive. Drive is logged in on first access.

---

### Function 15: Open File (F_OPEN)

**Entry:** C=15, DE=FCB address. **Return:** A=0-3 (success) or FFh (not found).

Opens existing file. Set DR, filename, extension in FCB. Set EX, S1, S2 to 0.
After open, set CR to 0 for sequential access. BDOS fills allocation map and RC.

---

### Function 16: Close File (F_CLOSE)

**Entry:** C=16, DE=FCB address. **Return:** A=0-3 (success) or FFh (error).

Writes FCB extent info back to directory. **Must** close after writing or data is lost.

---

### Function 17: Search First (F_SFIRST)

**Entry:** C=17, DE=FCB address. **Return:** A=0-3 (found) or FFh.

Searches directory for first match. Wildcards `?` supported. On success, matching
entry is at DMA + (A * 32). If DR byte is `?`, matches ALL entries including deleted.

---

### Function 18: Search Next (F_SNEXT)

**Entry:** C=18 (no params). **Return:** A=0-3 or FFh.

Continues search from function 17. Don't modify FCB or do disk ops between calls.

---

### Function 19: Delete File (F_DELETE)

**Entry:** C=19, DE=FCB address. **Return:** A=0-3 (success) or FFh.

Deletes all matching entries. Wildcards supported. Frees allocation blocks.

---

### Function 20: Read Sequential (F_READ)

**Entry:** C=20, DE=FCB address. **Return:** A=00h (ok) or 01h (EOF).

Reads 128 bytes at position CR into DMA buffer. Increments CR. Auto-advances
to next extent when CR reaches 128.

---

### Function 21: Write Sequential (F_WRITE)

**Entry:** C=21, DE=FCB address. **Return:** A=00h (ok), 01h (dir full), 02h (disk full).

Writes 128 bytes from DMA to file at position CR. Increments CR. Allocates blocks
as needed. **Close file after writes.**

---

### Function 22: Make File (F_MAKE)

**Entry:** C=22, DE=FCB address. **Return:** A=0-3 (success) or FFh (dir full).

Creates new file. Set DR, filename, extension, EX=S1=S2=0. Set CR to 0 after success.
Does NOT check for duplicates -- delete first if needed.

---

### Function 23: Rename File (F_RENAME)

**Entry:** C=23, DE=FCB address. **Return:** A=0-3 (success) or FFh.

FCB layout: bytes 0-15 = old name (DR + 11 chars), bytes 16-31 = new name (byte 16 = 0,
new name at 17-27). All extents of old name are renamed.

---

### Function 24: Login Vector (DRV_LOGINVEC)

**Entry:** C=24. **Return:** HL=16-bit bitmap.

Bit 0=A, bit 1=B, etc. Set bit = drive logged in.

---

### Function 25: Current Disk (DRV_GET)

**Entry:** C=25. **Return:** A=drive (0=A, 1=B, ...).

---

### Function 26: Set DMA Address (F_DMAOFF)

**Entry:** C=26, DE=address. **Return:** None.

Sets buffer for read/write/search operations. Default 0080h.

---

### Function 27: Get Allocation Vector (DRV_ALLOCVEC)

**Entry:** C=27. **Return:** HL=address of allocation vector.

Bit map of current drive. Bit 7 of first byte = block 0. 1=allocated, 0=free.
Size: (DSM/8)+1 bytes. Read-only.

---

### Function 28: Write Protect Disk (DRV_SETRO)

**Entry:** C=28. **Return:** None.

Sets current drive read-only until reset (function 13 or 37).

---

### Function 29: Read-Only Vector (DRV_ROVEC)

**Entry:** C=29. **Return:** HL=16-bit bitmap.

Bit 0=A, etc. Set bit = drive is read-only.

---

### Function 30: Set File Attributes (F_ATTRIB)

**Entry:** C=30, DE=FCB address. **Return:** A=0-3 (success) or FFh.

Updates directory attribute bits from FCB high bits. T1 bit 7=R/O, T2 bit 7=SYS,
T3 bit 7=Archive.

---

### Function 31: Get DPB Address (DRV_DPB)

**Entry:** C=31. **Return:** HL=DPB address for current drive.

---

### Function 32: Get/Set User Number (F_USERNUM)

**Entry:** C=32, E=FFh (get) or E=0-15 (set). **Return:** A=current user (when getting).

16 user areas (0-15) per drive. Default is 0.

---

### Function 33: Read Random (F_READRAND)

**Entry:** C=33, DE=FCB address. **Return:** A=error code.

Reads record at position R0/R1/R2 into DMA. File must be opened. Updates EX/CR
for subsequent sequential I/O.

| A | Meaning |
|---|---------|
| 0 | Success |
| 1 | Reading unwritten data |
| 3 | Cannot close extent |
| 4 | Seek to unwritten extent |
| 6 | Record out of range (R2 non-zero) |

---

### Function 34: Write Random (F_WRITERAND)

**Entry:** C=34, DE=FCB address. **Return:** A=error code.

Writes DMA to record at R0/R1/R2. Creates extents/blocks as needed. **Close after.**

| A | Meaning |
|---|---------|
| 0 | Success |
| 2 | Disk full |
| 3 | Cannot close extent |
| 5 | Directory full |
| 6 | Record out of range |

---

### Function 35: Compute File Size (F_SIZE)

**Entry:** C=35, DE=FCB address. **Return:** R0/R1/R2 set to file size in records.

Scans all extents. File need not be opened. Set EX=S2=0 before calling.

---

### Function 36: Set Random Record (F_RANDREC)

**Entry:** C=36, DE=FCB address. **Return:** R0/R1/R2 set from sequential position.

Converts EX/S2/CR to random record: `R = S2*4096 + EX*128 + CR`.

---

### Function 37: Reset Drive (DRV_RESET)

**Entry:** C=37, DE=drive bitmap. **Return:** A=00h.

Selectively resets drives. Bit 0=A, etc. Clears write-protect, un-logs drive.

---

### Function 40: Write Random with Zero Fill (F_WRITEZF)

**Entry:** C=40, DE=FCB address. **Return:** Same as function 34.

Same as function 34, but newly allocated blocks are zeroed first.

---

## Summary Table

| # | Name | Entry | Return | Description |
|---|------|-------|--------|-------------|
| 0 | System Reset | -- | no return | Warm boot |
| 1 | Console Input | -- | A=char | Read+echo |
| 2 | Console Output | E=char | -- | Write char |
| 3 | Aux Input | -- | A=char | Reader |
| 4 | Aux Output | E=char | -- | Punch |
| 5 | List Output | E=char | -- | Printer |
| 6 | Direct Con I/O | E=char/FFh | A=char/00h | Raw I/O |
| 7 | Get IOBYTE | -- | A=IOBYTE | |
| 8 | Set IOBYTE | E=val | -- | |
| 9 | Print String | DE=addr | -- | $-terminated |
| 10 | Read Buffer | DE=buf | chars in buf | Line input |
| 11 | Console Status | -- | A=00/FF | Char ready? |
| 12 | Version | -- | HL=0022h | CP/M 2.2 |
| 13 | Reset Disks | -- | -- | All drives |
| 14 | Select Disk | E=drive | -- | 0=A, 1=B... |
| 15 | Open File | DE=FCB | A=dir/FFh | |
| 16 | Close File | DE=FCB | A=dir/FFh | |
| 17 | Search First | DE=FCB | A=dir/FFh | |
| 18 | Search Next | -- | A=dir/FFh | |
| 19 | Delete File | DE=FCB | A=dir/FFh | |
| 20 | Read Sequential | DE=FCB | A=err | 128 bytes |
| 21 | Write Sequential | DE=FCB | A=err | 128 bytes |
| 22 | Make File | DE=FCB | A=dir/FFh | Create |
| 23 | Rename | DE=FCB | A=dir/FFh | |
| 24 | Login Vector | -- | HL=bitmap | |
| 25 | Current Disk | -- | A=drive | |
| 26 | Set DMA | DE=addr | -- | |
| 27 | Alloc Vector | -- | HL=addr | |
| 28 | Write Protect | -- | -- | |
| 29 | R/O Vector | -- | HL=bitmap | |
| 30 | Set Attributes | DE=FCB | A=dir/FFh | |
| 31 | Get DPB | -- | HL=addr | |
| 32 | User Number | E=FF/user | A=user | Get/set |
| 33 | Read Random | DE=FCB | A=err | |
| 34 | Write Random | DE=FCB | A=err | |
| 35 | File Size | DE=FCB | r0/r1/r2 | |
| 36 | Set Random Rec | DE=FCB | r0/r1/r2 | |
| 37 | Reset Drive | DE=bitmap | A=00h | |
| 40 | Write Rand Zero | DE=FCB | A=err | Zero fill |

---

## Error Code Reference

### Directory codes (functions 15-19, 22-23, 30)
- 0-3: Success (position in directory sector)
- FFh: Error (not found / dir full)

### Sequential I/O (functions 20, 21)
- 00h: Success
- 01h: EOF (read) / dir full (write)
- 02h: Disk full (write)

### Random I/O (functions 33, 34, 40)
- 0: Success
- 1: Unwritten data (read)
- 2: Disk full (write)
- 3: Cannot close extent
- 4: Unwritten extent (read)
- 5: Directory full (write)
- 6: Record out of range (R2 non-zero)

# CP/M 2.2 CCP (Console Command Processor) Reference

## Overview

The CCP is the command-line interface of CP/M. It reads commands from the
console (or from a submit file), parses them, executes built-in commands,
and loads/runs .COM files from disk. It occupies ~2KB (0x800 bytes) just
below the BDOS in memory.

## The Prompt

```
A>
```

Format: drive letter + `>`, no space. After switching to B:, it becomes `B>`.
A CR/LF is printed before each prompt.

---

## Command Line Parsing

1. Read line via BDOS function 10 (buffered input, max 127 chars)
2. Convert all lowercase to uppercase
3. Parse command name into FCB using `fillfcb`
4. If wildcards (`?`) in command name, error
5. If bare drive letter (e.g., `B:`), switch drives
6. Check built-in command table (only if no drive prefix on command)
7. If not built-in, load and run `<name>.COM`

### FCB Filling (`fillfcb`)

Delimiters: space, `=`, `_`, `.`, `:`, `;`, `<`, `>`, control chars, null.

1. Skip leading spaces
2. Check for drive prefix `X:` -- set drive code (1=A, 2=B, ...)
3. Parse up to 8 filename chars. `*` fills remaining with `?`
4. If `.` found, parse up to 3 extension chars. `*` fills with `?`
5. Pad unused positions with spaces (20h)
6. Zero out EX, S1, S2

### Important

Built-in commands are only recognized without a drive prefix. `A:DIR` tries
to load DIR.COM, it does NOT run the built-in DIR. This is because the
built-in check is skipped when a drive code is present.

---

## Built-in Commands

### DIR -- Directory Listing

```
DIR              (all files on current drive)
DIR d:           (all files on drive d)
DIR filespec     (matching files, wildcards OK)
```

- Displays 4 entries per line: `A: FILENAME TYP : FILENAME TYP`
- Skips files with System attribute (T2 bit 7 set)
- Prints `NO FILE` if no matches
- Break key (any key) aborts listing

### ERA -- Erase Files

```
ERA filespec     (wildcards OK)
ERA *.*          (prompts: ALL (Y/N)?)
```

- Prompts for confirmation on `*.*`
- Prints `NO FILE` if no matches
- Cannot delete read-only files (BDOS rejects it)

### TYPE -- Display File

```
TYPE filename.typ
```

- No wildcards allowed
- Reads sequentially, prints chars until Ctrl-Z (1Ah) or EOF
- Prints `READ ERROR` on disk error
- Break key aborts

### SAVE -- Save Memory to File

```
SAVE n filename.typ
```

- `n` = decimal number of 256-byte pages to save from 0100h
- Deletes existing file, creates new, writes n*2 sectors
- Prints `NO SPACE` if disk/directory full

### REN -- Rename File

```
REN newname.typ=oldname.typ
```

- `=` (or `_`) separates new name (left) from old name (right)
- No wildcards allowed
- Prints `FILE EXISTS` if new name already exists
- Prints `NO FILE` if old name not found

### USER -- Set User Number

```
USER n           (n = 0-15)
```

- Changes the current user area
- Files in different user areas are independent

---

## Loading .COM Files

When a command is not built-in:

1. Force extension to `COM`
2. Open file on specified (or current) drive
3. Load 128-byte sectors sequentially starting at 0100h
4. If load exceeds CCP base address, print `BAD LOAD` and abort
5. Fill default FCBs at 005Ch and 006Ch (see below)
6. Fill command tail at 0080h
7. Set DMA to 0080h
8. `CALL 0100h` (program can return via RET if CCP not overwritten)

### Error: `<name>?`

Printed when .COM file not found. The command name followed by `?`.

---

## Default FCBs and Command Tail

For a command like: `PROGRAM B:DATA.TXT OUTPUT.LST`

### Command tail at 0080h

```
0080h: 21          (length byte: 21 chars including leading space)
0081h: " B:DATA.TXT OUTPUT.LST"
```

The tail starts from the space after the command name.

### FCB at 005Ch (first argument)

```
005Ch: 02          (drive B:)
005Dh: "DATA    "  (8 chars, space-padded)
0065h: "TXT"       (3 chars)
0068h: 00 00 00    (EX, S1, S2 zeroed)
```

### FCB at 006Ch (second argument)

```
006Ch: 00          (default drive)
006Dh: "OUTPUT  "  (8 chars)
0075h: "LST"       (3 chars)
0078h: 00 00 00    (EX, S1, S2 zeroed)
```

### Critical: FCB overlap

The second FCB at 006Ch overlaps bytes 16-31 of the first FCB (the
allocation map area). A program must copy the second FCB elsewhere
before opening a file via the first FCB, or the second FCB will be
overwritten by the directory data.

---

## Drive Switching

```
A>B:
B>
```

Typing just a drive letter and colon switches the default drive. The CCP
selects the new drive via BDOS function 14 and updates the prompt.

A drive prefix on filenames temporarily selects that drive for the
operation, then restores the original:

```
A>TYPE B:README.TXT     (reads from B:, returns to A>)
```

---

## Error Messages

| Message | Condition |
|---------|-----------|
| `<name>?` | Unknown command, file not found, syntax error |
| `NO FILE` | DIR/ERA/REN found no matching files |
| `ALL (Y/N)?` | ERA *.* confirmation |
| `READ ERROR` | Disk error during TYPE |
| `NO SPACE` | Disk/directory full during SAVE |
| `FILE EXISTS` | REN target name exists |
| `BAD LOAD` | .COM file too large for TPA |

---

## SUBMIT Mechanism

SUBMIT.COM (a transient command) processes `.SUB` files for batch execution.

```
SUBMIT filename [param1 param2 ...]
```

1. Reads `filename.SUB`, substitutes `$1`, `$2`, etc. with parameters
2. Creates `$$$.SUB` on drive A: with commands in reverse order
3. CCP detects `$$$.SUB` via BDOS function 13 return value
4. Reads commands from `$$$.SUB` instead of console
5. Deletes `$$$.SUB` on completion or error

### $$$.SUB format

Each record (128 bytes) contains: length byte + command text.
Records stored last-command-first (CCP reads from end).

---

## CCP Startup Sequence

On warm boot, BIOS jumps to CCP with C register = `(user << 4) | drive`.

1. Extract user number from C high nibble, call BDOS Set User
2. Call BDOS Reset Disk System (function 13), get submit flag
3. Extract drive from C low nibble, call BDOS Select Disk
4. Enter main loop: prompt, read, parse, execute

---

## BDOS Functions Used by CCP

| Function | # | Usage |
|----------|---|-------|
| Console Input | 1 | Break key check |
| Console Output | 2 | Character output |
| Read Buffer | 10 | Command line input |
| Console Status | 11 | Break key detection |
| Reset Disks | 13 | Startup, submit detection |
| Select Disk | 14 | Drive switching |
| Open File | 15 | TYPE, transient loading |
| Close File | 16 | SAVE |
| Search First | 17 | DIR, ERA, REN |
| Search Next | 18 | DIR |
| Delete File | 19 | ERA, SAVE (overwrite) |
| Read Sequential | 20 | TYPE, transient loading |
| Write Sequential | 21 | SAVE |
| Make File | 22 | SAVE |
| Rename File | 23 | REN |
| Current Disk | 25 | Prompt display |
| Set DMA | 26 | File I/O |
| Get/Set User | 32 | USER command, startup |

Note: The CCP uses its own 0-terminated string print routine, NOT BDOS
function 9 (which uses $-terminated strings).

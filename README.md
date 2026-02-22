# ZOT: a Z80, ZX Spectrum 48K and CP/M 2.2 Emulators

The ZOT project is a Z80 CPU emulator, a ZX Spectrum 48K emulator, and a CP/M 2.2 operating system emulator, all built on top of the same Z80 core. Everything is written in C with no dependencies beyond the standard library (the SDL frontend is optional), and the Z80 core is small enough to run on microcontrollers like the RP2040 while being accurate enough to pass every documented and undocumented Z80 test in the ZEXALL suite.

The Spectrum emulator is about 3,500 lines of C across four files: the Z80 core (`z80.c`), the Spectrum emulation (`spectrum.c`), the tape player (`tzx.c`), and the SDL frontend (`zxsdl.c`). The CP/M emulator adds another ~2,000 lines across `cpm.c` and its frontends.

**This project was initially an experiment on clean room development with AI tools**, but turned out to be good enough to be published. Read later for more information about the AI setup used.

## AI Disclaimer

This emulator was implemented in a clean room setup using Claude Code Opus 4.6. The following is the process used in order to write the implementation and avoid contamination with other implementations.

* No other emulators source code were available to the agent.
* A human-written design file was provided with the main architecture and goals of the emulator.
* The agent did a research on the Z80, CP/M specifications and ZX Spectrum internals, ULA, and cassette / image file types. After the information was collected, the agent was restarted with a new session (and the old session removed from disk), so that no contamination of the LLM context with other implementations of Z80, CP/M or ZX Spectrum emulators were possibile.
* The implementation was performed with human feedbacks steering certain design decisions constantly. The agent had no Internet access during the implementation. No code was written by hand, only prompts and design documents.

## The Z80 core

The Z80 CPU emulator lives in `z80.h` and `z80.c`. The central design choice is that `z80_step()` executes one complete instruction and returns how many T-states (clock cycles) it consumed. This is sometimes called "instruction-stepping" as opposed to "cycle-stepping," where each T-state is a separate function call. Cycle-stepping would let you model things like what happens on the bus halfway through a memory write, but in practice no ZX Spectrum software depends on this -- what matters is that each instruction takes the right total number of T-states, and that the flags are set correctly. Instruction-stepping is dramatically simpler, faster, and uses less memory, which matters when the target is a microcontroller with 264KB of RAM.

The CPU state is a plain C struct with individual `uint8_t` fields for each register (A, F, B, C, D, E, H, L, and their shadow counterparts), `uint16_t` for IX, IY, SP, and PC, and function pointers for memory and I/O access. The function pointers are how the Z80 core stays decoupled from any specific system -- the Spectrum provides callbacks that implement its ROM/RAM layout and ULA port decoding, but you could wire the same core into a CP/M machine or an MSX by providing different callbacks.

All instructions are implemented: the 256 unprefixed opcodes, the CB-prefixed bit/rotate/shift group, the ED-prefixed extended group (block transfers, 16-bit arithmetic, I/O block operations), and the DD/FD-prefixed IX/IY variants. The double-prefixed DDCB and FDCB indexed bit operations are included too, along with every known undocumented opcode: SLL (shift left, inserting 1 into bit 0), the IX/IY half-register operations (LD A,IXH and friends), and the DDCB store-to-register variants where an indexed bit operation like `RLC (IX+d)` also copies its result into a register.

Flag computation follows the real silicon, including the undocumented X (bit 3) and Y (bit 5) flags. Most instructions set these to copies of the corresponding bits of the result, but certain instructions (notably CP and the block operations CPI/CPIR/CPD/CPDR) take them from the operand or an intermediate value instead. Getting this right is essential for passing ZEXALL. A precomputed 256-byte table (`sz53p_table`) provides the common S/Z/Y/X/P flags for any byte value, so most ALU operations just index into it and OR in the instruction-specific H, N, and C flags.

The R register (memory refresh counter) is incremented correctly: once per instruction fetch for unprefixed opcodes, and once per prefix byte for prefixed ones. Only the low 7 bits increment; bit 7 is preserved from the last `LD R,A`. This matters because some copy protection schemes use R for pseudo-random number generation.

### Testing

The Z80 core has 154 unit tests covering every instruction group, flag edge case, and timing requirement. More importantly, it passes both ZEXDOC and ZEXALL -- Frank Cringle's exhaustive Z80 test suites that systematically test every instruction against all flag combinations. ZEXDOC tests the documented flag behavior (67 tests), while ZEXALL also tests the undocumented X and Y flags (67 tests). Both pass at 100%. These tests run under a minimal CP/M harness that emulates just enough BDOS (calls 0, 2, and 9) to let the test programs print their results.

To run the tests:

    make test              # 154 unit tests + 49 Spectrum tests
    ./z80_test --zexdoc    # ZEXDOC suite (requires z80-specs/zexdoc.com)
    ./z80_test --zexall    # ZEXALL suite (requires z80-specs/zexall.com)

## The Spectrum emulator

The Spectrum emulation lives in `spectrum.h` and `spectrum.c`. It wraps the Z80 core with the hardware that makes a Spectrum a Spectrum: 16KB of ROM, 48KB of RAM, the ULA chip (video, keyboard, beeper, tape I/O), and memory contention.

### Memory map and I/O

The Spectrum's 64KB address space is simple: ROM at 0x0000-0x3FFF (read-only, writes silently ignored), and RAM at 0x4000-0xFFFF. Screen memory starts at 0x4000, which means the ULA and CPU fight over the same RAM -- more on that below.

I/O uses the Z80's separate port address space, but the Spectrum only partially decodes addresses. The ULA responds to any even port (bit 0 of the address is 0). On a read, it returns the keyboard state: the upper address byte selects which of the 8 half-rows to scan, and multiple rows can be selected simultaneously by pulling multiple address lines low, with results ANDed together. Bits 0-4 are the key data (active low), bit 6 is the EAR input from the tape, and bits 5 and 7 are always 1. On a write, bits 0-2 set the border color, bit 3 is MIC output (ignored), and bit 4 drives the beeper.

The Kempston joystick responds when address bits A7-A5 are all 0, returning an active-high byte with bits for right, left, down, up, and fire.

### Memory contention

The ULA and CPU share the 16KB of RAM at 0x4000-0x7FFF. During the 192 display scanlines, the ULA needs to fetch screen data, and it wins -- the CPU is stalled until the ULA's fetch cycle completes. The delay depends on where in the ULA's 8 T-state fetch cycle the CPU tries to access that RAM:

    Cycle position:  0  1  2  3  4  5  6  7
    CPU wait states: 6  5  4  3  2  1  0  0

Many emulators implement this with a 69,888-byte lookup table (one entry per T-state in a frame). We use arithmetic instead: given the current frame T-state, compute which scanline and position within that line, check if it's in the contended region (the first 128 T-states of display lines 64-255), and index into an 8-byte pattern table. This uses 8 bytes of data instead of 68KB, which matters for microcontroller targets.

The contention is applied inside the memory read/write callbacks, so the Z80 core doesn't need to know about it. Every time the CPU accesses an address in the 0x4000-0x7FFF range, the callback checks the current frame T-state and adds the appropriate delay. This means contention is transparent to the instruction execution -- the correct number of wait states is added to `frame_tstates` before the memory access completes.

### Frame timing

The ULA generates a PAL signal at 50.08 Hz. Each frame is exactly 69,888 T-states, divided into 312 scanlines of 224 T-states each. The frame starts with 8 lines of vertical sync, then 56 lines of top border, 192 lines of display, and 56 lines of bottom border. A maskable interrupt fires at T-state 0 of each frame. The Spectrum ROM's interrupt handler (in IM 1 mode) handles keyboard scanning and the FLASH attribute toggle.

The `zx_tick()` function is the core primitive. It executes Z80 instructions, renders scanlines as the beam crosses them, and detects frame boundaries. It takes a `min_tstates` parameter that controls granularity: pass 0 to execute a single instruction (needed for tape loading where the EAR bit must be updated between every instruction), pass 224 to execute one scanline's worth of instructions (useful for embedded systems that need to interleave rendering or I/O with emulation), or call `zx_frame()` which simply loops `zx_tick()` until a frame boundary.

The frame boundary always takes priority over the minimum T-state count -- `zx_tick()` returns immediately when it crosses a frame boundary so the caller never misses the event, even if it asked for more T-states. At the frame boundary, `zx_tick()` flushes remaining scanlines, renders audio, carries over excess T-states, increments the frame counter, toggles FLASH every 16 frames, and fires the next interrupt.

### Video rendering

The Spectrum's screen memory layout is famously non-linear. The 6,144-byte bitmap at 0x4000-0x57FF is divided into three "thirds" of 64 pixel rows each, and within each third, rows are interleaved by character cell. The address bits for pixel (x, y) are:

    010 Y7 Y6 Y2 Y1 Y0 | Y5 Y4 Y3 X7 X6 X5 X4 X3

This layout was an optimization for the ROM's character printing routine, where `INC H` moves down one pixel line within a character cell. The color attributes at 0x5800-0x5AFF are separate and linear: one byte per 8x8 character cell, encoding ink color (bits 0-2), paper color (bits 3-5), brightness (bit 6), and flash (bit 7).

The emulator supports two rendering modes. In *automatic mode*, you provide an RGB framebuffer via `zx_set_framebuffer()` and the emulator renders each scanline as the ULA beam reaches it, which captures mid-frame effects like multicolor techniques and split-screen scrolling. In *on-demand mode*, you call `zx_render_screen()` whenever you want a snapshot of the current screen, which is simpler but can't capture mid-frame changes. The automatic mode is what the SDL frontend uses; the on-demand mode is intended for embedded displays where you might push the framebuffer to an ST77xx SPI LCD at your own pace.

### Audio

The Spectrum's audio is a 1-bit beeper: writing to the ULA port toggles the speaker level. The emulator logs each toggle as a (T-state, level) pair during the frame. At the frame boundary, these events are converted to 882 signed 16-bit PCM samples (44100 Hz at 50 fps) by walking the event list and sampling the speaker state at evenly-spaced T-state positions. The SDL frontend queues these samples to the audio device, and the audio queue depth is what paces the entire emulator to real-time -- when the queue has more than ~3 frames buffered, the main loop sleeps until the sound card drains enough. No `usleep()` or frame timer needed; the audio hardware's crystal oscillator is the clock.

### API reference

The Spectrum emulator has no dependencies beyond the Z80 core and the C standard library. The SDL frontend is just one possible integration — the same API works with any graphics/audio backend or with `usleep()` for frame pacing.

**Initialization and ROM:**

```c
void zx_init(ZXSpectrum *zx, const uint8_t *rom);  // Reset CPU, clear RAM, store ROM pointer (not copied)
void zx_set_rom(ZXSpectrum *zx, const uint8_t *rom); // Change ROM pointer at runtime
```

**Execution — three granularity levels:**

```c
int  zx_tick(ZXSpectrum *zx, int min_tstates); // Run ≥min_tstates; returns 1 at frame boundary
void zx_frame(ZXSpectrum *zx);                 // Run one full frame (loops zx_tick internally)
```

`zx_tick(zx, 0)` = one instruction (tape loading). `zx_tick(zx, 224)` = one scanline. `zx_frame()` = one full frame (~69888 T-states). Frame boundaries always take priority: `zx_tick()` returns 1 immediately when the frame ends, even if `min_tstates` hasn't been reached.

**Input:**

```c
void zx_key_down(ZXSpectrum *zx, int row, int bit);  // row=0-7, bit=0-4 (see keyboard matrix in spectrum.h)
void zx_key_up(ZXSpectrum *zx, int row, int bit);
void zx_joy_down(ZXSpectrum *zx, int button);        // ZX_JOY_RIGHT/LEFT/DOWN/UP/FIRE
void zx_joy_up(ZXSpectrum *zx, int button);
void zx_set_ear(ZXSpectrum *zx, uint8_t level);      // EAR bit for tape loading (0 or 1)
```

**Video — two modes:**

```c
void zx_set_framebuffer(ZXSpectrum *zx, uint8_t *rgb); // Auto-render scanlines (NULL to disable)
void zx_render_screen(ZXSpectrum *zx, uint8_t *rgb);   // On-demand: snapshot current screen
int  zx_ula_scanline(ZXSpectrum *zx);                   // Current beam position (0-311, -1 during vsync)
```

Framebuffer format: `ZX_FB_WIDTH` x `ZX_FB_HEIGHT` (320x256) pixels, 3 bytes/pixel (R,G,B), row-major. That's 32px border on each side of the 256x192 display.

**Snapshot loading:**

```c
int zx_load_z80(ZXSpectrum *zx, const uint8_t *data, int size); // Returns 0 on success, -1 on error
```

**Key struct fields** (read after each frame boundary):

| Field | Type | Description |
|-------|------|-------------|
| `zx->cpu.clocks` | `uint64_t` | Total T-states since init (monotonic, never resets) |
| `zx->frame_tstates` | `uint32_t` | T-states into current frame (0–69887) |
| `zx->frame_count` | `uint32_t` | Frames elapsed (increments at each boundary) |
| `zx->flash_state` | `uint8_t` | FLASH toggle (flips every 16 frames) |
| `zx->audio_buffer` | `int16_t[882]` | PCM output for the completed frame |
| `zx->border_color` | `uint8_t` | Current border color (0–7) |
| `zx->speaker` | `uint8_t` | Current speaker level (0 or 1) |

**Constants:**

| Constant | Value | Description |
|----------|-------|-------------|
| `ZX_TSTATES_PER_FRAME` | 69888 | T-states per frame |
| `ZX_TSTATES_PER_LINE` | 224 | T-states per scanline |
| `ZX_LINES_PER_FRAME` | 312 | Scanlines per frame |
| `ZX_AUDIO_RATE` | 44100 | Audio sample rate (Hz) |
| `ZX_AUDIO_SAMPLES` | ~882 | PCM samples per frame |
| `ZX_FB_WIDTH` / `ZX_FB_HEIGHT` | 320 / 256 | Framebuffer dimensions (pixels) |

**Minimal integration example** (usleep-based, no audio):

```c
ZXSpectrum zx;
uint8_t fb[ZX_FB_WIDTH * ZX_FB_HEIGHT * 3];

zx_init(&zx, zx_spectrum_rom);
zx_set_framebuffer(&zx, fb);

while (running) {
    uint64_t t0 = now_usec();
    zx_frame(&zx);
    blit(fb, ZX_FB_WIDTH, ZX_FB_HEIGHT);   // Your rendering here
    int64_t remaining = 20000 - (now_usec() - t0);  // 20ms = 50 Hz
    if (remaining > 0) usleep(remaining);
}
```

To use audio for pacing instead (more accurate), queue `zx->audio_buffer` (882 samples, signed 16-bit, mono, 44100 Hz) after each frame and block when the audio buffer is full. The sound card's crystal becomes your clock.

### Snapshot loading

The emulator can load .z80 snapshot files (versions 1, 2, and 3, 48K only). These snapshots contain the full CPU register state and a dump of RAM, optionally compressed with an ED ED RLE scheme. The loader parses the header, decompresses each memory page into the appropriate address range, and restores all CPU registers and ULA state.

## The tape player

The tape player (`tzx.h` and `tzx.c`) loads games from TZX and TAP files by converting their data into pulse sequences and feeding the EAR bit to the Spectrum at the correct T-state timing.

### How Spectrum tape loading works

The Spectrum ROM contains a tape loading routine starting at address 0x056B. It works by sampling the EAR bit in a tight loop and measuring the time between signal edges (transitions from 0 to 1 or vice versa). The standard tape format consists of:

1. A **pilot tone**: thousands of identical pulses (2168 T-states each) that let the ROM's edge detector synchronize. Header blocks use 8063 pilot pulses (~5 seconds of audible "bzzz"), data blocks use 3223 (~2 seconds).
2. Two **sync pulses** of different lengths (667 and 735 T-states) that mark the start of actual data.
3. **Data bits**, where each bit is encoded as two identical pulses (one full wave). A zero bit uses short pulses (855 T-states each) and a one bit uses long pulses (1710 T-states). The ROM measures the pulse width and classifies it as 0 or 1.
4. A **pause** (typically 1 second of silence) between blocks.

The ROM doesn't care where these pulses come from -- a cassette player's audio output, a WAV file, or synthetic pulses from an emulator. It just measures edge timing on the EAR bit.

### TAP files

TAP is the simplest tape format. It's a raw concatenation of data blocks, each preceded by a 2-byte little-endian length. There's no timing information at all -- the player synthesizes the standard ROM loader pulse sequence for each block using the hardcoded timing constants above. The first byte of each block is the flag byte: values below 0x80 indicate a header block (long pilot), values 0x80 and above indicate a data block (short pilot).

### TZX files

TZX is a much richer format that can represent anything that was ever recorded on tape, including turbo loaders with custom timing, pulse-level protection schemes, and raw audio recordings. A TZX file starts with a 10-byte header ("ZXTape!" + 0x1A + version number), followed by a sequence of blocks, each identified by an ID byte.

The player supports these block types:

- **0x10 Standard Speed Data** -- Expands to standard ROM timing, same as TAP.
- **0x11 Turbo Speed Data** -- Like 0x10 but all timing parameters (pilot length, sync lengths, zero/one pulse widths, pilot count) come from the block header.
- **0x12 Pure Tone** -- N identical pulses of a given length, used by custom loaders for their pilot tones.
- **0x13 Pulse Sequence** -- A sequence of individually-timed pulses, used for custom sync patterns.
- **0x14 Pure Data** -- Data bits without a pilot or sync, for loaders that handle their own pilot via blocks 0x12/0x13.
- **0x15 Direct Recording** -- Raw 1-bit samples where each bit directly sets the level (not toggled), at a given number of T-states per sample.
- **0x20 Pause/Stop** -- Silence for N milliseconds, or stop the tape (N=0).
- **0x21/0x22 Group Start/End** -- Informational grouping.
- **0x24/0x25 Loop Start/End** -- Repeat a section of blocks N times.
- **0x2B Set Signal Level** -- Force the EAR level to a specific value.
- **0x30/0x32 Text/Archive Info** -- Metadata (printed to the console).

Unknown block types are skipped if their size can be determined, otherwise playback stops.

### Pulse generation

Internally, the tape player is a state machine that produces one pulse at a time. Each data block progresses through phases: PILOT, SYNC1, SYNC2, DATA, PAUSE. The `tzx_next_pulse()` function returns the T-state duration of the next pulse and advances the state machine. When a block is exhausted, it parses the next block header from the TZX/TAP data.

The hot path is `tzx_update()`, called before every Z80 instruction during tape playback:

```c
while (playing && cpu_clocks >= edge_clock) {
    level ^= 1;                      // Toggle EAR level
    pulse = tzx_next_pulse(p);       // Get next pulse duration
    if (!pulse) { stop; break; }     // End of tape
    edge_clock += pulse;             // Schedule next edge
}
return level;
```

The `edge_clock` field tracks the absolute `cpu.clocks` value when the next EAR transition should happen. As the Z80 executes instructions and `cpu.clocks` advances, `tzx_update()` toggles the level and advances through pulses to catch up. For PAUSE phases, `tzx_next_pulse()` forces the level low (overriding the toggle). For DIRECT recording blocks, it sets the level from each sample bit (also overriding the toggle). The caller doesn't need to know about these special cases -- it just feeds the returned level to `zx_set_ear()`.

In the SDL frontend's main loop, the tape-aware path replaces `zx_frame()` with a per-instruction tick loop:

```c
if (tzx_is_playing(&tape)) {
    do {
        zx_set_ear(zx, tzx_update(&tape, zx->cpu.clocks));
    } while (!zx_tick(zx, 0));
} else {
    zx_frame(zx);
}
```

This ensures the EAR bit is updated between every single instruction, which is exactly what the ROM loader needs to measure pulse widths accurately.

## The SDL frontend

The SDL frontend (`zxsdl.c`) is a minimal desktop interface. It maps the PC keyboard to the Spectrum's 40-key matrix, supports Kempston joystick input via arrow keys, and handles .z80 snapshot loading and .tzx/.tap tape playback via both command-line arguments and drag-and-drop.

    ./zxsdl                      # Boot into BASIC
    ./zxsdl game.z80             # Load and run a snapshot
    ./zxsdl game.tzx             # Load tape (type LOAD "" then press F3)
    ./zxsdl game.tap             # Same for TAP files

The keyboard maps directly to the Spectrum's 40-key layout: A-Z, 0-9, Space, and Enter all map 1:1. The Spectrum's two modifier keys are:

| PC key | Spectrum key | Notes |
|---|---|---|
| Left/Right Shift | CAPS SHIFT | Uppercase, cursor keys, DELETE, EDIT, etc. |
| Left/Right Ctrl | SYMBOL SHIFT | Punctuation, math symbols, colors |
| Backspace | DELETE | Automatically adds CAPS SHIFT |

Arrow keys and Tab are mapped to the Kempston joystick interface:

| PC key | Joystick |
|---|---|
| Up / Down / Left / Right | Directions |
| Tab | Fire |

Function keys:

| Key | Action |
|---|---|
| F2 | Reset |
| F3 | Play / restart tape from beginning |
| F4 | Stop tape |
| F5 | Toggle 2x / 3x scale |
| F11 | Toggle fullscreen |
| ESC | Quit |

## The CP/M emulator

The CP/M emulator (`cpm.h` and `cpm.c`) implements Digital Research's CP/M 2.2 operating system entirely in C. Instead of running actual CP/M code for the CCP, BDOS, and BIOS, it intercepts Z80 calls to the standard entry points and handles them natively. This means no CP/M system binary images are needed -- the emulator is self-contained. It can run Turbo Pascal, WordStar, and many other software.

### Architecture

CP/M programs make system calls via `CALL 0005h` with the function number in register C and a parameter in DE. At address 0005h we store a JP instruction pointing to a trap address in the BDOS area. Our execution loop checks the PC before each instruction -- when it sees the trap address, it intercepts:

1. Read register C for the BDOS function number
2. Read DE for the parameter
3. Handle the function in C code
4. Set return values in A/L or HL/BA
5. Pop the return address from the stack and resume

The same mechanism handles the BIOS: a 17-entry jump table at F200h (BOOT through SECTRAN) points to trap addresses at F240h. Programs like MBASIC read these addresses from the jump table and call them directly for faster console I/O, bypassing the BDOS overhead.

### BDOS functions

All 38 CP/M 2.2 BDOS functions are implemented:

- **Console I/O** (functions 1-12): input with echo, raw output, buffered line input, direct I/O (non-blocking read/write), console status, tab expansion, string output with `$` delimiter, I/O byte management, and CP/M version query.
- **File system** (functions 13-23, 30, 33-36, 40): open, close, read/write sequential, read/write random, make, delete, rename, search first/next, file size, set random record, set file attributes, disk reset, drive select, DMA address, and write random with zero fill.
- **System info** (functions 24-32): login vector, current disk, allocation vector, R/O vector, DPB address, user number.

The file system maps CP/M drives (A: through P:) to host directories. An open file table of 16 slots maps CP/M FCB names to host `FILE*` handles across BDOS calls. The emulated disk has 4K blocks and 8MB capacity (DSM=2047), with a Disk Parameter Block that programs can query via BDOS 31.

### CCP

The Console Command Processor is also implemented in C rather than Z80 code. It provides the interactive command line (`A>` prompt) with built-in commands: DIR, ERA, TYPE, REN, USER, and SAVE. Unrecognized commands are searched as .COM files on the current or specified drive. The CCP handles FCB setup (parsing filenames into the two default FCBs at 005Ch/006Ch) and the command tail at 0080h, just like the real CCP does.

### Running

The terminal frontend `cpmcon` provides raw-mode console access:

    ./cpmcon cpm-software/mbasic/              # Boot with MBASIC directory as A:
    ./cpmcon -A cpm-software/utils/ -B cpm-software/games/   # Mount multiple drives

Inside the emulator, you get a standard CP/M prompt:

    A>DIR
    A>MBASIC
    A>B:
    B>ZORK1

Ctrl-C sends a warm boot (returns to the CCP). Ctrl-\ quits the emulator.

### Terminal translation

Most CP/M programs were written for vintage terminals (ADM-3A, Kaypro, TeleVideo, VT52) that use different escape sequences than modern ANSI/VT100 terminals. The emulator includes a translation layer with three modes, selectable via `-t`:

    ./cpmcon cpm-software/ws/               # WordStar: auto mode works (default)
    ./cpmcon -t adm3a cpm-software/ws/      # Force full ADM-3A/Kaypro translation
    ./cpmcon -t ansi cpm-software/turbo/    # Turbo Pascal: skip translation entirely

**auto** (default) -- Translates the core ADM-3A/Kaypro codes that don't conflict with ANSI: single-byte cursor movement (0x0B up, 0x0C right), screen clearing (0x1A clear, 0x1E home, 0x17 clear-to-EOS, 0x18 clear-to-EOL), cursor addressing (ESC = row col), and Kaypro attributes (ESC B/C digit). Native ANSI sequences (ESC [) pass through untouched. This mode works for both WordStar (Kaypro terminal) and Turbo Pascal (ANSI terminal).

**adm3a** -- Full translation including TeleVideo codes (ESC G attributes, ESC T/Y clear operations), VT52 cursor movement (ESC A/D/H/I/J/K), insert/delete line (ESC E/R), and various attribute and clear-screen escape sequences. Use this when a program definitely targets a vintage terminal and you don't need ANSI pass-through. Note that some VT52 ESC codes (ESC D, ESC H) conflict with ANSI C1 codes, so ANSI-native programs may misbehave in this mode.

**ansi** -- No translation at all. Every byte passes through raw to the host terminal. Use for programs already configured for ANSI/VT100.

The translation maps vintage codes to ANSI equivalents:

| Vintage code | Meaning | ANSI output |
|---|---|---|
| 0x0B | Cursor up (ADM-3A) | ESC[A |
| 0x0C | Cursor right (ADM-3A) | ESC[C |
| 0x17 | Clear to end of screen (Kaypro) | ESC[J |
| 0x18 | Clear to end of line (Kaypro) | ESC[K |
| 0x1A | Clear screen + home (ADM-3A) | ESC[2J ESC[H |
| 0x1E | Home cursor (ADM-3A) | ESC[H |
| ESC = *row col* | Cursor address (ADM-3A/Kaypro) | ESC[*r*;*c*H |
| ESC B *digit* | Enable attribute (Kaypro) | ESC[7m / 2m / 5m / 4m |
| ESC C *digit* | Disable attribute (Kaypro) | ESC[0m |

Input is also translated in auto and adm3a modes. Modern arrow keys send ANSI escape sequences that CP/M programs don't understand. The emulator converts them to the WordStar control keys that virtually all CP/M editors (WordStar, Turbo Pascal, dBASE, etc.) support:

| Host key | ANSI sequence | CP/M key |
|---|---|---|
| Up | ESC [ A | Ctrl-E (0x05) |
| Down | ESC [ B | Ctrl-X (0x18) |
| Right | ESC [ C | Ctrl-D (0x04) |
| Left | ESC [ D | Ctrl-S (0x13) |
| Backspace | 0x7F | 0x08 (BS) |

Unknown ANSI sequences (Page Up, Home, F-keys, etc.) are consumed and discarded to prevent garbage characters. In ansi mode, no input translation is performed.

A download script is provided in `cpm-software/download.sh` that fetches classic CP/M software from public archives: WordStar 3.3, MBASIC 5.29, Turbo Pascal 3.00A, Zork I/II/III, Hitchhiker's Guide to the Galaxy, Microsoft FORTRAN-80, BBC BASIC, and standard CP/M utilities (STAT, PIP, DDT, ED, ASM). These are copyrighted programs and the script warns before downloading.

### Tested software

The following programs have been (superficially) verified to work:

- **WordStar** -- The pioneering word processing app by MicroPro International
- **Turbo Pascal** -- Borland very good implementation of the Pascal language
- **MBASIC 5.29** (Microsoft BASIC) -- uses direct BIOS calls for console I/O
- **Zork I, II, III** (Infocom) -- text adventure games using BDOS 6 (direct I/O)
- **Hitchhiker's Guide to the Galaxy** (Infocom) -- with ANSI terminal status line
- **STAT.COM** -- disk status and file statistics
- **BBC BASIC (Z80)** -- R.T. Russell's BBC BASIC interpreter
- **PIP, DDT, ED, ASM** -- standard CP/M utilities

### Testing

The CP/M emulator has 78 unit tests covering page zero setup, all BDOS console and file functions, FCB parsing, CCP built-in commands, .COM file execution, the BIOS jump table, and direct BIOS calls (CONST, CONIN, CONOUT, SELDSK).

## Building

The only external dependency is SDL2 (for the Spectrum frontend only -- the Z80 core, Spectrum emulation, tape player, and CP/M emulator have no dependencies beyond the C standard library).

    make            # Build everything
    make zxsdl      # Build just the Spectrum SDL frontend
    make cpmcon     # Build just the CP/M terminal frontend

## Testing

There are two levels of testing. The fast suite (`make test`) runs 154 Z80 unit tests, 49 Spectrum emulation tests, and 78 CP/M emulation tests in under a second. The full suite (`make fulltest`) additionally runs the ZEXDOC and ZEXALL exercisers, which take a few minutes but exhaustively verify every Z80 instruction against real hardware behavior.

    make test       # Fast: unit tests only (~1 second)
    make fulltest   # Full: unit tests + ZEXDOC + ZEXALL (~3 minutes)

The **Z80 unit tests** (`z80_test.c`) cover every instruction group: 8-bit and 16-bit loads, arithmetic, jumps, calls, returns, rotates, shifts, bit operations, block transfers, I/O, exchanges, DAA, interrupt handling, and IX/IY operations including undocumented opcodes. They also verify T-state timing for specific instructions and test integration scenarios like computing Fibonacci numbers and memory fills.

The **Spectrum tests** (`spectrum_test.c`) verify the emulation layer: ROM read-only behavior, RAM read/write, memory contention timing and patterns, the screen memory address calculation, keyboard matrix scanning, Kempston joystick I/O, ULA port decoding, beeper event recording, frame timing and interrupt generation, FLASH toggle period, both rendering modes, and .z80 snapshot loading.

The **CP/M tests** (`cpm_test.c`) verify the complete CP/M emulation: page zero vectors, all BDOS console I/O functions, file system operations (open, close, read, write, search, delete, rename, random access), FCB and command tail parsing, CCP built-in commands and .COM execution, BIOS jump table setup, and direct BIOS calls (CONST, CONIN, CONOUT, SELDSK). Each test loads a small Z80 program, runs it to completion, and checks register values, memory contents, or captured console output.

**ZEXDOC** and **ZEXALL** are Frank Cringle's Z80 instruction set exercisers, originally written for the YAZE emulator and ported to CP/M by J.G. Harston. They systematically test 67 instruction groups by running each opcode through a combinatorial set of inputs and comparing the resulting flags and register values against a CRC computed on real Z80 hardware. ZEXDOC tests only the six documented flags; ZEXALL also tests the undocumented X (bit 3) and Y (bit 5) flags, which are the hardest to get right because their behavior varies across instruction groups in non-obvious ways. Both pass at 100%. The .com files must be present in `z80-specs/` (they are not included in the repository by default -- see `z80-specs/README.md` for provenance and download links).

## File structure

| File | Lines | Description |
|------|-------|-------------|
| `z80.h` | 131 | Z80 CPU state struct and API |
| `z80.c` | 1780 | Complete Z80 instruction set implementation |
| `spectrum.h` | 209 | Spectrum state struct, constants, and API |
| `spectrum.c` | 699 | ULA emulation: video, audio, keyboard, contention |
| `tzx.h` | 118 | TZX/TAP tape player struct and API |
| `tzx.c` | 680 | Tape block parser and pulse generator |
| `zxsdl.c` | 426 | SDL2 Spectrum frontend |
| `cpm.h` | 200 | CP/M machine state, constants, and API |
| `cpm.c` | 2000 | BDOS, BIOS, CCP, and file system implementation |
| `cpmcon.c` | 490 | Terminal frontend with ADM-3A/Kaypro/VT52→ANSI translation |
| `cpm_debug.c` | 120 | Trace/debug tool for CP/M programs |
| `rom.h` | - | ZX Spectrum 48K ROM as a C array |
| `z80_test.c` | - | 154 Z80 unit tests + ZEXDOC/ZEXALL harness |
| `spectrum_test.c` | - | 49 Spectrum emulation tests |
| `cpm_test.c` | - | 78 CP/M emulation tests |

## License

MIT. The files in `z80-specs/` are under a different license -- see `z80-specs/README.md` for details.

### ZX Spectrum ROM

The file `rom.h` contains the ZX Spectrum 48K ROM, originally copyright Sinclair Research. The rights were acquired by Amstrad plc, then passed to Sky (which acquired Amstrad in 2007), and ultimately to Comcast (which acquired Sky in 2018). In August 1999, Cliff Lawson of Amstrad plc posted a statement to the `comp.sys.sinclair` newsgroup granting blanket permission for emulator authors to redistribute the ROM images, provided that copyright notices are not altered and that the ROM is not sold separately. Amstrad have kindly given their permission for the redistribution of their copyrighted material but retain that copyright.

This permission has been relied upon by the ZX Spectrum emulation community for over 25 years, and virtually every open-source Spectrum emulator distributes the ROM under these terms. That said, if the current rights holder wishes the ROM to be removed from this repository, please contact antirez@gmail.com and it will be removed promptly.


/* spectrum.h -- ZX Spectrum 48K emulator interface.
 *
 * This emulates the ZX Spectrum 48K home computer built around our Z80 core.
 * The Spectrum wraps a Z80 CPU with:
 *   - 16KB ROM + 48KB RAM (64KB total address space)
 *   - A custom chip called the ULA (Uncommitted Logic Array) that handles:
 *     * Video output (256x192 pixels with 32x24 attribute color cells)
 *     * Keyboard scanning (8 half-rows of 5 keys each)
 *     * 1-bit beeper sound
 *     * Tape I/O (EAR input via audio capture)
 *   - Memory contention (the ULA and CPU share RAM at 0x4000-0x7FFF)
 *
 * FRAME TIMING
 * ============
 * The ULA generates a PAL TV signal at ~50.08 Hz. Each frame takes exactly
 * 69,888 T-states (CPU clock cycles at 3.5 MHz). The frame is divided into
 * 312 scanlines of 224 T-states each:
 *   - Scanlines 0-7:     Vertical sync (8 lines)
 *   - Scanlines 8-63:    Top border (56 lines)
 *   - Scanlines 64-255:  Display area (192 lines of pixel data)
 *   - Scanlines 256-311: Bottom border (56 lines)
 *
 * The ULA fires a maskable interrupt at the start of each frame (T-state 0).
 *
 * RENDERING MODES
 * ===============
 * The emulator supports two modes for video output:
 *
 * 1. On-demand: caller queries zx_ula_scanline() for beam position and
 *    calls zx_render_screen() when appropriate. No framebuffer needed.
 *    Ideal for embedded targets (RP2040 + ST77xx display).
 *
 * 2. Automatic: caller provides an RGB buffer via zx_set_framebuffer().
 *    The emulator renders each scanline as the ULA reaches it, capturing
 *    mid-frame changes (multicolor effects, split-screen scrolling).
 */

#ifndef SPECTRUM_H
#define SPECTRUM_H

#include "z80.h"
#include <stdint.h>

/* ===================================================================
 * CONSTANTS
 * =================================================================== */

#define ZX_TSTATES_PER_FRAME   69888   /* Total T-states in one frame */
#define ZX_TSTATES_PER_LINE    224     /* T-states per scanline */
#define ZX_LINES_PER_FRAME     312     /* Total scanlines per frame */
#define ZX_DISPLAY_LINES       192     /* Active display lines */
#define ZX_FIRST_DISPLAY_LINE  64      /* First scanline with pixel data */
#define ZX_SCREEN_WIDTH        256     /* Display width in pixels */
#define ZX_SCREEN_HEIGHT       192     /* Display height in pixels */
#define ZX_BORDER_PX           32      /* Border width in pixels */
/* Framebuffer dimensions: display + border on all sides */
#define ZX_FB_WIDTH            (ZX_SCREEN_WIDTH + ZX_BORDER_PX * 2)   /* 320 */
#define ZX_FB_HEIGHT           (ZX_SCREEN_HEIGHT + ZX_BORDER_PX * 2)  /* 256 */

/* Memory contention: first contended T-state in a frame.
 * 64 top scanlines * 224 T-states/line = 14336, minus 1 because the
 * ULA fetch cycle begins one T-state before the first pixel appears. */
#define ZX_FIRST_CONTENDED     14335
#define ZX_CONTENDED_PER_LINE  128     /* T-states of contention per display line */

/* Beeper audio: ~882 samples per frame at 44.1 kHz using nominal 50 fps. */
#define ZX_AUDIO_RATE          44100
#define ZX_AUDIO_SAMPLES       ((ZX_AUDIO_RATE + 25) / 50)  /* ~882 */
#define ZX_MAX_BEEPER_EVENTS   4096

/* Kempston joystick button bits (active HIGH: 1 = pressed). */
#define ZX_JOY_RIGHT   0x01
#define ZX_JOY_LEFT    0x02
#define ZX_JOY_DOWN    0x04
#define ZX_JOY_UP      0x08
#define ZX_JOY_FIRE    0x10

/* ===================================================================
 * BEEPER EVENT (for audio rendering)
 * =================================================================== */

typedef struct {
    uint32_t tstates;  /* Frame T-state when the speaker level changed */
    uint8_t  level;    /* New speaker level (0 or 1) */
} ZXBeeperEvent;

/* ===================================================================
 * SPECTRUM STATE
 * =================================================================== */

typedef struct ZXSpectrum {
    Z80 cpu;                       /* Z80 CPU core */
    const uint8_t *rom;            /* 16KB ROM pointer (caller-provided, not copied) */
    uint8_t memory[49152];         /* 48KB RAM, index 0 = address 0x4000 */

    /* Keyboard: 8 half-rows, bits 0-4 per row.
     * 0 = key pressed, 1 = key released (active LOW).
     * Initialized to 0xFF (all keys released). */
    uint8_t keyboard[8];

    /* Kempston joystick state: 000FUDLR (active HIGH). */
    uint8_t kempston;

    /* ULA state */
    uint8_t border_color;          /* Current border color (0-7) */
    uint8_t speaker;               /* Current speaker level (0 or 1) */
    uint8_t mic;                   /* Last MIC output bit; kept for future Issue 2 EAR/MIC modeling */
    uint8_t ear;                   /* External EAR input level (0 or 1) */

    /* Frame timing */
    uint32_t frame_tstates;        /* T-states into current frame (0..69887) */
    uint32_t frame_count;          /* Total frames elapsed */
    uint8_t flash_state;           /* FLASH toggle (0 or 1), flips every 16 frames */

    /* Framebuffer (optional): caller-provided RGB buffer (320*256*3 bytes).
     * When non-NULL, the emulator renders each scanline automatically.
     * When NULL, the caller handles rendering via zx_render_screen(). */
    uint8_t *framebuffer;
    int fb_next_line;              /* Next scanline to render (0-311) */

    /* Audio: beeper event log for the current frame. */
    ZXBeeperEvent beeper_events[ZX_MAX_BEEPER_EVENTS];
    int beeper_event_count;
    int16_t audio_buffer[ZX_AUDIO_SAMPLES]; /* PCM output for current frame */
} ZXSpectrum;

/* ===================================================================
 * API
 * =================================================================== */

/* Initialize the Spectrum. Stores the ROM pointer (not copied).
 * Resets CPU, clears RAM, all keys released. */
void zx_init(ZXSpectrum *zx, const uint8_t *rom);

/* Set or change the 16KB ROM pointer. The data is not copied --
 * the pointer must remain valid for the lifetime of the emulator. */
void zx_set_rom(ZXSpectrum *zx, const uint8_t *rom);

/* Execute Z80 instructions for at least min_tstates T-states and
 * advance the ULA (scanline rendering, frame timing). Returns 1 when
 * a frame boundary is crossed (audio buffer is ready, frame_count
 * incremented, interrupt fired for the next frame). Returns 0 otherwise.
 *
 * Always breaks early on a frame boundary, even if min_tstates hasn't
 * been reached yet, so the caller never misses a frame event.
 *
 * Typical usage:
 *   min_tstates = 0  -- execute one instruction (tape loading, where
 *                       EAR must be updated between every instruction)
 *   min_tstates = 224 -- execute one scanline (embedded systems that
 *                        need to interleave rendering/IO with emulation)
 *   Use zx_frame() to run a full frame in one call. */
int zx_tick(ZXSpectrum *zx, int min_tstates);

/* Run one complete frame (~69888 T-states). Equivalent to calling
 * zx_tick() in a loop until it returns 1. Renders scanlines (if
 * framebuffer is set) and generates audio. Call ~50 times per second. */
void zx_frame(ZXSpectrum *zx);

/* --- Input --------------------------------------------------------- */

/* Press/release a key. row = 0-7, bit = 0-4. See keyboard matrix:
 *   Row 0 (0xFEFE): CAPS SHIFT, Z, X, C, V
 *   Row 1 (0xFDFE): A, S, D, F, G
 *   Row 2 (0xFBFE): Q, W, E, R, T
 *   Row 3 (0xF7FE): 1, 2, 3, 4, 5
 *   Row 4 (0xEFFE): 0, 9, 8, 7, 6
 *   Row 5 (0xDFFE): P, O, I, U, Y
 *   Row 6 (0xBFFE): ENTER, L, K, J, H
 *   Row 7 (0x7FFE): SPACE, SYM SHIFT, M, N, B */
void zx_key_down(ZXSpectrum *zx, int row, int bit);
void zx_key_up(ZXSpectrum *zx, int row, int bit);

/* Press/release a Kempston joystick button.
 * Use ZX_JOY_RIGHT, ZX_JOY_LEFT, ZX_JOY_DOWN, ZX_JOY_UP, ZX_JOY_FIRE. */
void zx_joy_down(ZXSpectrum *zx, int button);
void zx_joy_up(ZXSpectrum *zx, int button);

/* --- EAR input (tape loading) -------------------------------------- */

/* Set the EAR bit (0 or 1). The caller is responsible for updating this
 * at an appropriate rate. For tape loading, update between zx_tick()
 * calls based on audio input (microphone or WAV file). */
void zx_set_ear(ZXSpectrum *zx, uint8_t level);

/* --- Video --------------------------------------------------------- */

/* Get the current ULA scanline (0-311), or -1 during vsync.
 * Scanlines 0-7: vsync (returns -1), 8-63: top border,
 * 64-255: display, 256-311: bottom border.
 * Use this to decide when to grab screen content in on-demand mode. */
int zx_ula_scanline(ZXSpectrum *zx);

/* Render the current screen memory into an RGB buffer (on-demand mode).
 * Output: 320x256 pixels, 3 bytes/pixel (R,G,B), row-major.
 * 320x256 = 32px border + 256px display + 32px border each axis. */
void zx_render_screen(ZXSpectrum *zx, uint8_t *rgb);

/* Set a framebuffer for automatic per-scanline rendering.
 * Pass NULL to disable (switch to on-demand mode).
 * Buffer must be at least 320*256*3 = 245,760 bytes. */
void zx_set_framebuffer(ZXSpectrum *zx, uint8_t *rgb);

/* --- Snapshot loading ---------------------------------------------- */

/* Load a .z80 snapshot file. Returns 0 on success, -1 on error.
 * Supports v1, v2, and v3 formats. 48K snapshots only. */
int zx_load_z80(ZXSpectrum *zx, const uint8_t *data, int size);

#endif /* SPECTRUM_H */

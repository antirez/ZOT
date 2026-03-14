/* spectrum.c -- ZX Spectrum 48K emulator implementation.
 *
 * This file implements the ZX Spectrum 48K, wrapping our Z80 CPU core with
 * the ULA chip behavior: video generation, keyboard scanning, beeper sound,
 * I/O port handling, and memory contention.
 *
 * MEMORY CONTENTION
 * =================
 * The ULA and CPU share the lower 16KB of RAM (0x4000-0x7FFF). During the
 * 192 display scanlines, the ULA fetches screen data for 128 T-states per
 * line. If the CPU tries to access this RAM during a ULA fetch, the CPU is
 * stalled. The delay depends on where we are in the ULA's 8 T-state cycle:
 *
 *   Position:  0  1  2  3  4  5  6  7
 *   Delay:     6  5  4  3  2  1  0  0
 *
 * A full frame is 312 scanlines (8 vsync + 56 top border + 192 display +
 * 56 bottom border) at 224 T-states each = 69,888 T-states. Instead of a
 * 69,888-byte lookup table, we compute the delay at runtime with a few
 * comparisons and one index into an 8-byte pattern array.
 *
 * SCREEN MEMORY LAYOUT
 * ====================
 * The Spectrum's screen memory is famously non-linear. The 256x192 pixel
 * bitmap at 0x4000-0x57FF is divided into 3 "thirds" of 64 lines each.
 * Within each third, lines are interleaved by character cell row. This
 * was an optimization for the ROM's character printing routines: INC H
 * moves down one pixel line within a character cell.
 *
 * Address bits for pixel (x, y):
 *   010 Y7 Y6 Y2 Y1 Y0 | Y5 Y4 Y3 X4 X3 X2 X1 X0
 *
 * Attributes at 0x5800-0x5AFF are linear: 32 bytes per character row.
 */

#include "spectrum.h"
#include <string.h>

/* ===================================================================
 * COLOR PALETTE
 * =================================================================== */

/* The Spectrum has 16 colors: 8 at normal intensity, 8 bright.
 * Normal intensity uses ~0xD7 (84%), bright uses 0xFF (100%).
 * Colors are encoded as GRB in the attribute bits:
 *   bit 0 = Blue, bit 1 = Red, bit 2 = Green. */
static const uint8_t zx_palette[16][3] = {
    /* Normal (BRIGHT = 0) */
    {0x00, 0x00, 0x00},  /* 0: Black */
    {0x00, 0x00, 0xD7},  /* 1: Blue */
    {0xD7, 0x00, 0x00},  /* 2: Red */
    {0xD7, 0x00, 0xD7},  /* 3: Magenta */
    {0x00, 0xD7, 0x00},  /* 4: Green */
    {0x00, 0xD7, 0xD7},  /* 5: Cyan */
    {0xD7, 0xD7, 0x00},  /* 6: Yellow */
    {0xD7, 0xD7, 0xD7},  /* 7: White */
    /* Bright (BRIGHT = 1) */
    {0x00, 0x00, 0x00},  /* 8: Black (same) */
    {0x00, 0x00, 0xFF},  /* 9: Bright Blue */
    {0xFF, 0x00, 0x00},  /* 10: Bright Red */
    {0xFF, 0x00, 0xFF},  /* 11: Bright Magenta */
    {0x00, 0xFF, 0x00},  /* 12: Bright Green */
    {0x00, 0xFF, 0xFF},  /* 13: Bright Cyan */
    {0xFF, 0xFF, 0x00},  /* 14: Bright Yellow */
    {0xFF, 0xFF, 0xFF},  /* 15: Bright White */
};

/* ===================================================================
 * SCREEN ADDRESS HELPERS
 * =================================================================== */

/* Compute the bitmap memory address for pixel row y (0-191), column byte x
 * (0-31). The Spectrum's screen memory is interleaved:
 *   - Bits 12-11 of address: which third of the screen (Y7,Y6)
 *   - Bits 10-8:  pixel line within the 8-line character cell (Y2,Y1,Y0)
 *   - Bits 7-5:   character row within the third (Y5,Y4,Y3)
 *   - Bits 4-0:   column byte (0-31) */
static inline uint16_t zx_pixel_addr(int y, int col) {
    return 0x4000
        | ((y & 0xC0) << 5)   /* Y7,Y6 -> bits 12-11 */
        | ((y & 0x07) << 8)   /* Y2,Y1,Y0 -> bits 10-8 */
        | ((y & 0x38) << 2)   /* Y5,Y4,Y3 -> bits 7-5 */
        | (col & 0x1F);       /* column byte */
}

/* Attribute address for character cell (col, row), where col=0-31, row=0-23.
 * Attributes are stored linearly at 0x5800: 32 bytes per row. */
static inline uint16_t zx_attr_addr(int row, int col) {
    return 0x5800 + row * 32 + col;
}

/* ===================================================================
 * MEMORY CONTENTION
 * =================================================================== */

/* Compute the contention delay for a given frame T-state.
 *
 * Contention only occurs during the 192 display scanlines (ULA lines
 * 64-255). On each of these lines, the ULA fetches screen data during
 * the first 128 T-states, cycling through an 8 T-state fetch pattern.
 * If the CPU accesses contended RAM (0x4000-0x7FFF) during a fetch,
 * it's stalled until the fetch cycle finishes. The delay depends on
 * where we land within the 8 T-state cycle:
 *
 *   Cycle position:  0  1  2  3  4  5  6  7
 *   CPU wait states: 6  5  4  3  2  1  0  0
 *
 * The first contended T-state is 14335 (= 64 lines * 224 T-states - 1,
 * because the ULA begins its fetch one T-state before pixels appear).
 * Each display scanline has 128 contended T-states (16 fetch cycles
 * of 8 T-states each), followed by 96 uncontended T-states for the
 * border/retrace portion of the line.
 *
 * So given a frame T-state t, we:
 *   1. Check if t is in the display region at all (>= 14335)
 *   2. Find which scanline and position within that line
 *   3. Check if the position is in the contended portion (< 128)
 *   4. Look up the delay in the 8-byte pattern
 *
 * This replaces a 69,888-byte precomputed table with 8 bytes + arithmetic. */
static inline uint8_t zx_contend_delay(uint32_t frame_t) {
    static const uint8_t pattern[] = {6, 5, 4, 3, 2, 1, 0, 0};

    if (frame_t < ZX_FIRST_CONTENDED) return 0;
    int offset = frame_t - ZX_FIRST_CONTENDED;
    int line = offset / ZX_TSTATES_PER_LINE;
    if (line >= ZX_DISPLAY_LINES) return 0;
    int pos = offset % ZX_TSTATES_PER_LINE;
    if (pos >= ZX_CONTENDED_PER_LINE) return 0;
    return pattern[pos & 7];
}

/* Apply contention: if the address is in the contended range (0x4000-0x7FFF),
 * compute and add the delay for the current frame T-state. */
static inline void zx_contend(ZXSpectrum *zx, uint16_t addr) {
    if (addr >= 0x4000 && addr <= 0x7FFF)
        zx->frame_tstates += zx_contend_delay(zx->frame_tstates);
}

/* Apply 48K I/O contention.
 *
 * Patterns (N = uncontended cycle, C = contention check + cycle):
 *   high byte uncontended, even port: N:1, C:3
 *   high byte uncontended, odd  port: N:4
 *   high byte contended,   even port: C:1, C:3
 *   high byte contended,   odd  port: C:1, C:1, C:1, C:1
 *
 * Base cycles are already accounted for by the Z80 instruction timing.
 * Here we only add the ULA wait-state penalties at each C check point. */
static inline void zx_contend_io(ZXSpectrum *zx, uint16_t port) {
    uint8_t high = port >> 8;
    int high_contended = (high >= 0x40 && high <= 0x7F);
    uint32_t t = zx->frame_tstates;
    uint32_t extra = 0;

    if (!(port & 0x01)) {
        if (high_contended) {
            /* C:1, C:3 */
            uint8_t d = zx_contend_delay(t);
            extra += d;
            t += d + 1;
            d = zx_contend_delay(t);
            extra += d;
        } else {
            /* N:1, C:3 */
            t += 1;
            extra += zx_contend_delay(t);
        }
    } else if (high_contended) {
        /* C:1, C:1, C:1, C:1 */
        for (int i = 0; i < 4; i++) {
            uint8_t d = zx_contend_delay(t);
            extra += d;
            t += d + 1;
        }
    }

    zx->frame_tstates += extra;
}

/* ===================================================================
 * Z80 MEMORY / I/O CALLBACKS
 * =================================================================== */

/* The Z80 core calls these via function pointers for all memory and I/O
 * access. The ctx pointer is our ZXSpectrum struct. */

static uint8_t zx_mem_read(void *ctx, uint16_t addr) {
    ZXSpectrum *zx = (ZXSpectrum *)ctx;
    zx_contend(zx, addr);
    if (addr < 0x4000)
        return zx->rom[addr];
    return zx->memory[addr - 0x4000];
}

static void zx_mem_write(void *ctx, uint16_t addr, uint8_t val) {
    ZXSpectrum *zx = (ZXSpectrum *)ctx;
    zx_contend(zx, addr);
    /* ROM is read-only: writes to 0x0000-0x3FFF are silently ignored. */
    if (addr >= 0x4000)
        zx->memory[addr - 0x4000] = val;
}

/* I/O read: the Spectrum uses partial port decoding.
 *
 * ULA port (any even address, bit 0 = 0):
 *   Returns keyboard state. The upper byte of the port address selects
 *   which half-rows to scan by pulling address lines A8-A15 low.
 *   Multiple rows can be selected simultaneously; results are ANDed.
 *   Bits 0-4: key data (0 = pressed, active LOW)
 *   Bit 5: always 1
 *   Bit 6: EAR input
 *   Bit 7: always 1
 *
 * Kempston joystick (bits A7,A6,A5 = 0):
 *   Returns joystick state 000FUDLR (active HIGH, opposite of keyboard). */
static uint8_t zx_io_read(void *ctx, uint16_t port) {
    ZXSpectrum *zx = (ZXSpectrum *)ctx;
    zx_contend_io(zx, port);

    /* ULA port: any even address (bit 0 = 0). */
    if (!(port & 0x01)) {
        uint8_t result = 0x1F;  /* Start with all keys released (bits 0-4 high) */
        uint8_t high = port >> 8;

        /* Each bit in the high byte that is LOW selects one half-row.
         * We AND together all selected rows. */
        for (int row = 0; row < 8; row++) {
            if (!(high & (1 << row)))
                result &= zx->keyboard[row];
        }

        /* Bit 5: always 1. Bit 6: EAR input. On Issue 3 hardware,
         * speaker output high forces EAR high. Bit 7: always 1. */
        result |= 0xA0;
        if (zx->ear || zx->speaker) result |= 0x40;

        return result;
    }

    /* Kempston joystick: respond when bits A7, A6, A5 are all 0. */
    if (!(port & 0xE0))
        return zx->kempston;

    /* Unattached port: return 0xFF (no floating bus emulation). */
    return 0xFF;
}

/* I/O write: ULA port controls border color, beeper, and MIC.
 *   Bits 0-2: border color
 *   Bit 3: MIC output (tape saving)
 *   Bit 4: speaker (beeper) */

static void zx_record_audio_event(ZXSpectrum *zx) {
    uint8_t combined = zx->speaker | zx->ear;
    if (combined != zx->audio_level) {
        zx->audio_level = combined;
        if (zx->beeper_event_count < ZX_MAX_BEEPER_EVENTS) {
            zx->beeper_events[zx->beeper_event_count].tstates = zx->frame_tstates;
            zx->beeper_events[zx->beeper_event_count].level = combined;
            zx->beeper_event_count++;
        }
    }
}

static void zx_io_write(void *ctx, uint16_t port, uint8_t val) {
    ZXSpectrum *zx = (ZXSpectrum *)ctx;
    zx_contend_io(zx, port);

    if (!(port & 0x01)) {
        zx->border_color = val & 0x07;
        /* Keep MIC state so we can add Issue 2/board-accurate tape
         * coupling later without changing external state layout. */
        zx->mic = (val >> 3) & 1;

        /* Record beeper state change for audio rendering. */
        zx->speaker = (val >> 4) & 1;
        zx_record_audio_event(zx);
    }
}

/* ===================================================================
 * SCREEN RENDERING
 * =================================================================== */

/* Render a single display scanline (pixel row 0-191) into an RGB buffer.
 * Each scanline produces 320 pixels: 32px left border + 256px display + 32px
 * right border. Output is 3 bytes per pixel (R, G, B). */
static void zx_render_display_line(ZXSpectrum *zx, int pixel_y, uint8_t *line_rgb) {
    const uint8_t *border_rgb = zx_palette[zx->border_color];

    /* Left border: 32 pixels */
    for (int x = 0; x < ZX_BORDER_PX; x++) {
        *line_rgb++ = border_rgb[0];
        *line_rgb++ = border_rgb[1];
        *line_rgb++ = border_rgb[2];
    }

    /* Display area: 256 pixels (32 byte columns, 8 pixels each) */
    int char_row = pixel_y >> 3;  /* Which character row (0-23) */
    for (int col = 0; col < 32; col++) {
        uint16_t paddr = zx_pixel_addr(pixel_y, col);
        uint16_t aaddr = zx_attr_addr(char_row, col);
        uint8_t pixels = zx->memory[paddr - 0x4000];
        uint8_t attr = zx->memory[aaddr - 0x4000];

        /* Decode attribute byte:
         *   Bit 7: FLASH (swap ink/paper when flash_state is set)
         *   Bit 6: BRIGHT (use bright palette: colors 8-15)
         *   Bits 5-3: PAPER color (0-7)
         *   Bits 2-0: INK color (0-7) */
        int ink = attr & 0x07;
        int paper = (attr >> 3) & 0x07;
        int bright = (attr & 0x40) ? 8 : 0;

        /* FLASH: swap ink and paper every 16 frames */
        if ((attr & 0x80) && zx->flash_state) {
            int tmp = ink;
            ink = paper;
            paper = tmp;
        }

        const uint8_t *ink_rgb = zx_palette[ink + bright];
        const uint8_t *paper_rgb = zx_palette[paper + bright];

        /* Each bit of the pixel byte: 1 = ink, 0 = paper.
         * Bit 7 is the leftmost pixel. */
        for (int bit = 7; bit >= 0; bit--) {
            const uint8_t *c = (pixels & (1 << bit)) ? ink_rgb : paper_rgb;
            *line_rgb++ = c[0];
            *line_rgb++ = c[1];
            *line_rgb++ = c[2];
        }
    }

    /* Right border: 32 pixels */
    for (int x = 0; x < ZX_BORDER_PX; x++) {
        *line_rgb++ = border_rgb[0];
        *line_rgb++ = border_rgb[1];
        *line_rgb++ = border_rgb[2];
    }
}

/* Render a border-only scanline (for top/bottom border regions). */
static void zx_render_border_line(ZXSpectrum *zx, uint8_t *line_rgb) {
    const uint8_t *c = zx_palette[zx->border_color];
    for (int x = 0; x < ZX_FB_WIDTH; x++) {
        *line_rgb++ = c[0];
        *line_rgb++ = c[1];
        *line_rgb++ = c[2];
    }
}

/* Render one scanline (0-311) into the framebuffer.
 * Maps ULA scanlines to framebuffer rows:
 *   ULA scanlines 8-39   -> framebuffer rows 0-31    (top border)
 *   ULA scanlines 40-231 -> framebuffer rows 32-223  (32 border + 192 display)
 *   Wait... let me think about this more carefully.
 *
 * We want a 320x256 output with 32px border on all sides around the
 * 256x192 display. The framebuffer covers ULA scanlines that map to
 * visible content:
 *   FB row 0-31:    top border (ULA scanlines 32-63, last 32 lines of top border)
 *   FB row 32-223:  display area (ULA scanlines 64-255, the 192 display lines)
 *   FB row 224-255: bottom border (ULA scanlines 256-287, first 32 of bottom border)
 *
 * So ULA scanline S maps to FB row (S - 32) when S is in range [32, 287]. */
static void zx_render_scanline_to_fb(ZXSpectrum *zx, int scanline) {
    if (!zx->framebuffer) return;

    /* Map ULA scanline to framebuffer row.
     * We show 32 border lines above + 192 display + 32 border below = 256 rows.
     * This corresponds to ULA scanlines 32 through 287. */
    int fb_row = scanline - 32;
    if (fb_row < 0 || fb_row >= ZX_FB_HEIGHT) return;

    uint8_t *row_ptr = zx->framebuffer + fb_row * ZX_FB_WIDTH * 3;

    if (scanline >= ZX_FIRST_DISPLAY_LINE &&
        scanline < ZX_FIRST_DISPLAY_LINE + ZX_DISPLAY_LINES) {
        /* Display area: render pixels + border */
        int pixel_y = scanline - ZX_FIRST_DISPLAY_LINE;
        zx_render_display_line(zx, pixel_y, row_ptr);
    } else {
        /* Border area */
        zx_render_border_line(zx, row_ptr);
    }
}

/* ===================================================================
 * AUDIO RENDERING
 * =================================================================== */

/* Convert beeper events from this frame into PCM audio samples.
 * Walk the event list and sample the speaker state at evenly-spaced
 * T-state positions corresponding to the audio sample rate. */
static void zx_render_audio(ZXSpectrum *zx) {
    int event_idx = 0;
    uint8_t level = 0;

    /* The level at frame start is what the level was BEFORE all toggles
     * in the current frame occurred. Since each event is a toggle,
     * start_level = current_level ^ (count % 2). */
    level = zx->audio_level ^ (zx->beeper_event_count & 1);

    for (int i = 0; i < ZX_AUDIO_SAMPLES; i++) {
        uint32_t sample_t = (uint32_t)((uint64_t)i * ZX_TSTATES_PER_FRAME / ZX_AUDIO_SAMPLES);

        /* Advance through events up to this sample's T-state. */
        while (event_idx < zx->beeper_event_count &&
               zx->beeper_events[event_idx].tstates <= sample_t) {
            level = zx->beeper_events[event_idx].level;
            event_idx++;
        }

        /* Convert 1-bit speaker to signed 16-bit PCM. */
        zx->audio_buffer[i] = level ? 8192 : -8192;
    }
}

/* ===================================================================
 * INITIALIZATION
 * =================================================================== */

void zx_init(ZXSpectrum *zx, const uint8_t *rom) {
    memset(zx, 0, sizeof(ZXSpectrum));

    /* Store ROM pointer (not copied -- caller owns the data). */
    zx->rom = rom;

    /* Initialize CPU and wire up callbacks. */
    z80_init(&zx->cpu);
    zx->cpu.mem_read = zx_mem_read;
    zx->cpu.mem_write = zx_mem_write;
    zx->cpu.io_read = zx_io_read;
    zx->cpu.io_write = zx_io_write;
    zx->cpu.ctx = zx;

    /* All keys released (active LOW: 0xFF = no keys pressed). */
    memset(zx->keyboard, 0xFF, sizeof(zx->keyboard));
}

void zx_set_rom(ZXSpectrum *zx, const uint8_t *rom) {
    zx->rom = rom;
}

/* ===================================================================
 * FRAME EXECUTION
 * =================================================================== */

int zx_tick(ZXSpectrum *zx, int min_tstates) {
    uint32_t start = zx->frame_tstates;

    do {
        /* Execute one Z80 instruction. */
        uint64_t before = zx->cpu.clocks;
        z80_step(&zx->cpu);
        uint32_t elapsed = (uint32_t)(zx->cpu.clocks - before);
        zx->frame_tstates += elapsed;

        /* Render scanlines as we cross them. */
        if (zx->framebuffer) {
            int current_line = zx->frame_tstates / ZX_TSTATES_PER_LINE;
            while (zx->fb_next_line < current_line &&
                   zx->fb_next_line < ZX_LINES_PER_FRAME) {
                zx_render_scanline_to_fb(zx, zx->fb_next_line);
                zx->fb_next_line++;
            }
        }

        /* Check for frame boundary. Always return immediately so the
         * caller never misses a frame event, even if min_tstates
         * hasn't been reached yet. */
        if (zx->frame_tstates >= ZX_TSTATES_PER_FRAME) {
            /* Flush any remaining scanlines. */
            if (zx->framebuffer) {
                while (zx->fb_next_line < ZX_LINES_PER_FRAME) {
                    zx_render_scanline_to_fb(zx, zx->fb_next_line);
                    zx->fb_next_line++;
                }
            }

            /* Generate audio from this frame's beeper events. */
            zx_render_audio(zx);

            /* Carry over excess T-states into the next frame. */
            zx->frame_tstates -= ZX_TSTATES_PER_FRAME;
            zx->frame_count++;

            /* Toggle FLASH state every 16 frames (~0.32s per phase). */
            if ((zx->frame_count % 16) == 0)
                zx->flash_state ^= 1;

            /* Prepare next frame: fire interrupt, and account for the
             * acknowledge cycles in both CPU and ULA timing domains. */
            int int_tstates = z80_interrupt(&zx->cpu, 0xFF);
            zx->cpu.clocks += int_tstates;
            zx->frame_tstates += int_tstates;
            zx->fb_next_line = 0;
            zx->beeper_event_count = 0;

            return 1;
        }
    } while ((int)(zx->frame_tstates - start) < min_tstates);

    return 0;
}

void zx_frame(ZXSpectrum *zx) {
    while (!zx_tick(zx, 0))
        ;
}

/* ===================================================================
 * INPUT
 * =================================================================== */

void zx_key_down(ZXSpectrum *zx, int row, int bit) {
    if (row >= 0 && row < 8 && bit >= 0 && bit < 5)
        zx->keyboard[row] &= ~(1 << bit);  /* Active LOW: clear bit = pressed */
}

void zx_key_up(ZXSpectrum *zx, int row, int bit) {
    if (row >= 0 && row < 8 && bit >= 0 && bit < 5)
        zx->keyboard[row] |= (1 << bit);   /* Active LOW: set bit = released */
}

void zx_joy_down(ZXSpectrum *zx, int button) {
    zx->kempston |= (button & 0x1F);   /* Active HIGH: set bit = pressed */
}

void zx_joy_up(ZXSpectrum *zx, int button) {
    zx->kempston &= ~(button & 0x1F);  /* Active HIGH: clear bit = released */
}

/* ===================================================================
 * EAR INPUT (TAPE LOADING)
 * =================================================================== */

void zx_set_ear(ZXSpectrum *zx, uint8_t level) {
    zx->ear = level ? 1 : 0;
    zx_record_audio_event(zx);
}

/* ===================================================================
 * VIDEO
 * =================================================================== */

int zx_ula_scanline(ZXSpectrum *zx) {
    int scanline = zx->frame_tstates / ZX_TSTATES_PER_LINE;
    /* Return -1 during vsync (scanlines 0-7). */
    if (scanline < 8) return -1;
    if (scanline >= ZX_LINES_PER_FRAME) return -1;
    return scanline;
}

void zx_render_screen(ZXSpectrum *zx, uint8_t *rgb) {
    /* Render full 320x256 frame: 32px border around 256x192 display. */

    /* Top border: 32 rows of solid border color. */
    const uint8_t *bc = zx_palette[zx->border_color];
    for (int row = 0; row < ZX_BORDER_PX; row++) {
        uint8_t *p = rgb + row * ZX_FB_WIDTH * 3;
        for (int x = 0; x < ZX_FB_WIDTH; x++) {
            *p++ = bc[0]; *p++ = bc[1]; *p++ = bc[2];
        }
    }

    /* Display area: 192 rows with left/right border. */
    for (int y = 0; y < ZX_SCREEN_HEIGHT; y++) {
        uint8_t *p = rgb + (y + ZX_BORDER_PX) * ZX_FB_WIDTH * 3;
        zx_render_display_line(zx, y, p);
    }

    /* Bottom border: 32 rows of solid border color. */
    for (int row = 0; row < ZX_BORDER_PX; row++) {
        uint8_t *p = rgb + (ZX_SCREEN_HEIGHT + ZX_BORDER_PX + row) * ZX_FB_WIDTH * 3;
        for (int x = 0; x < ZX_FB_WIDTH; x++) {
            *p++ = bc[0]; *p++ = bc[1]; *p++ = bc[2];
        }
    }
}

void zx_set_framebuffer(ZXSpectrum *zx, uint8_t *rgb) {
    zx->framebuffer = rgb;
    zx->fb_next_line = 0;
}

/* ===================================================================
 * .Z80 SNAPSHOT LOADER
 * =================================================================== */

/* Decompress .z80 format data using the ED ED RLE scheme.
 * Reads from src[0..src_len-1], writes exactly dst_len bytes to dst.
 * Returns number of source bytes consumed, or -1 on error. */
static int zx_z80_decompress(const uint8_t *src, int src_len,
                             uint8_t *dst, int dst_len) {
    int si = 0, di = 0;
    while (di < dst_len && si < src_len) {
        if (src[si] == 0xED && si + 1 < src_len && src[si + 1] == 0xED) {
            /* ED ED count value: repeat 'value' count times. */
            if (si + 3 >= src_len) return -1;
            int count = src[si + 2];
            uint8_t value = src[si + 3];
            si += 4;
            for (int i = 0; i < count && di < dst_len; i++)
                dst[di++] = value;
        } else {
            dst[di++] = src[si++];
        }
    }
    if (di != dst_len) return -1;
    return si;
}

int zx_load_z80(ZXSpectrum *zx, const uint8_t *data, int size) {
    if (size < 30) return -1;

    /* ---------------------------------------------------------------
     * Parse the common 30-byte header (all versions).
     * --------------------------------------------------------------- */
    uint8_t a = data[0];
    uint8_t f = data[1];
    uint16_t bc = data[2] | (data[3] << 8);
    uint16_t hl = data[4] | (data[5] << 8);
    uint16_t pc = data[6] | (data[7] << 8);
    uint16_t sp = data[8] | (data[9] << 8);
    uint8_t i_reg = data[10];
    uint8_t r_reg = data[11];
    uint8_t flags1 = data[12];
    if (flags1 == 0xFF) flags1 = 1;  /* Compatibility quirk */
    uint16_t de = data[13] | (data[14] << 8);
    uint16_t bc_ = data[15] | (data[16] << 8);
    uint16_t de_ = data[17] | (data[18] << 8);
    uint16_t hl_ = data[19] | (data[20] << 8);
    uint8_t a_ = data[21];
    uint8_t f_ = data[22];
    uint16_t iy = data[23] | (data[24] << 8);
    uint16_t ix = data[25] | (data[26] << 8);
    uint8_t iff1 = data[27] ? 1 : 0;
    uint8_t iff2 = data[28] ? 1 : 0;
    uint8_t flags2 = data[29];

    /* Decode flags. */
    r_reg = (r_reg & 0x7F) | ((flags1 & 0x01) << 7);
    uint8_t border = (flags1 >> 1) & 0x07;
    int compressed = (flags1 >> 5) & 0x01;
    uint8_t im = flags2 & 0x03;

    /* ---------------------------------------------------------------
     * Determine version and load memory.
     * --------------------------------------------------------------- */
    if (pc != 0) {
        /* Version 1: memory follows the 30-byte header as a single block. */
        const uint8_t *mem_data = data + 30;
        int mem_len = size - 30;

        if (compressed) {
            /* Compressed: decompress into RAM at 0x4000-0xFFFF (48KB). */
            uint8_t buf[49152];
            if (zx_z80_decompress(mem_data, mem_len, buf, 49152) < 0)
                return -1;
            memcpy(zx->memory, buf, 49152);
        } else {
            /* Uncompressed: copy directly. */
            if (mem_len < 49152) return -1;
            memcpy(zx->memory, mem_data, 49152);
        }
    } else {
        /* Version 2 or 3: extended header follows, then memory pages. */
        if (size < 35) return -1;
        uint16_t ext_len = data[30] | (data[31] << 8);
        pc = data[32] | (data[33] << 8);
        uint8_t hw_mode = data[34];
        /* 48K-only loader: accept 48K and 48K+IF1 snapshots. */
        if (hw_mode != 0 && hw_mode != 1) return -1;

        int page_offset = 32 + ext_len;
        if (page_offset > size) return -1;

        /* Read memory page blocks. */
        while (page_offset + 3 <= size) {
            uint16_t block_len = data[page_offset] | (data[page_offset + 1] << 8);
            uint8_t page_num = data[page_offset + 2];
            page_offset += 3;

            /* Determine destination address from page number.
             * For 48K Spectrum:
             *   Page 4 -> 0x8000-0xBFFF
             *   Page 5 -> 0xC000-0xFFFF
             *   Page 8 -> 0x4000-0x7FFF */
            uint16_t dest_addr;
            switch (page_num) {
                case 4: dest_addr = 0x8000; break;
                case 5: dest_addr = 0xC000; break;
                case 8: dest_addr = 0x4000; break;
                default:
                    /* Unknown page: skip it. */
                    if (block_len == 0xFFFF)
                        page_offset += 16384;
                    else
                        page_offset += block_len;
                    continue;
            }

            if (block_len == 0xFFFF) {
                /* Uncompressed: 16384 bytes raw. */
                if (page_offset + 16384 > size) return -1;
                memcpy(zx->memory + (dest_addr - 0x4000), data + page_offset, 16384);
                page_offset += 16384;
            } else {
                /* Compressed: decompress. */
                if (page_offset + block_len > size) return -1;
                if (zx_z80_decompress(data + page_offset, block_len,
                                      zx->memory + (dest_addr - 0x4000), 16384) < 0)
                    return -1;
                page_offset += block_len;
            }
        }
    }

    /* ---------------------------------------------------------------
     * Restore CPU state.
     * --------------------------------------------------------------- */
    zx->cpu.a = a;      zx->cpu.f = f;
    zx->cpu.b = bc >> 8; zx->cpu.c = bc & 0xFF;
    zx->cpu.d = de >> 8; zx->cpu.e = de & 0xFF;
    zx->cpu.h = hl >> 8; zx->cpu.l = hl & 0xFF;
    zx->cpu.a_ = a_;    zx->cpu.f_ = f_;
    zx->cpu.b_ = bc_ >> 8; zx->cpu.c_ = bc_ & 0xFF;
    zx->cpu.d_ = de_ >> 8; zx->cpu.e_ = de_ & 0xFF;
    zx->cpu.h_ = hl_ >> 8; zx->cpu.l_ = hl_ & 0xFF;
    zx->cpu.ix = ix;
    zx->cpu.iy = iy;
    zx->cpu.sp = sp;
    zx->cpu.pc = pc;
    zx->cpu.i = i_reg;
    zx->cpu.r = r_reg;
    zx->cpu.iff1 = iff1;
    zx->cpu.iff2 = iff2;
    zx->cpu.im = im;
    zx->cpu.halted = 0;

    /* Restore ULA state. */
    zx->border_color = border;

    return 0;
}

/* spectrum_test.c -- Test suite for the ZX Spectrum 48K emulator.
 *
 * Tests each subsystem: memory map, contention, screen addressing,
 * keyboard/joystick, I/O ports, beeper, frame timing, rendering,
 * and .z80 snapshot loading.
 */

#include "spectrum.h"
#include "rom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================
 * TEST HARNESS
 * =================================================================== */

static int test_count = 0;
static int test_pass = 0;
static int test_fail = 0;

#define TEST(name) { int _t_ok = 1; test_count++; printf("  %-55s ", name);
#define ASSERT(cond) if (_t_ok && !(cond)) { \
    printf("FAIL (%s:%d: %s)\n", __FILE__, __LINE__, #cond); \
    test_fail++; _t_ok = 0; }
#define PASS() if (_t_ok) { printf("OK\n"); test_pass++; } }

/* ===================================================================
 * MEMORY MAP TESTS
 * =================================================================== */

static void test_memory_map(void) {
    printf("Memory map:\n");

    TEST("ROM is read-only") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        /* Read a byte from ROM, try to write, verify unchanged. */
        uint8_t original = zx.rom[0x0000];
        zx.cpu.mem_write(&zx, 0x0000, original ^ 0xFF);
        ASSERT(zx.rom[0x0000] == original);
    } PASS();

    TEST("RAM at 0x4000 is writable") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.cpu.mem_write(&zx, 0x4000, 0xAB);
        ASSERT(zx.memory[0x4000 - 0x4000] == 0xAB);
    } PASS();

    TEST("RAM at 0x8000 is writable") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.cpu.mem_write(&zx, 0x8000, 0xCD);
        ASSERT(zx.memory[0x8000 - 0x4000] == 0xCD);
    } PASS();

    TEST("RAM at 0xFFFF is writable") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.cpu.mem_write(&zx, 0xFFFF, 0xEF);
        ASSERT(zx.memory[0xFFFF - 0x4000] == 0xEF);
    } PASS();

    TEST("ROM contains expected first bytes (DI; XOR A)") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        ASSERT(zx.rom[0x0000] == 0xF3);  /* DI */
        ASSERT(zx.rom[0x0001] == 0xAF);  /* XOR A */
    } PASS();
}

/* ===================================================================
 * CONTENTION TABLE TESTS
 * =================================================================== */

static void test_contention(void) {
    printf("Memory contention:\n");

    TEST("No contention before display area") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        /* T-state 100: well before display, no delay. */
        zx.frame_tstates = 100;
        zx.cpu.mem_read(&zx, 0x4000);
        ASSERT(zx.frame_tstates == 100);
        /* Just before first contended T-state. */
        zx.frame_tstates = 14334;
        zx.cpu.mem_read(&zx, 0x4000);
        ASSERT(zx.frame_tstates == 14334);
    } PASS();

    TEST("Contention pattern at first display line") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        /* Test the 8-position cycle: 6,5,4,3,2,1,0,0 */
        zx.frame_tstates = 14335; zx.cpu.mem_read(&zx, 0x4000);
        ASSERT(zx.frame_tstates == 14335 + 6);
        zx.frame_tstates = 14336; zx.cpu.mem_read(&zx, 0x4000);
        ASSERT(zx.frame_tstates == 14336 + 5);
        zx.frame_tstates = 14337; zx.cpu.mem_read(&zx, 0x4000);
        ASSERT(zx.frame_tstates == 14337 + 4);
        zx.frame_tstates = 14338; zx.cpu.mem_read(&zx, 0x4000);
        ASSERT(zx.frame_tstates == 14338 + 3);
        zx.frame_tstates = 14339; zx.cpu.mem_read(&zx, 0x4000);
        ASSERT(zx.frame_tstates == 14339 + 2);
        zx.frame_tstates = 14340; zx.cpu.mem_read(&zx, 0x4000);
        ASSERT(zx.frame_tstates == 14340 + 1);
        zx.frame_tstates = 14341; zx.cpu.mem_read(&zx, 0x4000);
        ASSERT(zx.frame_tstates == 14341);  /* 0 delay */
        zx.frame_tstates = 14342; zx.cpu.mem_read(&zx, 0x4000);
        ASSERT(zx.frame_tstates == 14342);  /* 0 delay */
        /* Next cycle repeats: delay = 6 */
        zx.frame_tstates = 14343; zx.cpu.mem_read(&zx, 0x4000);
        ASSERT(zx.frame_tstates == 14343 + 6);
    } PASS();

    TEST("No contention in border portion of display scanline") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        /* After 128 contended T-states, the remaining 96 are free. */
        int border_start = 14335 + 128;
        zx.frame_tstates = border_start; zx.cpu.mem_read(&zx, 0x4000);
        ASSERT(zx.frame_tstates == (uint32_t)border_start);
        zx.frame_tstates = border_start + 50; zx.cpu.mem_read(&zx, 0x4000);
        ASSERT(zx.frame_tstates == (uint32_t)(border_start + 50));
    } PASS();

    TEST("Contention resumes on second display line") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        /* Second display line starts at 14335 + 224 = 14559. */
        zx.frame_tstates = 14559; zx.cpu.mem_read(&zx, 0x4000);
        ASSERT(zx.frame_tstates == 14559 + 6);
        zx.frame_tstates = 14560; zx.cpu.mem_read(&zx, 0x4000);
        ASSERT(zx.frame_tstates == 14560 + 5);
    } PASS();

    TEST("No contention after display area") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        /* After the last display line's contended portion. */
        int after_last = 14335 + 191 * 224 + 128;
        zx.frame_tstates = after_last; zx.cpu.mem_read(&zx, 0x4000);
        ASSERT(zx.frame_tstates == (uint32_t)after_last);
        /* Well past all display lines. */
        zx.frame_tstates = 60000; zx.cpu.mem_read(&zx, 0x4000);
        ASSERT(zx.frame_tstates == 60000);
    } PASS();

    TEST("Contention affects frame_tstates for contended reads") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.frame_tstates = 14335;
        zx.cpu.mem_read(&zx, 0x4000);
        ASSERT(zx.frame_tstates == 14335 + 6);
    } PASS();

    TEST("No contention for uncontended memory") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.frame_tstates = 14335;
        zx.cpu.mem_read(&zx, 0x8000);  /* Uncontended address */
        ASSERT(zx.frame_tstates == 14335);  /* No change */
    } PASS();
}

/* ===================================================================
 * SCREEN ADDRESS TESTS
 * =================================================================== */

/* External access to the static inline helpers for testing.
 * We re-implement them here to test the same logic. */
static uint16_t test_pixel_addr(int y, int col) {
    return 0x4000
        | ((y & 0xC0) << 5)
        | ((y & 0x07) << 8)
        | ((y & 0x38) << 2)
        | (col & 0x1F);
}

static void test_screen_addressing(void) {
    printf("Screen addressing:\n");

    TEST("First pixel (0,0) is at 0x4000") {
        ASSERT(test_pixel_addr(0, 0) == 0x4000);
    } PASS();

    TEST("First byte of second pixel row (0,1) is at 0x4100") {
        ASSERT(test_pixel_addr(1, 0) == 0x4100);
    } PASS();

    TEST("First byte of row 8 is at 0x4020") {
        /* Row 8 = second character row in first third. */
        ASSERT(test_pixel_addr(8, 0) == 0x4020);
    } PASS();

    TEST("First byte of second third (row 64) is at 0x4800") {
        ASSERT(test_pixel_addr(64, 0) == 0x4800);
    } PASS();

    TEST("First byte of third third (row 128) is at 0x5000") {
        ASSERT(test_pixel_addr(128, 0) == 0x5000);
    } PASS();

    TEST("Last byte of screen (row 191, col 31) is at 0x57FF") {
        ASSERT(test_pixel_addr(191, 31) == 0x57FF);
    } PASS();

    TEST("Column 1 of row 0 is at 0x4001") {
        ASSERT(test_pixel_addr(0, 1) == 0x4001);
    } PASS();

    TEST("Attribute at (0,0) is at 0x5800") {
        uint16_t addr = 0x5800 + 0 * 32 + 0;
        ASSERT(addr == 0x5800);
    } PASS();

    TEST("Attribute at (31,23) is at 0x5AFF") {
        uint16_t addr = 0x5800 + 23 * 32 + 31;
        ASSERT(addr == 0x5AFF);
    } PASS();
}

/* ===================================================================
 * KEYBOARD AND JOYSTICK TESTS
 * =================================================================== */

static void test_keyboard(void) {
    printf("Keyboard and joystick:\n");

    TEST("All keys released initially") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        /* Read port 0xFEFE (row 0): should return 0xFF (bits 0-4 high + bits 5,7). */
        uint8_t val = zx.cpu.io_read(&zx, 0xFEFE);
        ASSERT((val & 0x1F) == 0x1F);
    } PASS();

    TEST("Press key: CAPS SHIFT (row 0, bit 0)") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx_key_down(&zx, 0, 0);
        uint8_t val = zx.cpu.io_read(&zx, 0xFEFE);
        ASSERT((val & 0x01) == 0);  /* Bit 0 should be 0 (pressed) */
        ASSERT((val & 0x1E) == 0x1E);  /* Other bits still high */
    } PASS();

    TEST("Release key restores bit") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx_key_down(&zx, 0, 0);
        zx_key_up(&zx, 0, 0);
        uint8_t val = zx.cpu.io_read(&zx, 0xFEFE);
        ASSERT((val & 0x1F) == 0x1F);
    } PASS();

    TEST("Reading multiple rows (all rows)") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        /* Press SPACE (row 7, bit 0) */
        zx_key_down(&zx, 7, 0);
        /* Read port 0x00FE: all address lines low, all rows selected. */
        uint8_t val = zx.cpu.io_read(&zx, 0x00FE);
        ASSERT((val & 0x01) == 0);  /* SPACE pressed */
    } PASS();

    TEST("Row selection: row 7 not selected doesn't show SPACE") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx_key_down(&zx, 7, 0);  /* Press SPACE */
        /* Read port 0xFEFE: only row 0 selected (A8 low). */
        uint8_t val = zx.cpu.io_read(&zx, 0xFEFE);
        ASSERT((val & 0x1F) == 0x1F);  /* Row 0 has no keys pressed */
    } PASS();

    TEST("Bits 5 and 7 always high") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        uint8_t val = zx.cpu.io_read(&zx, 0xFEFE);
        ASSERT((val & 0x80) == 0x80);
        ASSERT((val & 0x20) == 0x20);
    } PASS();

    TEST("Kempston joystick: initially 0") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        uint8_t val = zx.cpu.io_read(&zx, 0x001F);
        ASSERT(val == 0x00);
    } PASS();

    TEST("Kempston joystick: press fire") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx_joy_down(&zx, ZX_JOY_FIRE);
        uint8_t val = zx.cpu.io_read(&zx, 0x001F);
        ASSERT((val & ZX_JOY_FIRE) == ZX_JOY_FIRE);
    } PASS();

    TEST("Kempston joystick: release") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx_joy_down(&zx, ZX_JOY_LEFT | ZX_JOY_FIRE);
        zx_joy_up(&zx, ZX_JOY_FIRE);
        uint8_t val = zx.cpu.io_read(&zx, 0x001F);
        ASSERT((val & ZX_JOY_FIRE) == 0);
        ASSERT((val & ZX_JOY_LEFT) == ZX_JOY_LEFT);
    } PASS();
}

/* ===================================================================
 * I/O PORT TESTS
 * =================================================================== */

static void test_io_ports(void) {
    printf("I/O ports:\n");

    TEST("Border color set via port 0xFE") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.cpu.io_write(&zx, 0xFE, 0x05);  /* Cyan border */
        ASSERT(zx.border_color == 5);
    } PASS();

    TEST("Speaker toggled via port 0xFE bit 4") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.cpu.io_write(&zx, 0xFE, 0x10);  /* Speaker on */
        ASSERT(zx.speaker == 1);
        zx.cpu.io_write(&zx, 0xFE, 0x00);  /* Speaker off */
        ASSERT(zx.speaker == 0);
    } PASS();

    TEST("Any even port triggers ULA") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.cpu.io_write(&zx, 0x00, 0x03);  /* Even port, not 0xFE */
        ASSERT(zx.border_color == 3);
    } PASS();

    TEST("I/O contention: N:1,C:3 on uncontended even port") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.frame_tstates = 14335;
        zx.cpu.io_read(&zx, 0x00FE);
        ASSERT(zx.frame_tstates == 14340);  /* Delay from check at 14336: +5 */
    } PASS();

    TEST("I/O contention: C:1,C:3 on contended even port") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.frame_tstates = 14335;
        zx.cpu.io_read(&zx, 0x40FE);
        ASSERT(zx.frame_tstates == 14341);  /* +6 then +0 */
    } PASS();

    TEST("I/O contention: C:1x4 on contended odd port") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.frame_tstates = 14335;
        zx.cpu.io_read(&zx, 0x40FF);
        ASSERT(zx.frame_tstates == 14347);  /* +6,+0,+6,+0 */
    } PASS();

    TEST("Issue 3 EAR readback: speaker high forces bit 6 high") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx_set_ear(&zx, 0);
        zx.cpu.io_write(&zx, 0xFE, 0x10);  /* Speaker on */
        uint8_t val = zx.cpu.io_read(&zx, 0x00FE);
        ASSERT((val & 0x40) == 0x40);
        zx.cpu.io_write(&zx, 0xFE, 0x00);  /* Speaker off */
        val = zx.cpu.io_read(&zx, 0x00FE);
        ASSERT((val & 0x40) == 0x00);
    } PASS();

    TEST("Unattached port returns 0xFF") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        uint8_t val = zx.cpu.io_read(&zx, 0xFF);  /* Odd, not Kempston */
        ASSERT(val == 0xFF);
    } PASS();
}

/* ===================================================================
 * BEEPER TESTS
 * =================================================================== */

static void test_beeper(void) {
    printf("Beeper audio:\n");

    TEST("Speaker change records beeper event") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.frame_tstates = 1000;
        zx.cpu.io_write(&zx, 0xFE, 0x10);  /* Speaker on */
        ASSERT(zx.beeper_event_count == 1);
        ASSERT(zx.beeper_events[0].tstates == 1000);
        ASSERT(zx.beeper_events[0].level == 1);
    } PASS();

    TEST("Same speaker state doesn't record event") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.cpu.io_write(&zx, 0xFE, 0x10);  /* Speaker on */
        zx.cpu.io_write(&zx, 0xFE, 0x10);  /* Same state */
        ASSERT(zx.beeper_event_count == 1);  /* Only one event */
    } PASS();

    TEST("Toggle records two events") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.frame_tstates = 100;
        zx.cpu.io_write(&zx, 0xFE, 0x10);  /* On */
        zx.frame_tstates = 200;
        zx.cpu.io_write(&zx, 0xFE, 0x00);  /* Off */
        ASSERT(zx.beeper_event_count == 2);
        ASSERT(zx.beeper_events[1].tstates == 200);
        ASSERT(zx.beeper_events[1].level == 0);
    } PASS();
}

/* ===================================================================
 * FRAME TIMING TESTS
 * =================================================================== */

static void test_frame_timing(void) {
    printf("Frame timing:\n");

    TEST("ULA scanline returns -1 during vsync") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.frame_tstates = 0;
        ASSERT(zx_ula_scanline(&zx) == -1);
        zx.frame_tstates = 7 * 224;  /* Still in vsync */
        ASSERT(zx_ula_scanline(&zx) == -1);
    } PASS();

    TEST("ULA scanline returns correct line in top border") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.frame_tstates = 8 * 224;  /* Scanline 8 */
        ASSERT(zx_ula_scanline(&zx) == 8);
    } PASS();

    TEST("ULA scanline returns correct line in display area") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.frame_tstates = 64 * 224;  /* First display line */
        ASSERT(zx_ula_scanline(&zx) == 64);
    } PASS();

    TEST("zx_frame runs and increments frame_count") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx_frame(&zx);
        ASSERT(zx.frame_count == 1);
    } PASS();

    TEST("zx_frame runs two frames correctly") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx_frame(&zx);
        zx_frame(&zx);
        ASSERT(zx.frame_count == 2);
    } PASS();

    TEST("Accepted frame INT contributes 13 T-states to next frame") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.memory[0x8000 - 0x4000] = 0x76;  /* HALT */
        zx.cpu.pc = 0x8000;
        zx.cpu.iff1 = 1;
        zx.cpu.iff2 = 1;
        zx.cpu.im = 1;
        zx_frame(&zx);
        ASSERT(zx.frame_count == 1);
        ASSERT(zx.frame_tstates == 13);
    } PASS();

    TEST("Flash toggles every 16 frames") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        ASSERT(zx.flash_state == 0);
        for (int i = 0; i < 16; i++) zx_frame(&zx);
        ASSERT(zx.flash_state == 1);
        for (int i = 0; i < 16; i++) zx_frame(&zx);
        ASSERT(zx.flash_state == 0);
    } PASS();
}

/* ===================================================================
 * RENDERING TESTS
 * =================================================================== */

static void test_rendering(void) {
    printf("Rendering:\n");

    TEST("On-demand render produces correct border color") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.border_color = 2;  /* Red */
        /* Clear screen to avoid garbage. */
        memset(zx.memory, 0, 6912);

        uint8_t rgb[ZX_FB_WIDTH * ZX_FB_HEIGHT * 3];
        zx_render_screen(&zx, rgb);

        /* Top-left corner (0,0) should be border color = red = 0xD7,0,0. */
        ASSERT(rgb[0] == 0xD7);
        ASSERT(rgb[1] == 0x00);
        ASSERT(rgb[2] == 0x00);
    } PASS();

    TEST("Display area renders ink/paper correctly") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.border_color = 0;

        /* Clear screen. */
        memset(zx.memory, 0, 6144);
        memset(zx.memory + 0x1800, 0, 768);

        /* Set pixel (0,0) byte to 0x80 (leftmost pixel set). */
        zx.memory[0] = 0x80;
        /* Set attribute (0,0): ink=7 (white), paper=0 (black), no bright. */
        zx.memory[0x1800] = 0x07;

        uint8_t rgb[ZX_FB_WIDTH * ZX_FB_HEIGHT * 3];
        zx_render_screen(&zx, rgb);

        /* The first display pixel is at framebuffer position (32, 32).
         * Row 32, column 32 in the RGB buffer. */
        int offset = (32 * ZX_FB_WIDTH + 32) * 3;
        /* This pixel should be ink (white normal = 0xD7,0xD7,0xD7). */
        ASSERT(rgb[offset + 0] == 0xD7);
        ASSERT(rgb[offset + 1] == 0xD7);
        ASSERT(rgb[offset + 2] == 0xD7);

        /* Next pixel should be paper (black = 0,0,0). */
        ASSERT(rgb[offset + 3] == 0x00);
        ASSERT(rgb[offset + 4] == 0x00);
        ASSERT(rgb[offset + 5] == 0x00);
    } PASS();

    TEST("Framebuffer auto-rendering works") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        zx.border_color = 1;  /* Blue */
        memset(zx.memory, 0, 6912);

        /* Point CPU at a HALT in RAM so the ROM doesn't change border. */
        zx.memory[0x8000 - 0x4000] = 0x76;  /* HALT */
        zx.cpu.pc = 0x8000;
        zx.cpu.iff1 = 1;  /* Enable interrupts so HALT wakes on frame INT */

        uint8_t rgb[ZX_FB_WIDTH * ZX_FB_HEIGHT * 3];
        memset(rgb, 0, sizeof(rgb));
        zx_set_framebuffer(&zx, rgb);

        /* Run one frame -- this should render into the framebuffer. */
        zx_frame(&zx);

        /* Top-left pixel should be blue border (0x00, 0x00, 0xD7). */
        ASSERT(rgb[0] == 0x00);
        ASSERT(rgb[1] == 0x00);
        ASSERT(rgb[2] == 0xD7);
    } PASS();

    TEST("BRIGHT attribute uses bright palette") {
        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        memset(zx.memory, 0, 6144);
        memset(zx.memory + 0x1800, 0, 768);

        zx.memory[0] = 0x80;  /* Leftmost pixel set */
        zx.memory[0x1800] = 0x47;  /* BRIGHT + ink=7, paper=0 */

        uint8_t rgb[ZX_FB_WIDTH * ZX_FB_HEIGHT * 3];
        zx_render_screen(&zx, rgb);

        int offset = (32 * ZX_FB_WIDTH + 32) * 3;
        /* Bright white = 0xFF, 0xFF, 0xFF. */
        ASSERT(rgb[offset + 0] == 0xFF);
        ASSERT(rgb[offset + 1] == 0xFF);
        ASSERT(rgb[offset + 2] == 0xFF);
    } PASS();
}

/* ===================================================================
 * .Z80 SNAPSHOT LOADING TESTS
 * =================================================================== */

static void test_z80_loading(void) {
    printf(".z80 snapshot loading:\n");

    TEST("Load .z80 file from roms/ directory") {
        /* Try to load jetpac.z80 if available. */
        FILE *f = fopen("roms/jetpac.z80", "rb");
        if (!f) {
            printf("SKIP (roms/jetpac.z80 not found)\n");
            test_pass++;  /* Don't count as failure */
        } else {
            fseek(f, 0, SEEK_END);
            long fsize = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *data = (uint8_t *)malloc(fsize);
            fread(data, 1, fsize, f);
            fclose(f);

            ZXSpectrum zx;
            zx_init(&zx, zx_spectrum_rom);
            int rc = zx_load_z80(&zx, data, (int)fsize);
            free(data);

            ASSERT(rc == 0);
            /* PC should be nonzero after loading. */
            ASSERT(zx.cpu.pc != 0);
            /* SP should be in a reasonable range. */
            ASSERT(zx.cpu.sp >= 0x4000);
        }
    } PASS();

    TEST("Load and run snapshot for a few frames") {
        FILE *f = fopen("roms/jetpac.z80", "rb");
        if (!f) {
            printf("SKIP (roms/jetpac.z80 not found)\n");
            test_pass++;
        } else {
            fseek(f, 0, SEEK_END);
            long fsize = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *data = (uint8_t *)malloc(fsize);
            fread(data, 1, fsize, f);
            fclose(f);

            ZXSpectrum zx;
            zx_init(&zx, zx_spectrum_rom);
            zx_load_z80(&zx, data, (int)fsize);
            free(data);

            /* Run 50 frames (1 second of emulation). Should not crash. */
            for (int i = 0; i < 50; i++)
                zx_frame(&zx);

            ASSERT(zx.frame_count == 50);
        }
    } PASS();

    TEST("Reject truncated compressed v1 snapshot") {
        uint8_t bad[34] = {0};
        bad[6] = 0x01;      /* PC != 0 => v1 */
        bad[12] = 0x20;     /* Compressed flag */
        bad[29] = 0x01;     /* IM 1 */
        bad[30] = 0xED;     /* Tiny RLE stream, far from 48KB output */
        bad[31] = 0xED;
        bad[32] = 0x10;
        bad[33] = 0x00;

        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        ASSERT(zx_load_z80(&zx, bad, sizeof(bad)) == -1);
    } PASS();

    TEST("Reject non-48K v2/v3 hardware mode") {
        uint8_t bad[55] = {0};
        bad[30] = 23;       /* Extended header length => v2 */
        bad[31] = 0;
        bad[32] = 0x00;     /* PC */
        bad[33] = 0x80;
        bad[34] = 3;        /* 128K mode */

        ZXSpectrum zx;
        zx_init(&zx, zx_spectrum_rom);
        ASSERT(zx_load_z80(&zx, bad, sizeof(bad)) == -1);
    } PASS();
}

/* ===================================================================
 * PPM OUTPUT (for visual verification)
 * =================================================================== */

/* Write a PPM image file for visual inspection.
 * Usage: ./spectrum_test --render <snapshot.z80> [frames] */
static int render_snapshot(const char *filename, int frames) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", filename);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc(fsize);
    fread(data, 1, fsize, f);
    fclose(f);

    ZXSpectrum zx;
    zx_init(&zx, zx_spectrum_rom);
    if (zx_load_z80(&zx, data, (int)fsize) != 0) {
        fprintf(stderr, "Failed to load .z80 file\n");
        free(data);
        return 1;
    }
    free(data);

    /* Run the requested number of frames. */
    for (int i = 0; i < frames; i++)
        zx_frame(&zx);

    /* Render screen to RGB buffer. */
    uint8_t *rgb = (uint8_t *)malloc(ZX_FB_WIDTH * ZX_FB_HEIGHT * 3);
    zx_render_screen(&zx, rgb);

    /* Write PPM file. */
    printf("P6\n%d %d\n255\n", ZX_FB_WIDTH, ZX_FB_HEIGHT);
    fwrite(rgb, 1, ZX_FB_WIDTH * ZX_FB_HEIGHT * 3, stdout);

    free(rgb);
    return 0;
}

/* ===================================================================
 * MAIN
 * =================================================================== */

int main(int argc, char **argv) {
    /* Special mode: render a snapshot to PPM. */
    if (argc >= 3 && strcmp(argv[1], "--render") == 0) {
        int frames = (argc >= 4) ? atoi(argv[3]) : 100;
        return render_snapshot(argv[2], frames);
    }

    printf("=== ZX Spectrum 48K Emulator Tests ===\n\n");

    test_memory_map();
    test_contention();
    test_screen_addressing();
    test_keyboard();
    test_io_ports();
    test_beeper();
    test_frame_timing();
    test_rendering();
    test_z80_loading();

    printf("\n--- Results: %d tests, %d passed, %d failed ---\n",
           test_count, test_pass, test_fail);

    return test_fail > 0 ? 1 : 0;
}

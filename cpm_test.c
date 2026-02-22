/* cpm_test.c -- Test suite for the CP/M emulator.
 *
 * Tests the CP/M BDOS system call implementation by loading tiny Z80
 * programs that call BDOS functions, running them, and verifying the
 * results (console output, register values, memory contents).
 *
 * Test approach:
 *   Each test initializes a CPMachine, optionally sets up console
 *   callbacks for capturing output or providing input, loads a small
 *   Z80 program at 0100h, runs it until it halts or warm boots,
 *   and checks the expected outcomes.
 */

#include "cpm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

/* ===================================================================
 * TEST HARNESS
 * =================================================================== */

static int test_count = 0;
static int test_pass = 0;
static int test_fail = 0;

#define TEST(name) { int _t_ok = 1; test_count++; printf("  %-50s ", name);
#define ASSERT(cond) if (_t_ok && !(cond)) { \
    _t_ok = 0; test_fail++; printf("FAIL (%s:%d: %s)\n", __FILE__, __LINE__, #cond); }
#define PASS() if (_t_ok) { test_pass++; printf("OK\n"); } }

/* ===================================================================
 * CONSOLE I/O CAPTURE
 *
 * For testing, we capture console output into a buffer and provide
 * canned console input from a string.
 * =================================================================== */

static char test_output[4096];
static int test_output_len;

static char test_input[256];
static int test_input_pos;
static int test_input_len;

static void test_con_out(void *ctx, uint8_t ch) {
    (void)ctx;
    if (test_output_len < (int)sizeof(test_output) - 1) {
        test_output[test_output_len++] = ch;
        test_output[test_output_len] = '\0';
    }
}

static int test_con_in(void *ctx) {
    (void)ctx;
    if (test_input_pos < test_input_len)
        return test_input[test_input_pos++];
    return -1;  /* No more input */
}

static int test_con_status(void *ctx) {
    (void)ctx;
    return test_input_pos < test_input_len;
}

static void test_io_reset(void) {
    test_output_len = 0;
    test_output[0] = '\0';
    test_input_pos = 0;
    test_input_len = 0;
}

static void test_set_input(const char *s) {
    test_input_len = strlen(s);
    if (test_input_len > (int)sizeof(test_input))
        test_input_len = sizeof(test_input);
    memcpy(test_input, s, test_input_len);
    test_input_pos = 0;
}

/* ===================================================================
 * TEST HELPERS
 * =================================================================== */

/* Initialize a CPMachine with test console callbacks. */
static void test_cpm_init(CPMachine *cpm) {
    cpm_init(cpm);
    cpm->con_out    = test_con_out;
    cpm->con_in     = test_con_in;
    cpm->con_status = test_con_status;
    cpm->con_ctx    = NULL;
    test_io_reset();
}

/* Load a Z80 program at 0100h and run it until halt or warm boot.
 * Returns 1 if warm boot was triggered, 0 if halted. */
static int test_load_run(CPMachine *cpm, const uint8_t *prog, int len) {
    memcpy(&cpm->memory[0x0100], prog, len);
    cpm->cpu.pc = 0x0100;
    cpm->cpu.halted = 0;  /* Clear halt from any previous run */

    /* Set up stack with 0x0000 as return address (triggers warm boot
     * if the program does RET). */
    cpm->cpu.sp = CPM_CCP_BASE;
    cpm->cpu.sp -= 2;
    cpm->memory[cpm->cpu.sp]     = 0x00;
    cpm->memory[cpm->cpu.sp + 1] = 0x00;

    return cpm_run(cpm);
}

/* ===================================================================
 * TESTS: PAGE ZERO SETUP
 * =================================================================== */

static void test_page_zero(void) {
    TEST("Page zero: warm boot vector at 0000h") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* 0000h should be JP F203h (warm boot trap). */
        ASSERT(cpm.memory[0x0000] == 0xC3);  /* JP opcode */
        ASSERT(cpm.memory[0x0001] == (CPM_WBOOT_TRAP & 0xFF));
        ASSERT(cpm.memory[0x0002] == (CPM_WBOOT_TRAP >> 8));
        PASS();
    }

    TEST("Page zero: BDOS entry at 0005h") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* 0005h should be JP E406h (BDOS trap). */
        ASSERT(cpm.memory[0x0005] == 0xC3);
        ASSERT(cpm.memory[0x0006] == (CPM_BDOS_TRAP & 0xFF));
        ASSERT(cpm.memory[0x0007] == (CPM_BDOS_TRAP >> 8));
        PASS();
    }

    TEST("Page zero: IOBYTE and drive/user") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        ASSERT(cpm.memory[0x0003] == 0x00);  /* IOBYTE = 0 */
        ASSERT(cpm.memory[0x0004] == 0x00);  /* Drive A, User 0 */
        PASS();
    }
}

/* ===================================================================
 * TESTS: BDOS CONSOLE OUTPUT (FUNCTIONS 2, 9)
 * =================================================================== */

static void test_console_output(void) {
    TEST("BDOS 2: Console output single char") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* LD C,2 ; LD E,'A' ; CALL 0005h ; HALT */
        uint8_t prog[] = { 0x0E, 0x02, 0x1E, 'A', 0xCD, 0x05, 0x00, 0x76 };
        test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(test_output_len == 1);
        ASSERT(test_output[0] == 'A');
        PASS();
    }

    TEST("BDOS 2: Tab expansion") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* Output "X" then TAB. Tab should expand to 7 spaces (X is col 1,
         * next tab stop is col 8, so 7 spaces). */
        uint8_t prog[] = {
            0x0E, 0x02, 0x1E, 'X',    0xCD, 0x05, 0x00,  /* LD C,2; LD E,'X'; CALL 5 */
            0x0E, 0x02, 0x1E, '\t',   0xCD, 0x05, 0x00,  /* LD C,2; LD E,TAB; CALL 5 */
            0x76                                           /* HALT */
        };
        test_load_run(&cpm, prog, sizeof(prog));

        /* Expected: 'X' + 7 spaces = 8 chars total */
        ASSERT(test_output_len == 8);
        ASSERT(test_output[0] == 'X');
        for (int i = 1; i < 8; i++)
            ASSERT(test_output[i] == ' ');
        PASS();
    }

    TEST("BDOS 9: Print $-terminated string") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* Store "Hello$" at 0200h, then:
         * LD C,9 ; LD DE,0200h ; CALL 0005h ; HALT */
        const char *msg = "Hello$";
        memcpy(&cpm.memory[0x0200], msg, 6);

        uint8_t prog[] = {
            0x0E, 0x09,                     /* LD C, 9 */
            0x11, 0x00, 0x02,               /* LD DE, 0200h */
            0xCD, 0x05, 0x00,               /* CALL 0005h */
            0x76                            /* HALT */
        };
        test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(test_output_len == 5);
        ASSERT(strcmp(test_output, "Hello") == 0);
        PASS();
    }

    TEST("BDOS 9: Print string with tab") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* "A\tB$" -- A at col 0, tab to col 8, then B */
        const char *msg = "A\tB$";
        memcpy(&cpm.memory[0x0200], msg, 4);

        uint8_t prog[] = {
            0x0E, 0x09, 0x11, 0x00, 0x02, 0xCD, 0x05, 0x00, 0x76
        };
        test_load_run(&cpm, prog, sizeof(prog));

        /* Expected: 'A' + 7 spaces + 'B' = 9 chars */
        ASSERT(test_output_len == 9);
        ASSERT(test_output[0] == 'A');
        ASSERT(test_output[8] == 'B');
        PASS();
    }
}

/* ===================================================================
 * TESTS: BDOS VERSION AND SYSTEM CALLS
 * =================================================================== */

static void test_system_calls(void) {
    TEST("BDOS 12: Version returns 0x0022") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* LD C,12 ; CALL 0005h ; HALT */
        uint8_t prog[] = { 0x0E, 0x0C, 0xCD, 0x05, 0x00, 0x76 };
        test_load_run(&cpm, prog, sizeof(prog));

        /* 8-bit return: A = L = 0x22 */
        ASSERT(cpm.cpu.a == 0x22);
        ASSERT(cpm.cpu.l == 0x22);
        /* 16-bit return: H = B = 0x00 */
        ASSERT(cpm.cpu.h == 0x00);
        ASSERT(cpm.cpu.b == 0x00);
        PASS();
    }

    TEST("BDOS 0: System reset triggers warm boot") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* LD C,0 ; CALL 0005h */
        uint8_t prog[] = { 0x0E, 0x00, 0xCD, 0x05, 0x00 };
        int wb = test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(wb == 1);
        ASSERT(cpm.warm_boot == 1);
        PASS();
    }

    TEST("RET triggers warm boot via 0000h") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* RET -- pops 0x0000 from stack, goes to JP F203h → warm boot */
        uint8_t prog[] = { 0xC9 };  /* RET */
        int wb = test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(wb == 1);
        PASS();
    }
}

/* ===================================================================
 * TESTS: DIRECT CONSOLE I/O (FUNCTION 6)
 * =================================================================== */

static void test_direct_io(void) {
    TEST("BDOS 6: Direct output") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* LD C,6 ; LD E,'Z' ; CALL 0005h ; HALT */
        uint8_t prog[] = { 0x0E, 0x06, 0x1E, 'Z', 0xCD, 0x05, 0x00, 0x76 };
        test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(test_output_len == 1);
        ASSERT(test_output[0] == 'Z');
        PASS();
    }

    TEST("BDOS 6: Direct input with char ready") {
        CPMachine cpm;
        test_cpm_init(&cpm);
        test_set_input("Q");

        /* LD C,6 ; LD E,FFh ; CALL 0005h ; HALT */
        uint8_t prog[] = { 0x0E, 0x06, 0x1E, 0xFF, 0xCD, 0x05, 0x00, 0x76 };
        test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(cpm.cpu.a == 'Q');
        PASS();
    }

    TEST("BDOS 6: Direct input with nothing ready") {
        CPMachine cpm;
        test_cpm_init(&cpm);
        /* No input set */

        /* LD C,6 ; LD E,FFh ; CALL 0005h ; HALT */
        uint8_t prog[] = { 0x0E, 0x06, 0x1E, 0xFF, 0xCD, 0x05, 0x00, 0x76 };
        test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(cpm.cpu.a == 0x00);
        PASS();
    }
}

/* ===================================================================
 * TESTS: CONSOLE STATUS (FUNCTION 11)
 * =================================================================== */

static void test_console_status(void) {
    TEST("BDOS 11: No char ready returns 00h") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        uint8_t prog[] = { 0x0E, 0x0B, 0xCD, 0x05, 0x00, 0x76 };
        test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(cpm.cpu.a == 0x00);
        PASS();
    }

    TEST("BDOS 11: Char ready returns FFh") {
        CPMachine cpm;
        test_cpm_init(&cpm);
        test_set_input("X");

        uint8_t prog[] = { 0x0E, 0x0B, 0xCD, 0x05, 0x00, 0x76 };
        test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(cpm.cpu.a == 0xFF);
        PASS();
    }
}

/* ===================================================================
 * TESTS: IOBYTE (FUNCTIONS 7, 8)
 * =================================================================== */

static void test_iobyte(void) {
    TEST("BDOS 7/8: Set then get IOBYTE") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* LD C,8 ; LD E,42h ; CALL 5 ; LD C,7 ; CALL 5 ; HALT */
        uint8_t prog[] = {
            0x0E, 0x08, 0x1E, 0x42, 0xCD, 0x05, 0x00,  /* Set IOBYTE to 42h */
            0x0E, 0x07,             0xCD, 0x05, 0x00,  /* Get IOBYTE */
            0x76
        };
        test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(cpm.cpu.a == 0x42);
        ASSERT(cpm.memory[CPM_IOBYTE] == 0x42);
        PASS();
    }
}

/* ===================================================================
 * TESTS: DISK AND DMA (FUNCTIONS 14, 25, 26)
 * =================================================================== */

static void test_disk_dma(void) {
    TEST("BDOS 14/25: Select disk then get current") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* LD C,14 ; LD E,2 ; CALL 5  (select C:)
         * LD C,25 ; CALL 5  (get current → should be 2)
         * HALT */
        uint8_t prog[] = {
            0x0E, 0x0E, 0x1E, 0x02, 0xCD, 0x05, 0x00,
            0x0E, 0x19,             0xCD, 0x05, 0x00,
            0x76
        };
        test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(cpm.cpu.a == 0x02);
        ASSERT(cpm.current_drive == 2);
        PASS();
    }

    TEST("BDOS 26: Set DMA address") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* LD C,26 ; LD DE,8000h ; CALL 5 ; HALT */
        uint8_t prog[] = {
            0x0E, 0x1A, 0x11, 0x00, 0x80, 0xCD, 0x05, 0x00, 0x76
        };
        test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(cpm.dma == 0x8000);
        PASS();
    }
}

/* ===================================================================
 * TESTS: USER NUMBER (FUNCTION 32)
 * =================================================================== */

static void test_user_number(void) {
    TEST("BDOS 32: Set then get user number") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* LD C,32 ; LD E,5 ; CALL 5  (set user 5)
         * LD C,32 ; LD E,FFh ; CALL 5  (get user)
         * HALT */
        uint8_t prog[] = {
            0x0E, 0x20, 0x1E, 0x05, 0xCD, 0x05, 0x00,   /* Set user 5 */
            0x0E, 0x20, 0x1E, 0xFF, 0xCD, 0x05, 0x00,   /* Get user */
            0x76
        };
        test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(cpm.cpu.a == 5);
        ASSERT(cpm.user_number == 5);
        /* Check drive/user byte in memory */
        ASSERT((cpm.memory[CPM_DRIVE_USER] >> 4) == 5);
        PASS();
    }
}

/* ===================================================================
 * TESTS: CONSOLE INPUT (FUNCTIONS 1, 10)
 * =================================================================== */

static void test_console_input(void) {
    TEST("BDOS 1: Console input with echo") {
        CPMachine cpm;
        test_cpm_init(&cpm);
        test_set_input("A");

        /* LD C,1 ; CALL 5 ; HALT */
        uint8_t prog[] = { 0x0E, 0x01, 0xCD, 0x05, 0x00, 0x76 };
        test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(cpm.cpu.a == 'A');
        /* Character should be echoed to output */
        ASSERT(test_output_len == 1);
        ASSERT(test_output[0] == 'A');
        PASS();
    }

    TEST("BDOS 10: Read buffer basic") {
        CPMachine cpm;
        test_cpm_init(&cpm);
        test_set_input("Hello\r");

        /* Set up buffer at 0200h: max_len=80 */
        cpm.memory[0x0200] = 80;
        cpm.memory[0x0201] = 0;

        /* LD C,10 ; LD DE,0200h ; CALL 5 ; HALT */
        uint8_t prog[] = {
            0x0E, 0x0A, 0x11, 0x00, 0x02, 0xCD, 0x05, 0x00, 0x76
        };
        test_load_run(&cpm, prog, sizeof(prog));

        /* Check buffer: count should be 5, data "Hello" */
        ASSERT(cpm.memory[0x0201] == 5);
        ASSERT(memcmp(&cpm.memory[0x0202], "Hello", 5) == 0);
        /* Echo should include "Hello" + CR + LF */
        ASSERT(strstr(test_output, "Hello") != NULL);
        PASS();
    }

    TEST("BDOS 10: Read buffer with backspace") {
        CPMachine cpm;
        test_cpm_init(&cpm);
        test_set_input("Helo\x08lo\r");  /* Type "Helo", backspace, "lo" → "Hello" */

        cpm.memory[0x0200] = 80;
        cpm.memory[0x0201] = 0;

        uint8_t prog[] = {
            0x0E, 0x0A, 0x11, 0x00, 0x02, 0xCD, 0x05, 0x00, 0x76
        };
        test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(cpm.memory[0x0201] == 5);
        ASSERT(memcmp(&cpm.memory[0x0202], "Hello", 5) == 0);
        PASS();
    }
}

/* ===================================================================
 * TESTS: MULTIPLE BDOS CALLS
 * =================================================================== */

static void test_multiple_calls(void) {
    TEST("Multiple BDOS calls in one program") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* Program that prints "Hi" using two function 2 calls:
         *   LD C,2 ; LD E,'H' ; CALL 5
         *   LD C,2 ; LD E,'i' ; CALL 5
         *   HALT */
        uint8_t prog[] = {
            0x0E, 0x02, 0x1E, 'H', 0xCD, 0x05, 0x00,
            0x0E, 0x02, 0x1E, 'i', 0xCD, 0x05, 0x00,
            0x76
        };
        test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(test_output_len == 2);
        ASSERT(test_output[0] == 'H');
        ASSERT(test_output[1] == 'i');
        PASS();
    }

    TEST("Print string then get version") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* Store "OK$" at 0200h */
        memcpy(&cpm.memory[0x0200], "OK$", 3);

        /* Print "OK", then get version */
        uint8_t prog[] = {
            0x0E, 0x09, 0x11, 0x00, 0x02, 0xCD, 0x05, 0x00,  /* Print "OK" */
            0x0E, 0x0C,                   0xCD, 0x05, 0x00,  /* Get version */
            0x76
        };
        test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(strcmp(test_output, "OK") == 0);
        ASSERT(cpm.cpu.a == 0x22);
        PASS();
    }
}

/* ===================================================================
 * TESTS: FCB AND COMMAND TAIL SETUP
 * =================================================================== */

static void test_fcb_setup(void) {
    TEST("FCB setup: simple filename") {
        CPMachine cpm;
        test_cpm_init(&cpm);
        cpm_setup_fcbs(&cpm, "FILE.TXT");

        /* FCB1 at 005Ch: drive=0, name="FILE    ", ext="TXT" */
        ASSERT(cpm.memory[CPM_FCB1] == 0);  /* Default drive */
        ASSERT(memcmp(&cpm.memory[CPM_FCB1 + 1], "FILE    ", 8) == 0);
        ASSERT(memcmp(&cpm.memory[CPM_FCB1 + 9], "TXT", 3) == 0);
        PASS();
    }

    TEST("FCB setup: drive prefix") {
        CPMachine cpm;
        test_cpm_init(&cpm);
        cpm_setup_fcbs(&cpm, "B:DATA.COM");

        ASSERT(cpm.memory[CPM_FCB1] == 2);  /* Drive B: */
        ASSERT(memcmp(&cpm.memory[CPM_FCB1 + 1], "DATA    ", 8) == 0);
        ASSERT(memcmp(&cpm.memory[CPM_FCB1 + 9], "COM", 3) == 0);
        PASS();
    }

    TEST("FCB setup: two arguments") {
        CPMachine cpm;
        test_cpm_init(&cpm);
        cpm_setup_fcbs(&cpm, "INPUT.DAT OUTPUT.LST");

        /* FCB1 */
        ASSERT(cpm.memory[CPM_FCB1] == 0);
        ASSERT(memcmp(&cpm.memory[CPM_FCB1 + 1], "INPUT   ", 8) == 0);
        ASSERT(memcmp(&cpm.memory[CPM_FCB1 + 9], "DAT", 3) == 0);

        /* FCB2 at 006Ch */
        ASSERT(cpm.memory[CPM_FCB2] == 0);
        ASSERT(memcmp(&cpm.memory[CPM_FCB2 + 1], "OUTPUT  ", 8) == 0);
        ASSERT(memcmp(&cpm.memory[CPM_FCB2 + 9], "LST", 3) == 0);
        PASS();
    }

    TEST("FCB setup: wildcard expansion") {
        CPMachine cpm;
        test_cpm_init(&cpm);
        cpm_setup_fcbs(&cpm, "*.COM");

        ASSERT(memcmp(&cpm.memory[CPM_FCB1 + 1], "????????", 8) == 0);
        ASSERT(memcmp(&cpm.memory[CPM_FCB1 + 9], "COM", 3) == 0);
        PASS();
    }

    TEST("FCB setup: command tail") {
        CPMachine cpm;
        test_cpm_init(&cpm);
        cpm_setup_fcbs(&cpm, "FILE1.TXT FILE2.TXT");

        /* Tail at 0080h should have the full argument string */
        uint8_t len = cpm.memory[0x0080];
        ASSERT(len == 19);
        ASSERT(memcmp(&cpm.memory[0x0081], "FILE1.TXT FILE2.TXT", 19) == 0);
        PASS();
    }

    TEST("FCB setup: no arguments") {
        CPMachine cpm;
        test_cpm_init(&cpm);
        cpm_setup_fcbs(&cpm, "");

        /* FCB1 should be blank */
        ASSERT(cpm.memory[CPM_FCB1] == 0);
        ASSERT(memcmp(&cpm.memory[CPM_FCB1 + 1], "        ", 8) == 0);
        ASSERT(memcmp(&cpm.memory[CPM_FCB1 + 9], "   ", 3) == 0);
        /* Tail length = 0 */
        ASSERT(cpm.memory[0x0080] == 0);
        PASS();
    }

    TEST("FCB setup: lowercase to uppercase") {
        CPMachine cpm;
        test_cpm_init(&cpm);
        cpm_setup_fcbs(&cpm, "hello.txt");

        ASSERT(memcmp(&cpm.memory[CPM_FCB1 + 1], "HELLO   ", 8) == 0);
        ASSERT(memcmp(&cpm.memory[CPM_FCB1 + 9], "TXT", 3) == 0);
        PASS();
    }
}

/* ===================================================================
 * TESTS: .COM FILE LOADING
 * =================================================================== */

static void test_com_loading(void) {
    TEST("Load nonexistent .COM returns -1") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        int ret = cpm_load_com(&cpm, "/nonexistent/path/file.com");
        ASSERT(ret == -1);
        PASS();
    }
}

/* ===================================================================
 * FILE SYSTEM TEST INFRASTRUCTURE
 *
 * File system tests need real host files. We create a temporary
 * directory, mount it as CP/M drive A:, create test files, and
 * clean up after each test.
 * =================================================================== */

static char test_dir[256];

/* Create a temporary directory for CP/M drive testing. */
static void create_test_dir(void) {
    snprintf(test_dir, sizeof(test_dir), "/tmp/cpm_test_XXXXXX");
    char *result = mkdtemp(test_dir);
    if (!result) {
        fprintf(stderr, "Failed to create test directory\n");
        exit(1);
    }
}

/* Create a test file in the temp directory with given content. */
static void create_test_file(const char *name, const void *data, int size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", test_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Cannot create %s\n", path); exit(1); }
    if (size > 0) fwrite(data, 1, size, f);
    fclose(f);
}

/* Check if a file exists in the temp directory. */
static int test_file_exists(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", test_dir, name);
    struct stat st;
    return stat(path, &st) == 0;
}

/* Read a test file's contents. Returns size, -1 on error. */
static int read_test_file(const char *name, void *buf, int bufsize) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", test_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int n = fread(buf, 1, bufsize, f);
    fclose(f);
    return n;
}

/* Remove all files from the temp directory and the directory itself. */
static void cleanup_test_dir(void) {
    DIR *dir = opendir(test_dir);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", test_dir, ent->d_name);
        unlink(path);
    }
    closedir(dir);
    rmdir(test_dir);
}

/* Set up an FCB in Z80 memory at the given address.
 * drive: 0=default, 1=A:, 2=B:, ...
 * name: up to 8 chars, ext: up to 3 chars (uppercase, space-padded). */
static void setup_fcb(CPMachine *cpm, uint16_t addr, int drive,
                      const char *name, const char *ext) {
    cpm->memory[addr] = drive;
    memset(&cpm->memory[addr + 1], ' ', 11);
    for (int i = 0; i < 8 && name[i]; i++)
        cpm->memory[addr + 1 + i] = name[i];
    for (int i = 0; i < 3 && ext[i]; i++)
        cpm->memory[addr + 9 + i] = ext[i];
    memset(&cpm->memory[addr + 12], 0, 24);  /* EX,S1,S2,RC,alloc,CR,R0,R1,R2 */
}

/* Standard Z80 program: LD C,func ; LD DE,fcb_addr ; CALL 5 ; HALT
 * This is the minimal program to invoke a BDOS function. */
static const uint8_t bdos_call_prog[] = {
    0x0E, 0x00,             /* LD C, <func>  -- byte [1] patched */
    0x11, 0x00, 0x02,       /* LD DE, 0200h  -- FCB address */
    0xCD, 0x05, 0x00,       /* CALL 0005h */
    0x76                    /* HALT */
};

/* Load and run a single BDOS call with the given function number.
 * FCB should already be set up at 0x0200. */
static void run_bdos_func(CPMachine *cpm, uint8_t func) {
    uint8_t prog[sizeof(bdos_call_prog)];
    memcpy(prog, bdos_call_prog, sizeof(prog));
    prog[1] = func;
    test_load_run(cpm, prog, sizeof(prog));
}

/* Initialize a CPMachine with test dir mounted as drive A:. */
static void test_cpm_init_fs(CPMachine *cpm) {
    test_cpm_init(cpm);
    cpm_mount(cpm, 0, test_dir);  /* Mount temp dir as A: */
}

/* ===================================================================
 * TESTS: FILE OPEN/CLOSE (BDOS 15, 16)
 * =================================================================== */

static void test_file_open_close(void) {
    create_test_dir();

    TEST("BDOS 15: Open existing file succeeds") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        /* Create a 256-byte test file. */
        char data[256];
        memset(data, 'A', 256);
        create_test_file("HELLO.TXT", data, 256);

        /* Set up FCB for HELLO.TXT at 0200h. */
        setup_fcb(&cpm, 0x0200, 0, "HELLO", "TXT");

        run_bdos_func(&cpm, 15);  /* Open file */

        ASSERT(cpm.cpu.a == 0x00);  /* Success */
        /* RC should be 2 (256 bytes = 2 records) */
        ASSERT(cpm.memory[0x0200 + 15] == 2);
        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("BDOS 15: Open non-existent file returns FFh") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);
        setup_fcb(&cpm, 0x0200, 0, "NOFILE", "COM");

        run_bdos_func(&cpm, 15);
        ASSERT(cpm.cpu.a == 0xFF);
        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("BDOS 16: Close file succeeds") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        create_test_file("TEST.DAT", "data", 4);
        setup_fcb(&cpm, 0x0200, 0, "TEST", "DAT");

        run_bdos_func(&cpm, 15);  /* Open */
        ASSERT(cpm.cpu.a == 0x00);

        run_bdos_func(&cpm, 16);  /* Close */
        ASSERT(cpm.cpu.a == 0x00);
        cpm_cleanup(&cpm);
        PASS();
    }

    cleanup_test_dir();
}

/* ===================================================================
 * TESTS: SEQUENTIAL READ/WRITE (BDOS 20, 21)
 * =================================================================== */

static void test_sequential_io(void) {
    create_test_dir();

    TEST("BDOS 20: Sequential read returns correct data") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        /* Create a file with known content: 128 bytes of 'X'. */
        char data[128];
        memset(data, 'X', 128);
        create_test_file("READ.DAT", data, 128);

        setup_fcb(&cpm, 0x0200, 0, "READ", "DAT");
        cpm.memory[0x0200 + 32] = 0;  /* CR = 0 */

        run_bdos_func(&cpm, 15);  /* Open */
        ASSERT(cpm.cpu.a == 0x00);

        run_bdos_func(&cpm, 20);  /* Read sequential */
        ASSERT(cpm.cpu.a == 0x00);

        /* DMA (0080h) should contain 128 'X's. */
        int match = 1;
        for (int i = 0; i < 128; i++)
            if (cpm.memory[0x0080 + i] != 'X') { match = 0; break; }
        ASSERT(match);

        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("BDOS 20: Sequential read at EOF returns 01h") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        /* Create a 128-byte file, read it, then try to read again. */
        char data[128];
        memset(data, 'Y', 128);
        create_test_file("SHORT.DAT", data, 128);

        setup_fcb(&cpm, 0x0200, 0, "SHORT", "DAT");
        cpm.memory[0x0200 + 32] = 0;

        run_bdos_func(&cpm, 15);  /* Open */
        run_bdos_func(&cpm, 20);  /* Read first record -- OK */
        ASSERT(cpm.cpu.a == 0x00);

        run_bdos_func(&cpm, 20);  /* Read second record -- EOF */
        ASSERT(cpm.cpu.a == 0x01);

        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("BDOS 20: Read multiple records sequentially") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        /* Create a 384-byte file (3 records). */
        char data[384];
        for (int i = 0; i < 384; i++) data[i] = (i / 128) + '1';
        create_test_file("MULTI.DAT", data, 384);

        setup_fcb(&cpm, 0x0200, 0, "MULTI", "DAT");
        cpm.memory[0x0200 + 32] = 0;

        run_bdos_func(&cpm, 15);  /* Open */

        /* Read 3 records: each should have different content. */
        run_bdos_func(&cpm, 20);
        ASSERT(cpm.cpu.a == 0x00);
        ASSERT(cpm.memory[0x0080] == '1');

        run_bdos_func(&cpm, 20);
        ASSERT(cpm.cpu.a == 0x00);
        ASSERT(cpm.memory[0x0080] == '2');

        run_bdos_func(&cpm, 20);
        ASSERT(cpm.cpu.a == 0x00);
        ASSERT(cpm.memory[0x0080] == '3');

        /* Fourth read should be EOF. */
        run_bdos_func(&cpm, 20);
        ASSERT(cpm.cpu.a == 0x01);

        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("BDOS 21: Sequential write creates file data") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        setup_fcb(&cpm, 0x0200, 0, "WRITE", "DAT");
        cpm.memory[0x0200 + 32] = 0;

        run_bdos_func(&cpm, 22);  /* Make file */
        ASSERT(cpm.cpu.a == 0x00);

        /* Write 128 bytes of 'Z' from DMA. */
        memset(&cpm.memory[0x0080], 'Z', 128);
        run_bdos_func(&cpm, 21);  /* Write sequential */
        ASSERT(cpm.cpu.a == 0x00);

        run_bdos_func(&cpm, 16);  /* Close */

        /* Verify the host file. */
        char buf[256];
        int n = read_test_file("WRITE.DAT", buf, sizeof(buf));
        ASSERT(n == 128);
        ASSERT(buf[0] == 'Z');
        ASSERT(buf[127] == 'Z');

        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("BDOS 21: Write then read back") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        setup_fcb(&cpm, 0x0200, 0, "WRTRD", "DAT");
        cpm.memory[0x0200 + 32] = 0;

        run_bdos_func(&cpm, 22);  /* Make file */

        /* Write two records. */
        memset(&cpm.memory[0x0080], 'A', 128);
        run_bdos_func(&cpm, 21);
        memset(&cpm.memory[0x0080], 'B', 128);
        run_bdos_func(&cpm, 21);

        run_bdos_func(&cpm, 16);  /* Close */

        /* Re-open and read back. */
        setup_fcb(&cpm, 0x0200, 0, "WRTRD", "DAT");
        cpm.memory[0x0200 + 32] = 0;
        run_bdos_func(&cpm, 15);  /* Open */

        run_bdos_func(&cpm, 20);  /* Read record 1 */
        ASSERT(cpm.cpu.a == 0x00);
        ASSERT(cpm.memory[0x0080] == 'A');

        run_bdos_func(&cpm, 20);  /* Read record 2 */
        ASSERT(cpm.cpu.a == 0x00);
        ASSERT(cpm.memory[0x0080] == 'B');

        cpm_cleanup(&cpm);
        PASS();
    }

    cleanup_test_dir();
}

/* ===================================================================
 * TESTS: MAKE FILE (BDOS 22)
 * =================================================================== */

static void test_make_file(void) {
    create_test_dir();

    TEST("BDOS 22: Make creates a new file") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        setup_fcb(&cpm, 0x0200, 0, "NEW", "TXT");
        run_bdos_func(&cpm, 22);

        ASSERT(cpm.cpu.a == 0x00);
        ASSERT(test_file_exists("NEW.TXT"));

        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("BDOS 22: Make on unmounted drive returns FFh") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        /* Drive B: not mounted. Use drive code 2 = B: */
        setup_fcb(&cpm, 0x0200, 2, "FILE", "DAT");
        run_bdos_func(&cpm, 22);

        ASSERT(cpm.cpu.a == 0xFF);
        cpm_cleanup(&cpm);
        PASS();
    }

    cleanup_test_dir();
}

/* ===================================================================
 * TESTS: SEARCH FIRST/NEXT (BDOS 17, 18)
 * =================================================================== */

static void test_search(void) {
    create_test_dir();

    TEST("BDOS 17: Search finds existing file") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        create_test_file("FIND.ME", "x", 1);

        setup_fcb(&cpm, 0x0200, 0, "FIND", "ME ");
        run_bdos_func(&cpm, 17);  /* Search first */

        ASSERT(cpm.cpu.a == 0x00);  /* Found at slot 0 */

        /* Check directory entry at DMA: name should be "FIND    ME " */
        ASSERT(memcmp(&cpm.memory[0x0080 + 1], "FIND    ", 8) == 0);
        ASSERT(memcmp(&cpm.memory[0x0080 + 9], "ME ", 3) == 0);

        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("BDOS 17: Search with wildcard *.COM") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        create_test_file("PROG1.COM", "x", 1);
        create_test_file("PROG2.COM", "x", 1);
        create_test_file("DATA.TXT", "x", 1);

        /* Wildcard pattern: ????????COM */
        setup_fcb(&cpm, 0x0200, 0, "????????", "COM");

        run_bdos_func(&cpm, 17);  /* Search first */
        ASSERT(cpm.cpu.a == 0x00);  /* Found one */

        /* Search next should find the other .COM file. */
        uint8_t next_prog[] = { 0x0E, 18, 0xCD, 0x05, 0x00, 0x76 };
        test_load_run(&cpm, next_prog, sizeof(next_prog));
        ASSERT(cpm.cpu.a == 0x00);  /* Found second */

        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("BDOS 17: Search for non-existent returns FFh") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        setup_fcb(&cpm, 0x0200, 0, "NOEXIST", "XXX");
        run_bdos_func(&cpm, 17);

        ASSERT(cpm.cpu.a == 0xFF);
        cpm_cleanup(&cpm);
        PASS();
    }

    cleanup_test_dir();
}

/* ===================================================================
 * TESTS: DELETE AND RENAME (BDOS 19, 23)
 * =================================================================== */

static void test_delete_rename(void) {
    create_test_dir();

    TEST("BDOS 19: Delete removes file") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        create_test_file("DELME.TXT", "data", 4);
        ASSERT(test_file_exists("DELME.TXT"));

        setup_fcb(&cpm, 0x0200, 0, "DELME", "TXT");
        run_bdos_func(&cpm, 19);

        ASSERT(cpm.cpu.a == 0x00);
        ASSERT(!test_file_exists("DELME.TXT"));
        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("BDOS 19: Delete non-existent returns FFh") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        setup_fcb(&cpm, 0x0200, 0, "GHOST", "XXX");
        run_bdos_func(&cpm, 19);

        ASSERT(cpm.cpu.a == 0xFF);
        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("BDOS 23: Rename file") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        create_test_file("OLD.NAM", "data", 4);

        /* FCB for rename: bytes 0-15 = old name, bytes 16-31 = new name.
         * Old: drive=0, name="OLD     NAM"
         * New: at offset 17-27, name="NEW     NAM" */
        setup_fcb(&cpm, 0x0200, 0, "OLD", "NAM");
        /* Set new name at offset 17-27. */
        memset(&cpm.memory[0x0200 + 16], 0, 16);
        memset(&cpm.memory[0x0200 + 17], ' ', 11);
        memcpy(&cpm.memory[0x0200 + 17], "NEW", 3);
        memcpy(&cpm.memory[0x0200 + 25], "NAM", 3);

        run_bdos_func(&cpm, 23);

        ASSERT(cpm.cpu.a == 0x00);
        ASSERT(!test_file_exists("OLD.NAM"));
        ASSERT(test_file_exists("NEW.NAM"));
        cpm_cleanup(&cpm);
        PASS();
    }

    cleanup_test_dir();
}

/* ===================================================================
 * TESTS: RANDOM I/O (BDOS 33, 34, 35, 36)
 * =================================================================== */

static void test_random_io(void) {
    create_test_dir();

    TEST("BDOS 34/33: Write then read random record") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        setup_fcb(&cpm, 0x0200, 0, "RANDOM", "DAT");
        run_bdos_func(&cpm, 22);  /* Make file */

        /* Write record 5 (offset 640). */
        cpm.memory[0x0200 + 33] = 5;  /* R0 */
        cpm.memory[0x0200 + 34] = 0;  /* R1 */
        cpm.memory[0x0200 + 35] = 0;  /* R2 */
        memset(&cpm.memory[0x0080], 'R', 128);
        run_bdos_func(&cpm, 34);  /* Write random */
        ASSERT(cpm.cpu.a == 0x00);

        /* Read record 5 back. */
        cpm.memory[0x0200 + 33] = 5;
        cpm.memory[0x0200 + 34] = 0;
        cpm.memory[0x0200 + 35] = 0;
        memset(&cpm.memory[0x0080], 0, 128);
        run_bdos_func(&cpm, 33);  /* Read random */
        ASSERT(cpm.cpu.a == 0x00);
        ASSERT(cpm.memory[0x0080] == 'R');
        ASSERT(cpm.memory[0x0080 + 127] == 'R');

        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("BDOS 33: Read random out of range (R2 non-zero)") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        create_test_file("SMALL.DAT", "x", 1);
        setup_fcb(&cpm, 0x0200, 0, "SMALL", "DAT");
        run_bdos_func(&cpm, 15);  /* Open */

        cpm.memory[0x0200 + 33] = 0;
        cpm.memory[0x0200 + 34] = 0;
        cpm.memory[0x0200 + 35] = 1;  /* R2=1 → out of range */
        run_bdos_func(&cpm, 33);
        ASSERT(cpm.cpu.a == 0x06);

        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("BDOS 35: File size returns correct record count") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        /* 300 bytes → ceil(300/128) = 3 records. */
        char data[300];
        memset(data, 'S', 300);
        create_test_file("SIZE.DAT", data, 300);

        setup_fcb(&cpm, 0x0200, 0, "SIZE", "DAT");
        run_bdos_func(&cpm, 35);  /* File size */

        ASSERT(cpm.memory[0x0200 + 33] == 3);  /* R0 = 3 records */
        ASSERT(cpm.memory[0x0200 + 34] == 0);
        ASSERT(cpm.memory[0x0200 + 35] == 0);

        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("BDOS 36: Set random record from sequential position") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        /* Set FCB to EX=2, S2=0, CR=5 → record = 2*128 + 5 = 261 */
        setup_fcb(&cpm, 0x0200, 0, "POS", "DAT");
        cpm.memory[0x0200 + 12] = 2;   /* EX */
        cpm.memory[0x0200 + 14] = 0;   /* S2 */
        cpm.memory[0x0200 + 32] = 5;   /* CR */

        run_bdos_func(&cpm, 36);  /* Set random record */

        /* Record 261 = 0x0105 */
        ASSERT(cpm.memory[0x0200 + 33] == 0x05);  /* R0 */
        ASSERT(cpm.memory[0x0200 + 34] == 0x01);  /* R1 */
        ASSERT(cpm.memory[0x0200 + 35] == 0x00);  /* R2 */

        cpm_cleanup(&cpm);
        PASS();
    }

    cleanup_test_dir();
}

/* ===================================================================
 * TESTS: DPB AND DRIVE INFO (BDOS 24, 27, 31)
 * =================================================================== */

static void test_drive_info(void) {
    TEST("BDOS 24: Login vector reflects mounted drives") {
        CPMachine cpm;
        test_cpm_init(&cpm);
        cpm_mount(&cpm, 0, "/tmp");  /* A: */
        cpm_mount(&cpm, 2, "/tmp");  /* C: */

        /* LD C,24 ; CALL 5 ; HALT */
        uint8_t prog[] = { 0x0E, 24, 0xCD, 0x05, 0x00, 0x76 };
        test_load_run(&cpm, prog, sizeof(prog));

        /* Expect bits 0 and 2 set: 0x05 */
        uint16_t vec = ((uint16_t)cpm.cpu.h << 8) | cpm.cpu.l;
        ASSERT(vec == 0x0005);
        PASS();
    }

    TEST("BDOS 31: DPB address is valid") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        uint8_t prog[] = { 0x0E, 31, 0xCD, 0x05, 0x00, 0x76 };
        test_load_run(&cpm, prog, sizeof(prog));

        uint16_t addr = ((uint16_t)cpm.cpu.h << 8) | cpm.cpu.l;
        ASSERT(addr == CPM_DPB_ADDR);
        /* Check BSH field (should be 5 for 4K blocks). */
        ASSERT(cpm.memory[addr + 2] == 5);
        PASS();
    }

    TEST("BDOS 27: Alloc vector address is valid") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        uint8_t prog[] = { 0x0E, 27, 0xCD, 0x05, 0x00, 0x76 };
        test_load_run(&cpm, prog, sizeof(prog));

        uint16_t addr = ((uint16_t)cpm.cpu.h << 8) | cpm.cpu.l;
        ASSERT(addr == CPM_ALV_ADDR);
        PASS();
    }
}

/* ===================================================================
 * TESTS: CLEANUP
 * =================================================================== */

static void test_cleanup(void) {
    create_test_dir();

    TEST("cpm_cleanup closes open files") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        create_test_file("OPEN.DAT", "data", 4);
        setup_fcb(&cpm, 0x0200, 0, "OPEN", "DAT");
        run_bdos_func(&cpm, 15);  /* Open */
        ASSERT(cpm.cpu.a == 0x00);

        /* File should be in the table. */
        int found = 0;
        for (int i = 0; i < CPM_MAX_FILES; i++)
            if (cpm.files[i].active) found++;
        ASSERT(found == 1);

        cpm_cleanup(&cpm);

        /* All slots should be free now. */
        found = 0;
        for (int i = 0; i < CPM_MAX_FILES; i++)
            if (cpm.files[i].active) found++;
        ASSERT(found == 0);
        PASS();
    }

    cleanup_test_dir();
}

/* ===================================================================
 * TESTS: CCP (Console Command Processor)
 *
 * These tests run the CCP with canned input and verify the captured
 * console output. The input sequence ends with EOF (-1 from con_in)
 * which causes the CCP to exit its loop.
 * =================================================================== */

static void test_ccp(void) {
    create_test_dir();

    TEST("CCP: Prompt shows current drive") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        /* Empty line + EOF: CCP prints prompt, reads empty, re-prompts,
         * then gets EOF and exits. */
        test_set_input("\r");
        cpm_ccp(&cpm);

        /* Output should contain "A>" prompt. */
        ASSERT(strstr(test_output, "A>") != NULL);
        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("CCP: Drive switch changes prompt") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);
        cpm_mount(&cpm, 1, test_dir);  /* Mount B: too */

        test_set_input("B:\r\r");  /* Switch to B:, then empty line */
        cpm_ccp(&cpm);

        /* Should see "B>" after the drive switch. */
        ASSERT(strstr(test_output, "B>") != NULL);
        ASSERT(cpm.current_drive == 1);
        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("CCP: DIR lists files") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        create_test_file("FOO.TXT", "x", 1);
        create_test_file("BAR.COM", "x", 1);

        test_set_input("DIR\r");
        cpm_ccp(&cpm);

        /* Should list both files. */
        ASSERT(strstr(test_output, "FOO") != NULL);
        ASSERT(strstr(test_output, "BAR") != NULL);
        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("CCP: DIR with wildcard pattern") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        create_test_file("PROG1.COM", "x", 1);
        create_test_file("PROG2.COM", "x", 1);
        create_test_file("DATA.TXT", "x", 1);

        test_set_input("DIR *.COM\r");
        cpm_ccp(&cpm);

        ASSERT(strstr(test_output, "PROG1") != NULL);
        ASSERT(strstr(test_output, "PROG2") != NULL);
        /* DATA.TXT should NOT appear. */
        ASSERT(strstr(test_output, "DATA") == NULL);
        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("CCP: DIR no files shows NO FILE") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        test_set_input("DIR *.ZZZ\r");
        cpm_ccp(&cpm);

        ASSERT(strstr(test_output, "NO FILE") != NULL);
        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("CCP: TYPE displays file content") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        create_test_file("HELLO.TXT", "Hello, World!\x1A", 14);

        test_set_input("TYPE HELLO.TXT\r");
        cpm_ccp(&cpm);

        ASSERT(strstr(test_output, "Hello, World!") != NULL);
        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("CCP: ERA deletes file") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        create_test_file("KILL.ME", "x", 1);
        ASSERT(test_file_exists("KILL.ME"));

        test_set_input("ERA KILL.ME\r");
        cpm_ccp(&cpm);

        ASSERT(!test_file_exists("KILL.ME"));
        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("CCP: REN renames file") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        create_test_file("OLD.NAM", "data", 4);

        test_set_input("REN NEW.NAM=OLD.NAM\r");
        cpm_ccp(&cpm);

        ASSERT(!test_file_exists("OLD.NAM"));
        ASSERT(test_file_exists("NEW.NAM"));
        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("CCP: USER changes user number") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        test_set_input("USER 7\r");
        cpm_ccp(&cpm);

        ASSERT(cpm.user_number == 7);
        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("CCP: Unknown command shows ? error") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        test_set_input("BOGUS\r");
        cpm_ccp(&cpm);

        ASSERT(strstr(test_output, "BOGUS?") != NULL);
        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("CCP: .COM file execution") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        /* Create a tiny .COM file that prints "HI" via BDOS 9 and returns.
         * Code at 0100h:
         *   0100: LD C,9         (0E 09)
         *   0102: LD DE,0109h    (11 09 01)  → points to "HI$"
         *   0105: CALL 0005h     (CD 05 00)
         *   0108: RET            (C9)
         *   0109: "HI$"          (48 49 24) */
        uint8_t com[] = {
            0x0E, 0x09,              /* LD C, 9 */
            0x11, 0x09, 0x01,        /* LD DE, 0109h */
            0xCD, 0x05, 0x00,        /* CALL 0005h */
            0xC9,                    /* RET */
            'H', 'I', '$'           /* message */
        };
        create_test_file("GREET.COM", com, sizeof(com));

        test_set_input("GREET\r");
        cpm_ccp(&cpm);

        ASSERT(strstr(test_output, "HI") != NULL);
        cpm_cleanup(&cpm);
        PASS();
    }

    TEST("CCP: SAVE writes TPA to file") {
        CPMachine cpm;
        test_cpm_init_fs(&cpm);

        /* Fill TPA with a known pattern. */
        memset(&cpm.memory[CPM_TPA_BASE], 0x42, 256);

        test_set_input("SAVE 1 DUMP.BIN\r");
        cpm_ccp(&cpm);

        /* Verify the saved file: 1 page = 256 bytes of 0x42. */
        char buf[512];
        int n = read_test_file("DUMP.BIN", buf, sizeof(buf));
        ASSERT(n == 256);
        ASSERT((uint8_t)buf[0] == 0x42);
        ASSERT((uint8_t)buf[255] == 0x42);
        cpm_cleanup(&cpm);
        PASS();
    }

    cleanup_test_dir();
}

/* ===================================================================
 * TESTS: BIOS JUMP TABLE AND DIRECT CALLS
 *
 * Programs like MBASIC bypass BDOS for console I/O by reading the
 * BIOS jump table at F200h. They extract the JP target addresses
 * and CALL them directly. These tests verify that:
 *   1. The jump table is correctly set up
 *   2. Direct BIOS calls work (CONST, CONIN, CONOUT)
 *   3. The MBASIC-style address extraction pattern works
 * =================================================================== */

static void test_bios(void) {
    TEST("BIOS: Jump table has 17 JP instructions") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* Each entry at F200+3*n should be JP (F240+3*n). */
        int ok = 1;
        for (int i = 0; i < CPM_BIOS_NFUNCS; i++) {
            uint16_t entry = CPM_BIOS_BASE + i * 3;
            uint16_t trap  = CPM_BIOS_TRAP_BASE + i * 3;
            if (cpm.memory[entry] != 0xC3) { ok = 0; break; }
            uint16_t target = cpm.memory[entry + 1] |
                              (cpm.memory[entry + 2] << 8);
            if (target != trap) { ok = 0; break; }
        }
        ASSERT(ok);
        PASS();
    }

    TEST("BIOS: Page zero warm boot points to BIOS WBOOT entry") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* 0001h-0002h should point to F203 (BIOS entry 1 = WBOOT). */
        uint16_t addr = cpm.memory[0x0001] | (cpm.memory[0x0002] << 8);
        ASSERT(addr == CPM_BIOS_BASE + 3);  /* F203 */

        /* F203 should be JP F243 (trap for WBOOT). */
        ASSERT(cpm.memory[0xF203] == 0xC3);
        uint16_t trap = cpm.memory[0xF204] | (cpm.memory[0xF205] << 8);
        ASSERT(trap == CPM_BIOS_TRAP_BASE + 3);  /* F243 */
        PASS();
    }

    TEST("BIOS CONST: Returns 00h when no input ready") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* Call BIOS CONST by extracting address from the jump table
         * like MBASIC does. CONST is entry 2 at F206, the JP target
         * is at F207/F208.
         *
         * Program:
         *   LD HL,(F207h)     ; read CONST handler address
         *   CALL (HL)         ; call it... but we can't do CALL (HL)!
         *
         * Actually Z80 doesn't have CALL (HL). Programs use JP (HL) or
         * push return addr + JP (HL). Simpler: just CALL the trap addr
         * directly since we know it. */
        uint16_t const_trap = CPM_BIOS_TRAP_BASE + 2 * 3;  /* F246 */
        uint8_t prog[] = {
            0xCD, const_trap & 0xFF, const_trap >> 8,  /* CALL F246h */
            0x76                                        /* HALT */
        };
        test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(cpm.cpu.a == 0x00);  /* No input ready */
        PASS();
    }

    TEST("BIOS CONST: Returns FFh when input ready") {
        CPMachine cpm;
        test_cpm_init(&cpm);
        test_set_input("X");

        uint16_t const_trap = CPM_BIOS_TRAP_BASE + 2 * 3;
        uint8_t prog[] = {
            0xCD, const_trap & 0xFF, const_trap >> 8,
            0x76
        };
        test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(cpm.cpu.a == 0xFF);
        PASS();
    }

    TEST("BIOS CONIN: Returns character in A") {
        CPMachine cpm;
        test_cpm_init(&cpm);
        test_set_input("Q");

        uint16_t conin_trap = CPM_BIOS_TRAP_BASE + 3 * 3;  /* F249 */
        uint8_t prog[] = {
            0xCD, conin_trap & 0xFF, conin_trap >> 8,
            0x76
        };
        test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(cpm.cpu.a == 'Q');
        PASS();
    }

    TEST("BIOS CONOUT: Outputs character from C register") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        uint16_t conout_trap = CPM_BIOS_TRAP_BASE + 4 * 3;  /* F24C */
        uint8_t prog[] = {
            0x0E, 'Z',                                        /* LD C, 'Z' */
            0xCD, conout_trap & 0xFF, conout_trap >> 8,       /* CALL F24Ch */
            0x76                                               /* HALT */
        };
        test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(test_output_len == 1);
        ASSERT(test_output[0] == 'Z');
        PASS();
    }

    TEST("BIOS: MBASIC-style address extraction works") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* Simulate what MBASIC does:
         *   1. Read (0001h) → F203 (WBOOT entry in BIOS table)
         *   2. Add 3 → F206 (CONST entry)
         *   3. Read target address from F207/F208
         *   4. CALL that address
         *
         * Program:
         *   LD HL,(0001h)   ; HL = F203
         *   LD BC,3
         *   ADD HL,BC       ; HL = F206 (CONST entry)
         *   INC HL          ; HL = F207 (low byte of JP target)
         *   LD E,(HL)       ; E = low byte of CONST trap addr
         *   INC HL
         *   LD D,(HL)       ; D = high byte of CONST trap addr
         *   ; Now DE = F246 (CONST trap)
         *   ; To call DE, we push return addr and JP (DE):
         *   ; But easier to just verify DE is correct
         *   HALT */
        uint8_t prog[] = {
            0x2A, 0x01, 0x00,       /* LD HL,(0001h) */
            0x01, 0x03, 0x00,       /* LD BC,3 */
            0x09,                   /* ADD HL,BC */
            0x23,                   /* INC HL */
            0x5E,                   /* LD E,(HL) */
            0x23,                   /* INC HL */
            0x56,                   /* LD D,(HL) */
            0x76                    /* HALT */
        };
        test_load_run(&cpm, prog, sizeof(prog));

        uint16_t de = (cpm.cpu.d << 8) | cpm.cpu.e;
        ASSERT(de == CPM_BIOS_TRAP_BASE + 2 * 3);  /* F246 = CONST trap */
        PASS();
    }

    TEST("BIOS: Direct WBOOT trap via extracted address") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        /* Call the WBOOT trap address directly (F243). This should
         * trigger a warm boot just like calling through 0000h. */
        uint16_t wboot_trap = CPM_BIOS_TRAP_BASE + 1 * 3;  /* F243 */
        uint8_t prog[] = {
            0xCD, wboot_trap & 0xFF, wboot_trap >> 8,
        };
        int wb = test_load_run(&cpm, prog, sizeof(prog));

        ASSERT(wb == 1);
        ASSERT(cpm.warm_boot == 1);
        PASS();
    }

    TEST("BIOS SELDSK: Returns DPH for mounted drive") {
        CPMachine cpm;
        test_cpm_init(&cpm);
        cpm_mount(&cpm, 0, "/tmp");

        uint16_t seldsk_trap = CPM_BIOS_TRAP_BASE + 9 * 3;  /* F25B */
        uint8_t prog[] = {
            0x0E, 0x00,                                        /* LD C, 0 (drive A) */
            0xCD, seldsk_trap & 0xFF, seldsk_trap >> 8,       /* CALL SELDSK */
            0x76                                               /* HALT */
        };
        test_load_run(&cpm, prog, sizeof(prog));

        uint16_t hl = (cpm.cpu.h << 8) | cpm.cpu.l;
        ASSERT(hl == CPM_DPH_ADDR);
        PASS();
    }

    TEST("BIOS SELDSK: Returns 0 for unmounted drive") {
        CPMachine cpm;
        test_cpm_init(&cpm);

        uint16_t seldsk_trap = CPM_BIOS_TRAP_BASE + 9 * 3;
        uint8_t prog[] = {
            0x0E, 0x05,                                        /* LD C, 5 (drive F, not mounted) */
            0xCD, seldsk_trap & 0xFF, seldsk_trap >> 8,
            0x76
        };
        test_load_run(&cpm, prog, sizeof(prog));

        uint16_t hl = (cpm.cpu.h << 8) | cpm.cpu.l;
        ASSERT(hl == 0);
        PASS();
    }
}

/* ===================================================================
 * MAIN
 * =================================================================== */

int main(void) {
    printf("CP/M Emulator Tests\n");
    printf("===================\n\n");

    printf("Page Zero:\n");
    test_page_zero();

    printf("\nConsole Output (BDOS 2, 9):\n");
    test_console_output();

    printf("\nSystem Calls (BDOS 0, 12):\n");
    test_system_calls();

    printf("\nDirect Console I/O (BDOS 6):\n");
    test_direct_io();

    printf("\nConsole Status (BDOS 11):\n");
    test_console_status();

    printf("\nIOBYTE (BDOS 7, 8):\n");
    test_iobyte();

    printf("\nDisk and DMA (BDOS 14, 25, 26):\n");
    test_disk_dma();

    printf("\nUser Number (BDOS 32):\n");
    test_user_number();

    printf("\nConsole Input (BDOS 1, 10):\n");
    test_console_input();

    printf("\nMultiple BDOS Calls:\n");
    test_multiple_calls();

    printf("\nFCB and Command Tail Setup:\n");
    test_fcb_setup();

    printf("\n.COM Loading:\n");
    test_com_loading();

    printf("\nFile Open/Close (BDOS 15, 16):\n");
    test_file_open_close();

    printf("\nSequential I/O (BDOS 20, 21):\n");
    test_sequential_io();

    printf("\nMake File (BDOS 22):\n");
    test_make_file();

    printf("\nSearch First/Next (BDOS 17, 18):\n");
    test_search();

    printf("\nDelete and Rename (BDOS 19, 23):\n");
    test_delete_rename();

    printf("\nRandom I/O (BDOS 33, 34, 35, 36):\n");
    test_random_io();

    printf("\nDrive Info (BDOS 24, 27, 31):\n");
    test_drive_info();

    printf("\nCleanup:\n");
    test_cleanup();

    printf("\nCCP (Console Command Processor):\n");
    test_ccp();

    printf("\nBIOS Jump Table:\n");
    test_bios();

    printf("\n---\n");
    printf("Total: %d  Passed: %d  Failed: %d\n", test_count, test_pass, test_fail);
    return test_fail ? 1 : 0;
}

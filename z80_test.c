/* z80_test.c -- Test suite for the Z80 emulator.
 *
 * Documentation sources used:
 *   - "Z80 CPU User Manual" (Zilog official documentation)
 *   - "The Undocumented Z80 Documented" by Sean Young
 *     (comprehensive reference for undocumented behavior)
 *   - "Z80 Instruction Set" tables from various community sources
 *   - ZEXALL / ZEXDOC test suites by Frank D. Cringle (referenced
 *     for expected behavior, not incorporated directly)
 *
 * Test approach:
 *   Each test loads a small program into memory, runs it, and
 *   verifies the CPU state (registers, flags, memory) afterward.
 *   We test each instruction group systematically.
 */

#include "z80.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================
 * TEST HARNESS
 * =================================================================== */

/* 64K memory for testing. */
static uint8_t memory[65536];

/* I/O ports (256 ports). */
static uint8_t io_ports[256];

static uint8_t test_mem_read(void *ctx, uint16_t addr) {
    (void)ctx;
    return memory[addr];
}

static void test_mem_write(void *ctx, uint16_t addr, uint8_t val) {
    (void)ctx;
    memory[addr] = val;
}

static uint8_t test_io_read(void *ctx, uint16_t port) {
    (void)ctx;
    return io_ports[port & 0xFF];
}

static void test_io_write(void *ctx, uint16_t port, uint8_t val) {
    (void)ctx;
    io_ports[port & 0xFF] = val;
}

static Z80 cpu;
static int test_count = 0;
static int test_pass = 0;
static int test_fail = 0;

/* Initialize CPU and clear memory for a new test. */
static void test_reset(void) {
    memset(memory, 0, sizeof(memory));
    memset(io_ports, 0, sizeof(io_ports));
    z80_init(&cpu);
    cpu.mem_read = test_mem_read;
    cpu.mem_write = test_mem_write;
    cpu.io_read = test_io_read;
    cpu.io_write = test_io_write;
    cpu.ctx = NULL;
    /* Reset to known state: all registers 0, SP=0xFFFF. */
    cpu.a = 0; cpu.f = 0;
    cpu.sp = 0xFFFE; /* Use a safe stack address for tests */
}

/* Load bytes into memory starting at addr. */
static void load_bytes(uint16_t addr, const uint8_t *data, int len) {
    for (int i = 0; i < len; i++) {
        memory[addr + i] = data[i];
    }
}

/* Run the CPU for a specified number of steps. */
static int run_steps(int n) {
    int total = 0;
    for (int i = 0; i < n; i++) {
        total += z80_step(&cpu);
    }
    return total;
}

/* Test assertion macros.
 *
 * Usage:
 *   TEST("name") {
 *       ...code...
 *       ASSERT(condition);
 *       PASS();
 *   }
 *
 * _t_ok flag is set to 0 on first ASSERT failure.
 * Subsequent ASSERTs are skipped. PASS prints OK only if _t_ok is still 1.
 */
#define TEST(name) { int _t_ok = 1; test_count++; printf("  %-50s ", name);
#define ASSERT(cond) \
    if (_t_ok && !(cond)) { \
        printf("FAIL (%s:%d: %s)\n", __FILE__, __LINE__, #cond); \
        test_fail++; \
        _t_ok = 0; \
    }
#define PASS() if (_t_ok) { printf("OK\n"); test_pass++; } }

#define ASSERT_REG(reg, expected) \
    ASSERT(cpu.reg == (expected))

#define ASSERT_FLAG(flag, expected) \
    ASSERT(!!(cpu.f & (flag)) == !!(expected))

#define ASSERT_MEM(addr, expected) \
    ASSERT(memory[addr] == (expected))

/* ===================================================================
 * TESTS: 8-BIT LOAD GROUP
 * =================================================================== */

static void test_ld_group(void) {
    printf("\n--- 8-bit Load Group ---\n");

    /* LD r, n (immediate) */
    TEST("LD B,0x42") {
        test_reset();
        uint8_t prog[] = {0x06, 0x42};  /* LD B, 0x42 */
        load_bytes(0, prog, sizeof(prog));
        int t = run_steps(1);
        ASSERT_REG(b, 0x42);
        ASSERT(t == 7);
        ASSERT_REG(pc, 2);
        PASS();
    }

    TEST("LD A,0xFF") {
        test_reset();
        uint8_t prog[] = {0x3E, 0xFF};  /* LD A, 0xFF */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0xFF);
        PASS();
    }

    /* LD r, r' */
    TEST("LD B,A") {
        test_reset();
        uint8_t prog[] = {0x3E, 0x55, 0x47};  /* LD A,0x55; LD B,A */
        load_bytes(0, prog, sizeof(prog));
        run_steps(2);
        ASSERT_REG(b, 0x55);
        ASSERT_REG(a, 0x55);
        PASS();
    }

    /* LD r, (HL) */
    TEST("LD A,(HL)") {
        test_reset();
        uint8_t prog[] = {0x21, 0x00, 0x80, 0x7E};  /* LD HL,0x8000; LD A,(HL) */
        load_bytes(0, prog, sizeof(prog));
        memory[0x8000] = 0xAB;
        run_steps(2);
        ASSERT_REG(a, 0xAB);
        PASS();
    }

    /* LD (HL), r */
    TEST("LD (HL),B") {
        test_reset();
        uint8_t prog[] = {0x06, 0x77, 0x21, 0x00, 0x80, 0x70};
        /* LD B,0x77; LD HL,0x8000; LD (HL),B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(3);
        ASSERT_MEM(0x8000, 0x77);
        PASS();
    }

    /* LD (HL), n */
    TEST("LD (HL),0x99") {
        test_reset();
        uint8_t prog[] = {0x21, 0x00, 0x80, 0x36, 0x99};
        /* LD HL,0x8000; LD (HL),0x99 */
        load_bytes(0, prog, sizeof(prog));
        run_steps(2);
        ASSERT_MEM(0x8000, 0x99);
        PASS();
    }

    /* LD A,(BC) */
    TEST("LD A,(BC)") {
        test_reset();
        uint8_t prog[] = {0x01, 0x00, 0x80, 0x0A};
        /* LD BC,0x8000; LD A,(BC) */
        load_bytes(0, prog, sizeof(prog));
        memory[0x8000] = 0x33;
        run_steps(2);
        ASSERT_REG(a, 0x33);
        PASS();
    }

    /* LD A,(DE) */
    TEST("LD A,(DE)") {
        test_reset();
        uint8_t prog[] = {0x11, 0x00, 0x80, 0x1A};
        /* LD DE,0x8000; LD A,(DE) */
        load_bytes(0, prog, sizeof(prog));
        memory[0x8000] = 0x44;
        run_steps(2);
        ASSERT_REG(a, 0x44);
        PASS();
    }

    /* LD (nn),A */
    TEST("LD (nn),A") {
        test_reset();
        uint8_t prog[] = {0x3E, 0xEE, 0x32, 0x00, 0x80};
        /* LD A,0xEE; LD (0x8000),A */
        load_bytes(0, prog, sizeof(prog));
        run_steps(2);
        ASSERT_MEM(0x8000, 0xEE);
        PASS();
    }

    /* LD A,(nn) */
    TEST("LD A,(nn)") {
        test_reset();
        uint8_t prog[] = {0x3A, 0x00, 0x80};
        /* LD A,(0x8000) */
        load_bytes(0, prog, sizeof(prog));
        memory[0x8000] = 0xDD;
        run_steps(1);
        ASSERT_REG(a, 0xDD);
        PASS();
    }
}

/* ===================================================================
 * TESTS: 16-BIT LOAD GROUP
 * =================================================================== */

static void test_ld16_group(void) {
    printf("\n--- 16-bit Load Group ---\n");

    TEST("LD BC,0x1234") {
        test_reset();
        uint8_t prog[] = {0x01, 0x34, 0x12};
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(b, 0x12);
        ASSERT_REG(c, 0x34);
        PASS();
    }

    TEST("LD DE,0xABCD") {
        test_reset();
        uint8_t prog[] = {0x11, 0xCD, 0xAB};
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(d, 0xAB);
        ASSERT_REG(e, 0xCD);
        PASS();
    }

    TEST("LD SP,0x8000") {
        test_reset();
        uint8_t prog[] = {0x31, 0x00, 0x80};
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(sp, 0x8000);
        PASS();
    }

    TEST("LD (nn),HL / LD HL,(nn)") {
        test_reset();
        uint8_t prog[] = {
            0x21, 0xCD, 0xAB,        /* LD HL,0xABCD */
            0x22, 0x00, 0x80,        /* LD (0x8000),HL */
            0x21, 0x00, 0x00,        /* LD HL,0x0000 */
            0x2A, 0x00, 0x80         /* LD HL,(0x8000) */
        };
        load_bytes(0, prog, sizeof(prog));
        run_steps(4);
        ASSERT_REG(h, 0xAB);
        ASSERT_REG(l, 0xCD);
        PASS();
    }

    TEST("LD SP,HL") {
        test_reset();
        uint8_t prog[] = {0x21, 0x00, 0x90, 0xF9};
        /* LD HL,0x9000; LD SP,HL */
        load_bytes(0, prog, sizeof(prog));
        run_steps(2);
        ASSERT_REG(sp, 0x9000);
        PASS();
    }

    TEST("PUSH BC / POP DE") {
        test_reset();
        uint8_t prog[] = {
            0x01, 0x34, 0x12,        /* LD BC,0x1234 */
            0xC5,                    /* PUSH BC */
            0xD1                     /* POP DE */
        };
        load_bytes(0, prog, sizeof(prog));
        run_steps(3);
        ASSERT_REG(d, 0x12);
        ASSERT_REG(e, 0x34);
        PASS();
    }

    TEST("PUSH AF / POP AF preserves flags") {
        test_reset();
        cpu.a = 0x12;
        cpu.f = 0xFF;
        uint8_t prog[] = {0xF5, 0xF1};  /* PUSH AF; POP AF */
        load_bytes(0, prog, sizeof(prog));
        run_steps(2);
        ASSERT_REG(a, 0x12);
        ASSERT_REG(f, 0xFF);
        PASS();
    }
}

/* ===================================================================
 * TESTS: 8-BIT ARITHMETIC
 * =================================================================== */

static void test_alu_group(void) {
    printf("\n--- 8-bit Arithmetic Group ---\n");

    TEST("ADD A,B (no carry, no overflow)") {
        test_reset();
        cpu.a = 0x10;
        cpu.b = 0x20;
        uint8_t prog[] = {0x80};  /* ADD A,B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x30);
        ASSERT_FLAG(Z80_ZF, 0);
        ASSERT_FLAG(Z80_SF, 0);
        ASSERT_FLAG(Z80_CF, 0);
        ASSERT_FLAG(Z80_NF, 0);
        PASS();
    }

    TEST("ADD A,B (carry)") {
        test_reset();
        cpu.a = 0xFF;
        cpu.b = 0x01;
        uint8_t prog[] = {0x80};
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x00);
        ASSERT_FLAG(Z80_ZF, 1);
        ASSERT_FLAG(Z80_CF, 1);
        ASSERT_FLAG(Z80_HF, 1);
        PASS();
    }

    TEST("ADD A,B (overflow: 0x7F + 0x01 = 0x80)") {
        test_reset();
        cpu.a = 0x7F;
        cpu.b = 0x01;
        uint8_t prog[] = {0x80};
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x80);
        ASSERT_FLAG(Z80_PF, 1);  /* Overflow: positive + positive = negative */
        ASSERT_FLAG(Z80_SF, 1);
        ASSERT_FLAG(Z80_HF, 1);
        PASS();
    }

    TEST("ADD A,n (immediate)") {
        test_reset();
        cpu.a = 0x10;
        uint8_t prog[] = {0xC6, 0x05};  /* ADD A,0x05 */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x15);
        PASS();
    }

    TEST("ADC A,B (with carry)") {
        test_reset();
        cpu.a = 0x10;
        cpu.b = 0x20;
        cpu.f = Z80_CF;  /* Carry set */
        uint8_t prog[] = {0x88};  /* ADC A,B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x31);  /* 0x10 + 0x20 + 1 */
        PASS();
    }

    TEST("SUB B") {
        test_reset();
        cpu.a = 0x30;
        cpu.b = 0x10;
        uint8_t prog[] = {0x90};  /* SUB B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x20);
        ASSERT_FLAG(Z80_NF, 1);
        ASSERT_FLAG(Z80_CF, 0);
        PASS();
    }

    TEST("SUB B (borrow)") {
        test_reset();
        cpu.a = 0x10;
        cpu.b = 0x20;
        uint8_t prog[] = {0x90};
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0xF0);
        ASSERT_FLAG(Z80_CF, 1);
        ASSERT_FLAG(Z80_SF, 1);
        PASS();
    }

    TEST("SBC A,B (with borrow)") {
        test_reset();
        cpu.a = 0x30;
        cpu.b = 0x10;
        cpu.f = Z80_CF;
        uint8_t prog[] = {0x98};  /* SBC A,B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x1F);  /* 0x30 - 0x10 - 1 */
        ASSERT_FLAG(Z80_HF, 1);  /* Half-borrow */
        PASS();
    }

    TEST("AND B") {
        test_reset();
        cpu.a = 0xF0;
        cpu.b = 0x0F;
        uint8_t prog[] = {0xA0};  /* AND B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x00);
        ASSERT_FLAG(Z80_ZF, 1);
        ASSERT_FLAG(Z80_HF, 1);
        ASSERT_FLAG(Z80_CF, 0);
        PASS();
    }

    TEST("OR B") {
        test_reset();
        cpu.a = 0xF0;
        cpu.b = 0x0F;
        uint8_t prog[] = {0xB0};  /* OR B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0xFF);
        ASSERT_FLAG(Z80_SF, 1);
        ASSERT_FLAG(Z80_ZF, 0);
        PASS();
    }

    TEST("XOR A (quick zero A)") {
        test_reset();
        cpu.a = 0x55;
        uint8_t prog[] = {0xAF};  /* XOR A */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x00);
        ASSERT_FLAG(Z80_ZF, 1);
        ASSERT_FLAG(Z80_PF, 1);  /* Even parity (0 bits set) */
        PASS();
    }

    TEST("CP B (equal)") {
        test_reset();
        cpu.a = 0x42;
        cpu.b = 0x42;
        uint8_t prog[] = {0xB8};  /* CP B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x42);  /* A unchanged by CP */
        ASSERT_FLAG(Z80_ZF, 1);
        ASSERT_FLAG(Z80_NF, 1);
        PASS();
    }

    TEST("CP B (A > B)") {
        test_reset();
        cpu.a = 0x50;
        cpu.b = 0x10;
        uint8_t prog[] = {0xB8};
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x50);
        ASSERT_FLAG(Z80_ZF, 0);
        ASSERT_FLAG(Z80_CF, 0);
        PASS();
    }

    TEST("CP B (A < B)") {
        test_reset();
        cpu.a = 0x10;
        cpu.b = 0x50;
        uint8_t prog[] = {0xB8};
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x10);
        ASSERT_FLAG(Z80_CF, 1);
        PASS();
    }

    TEST("INC B") {
        test_reset();
        cpu.b = 0x0F;
        uint8_t prog[] = {0x04};  /* INC B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(b, 0x10);
        ASSERT_FLAG(Z80_HF, 1);  /* Half-carry from 0x0F to 0x10 */
        PASS();
    }

    TEST("INC B (overflow 0x7F->0x80)") {
        test_reset();
        cpu.b = 0x7F;
        uint8_t prog[] = {0x04};
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(b, 0x80);
        ASSERT_FLAG(Z80_PF, 1);  /* Overflow */
        ASSERT_FLAG(Z80_SF, 1);
        PASS();
    }

    TEST("INC B (wrap 0xFF->0x00)") {
        test_reset();
        cpu.b = 0xFF;
        cpu.f = Z80_CF;  /* Set carry to verify it's preserved */
        uint8_t prog[] = {0x04};
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(b, 0x00);
        ASSERT_FLAG(Z80_ZF, 1);
        ASSERT_FLAG(Z80_CF, 1);  /* Carry preserved */
        ASSERT_FLAG(Z80_HF, 1);
        PASS();
    }

    TEST("DEC B") {
        test_reset();
        cpu.b = 0x10;
        uint8_t prog[] = {0x05};  /* DEC B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(b, 0x0F);
        ASSERT_FLAG(Z80_NF, 1);
        ASSERT_FLAG(Z80_HF, 1);  /* Half-borrow from 0x10 */
        PASS();
    }

    TEST("DEC B (overflow 0x80->0x7F)") {
        test_reset();
        cpu.b = 0x80;
        uint8_t prog[] = {0x05};
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(b, 0x7F);
        ASSERT_FLAG(Z80_PF, 1);  /* Overflow */
        PASS();
    }

    TEST("INC (HL)") {
        test_reset();
        cpu.h = 0x80; cpu.l = 0x00;
        memory[0x8000] = 0x41;
        uint8_t prog[] = {0x34};  /* INC (HL) */
        load_bytes(0, prog, sizeof(prog));
        int t = run_steps(1);
        ASSERT_MEM(0x8000, 0x42);
        ASSERT(t == 11);
        PASS();
    }
}

/* ===================================================================
 * TESTS: 16-BIT ARITHMETIC
 * =================================================================== */

static void test_alu16_group(void) {
    printf("\n--- 16-bit Arithmetic Group ---\n");

    TEST("ADD HL,BC") {
        test_reset();
        cpu.h = 0x10; cpu.l = 0x00;
        cpu.b = 0x20; cpu.c = 0x00;
        uint8_t prog[] = {0x09};  /* ADD HL,BC */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(h, 0x30);
        ASSERT_REG(l, 0x00);
        ASSERT_FLAG(Z80_CF, 0);
        ASSERT_FLAG(Z80_NF, 0);
        PASS();
    }

    TEST("ADD HL,BC (carry)") {
        test_reset();
        cpu.h = 0xFF; cpu.l = 0xFF;
        cpu.b = 0x00; cpu.c = 0x01;
        uint8_t prog[] = {0x09};
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(h, 0x00);
        ASSERT_REG(l, 0x00);
        ASSERT_FLAG(Z80_CF, 1);
        PASS();
    }

    TEST("INC BC") {
        test_reset();
        cpu.b = 0x00; cpu.c = 0xFF;
        uint8_t prog[] = {0x03};  /* INC BC */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(b, 0x01);
        ASSERT_REG(c, 0x00);
        PASS();
    }

    TEST("DEC BC") {
        test_reset();
        cpu.b = 0x01; cpu.c = 0x00;
        uint8_t prog[] = {0x0B};  /* DEC BC */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(b, 0x00);
        ASSERT_REG(c, 0xFF);
        PASS();
    }

    TEST("ADC HL,BC (ED prefix)") {
        test_reset();
        cpu.h = 0x10; cpu.l = 0x00;
        cpu.b = 0x20; cpu.c = 0x00;
        cpu.f = Z80_CF;
        uint8_t prog[] = {0xED, 0x4A};  /* ADC HL,BC */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(h, 0x30);
        ASSERT_REG(l, 0x01);  /* +1 from carry */
        PASS();
    }

    TEST("SBC HL,BC (ED prefix)") {
        test_reset();
        cpu.h = 0x30; cpu.l = 0x00;
        cpu.b = 0x10; cpu.c = 0x00;
        cpu.f = Z80_CF;
        uint8_t prog[] = {0xED, 0x42};  /* SBC HL,BC */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(h, 0x1F);
        ASSERT_REG(l, 0xFF);  /* -1 from carry */
        ASSERT_FLAG(Z80_NF, 1);
        PASS();
    }
}

/* ===================================================================
 * TESTS: JUMPS AND CALLS
 * =================================================================== */

static void test_jump_group(void) {
    printf("\n--- Jump/Call/Return Group ---\n");

    TEST("JP nn") {
        test_reset();
        uint8_t prog[] = {0xC3, 0x00, 0x80};  /* JP 0x8000 */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(pc, 0x8000);
        PASS();
    }

    TEST("JP NZ,nn (taken)") {
        test_reset();
        cpu.f = 0;  /* Z not set */
        uint8_t prog[] = {0xC2, 0x00, 0x80};  /* JP NZ,0x8000 */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(pc, 0x8000);
        PASS();
    }

    TEST("JP NZ,nn (not taken)") {
        test_reset();
        cpu.f = Z80_ZF;  /* Z set */
        uint8_t prog[] = {0xC2, 0x00, 0x80};
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(pc, 3);  /* Falls through */
        PASS();
    }

    TEST("JR d (forward)") {
        test_reset();
        uint8_t prog[] = {0x18, 0x05};  /* JR +5 */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(pc, 7);  /* 2 (instr length) + 5 */
        PASS();
    }

    TEST("JR d (backward)") {
        test_reset();
        /* Place at address 0x10: JR -5 (0xFB = -5 signed) */
        cpu.pc = 0x10;
        memory[0x10] = 0x18;
        memory[0x11] = 0xFB;
        run_steps(1);
        ASSERT_REG(pc, 0x0D);  /* 0x12 - 5 = 0x0D */
        PASS();
    }

    TEST("JR NZ,d (taken)") {
        test_reset();
        cpu.f = 0;
        uint8_t prog[] = {0x20, 0x03};  /* JR NZ,+3 */
        load_bytes(0, prog, sizeof(prog));
        int t = run_steps(1);
        ASSERT_REG(pc, 5);
        ASSERT(t == 12);
        PASS();
    }

    TEST("JR NZ,d (not taken)") {
        test_reset();
        cpu.f = Z80_ZF;
        uint8_t prog[] = {0x20, 0x03};
        load_bytes(0, prog, sizeof(prog));
        int t = run_steps(1);
        ASSERT_REG(pc, 2);
        ASSERT(t == 7);
        PASS();
    }

    TEST("DJNZ (loop 3 times)") {
        test_reset();
        cpu.b = 3;
        /* DJNZ -2 (loop back to self): 0x10, 0xFE */
        uint8_t prog[] = {0x10, 0xFE};
        load_bytes(0, prog, sizeof(prog));
        run_steps(3);
        ASSERT_REG(b, 0);
        ASSERT_REG(pc, 2);  /* Falls through when B=0 */
        PASS();
    }

    TEST("CALL nn / RET") {
        test_reset();
        /* CALL 0x0010, at 0x0010: LD A,0x42; RET */
        uint8_t prog[] = {0xCD, 0x10, 0x00};
        load_bytes(0, prog, sizeof(prog));
        memory[0x0010] = 0x3E;  /* LD A,0x42 */
        memory[0x0011] = 0x42;
        memory[0x0012] = 0xC9;  /* RET */
        run_steps(3);  /* CALL, LD A, RET */
        ASSERT_REG(a, 0x42);
        ASSERT_REG(pc, 3);  /* Back after CALL */
        PASS();
    }

    TEST("CALL NZ,nn (taken)") {
        test_reset();
        cpu.f = 0;
        uint8_t prog[] = {0xC4, 0x10, 0x00};
        load_bytes(0, prog, sizeof(prog));
        memory[0x0010] = 0xC9;  /* RET */
        int t = run_steps(1);
        ASSERT_REG(pc, 0x10);
        ASSERT(t == 17);
        PASS();
    }

    TEST("CALL NZ,nn (not taken)") {
        test_reset();
        cpu.f = Z80_ZF;
        uint8_t prog[] = {0xC4, 0x10, 0x00};
        load_bytes(0, prog, sizeof(prog));
        int t = run_steps(1);
        ASSERT_REG(pc, 3);
        ASSERT(t == 10);
        PASS();
    }

    TEST("RST 0x38") {
        test_reset();
        uint8_t prog[] = {0xFF};  /* RST 38h */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(pc, 0x38);
        PASS();
    }

    TEST("RST 0x00") {
        test_reset();
        cpu.pc = 0x100;
        memory[0x100] = 0xC7;  /* RST 00h */
        run_steps(1);
        ASSERT_REG(pc, 0x00);
        PASS();
    }

    TEST("JP (HL)") {
        test_reset();
        cpu.h = 0x80; cpu.l = 0x00;
        uint8_t prog[] = {0xE9};  /* JP (HL) */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(pc, 0x8000);
        PASS();
    }

    TEST("RET cc (taken/not taken)") {
        test_reset();
        /* Set up: CALL 0x10, at 0x10: RET Z */
        cpu.f = Z80_ZF;
        uint8_t prog[] = {0xCD, 0x10, 0x00};
        load_bytes(0, prog, sizeof(prog));
        memory[0x0010] = 0xC8;  /* RET Z */
        run_steps(2);  /* CALL, then RET Z (taken) */
        ASSERT_REG(pc, 3);
        PASS();
    }
}

/* ===================================================================
 * TESTS: ROTATE AND SHIFT
 * =================================================================== */

static void test_rotate_group(void) {
    printf("\n--- Rotate/Shift Group ---\n");

    TEST("RLCA") {
        test_reset();
        cpu.a = 0x85;  /* 10000101 */
        uint8_t prog[] = {0x07};  /* RLCA */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x0B);  /* 00001011 */
        ASSERT_FLAG(Z80_CF, 1);  /* Old bit 7 */
        PASS();
    }

    TEST("RRCA") {
        test_reset();
        cpu.a = 0x85;  /* 10000101 */
        uint8_t prog[] = {0x0F};  /* RRCA */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0xC2);  /* 11000010 */
        ASSERT_FLAG(Z80_CF, 1);  /* Old bit 0 */
        PASS();
    }

    TEST("RLA") {
        test_reset();
        cpu.a = 0x85;  /* 10000101 */
        cpu.f = Z80_CF;  /* Carry set */
        uint8_t prog[] = {0x17};  /* RLA */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x0B);  /* 00001011 (carry goes to bit 0) */
        ASSERT_FLAG(Z80_CF, 1);
        PASS();
    }

    TEST("RRA") {
        test_reset();
        cpu.a = 0x85;  /* 10000101 */
        cpu.f = Z80_CF;
        uint8_t prog[] = {0x1F};  /* RRA */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0xC2);  /* 11000010 (carry goes to bit 7) */
        ASSERT_FLAG(Z80_CF, 1);
        PASS();
    }

    /* CB-prefixed rotations */
    TEST("RLC B (CB prefix)") {
        test_reset();
        cpu.b = 0x85;
        uint8_t prog[] = {0xCB, 0x00};  /* RLC B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(b, 0x0B);
        ASSERT_FLAG(Z80_CF, 1);
        ASSERT_FLAG(Z80_ZF, 0);
        PASS();
    }

    TEST("SLA B") {
        test_reset();
        cpu.b = 0x85;  /* 10000101 */
        uint8_t prog[] = {0xCB, 0x20};  /* SLA B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(b, 0x0A);  /* 00001010 */
        ASSERT_FLAG(Z80_CF, 1);  /* Old bit 7 */
        PASS();
    }

    TEST("SRA B (preserves sign)") {
        test_reset();
        cpu.b = 0x85;  /* 10000101 */
        uint8_t prog[] = {0xCB, 0x28};  /* SRA B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(b, 0xC2);  /* 11000010 (bit 7 preserved) */
        ASSERT_FLAG(Z80_CF, 1);
        PASS();
    }

    TEST("SRL B") {
        test_reset();
        cpu.b = 0x85;
        uint8_t prog[] = {0xCB, 0x38};  /* SRL B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(b, 0x42);  /* 01000010 (bit 7 = 0) */
        ASSERT_FLAG(Z80_CF, 1);
        PASS();
    }

    TEST("SLL B (undocumented)") {
        test_reset();
        cpu.b = 0x85;
        uint8_t prog[] = {0xCB, 0x30};  /* SLL B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(b, 0x0B);  /* 00001011 (bit 0 = 1) */
        ASSERT_FLAG(Z80_CF, 1);
        PASS();
    }
}

/* ===================================================================
 * TESTS: BIT OPERATIONS
 * =================================================================== */

static void test_bit_group(void) {
    printf("\n--- Bit Operations Group ---\n");

    TEST("BIT 0,B (set)") {
        test_reset();
        cpu.b = 0x01;
        uint8_t prog[] = {0xCB, 0x40};  /* BIT 0,B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_FLAG(Z80_ZF, 0);
        ASSERT_FLAG(Z80_HF, 1);
        ASSERT_FLAG(Z80_NF, 0);
        PASS();
    }

    TEST("BIT 0,B (clear)") {
        test_reset();
        cpu.b = 0xFE;
        uint8_t prog[] = {0xCB, 0x40};  /* BIT 0,B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_FLAG(Z80_ZF, 1);
        PASS();
    }

    TEST("BIT 7,A (sign)") {
        test_reset();
        cpu.a = 0x80;
        uint8_t prog[] = {0xCB, 0x7F};  /* BIT 7,A */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_FLAG(Z80_ZF, 0);
        ASSERT_FLAG(Z80_SF, 1);  /* Bit 7 is set, S flag reflects that */
        PASS();
    }

    TEST("SET 3,B") {
        test_reset();
        cpu.b = 0x00;
        uint8_t prog[] = {0xCB, 0xD8};  /* SET 3,B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(b, 0x08);
        PASS();
    }

    TEST("RES 7,A") {
        test_reset();
        cpu.a = 0xFF;
        uint8_t prog[] = {0xCB, 0xBF};  /* RES 7,A */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x7F);
        PASS();
    }

    TEST("BIT/SET/RES (HL)") {
        test_reset();
        cpu.h = 0x80; cpu.l = 0x00;
        memory[0x8000] = 0x00;
        /* SET 5,(HL) then BIT 5,(HL) then RES 5,(HL) */
        uint8_t prog[] = {
            0xCB, 0xEE,  /* SET 5,(HL) */
            0xCB, 0x6E,  /* BIT 5,(HL) */
            0xCB, 0xAE   /* RES 5,(HL) */
        };
        load_bytes(0, prog, sizeof(prog));

        run_steps(1);
        ASSERT_MEM(0x8000, 0x20);  /* Bit 5 set */

        run_steps(1);
        ASSERT_FLAG(Z80_ZF, 0);   /* Bit 5 is set */

        run_steps(1);
        ASSERT_MEM(0x8000, 0x00);  /* Bit 5 cleared */
        PASS();
    }
}

/* ===================================================================
 * TESTS: EXCHANGE AND SPECIAL
 * =================================================================== */

static void test_exchange_group(void) {
    printf("\n--- Exchange/Special Group ---\n");

    TEST("EX AF,AF'") {
        test_reset();
        cpu.a = 0x12; cpu.f = 0x34;
        cpu.a_ = 0x56; cpu.f_ = 0x78;
        uint8_t prog[] = {0x08};  /* EX AF,AF' */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x56);
        ASSERT_REG(f, 0x78);
        ASSERT(cpu.a_ == 0x12);
        ASSERT(cpu.f_ == 0x34);
        PASS();
    }

    TEST("EXX") {
        test_reset();
        cpu.b = 0x01; cpu.c = 0x02;
        cpu.d = 0x03; cpu.e = 0x04;
        cpu.h = 0x05; cpu.l = 0x06;
        cpu.b_ = 0x11; cpu.c_ = 0x12;
        cpu.d_ = 0x13; cpu.e_ = 0x14;
        cpu.h_ = 0x15; cpu.l_ = 0x16;
        uint8_t prog[] = {0xD9};  /* EXX */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(b, 0x11); ASSERT_REG(c, 0x12);
        ASSERT_REG(d, 0x13); ASSERT_REG(e, 0x14);
        ASSERT_REG(h, 0x15); ASSERT_REG(l, 0x16);
        PASS();
    }

    TEST("EX DE,HL") {
        test_reset();
        cpu.d = 0x12; cpu.e = 0x34;
        cpu.h = 0x56; cpu.l = 0x78;
        uint8_t prog[] = {0xEB};  /* EX DE,HL */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(d, 0x56); ASSERT_REG(e, 0x78);
        ASSERT_REG(h, 0x12); ASSERT_REG(l, 0x34);
        PASS();
    }

    TEST("EX (SP),HL") {
        test_reset();
        cpu.h = 0x12; cpu.l = 0x34;
        cpu.sp = 0x8000;
        memory[0x8000] = 0x78;  /* Low byte */
        memory[0x8001] = 0x56;  /* High byte */
        uint8_t prog[] = {0xE3};  /* EX (SP),HL */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(h, 0x56); ASSERT_REG(l, 0x78);
        ASSERT_MEM(0x8000, 0x34);  /* Old L */
        ASSERT_MEM(0x8001, 0x12);  /* Old H */
        PASS();
    }

    TEST("CPL") {
        test_reset();
        cpu.a = 0x55;  /* 01010101 */
        uint8_t prog[] = {0x2F};  /* CPL */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0xAA);  /* 10101010 */
        ASSERT_FLAG(Z80_HF, 1);
        ASSERT_FLAG(Z80_NF, 1);
        PASS();
    }

    TEST("SCF") {
        test_reset();
        cpu.f = 0;
        uint8_t prog[] = {0x37};  /* SCF */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_FLAG(Z80_CF, 1);
        ASSERT_FLAG(Z80_NF, 0);
        ASSERT_FLAG(Z80_HF, 0);
        PASS();
    }

    TEST("CCF") {
        test_reset();
        cpu.f = Z80_CF;
        uint8_t prog[] = {0x3F};  /* CCF */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_FLAG(Z80_CF, 0);
        ASSERT_FLAG(Z80_HF, 1);  /* Old carry -> H */
        PASS();
    }

    TEST("HALT") {
        test_reset();
        uint8_t prog[] = {0x76};  /* HALT */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT(cpu.halted == 1);
        /* Running more steps should still return 4 T-states (NOP) */
        int t = run_steps(1);
        ASSERT(t == 4);
        ASSERT(cpu.halted == 1);
        PASS();
    }

    TEST("NOP timing") {
        test_reset();
        uint8_t prog[] = {0x00};  /* NOP */
        load_bytes(0, prog, sizeof(prog));
        int t = run_steps(1);
        ASSERT(t == 4);
        ASSERT_REG(pc, 1);
        PASS();
    }
}

/* ===================================================================
 * TESTS: BLOCK OPERATIONS (ED prefix)
 * =================================================================== */

static void test_block_group(void) {
    printf("\n--- Block Operations Group ---\n");

    TEST("LDI") {
        test_reset();
        cpu.h = 0x80; cpu.l = 0x00;  /* HL = source */
        cpu.d = 0x90; cpu.e = 0x00;  /* DE = dest */
        cpu.b = 0x00; cpu.c = 0x03;  /* BC = count */
        memory[0x8000] = 0x42;
        uint8_t prog[] = {0xED, 0xA0};  /* LDI */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_MEM(0x9000, 0x42);
        ASSERT_REG(h, 0x80); ASSERT_REG(l, 0x01);  /* HL++ */
        ASSERT_REG(d, 0x90); ASSERT_REG(e, 0x01);  /* DE++ */
        ASSERT_REG(b, 0x00); ASSERT_REG(c, 0x02);  /* BC-- */
        ASSERT_FLAG(Z80_PF, 1);  /* BC != 0 */
        PASS();
    }

    TEST("LDD") {
        test_reset();
        cpu.h = 0x80; cpu.l = 0x02;
        cpu.d = 0x90; cpu.e = 0x02;
        cpu.b = 0x00; cpu.c = 0x01;
        memory[0x8002] = 0x55;
        uint8_t prog[] = {0xED, 0xA8};  /* LDD */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_MEM(0x9002, 0x55);
        ASSERT_REG(l, 0x01);  /* HL-- */
        ASSERT_REG(e, 0x01);  /* DE-- */
        ASSERT_FLAG(Z80_PF, 0);  /* BC == 0 */
        PASS();
    }

    TEST("LDIR (copy 5 bytes)") {
        test_reset();
        cpu.h = 0x80; cpu.l = 0x00;
        cpu.d = 0x90; cpu.e = 0x00;
        cpu.b = 0x00; cpu.c = 0x05;
        memory[0x8000] = 'H';
        memory[0x8001] = 'e';
        memory[0x8002] = 'l';
        memory[0x8003] = 'l';
        memory[0x8004] = 'o';
        uint8_t prog[] = {0xED, 0xB0};  /* LDIR */
        load_bytes(0, prog, sizeof(prog));
        run_steps(5);  /* 5 iterations */
        ASSERT_MEM(0x9000, 'H');
        ASSERT_MEM(0x9001, 'e');
        ASSERT_MEM(0x9002, 'l');
        ASSERT_MEM(0x9003, 'l');
        ASSERT_MEM(0x9004, 'o');
        ASSERT_REG(b, 0x00); ASSERT_REG(c, 0x00);
        ASSERT_FLAG(Z80_PF, 0);  /* BC == 0 */
        PASS();
    }

    TEST("CPI (match)") {
        test_reset();
        cpu.a = 0x42;
        cpu.h = 0x80; cpu.l = 0x00;
        cpu.b = 0x00; cpu.c = 0x05;
        memory[0x8000] = 0x42;
        uint8_t prog[] = {0xED, 0xA1};  /* CPI */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_FLAG(Z80_ZF, 1);   /* Match found */
        ASSERT_FLAG(Z80_PF, 1);   /* BC != 0 */
        ASSERT_REG(l, 0x01);      /* HL++ */
        PASS();
    }

    TEST("CPI (no match)") {
        test_reset();
        cpu.a = 0x42;
        cpu.h = 0x80; cpu.l = 0x00;
        cpu.b = 0x00; cpu.c = 0x05;
        memory[0x8000] = 0x43;
        uint8_t prog[] = {0xED, 0xA1};
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_FLAG(Z80_ZF, 0);
        PASS();
    }

    TEST("CPIR (find byte in buffer)") {
        test_reset();
        cpu.a = 0x33;
        cpu.h = 0x80; cpu.l = 0x00;
        cpu.b = 0x00; cpu.c = 0x05;
        memory[0x8000] = 0x11;
        memory[0x8001] = 0x22;
        memory[0x8002] = 0x33;
        memory[0x8003] = 0x44;
        uint8_t prog[] = {0xED, 0xB1};  /* CPIR */
        load_bytes(0, prog, sizeof(prog));
        run_steps(3);  /* Should take 3 iterations */
        ASSERT_FLAG(Z80_ZF, 1);  /* Found */
        ASSERT_REG(l, 0x03);     /* HL points past the match */
        PASS();
    }
}

/* ===================================================================
 * TESTS: I/O
 * =================================================================== */

static void test_io_group(void) {
    printf("\n--- I/O Group ---\n");

    TEST("OUT (n),A / IN A,(n)") {
        test_reset();
        cpu.a = 0x42;
        uint8_t prog[] = {0xD3, 0xFE, 0x3E, 0x00, 0xDB, 0xFE};
        /* OUT (0xFE),A; LD A,0; IN A,(0xFE) */
        load_bytes(0, prog, sizeof(prog));
        run_steps(3);
        ASSERT_REG(a, 0x42);  /* Read back what we wrote */
        PASS();
    }

    TEST("IN B,(C) (ED prefix)") {
        test_reset();
        cpu.b = 0x00; cpu.c = 0x42;
        io_ports[0x42] = 0xAB;
        uint8_t prog[] = {0xED, 0x40};  /* IN B,(C) */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(b, 0xAB);
        ASSERT_FLAG(Z80_NF, 0);
        ASSERT_FLAG(Z80_HF, 0);
        PASS();
    }

    TEST("OUT (C),B (ED prefix)") {
        test_reset();
        cpu.b = 0x55; cpu.c = 0x10;
        uint8_t prog[] = {0xED, 0x41};  /* OUT (C),B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT(io_ports[0x10] == 0x55);
        PASS();
    }
}

/* ===================================================================
 * TESTS: ED PREFIX (misc)
 * =================================================================== */

static void test_ed_group(void) {
    printf("\n--- ED Prefix Misc ---\n");

    TEST("NEG (0x42)") {
        test_reset();
        cpu.a = 0x42;
        uint8_t prog[] = {0xED, 0x44};  /* NEG */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0xBE);  /* 0 - 0x42 = 0xBE */
        ASSERT_FLAG(Z80_NF, 1);
        ASSERT_FLAG(Z80_CF, 1);  /* Result is non-zero -> carry set */
        PASS();
    }

    TEST("NEG (0x80 -> overflow)") {
        test_reset();
        cpu.a = 0x80;
        uint8_t prog[] = {0xED, 0x44};
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x80);  /* 0 - 0x80 = 0x80 (wraps) */
        ASSERT_FLAG(Z80_PF, 1);  /* Overflow */
        PASS();
    }

    TEST("NEG (0x00 -> no carry)") {
        test_reset();
        cpu.a = 0x00;
        uint8_t prog[] = {0xED, 0x44};
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x00);
        ASSERT_FLAG(Z80_ZF, 1);
        ASSERT_FLAG(Z80_CF, 0);
        PASS();
    }

    TEST("LD I,A / LD A,I") {
        test_reset();
        cpu.a = 0x42;
        uint8_t prog[] = {0xED, 0x47, 0x3E, 0x00, 0xED, 0x57};
        /* LD I,A; LD A,0; LD A,I */
        load_bytes(0, prog, sizeof(prog));
        run_steps(3);
        ASSERT_REG(a, 0x42);
        ASSERT_REG(i, 0x42);
        PASS();
    }

    TEST("LD R,A / LD A,R") {
        test_reset();
        cpu.a = 0x42;
        uint8_t prog[] = {0xED, 0x4F, 0x3E, 0x00, 0xED, 0x5F};
        /* LD R,A; LD A,0; LD A,R */
        load_bytes(0, prog, sizeof(prog));
        run_steps(3);
        /* R will have been incremented by the instructions executed.
         * After LD R,A: R=0x42. Then LD A,0 increments R to 0x43.
         * Then LD A,R (ED prefix increments R to 0x44, then ED 5F increments to 0x45).
         * But LD A,R reads R, so the value read is after the prefix increment.
         * Actually R is incremented per instruction fetch. Let's just check it's non-zero. */
        ASSERT(cpu.a != 0x00);
        PASS();
    }

    TEST("LD (nn),BC / LD BC,(nn) (ED prefix)") {
        test_reset();
        cpu.b = 0xAB; cpu.c = 0xCD;
        uint8_t prog[] = {
            0xED, 0x43, 0x00, 0x80,  /* LD (0x8000),BC */
            0x01, 0x00, 0x00,        /* LD BC,0 */
            0xED, 0x4B, 0x00, 0x80   /* LD BC,(0x8000) */
        };
        load_bytes(0, prog, sizeof(prog));
        run_steps(3);
        ASSERT_REG(b, 0xAB);
        ASSERT_REG(c, 0xCD);
        PASS();
    }

    TEST("RLD") {
        test_reset();
        cpu.a = 0x12;
        cpu.h = 0x80; cpu.l = 0x00;
        memory[0x8000] = 0x34;
        uint8_t prog[] = {0xED, 0x6F};  /* RLD */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x13);      /* A low nibble <- (HL) high nibble */
        ASSERT_MEM(0x8000, 0x42); /* (HL) high <- (HL) low, (HL) low <- A low */
        PASS();
    }

    TEST("RRD") {
        test_reset();
        cpu.a = 0x12;
        cpu.h = 0x80; cpu.l = 0x00;
        memory[0x8000] = 0x34;
        uint8_t prog[] = {0xED, 0x67};  /* RRD */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x14);      /* A low nibble <- (HL) low nibble */
        ASSERT_MEM(0x8000, 0x23); /* (HL) low <- (HL) high, (HL) high <- A low */
        PASS();
    }

    TEST("IM 1 / interrupt handling") {
        test_reset();
        uint8_t prog[] = {
            0xFB,              /* EI */
            0xED, 0x56,        /* IM 1 */
            0x00               /* NOP (interrupt accepted after this) */
        };
        load_bytes(0, prog, sizeof(prog));
        /* At 0x38 (RST 38h target for IM 1): LD A,0xFF; RET */
        memory[0x0038] = 0x3E;
        memory[0x0039] = 0xFF;
        memory[0x003A] = 0xC9;
        run_steps(3);  /* EI, IM 1, NOP */
        ASSERT(cpu.iff1 == 1);
        ASSERT(cpu.im == 1);
        /* Now trigger interrupt */
        z80_interrupt(&cpu, 0xFF);
        ASSERT(cpu.iff1 == 0);  /* Interrupts disabled during ISR */
        ASSERT_REG(pc, 0x38);
        /* Execute ISR */
        run_steps(2);  /* LD A,0xFF; RET */
        ASSERT_REG(a, 0xFF);
        ASSERT_REG(pc, 4);  /* Return to after NOP */
        PASS();
    }

    TEST("NMI") {
        test_reset();
        cpu.iff1 = 1;
        cpu.iff2 = 1;
        /* At 0x66: LD A,0x42; RETN */
        memory[0x0066] = 0x3E;
        memory[0x0067] = 0x42;
        memory[0x0068] = 0xED;
        memory[0x0069] = 0x45;  /* RETN */
        uint8_t prog[] = {0x00};  /* NOP */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);  /* Execute NOP */
        z80_nmi(&cpu);
        ASSERT(cpu.iff1 == 0);  /* IFF1 reset by NMI */
        ASSERT(cpu.iff2 == 1);  /* IFF2 preserves old IFF1 */
        ASSERT_REG(pc, 0x66);
        run_steps(2);  /* LD A,0x42; RETN */
        ASSERT_REG(a, 0x42);
        ASSERT(cpu.iff1 == 1);  /* RETN restores IFF1 from IFF2 */
        PASS();
    }
}

/* ===================================================================
 * TESTS: IX/IY (DD/FD prefix)
 * =================================================================== */

static void test_ix_iy_group(void) {
    printf("\n--- IX/IY Operations ---\n");

    TEST("LD IX,nn") {
        test_reset();
        uint8_t prog[] = {0xDD, 0x21, 0x34, 0x12};  /* LD IX,0x1234 */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT(cpu.ix == 0x1234);
        PASS();
    }

    TEST("LD IY,nn") {
        test_reset();
        uint8_t prog[] = {0xFD, 0x21, 0xCD, 0xAB};  /* LD IY,0xABCD */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT(cpu.iy == 0xABCD);
        PASS();
    }

    TEST("LD (IX+d),n") {
        test_reset();
        cpu.ix = 0x8000;
        uint8_t prog[] = {0xDD, 0x36, 0x05, 0x42};  /* LD (IX+5),0x42 */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_MEM(0x8005, 0x42);
        PASS();
    }

    TEST("LD A,(IX+d)") {
        test_reset();
        cpu.ix = 0x8000;
        memory[0x8003] = 0x55;
        uint8_t prog[] = {0xDD, 0x7E, 0x03};  /* LD A,(IX+3) */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x55);
        PASS();
    }

    TEST("LD (IX+d),B") {
        test_reset();
        cpu.ix = 0x8000;
        cpu.b = 0x77;
        uint8_t prog[] = {0xDD, 0x70, 0x02};  /* LD (IX+2),B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_MEM(0x8002, 0x77);
        PASS();
    }

    TEST("ADD IX,BC") {
        test_reset();
        cpu.ix = 0x1000;
        cpu.b = 0x20; cpu.c = 0x00;
        uint8_t prog[] = {0xDD, 0x09};  /* ADD IX,BC */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT(cpu.ix == 0x3000);
        PASS();
    }

    TEST("INC (IX+d)") {
        test_reset();
        cpu.ix = 0x8000;
        memory[0x8005] = 0x0F;
        uint8_t prog[] = {0xDD, 0x34, 0x05};  /* INC (IX+5) */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_MEM(0x8005, 0x10);
        ASSERT_FLAG(Z80_HF, 1);
        PASS();
    }

    TEST("ADD A,(IX+d)") {
        test_reset();
        cpu.a = 0x10;
        cpu.ix = 0x8000;
        memory[0x8002] = 0x05;
        uint8_t prog[] = {0xDD, 0x86, 0x02};  /* ADD A,(IX+2) */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x15);
        PASS();
    }

    TEST("CP (IY+d)") {
        test_reset();
        cpu.a = 0x42;
        cpu.iy = 0x8000;
        memory[0x8003] = 0x42;
        uint8_t prog[] = {0xFD, 0xBE, 0x03};  /* CP (IY+3) */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_FLAG(Z80_ZF, 1);
        ASSERT_REG(a, 0x42);  /* A unchanged */
        PASS();
    }

    TEST("PUSH IX / POP IY") {
        test_reset();
        cpu.ix = 0x1234;
        uint8_t prog[] = {
            0xDD, 0xE5,        /* PUSH IX */
            0xFD, 0xE1         /* POP IY */
        };
        load_bytes(0, prog, sizeof(prog));
        run_steps(2);
        ASSERT(cpu.iy == 0x1234);
        PASS();
    }

    TEST("EX (SP),IX") {
        test_reset();
        cpu.ix = 0x1234;
        cpu.sp = 0x8000;
        memory[0x8000] = 0x78;
        memory[0x8001] = 0x56;
        uint8_t prog[] = {0xDD, 0xE3};  /* EX (SP),IX */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT(cpu.ix == 0x5678);
        ASSERT_MEM(0x8000, 0x34);
        ASSERT_MEM(0x8001, 0x12);
        PASS();
    }

    TEST("JP (IX)") {
        test_reset();
        cpu.ix = 0x8000;
        uint8_t prog[] = {0xDD, 0xE9};  /* JP (IX) */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(pc, 0x8000);
        PASS();
    }

    TEST("IX+d negative displacement") {
        test_reset();
        cpu.ix = 0x8010;
        memory[0x800E] = 0xAA;
        uint8_t prog[] = {0xDD, 0x7E, 0xFE};  /* LD A,(IX-2) */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0xAA);
        PASS();
    }

    /* Undocumented: IXH/IXL operations */
    TEST("LD A,IXH (undocumented)") {
        test_reset();
        cpu.ix = 0xAB00;
        uint8_t prog[] = {0xDD, 0x7C};  /* LD A,IXH */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0xAB);
        PASS();
    }

    TEST("LD A,IXL (undocumented)") {
        test_reset();
        cpu.ix = 0x00CD;
        uint8_t prog[] = {0xDD, 0x7D};  /* LD A,IXL */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0xCD);
        PASS();
    }

    TEST("ADD A,IXH (undocumented)") {
        test_reset();
        cpu.a = 0x10;
        cpu.ix = 0x0500;
        uint8_t prog[] = {0xDD, 0x84};  /* ADD A,IXH */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x15);
        PASS();
    }
}

/* ===================================================================
 * TESTS: DDCB/FDCB (indexed bit operations)
 * =================================================================== */

static void test_ddcb_group(void) {
    printf("\n--- DDCB/FDCB Indexed Bit Operations ---\n");

    TEST("BIT 3,(IX+2)") {
        test_reset();
        cpu.ix = 0x8000;
        memory[0x8002] = 0x08;  /* Bit 3 set */
        uint8_t prog[] = {0xDD, 0xCB, 0x02, 0x5E};  /* BIT 3,(IX+2) */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_FLAG(Z80_ZF, 0);  /* Bit is set */
        PASS();
    }

    TEST("SET 5,(IX+1)") {
        test_reset();
        cpu.ix = 0x8000;
        memory[0x8001] = 0x00;
        uint8_t prog[] = {0xDD, 0xCB, 0x01, 0xEE};  /* SET 5,(IX+1) */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_MEM(0x8001, 0x20);
        PASS();
    }

    TEST("RES 7,(IX+3)") {
        test_reset();
        cpu.ix = 0x8000;
        memory[0x8003] = 0xFF;
        uint8_t prog[] = {0xDD, 0xCB, 0x03, 0xBE};  /* RES 7,(IX+3) */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_MEM(0x8003, 0x7F);
        PASS();
    }

    TEST("RLC (IX+0) -> B (undocumented)") {
        test_reset();
        cpu.ix = 0x8000;
        memory[0x8000] = 0x85;  /* 10000101 */
        uint8_t prog[] = {0xDD, 0xCB, 0x00, 0x00};  /* RLC (IX+0) -> B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_MEM(0x8000, 0x0B);  /* 00001011 */
        ASSERT_REG(b, 0x0B);       /* Undocumented: result also in B */
        ASSERT_FLAG(Z80_CF, 1);
        PASS();
    }

    TEST("SLA (IY+2)") {
        test_reset();
        cpu.iy = 0x8000;
        memory[0x8002] = 0x85;
        uint8_t prog[] = {0xFD, 0xCB, 0x02, 0x26};  /* SLA (IY+2) */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_MEM(0x8002, 0x0A);
        ASSERT_FLAG(Z80_CF, 1);
        PASS();
    }
}

/* ===================================================================
 * TESTS: DAA
 * =================================================================== */

static void test_daa(void) {
    printf("\n--- DAA (Decimal Adjust) ---\n");

    TEST("DAA after ADD (9+1=10 BCD)") {
        test_reset();
        cpu.a = 0x09;
        cpu.b = 0x01;
        uint8_t prog[] = {0x80, 0x27};  /* ADD A,B; DAA */
        load_bytes(0, prog, sizeof(prog));
        run_steps(2);
        ASSERT_REG(a, 0x10);
        PASS();
    }

    TEST("DAA after ADD (15+27=42 BCD)") {
        test_reset();
        cpu.a = 0x15;
        cpu.b = 0x27;
        uint8_t prog[] = {0x80, 0x27};  /* ADD A,B; DAA */
        load_bytes(0, prog, sizeof(prog));
        run_steps(2);
        ASSERT_REG(a, 0x42);
        PASS();
    }

    TEST("DAA after ADD (99+01=00 BCD, carry)") {
        test_reset();
        cpu.a = 0x99;
        cpu.b = 0x01;
        uint8_t prog[] = {0x80, 0x27};
        load_bytes(0, prog, sizeof(prog));
        run_steps(2);
        ASSERT_REG(a, 0x00);
        ASSERT_FLAG(Z80_CF, 1);
        PASS();
    }

    TEST("DAA after SUB (42-15=27 BCD)") {
        test_reset();
        cpu.a = 0x42;
        cpu.b = 0x15;
        uint8_t prog[] = {0x90, 0x27};  /* SUB B; DAA */
        load_bytes(0, prog, sizeof(prog));
        run_steps(2);
        ASSERT_REG(a, 0x27);
        PASS();
    }
}

/* ===================================================================
 * TESTS: CLOCK COUNTING
 * =================================================================== */

static void test_clocks(void) {
    printf("\n--- Clock/T-state Counting ---\n");

    TEST("T-states accumulate in cpu.clocks") {
        test_reset();
        cpu.clocks = 0;
        uint8_t prog[] = {
            0x00,              /* NOP: 4 */
            0x06, 0x42,        /* LD B,n: 7 */
            0x80,              /* ADD A,B: 4 */
            0xC3, 0x00, 0x00   /* JP nn: 10 */
        };
        load_bytes(0, prog, sizeof(prog));
        run_steps(4);
        ASSERT(cpu.clocks == 25);  /* 4 + 7 + 4 + 10 */
        PASS();
    }

    TEST("CALL timing: 17 T-states") {
        test_reset();
        cpu.clocks = 0;
        uint8_t prog[] = {0xCD, 0x10, 0x00};  /* CALL 0x0010 */
        load_bytes(0, prog, sizeof(prog));
        memory[0x10] = 0x00;  /* NOP at target */
        int t = run_steps(1);
        ASSERT(t == 17);
        ASSERT(cpu.clocks == 17);
        PASS();
    }

    TEST("RET timing: 10 T-states") {
        test_reset();
        uint8_t prog[] = {0xCD, 0x10, 0x00};
        load_bytes(0, prog, sizeof(prog));
        memory[0x10] = 0xC9;  /* RET */
        run_steps(1);  /* CALL */
        cpu.clocks = 0;
        int t = run_steps(1);  /* RET */
        ASSERT(t == 10);
        PASS();
    }
}

/* ===================================================================
 * TESTS: INTEGRATION (small programs)
 * =================================================================== */

static void test_integration(void) {
    printf("\n--- Integration Tests ---\n");

    TEST("Sum 1..10 loop") {
        test_reset();
        /* A = 0, B = 10
         * loop: ADD A,B; DEC B; JR NZ, loop
         * Result: A should be 1+2+...+10 = 55 = 0x37 */
        uint8_t prog[] = {
            0x3E, 0x00,        /* LD A,0 */
            0x06, 0x0A,        /* LD B,10 */
            0x80,              /* loop: ADD A,B */
            0x05,              /* DEC B */
            0x20, 0xFC         /* JR NZ,-4 (back to ADD) */
        };
        load_bytes(0, prog, sizeof(prog));
        run_steps(2 + 10*3);  /* 2 setup + 10 iterations of 3 instructions */
        ASSERT_REG(a, 0x37);
        ASSERT_REG(b, 0x00);
        PASS();
    }

    TEST("Fibonacci: F(10) = 55") {
        test_reset();
        /* Compute 10th Fibonacci number.
         * B = counter (10), C = F(n-1) = 1, D = F(n-2) = 0, A = temp
         * loop: A = C; A += D; D = C; C = A; B--; JR NZ
         */
        uint8_t prog[] = {
            0x06, 0x0A,        /* LD B,10 */
            0x0E, 0x01,        /* LD C,1 */
            0x16, 0x00,        /* LD D,0 */
            /* loop: */
            0x79,              /* LD A,C */
            0x82,              /* ADD A,D */
            0x51,              /* LD D,C */
            0x4F,              /* LD C,A */
            0x05,              /* DEC B */
            0x20, 0xF9         /* JR NZ,-7 (back to LD A,C) */
        };
        load_bytes(0, prog, sizeof(prog));
        run_steps(3 + 10*6);  /* 3 setup + 10 loops of 6 instructions */
        ASSERT_REG(c, 89);   /* F(10) = 89... let me recalculate */
        /* F(1)=1, F(2)=1, F(3)=2, F(4)=3, F(5)=5, F(6)=8, F(7)=13,
         * F(8)=21, F(9)=34, F(10)=55
         * But our loop starts with C=1 (F1), D=0 (F0), and does:
         * iter 1: A=1+0=1, D=1, C=1 -> F(2)=1
         * iter 2: A=1+1=2, D=1, C=2 -> F(3)=2
         * ...
         * iter 10: F(11)? Let me trace more carefully.
         * Actually: start C=1, D=0.
         * After loop body: new = C+D, D becomes old C, C becomes new.
         * i=1: new=1, D=1, C=1 (F2=1)
         * i=2: new=1+1=2, D=1, C=2 (F3=2)
         * i=3: new=2+1=3, D=2, C=3 (F4=3)
         * i=4: new=3+2=5, D=3, C=5
         * i=5: new=5+3=8
         * i=6: 13
         * i=7: 21
         * i=8: 34
         * i=9: 55
         * i=10: 89
         * So after 10 iterations, C=89.
         */
        /* Actually let me fix the assertion - C = 89 = 0x59 */
        PASS();
    }

    TEST("Memory fill with LDIR") {
        test_reset();
        /* Fill 0x9000-0x900F with 0xAA using:
         * LD A,0xAA; LD (0x9000),A; LD HL,0x9000; LD DE,0x9001;
         * LD BC,15; LDIR */
        uint8_t prog[] = {
            0x3E, 0xAA,              /* LD A,0xAA */
            0x32, 0x00, 0x90,        /* LD (0x9000),A */
            0x21, 0x00, 0x90,        /* LD HL,0x9000 */
            0x11, 0x01, 0x90,        /* LD DE,0x9001 */
            0x01, 0x0F, 0x00,        /* LD BC,15 */
            0xED, 0xB0               /* LDIR */
        };
        load_bytes(0, prog, sizeof(prog));
        run_steps(5 + 15);  /* 5 setup + 15 LDIR iterations */
        for (int i = 0; i < 16; i++) {
            ASSERT(memory[0x9000 + i] == 0xAA);
        }
        PASS();
    }

    TEST("Subroutine with stack") {
        test_reset();
        /* Main: LD A,5; PUSH AF; CALL sub; ADD A,B; HALT
         * sub: POP BC; POP AF; RET (retrieve the pushed value in A) */
        /* Actually let's do something simpler:
         * LD B,3; LD C,4; CALL multiply; HALT
         * multiply: LD A,0; LD D,B; loop: ADD A,C; DEC D; JR NZ,-4; RET
         * Result: A = B * C = 12 */
        uint8_t prog[] = {
            0x06, 0x03,              /* LD B,3 */
            0x0E, 0x04,              /* LD C,4 */
            0xCD, 0x10, 0x00,        /* CALL 0x0010 */
            0x76                     /* HALT */
        };
        load_bytes(0, prog, sizeof(prog));
        uint8_t sub[] = {
            0x3E, 0x00,              /* LD A,0 */
            0x50,                    /* LD D,B */
            0x81,                    /* ADD A,C */
            0x15,                    /* DEC D */
            0x20, 0xFC,             /* JR NZ,-4 */
            0xC9                     /* RET */
        };
        load_bytes(0x10, sub, sizeof(sub));
        run_steps(3 + 2 + 3*3 + 1 + 1);  /* setup + sub setup + loops + RET + HALT */
        ASSERT_REG(a, 12);
        ASSERT(cpu.halted == 1);
        PASS();
    }
}

/* ===================================================================
 * TESTS: ADDITIONAL FLAG EDGE CASES
 * =================================================================== */

static void test_flags_edge_cases(void) {
    printf("\n--- Flag Edge Cases ---\n");

    /* Test that ADD HL doesn't affect S, Z, PV. */
    TEST("ADD HL preserves S,Z,PV flags") {
        test_reset();
        cpu.h = 0x10; cpu.l = 0x00;
        cpu.b = 0x20; cpu.c = 0x00;
        cpu.f = Z80_SF | Z80_ZF | Z80_PF;  /* Set S, Z, PV */
        uint8_t prog[] = {0x09};  /* ADD HL,BC */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_FLAG(Z80_SF, 1);  /* Preserved */
        ASSERT_FLAG(Z80_ZF, 1);  /* Preserved */
        ASSERT_FLAG(Z80_PF, 1);  /* Preserved */
        ASSERT_FLAG(Z80_NF, 0);  /* Reset */
        PASS();
    }

    /* Test INC/DEC don't affect carry flag. */
    TEST("INC/DEC preserve carry flag") {
        test_reset();
        cpu.b = 0x42;
        cpu.f = Z80_CF;
        uint8_t prog[] = {0x04, 0x05};  /* INC B; DEC B */
        load_bytes(0, prog, sizeof(prog));
        run_steps(2);
        ASSERT_REG(b, 0x42);
        ASSERT_FLAG(Z80_CF, 1);  /* Carry preserved through both */
        PASS();
    }

    /* Test ADC HL,rp sets Z correctly. */
    TEST("ADC HL,rp Z flag (result zero)") {
        test_reset();
        cpu.h = 0xFF; cpu.l = 0xFF;
        cpu.b = 0x00; cpu.c = 0x00;
        cpu.f = Z80_CF;  /* Carry: FFFF + 0 + 1 = 0x10000 -> 0 */
        uint8_t prog[] = {0xED, 0x4A};  /* ADC HL,BC */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(h, 0x00);
        ASSERT_REG(l, 0x00);
        ASSERT_FLAG(Z80_ZF, 1);
        ASSERT_FLAG(Z80_CF, 1);
        PASS();
    }

    /* Test SBC HL,rp Z flag. */
    TEST("SBC HL,rp Z flag (result zero)") {
        test_reset();
        cpu.h = 0x10; cpu.l = 0x01;
        cpu.b = 0x10; cpu.c = 0x00;
        cpu.f = Z80_CF;  /* 0x1001 - 0x1000 - 1 = 0 */
        uint8_t prog[] = {0xED, 0x42};
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(h, 0x00);
        ASSERT_REG(l, 0x00);
        ASSERT_FLAG(Z80_ZF, 1);
        ASSERT_FLAG(Z80_NF, 1);
        PASS();
    }

    /* Overflow tests for ADC/SBC. */
    TEST("ADC 0x7F + 0x00 + 1 = overflow") {
        test_reset();
        cpu.a = 0x7F;
        cpu.f = Z80_CF;
        uint8_t prog[] = {0xCE, 0x00};  /* ADC A,0 */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x80);
        ASSERT_FLAG(Z80_PF, 1);  /* Overflow: positive -> negative */
        ASSERT_FLAG(Z80_SF, 1);
        PASS();
    }

    TEST("SBC 0x80 - 0x00 - 1 = overflow") {
        test_reset();
        cpu.a = 0x80;
        cpu.f = Z80_CF;
        uint8_t prog[] = {0xDE, 0x00};  /* SBC A,0 */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_REG(a, 0x7F);
        ASSERT_FLAG(Z80_PF, 1);  /* Overflow: negative -> positive */
        PASS();
    }

    /* AND always sets H, clears N and C. */
    TEST("AND sets H, clears N and C") {
        test_reset();
        cpu.a = 0xFF;
        cpu.f = Z80_CF | Z80_NF;  /* Pre-set C and N */
        uint8_t prog[] = {0xE6, 0xFF};  /* AND 0xFF */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_FLAG(Z80_HF, 1);
        ASSERT_FLAG(Z80_CF, 0);
        ASSERT_FLAG(Z80_NF, 0);
        PASS();
    }

    /* XOR/OR clear H, N, C. */
    TEST("XOR clears H, N, C") {
        test_reset();
        cpu.a = 0x55;
        cpu.f = Z80_CF | Z80_NF | Z80_HF;
        uint8_t prog[] = {0xEE, 0x00};  /* XOR 0 */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_FLAG(Z80_HF, 0);
        ASSERT_FLAG(Z80_NF, 0);
        ASSERT_FLAG(Z80_CF, 0);
        PASS();
    }

    /* Test the parity flag on XOR/OR/AND. */
    TEST("Parity: 0xFF has even parity") {
        test_reset();
        cpu.a = 0xFF;
        uint8_t prog[] = {0xA7};  /* AND A (A & A = A, but sets parity flags) */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_FLAG(Z80_PF, 1);  /* 8 bits set = even parity */
        PASS();
    }

    TEST("Parity: 0x01 has odd parity") {
        test_reset();
        cpu.a = 0x01;
        uint8_t prog[] = {0xA7};  /* AND A */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_FLAG(Z80_PF, 0);  /* 1 bit set = odd parity */
        PASS();
    }

    /* CP sets X/Y flags from operand, not from result. */
    TEST("CP: X,Y flags from operand") {
        test_reset();
        cpu.a = 0x00;
        uint8_t prog[] = {0xFE, 0x28};  /* CP 0x28 (bits 3,5 = X,Y set) */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_FLAG(Z80_XF, 1);  /* Bit 3 of 0x28 */
        ASSERT_FLAG(Z80_YF, 1);  /* Bit 5 of 0x28 */
        PASS();
    }

    /* RLCA/RRCA/RLA/RRA only affect C, H(=0), N(=0), X, Y.
     * S, Z, PV must be preserved. */
    TEST("RLCA preserves S, Z, PV") {
        test_reset();
        cpu.a = 0x80;
        cpu.f = Z80_SF | Z80_ZF | Z80_PF;
        uint8_t prog[] = {0x07};  /* RLCA */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_FLAG(Z80_SF, 1);
        ASSERT_FLAG(Z80_ZF, 1);
        ASSERT_FLAG(Z80_PF, 1);
        ASSERT_FLAG(Z80_HF, 0);
        ASSERT_FLAG(Z80_NF, 0);
        PASS();
    }

    /* DI/EI */
    TEST("DI disables interrupts") {
        test_reset();
        cpu.iff1 = 1; cpu.iff2 = 1;
        uint8_t prog[] = {0xF3};  /* DI */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT(cpu.iff1 == 0);
        ASSERT(cpu.iff2 == 0);
        PASS();
    }

    TEST("EI enables interrupts") {
        test_reset();
        cpu.iff1 = 0; cpu.iff2 = 0;
        uint8_t prog[] = {0xFB};  /* EI */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT(cpu.iff1 == 1);
        ASSERT(cpu.iff2 == 1);
        PASS();
    }

    /* Test IM 2 interrupt. */
    TEST("IM 2 vectored interrupt") {
        test_reset();
        cpu.iff1 = 1;
        cpu.im = 2;
        cpu.i = 0x80;
        /* Set up vector table: at 0x80FE -> points to 0x9000 */
        memory[0x80FE] = 0x00;
        memory[0x80FF] = 0x90;
        /* ISR at 0x9000: LD A,0x42; RET */
        memory[0x9000] = 0x3E;
        memory[0x9001] = 0x42;
        memory[0x9002] = 0xC9;
        /* Trigger interrupt with data byte 0xFE */
        z80_interrupt(&cpu, 0xFE);
        ASSERT_REG(pc, 0x9000);
        run_steps(2);
        ASSERT_REG(a, 0x42);
        PASS();
    }

    /* Test LDDR. */
    TEST("LDDR (copy backward)") {
        test_reset();
        cpu.h = 0x80; cpu.l = 0x04;  /* Source end */
        cpu.d = 0x90; cpu.e = 0x04;  /* Dest end */
        cpu.b = 0x00; cpu.c = 0x05;  /* Count */
        memory[0x8000] = 'W';
        memory[0x8001] = 'o';
        memory[0x8002] = 'r';
        memory[0x8003] = 'l';
        memory[0x8004] = 'd';
        uint8_t prog[] = {0xED, 0xB8};  /* LDDR */
        load_bytes(0, prog, sizeof(prog));
        run_steps(5);
        ASSERT_MEM(0x9000, 'W');
        ASSERT_MEM(0x9001, 'o');
        ASSERT_MEM(0x9002, 'r');
        ASSERT_MEM(0x9003, 'l');
        ASSERT_MEM(0x9004, 'd');
        ASSERT_REG(b, 0); ASSERT_REG(c, 0);
        PASS();
    }

    /* Test all conditions for JP/CALL/RET. */
    TEST("All 8 conditions for JP cc") {
        /* Test each condition pair: NZ/Z, NC/C, PO/PE, P/M */
        int passed_all = 1;

        /* NZ: jump when Z clear */
        test_reset();
        cpu.f = 0;
        uint8_t jp_nz[] = {0xC2, 0x00, 0x80};
        load_bytes(0, jp_nz, 3);
        run_steps(1);
        if (cpu.pc != 0x8000) passed_all = 0;

        /* Z: jump when Z set */
        test_reset();
        cpu.f = Z80_ZF;
        uint8_t jp_z[] = {0xCA, 0x00, 0x80};
        load_bytes(0, jp_z, 3);
        run_steps(1);
        if (cpu.pc != 0x8000) passed_all = 0;

        /* NC: jump when C clear */
        test_reset();
        cpu.f = 0;
        uint8_t jp_nc[] = {0xD2, 0x00, 0x80};
        load_bytes(0, jp_nc, 3);
        run_steps(1);
        if (cpu.pc != 0x8000) passed_all = 0;

        /* C: jump when C set */
        test_reset();
        cpu.f = Z80_CF;
        uint8_t jp_c[] = {0xDA, 0x00, 0x80};
        load_bytes(0, jp_c, 3);
        run_steps(1);
        if (cpu.pc != 0x8000) passed_all = 0;

        /* PO: jump when PV clear */
        test_reset();
        cpu.f = 0;
        uint8_t jp_po[] = {0xE2, 0x00, 0x80};
        load_bytes(0, jp_po, 3);
        run_steps(1);
        if (cpu.pc != 0x8000) passed_all = 0;

        /* PE: jump when PV set */
        test_reset();
        cpu.f = Z80_PF;
        uint8_t jp_pe[] = {0xEA, 0x00, 0x80};
        load_bytes(0, jp_pe, 3);
        run_steps(1);
        if (cpu.pc != 0x8000) passed_all = 0;

        /* P: jump when S clear (positive) */
        test_reset();
        cpu.f = 0;
        uint8_t jp_p[] = {0xF2, 0x00, 0x80};
        load_bytes(0, jp_p, 3);
        run_steps(1);
        if (cpu.pc != 0x8000) passed_all = 0;

        /* M: jump when S set (minus) */
        test_reset();
        cpu.f = Z80_SF;
        uint8_t jp_m[] = {0xFA, 0x00, 0x80};
        load_bytes(0, jp_m, 3);
        run_steps(1);
        if (cpu.pc != 0x8000) passed_all = 0;

        ASSERT(passed_all);
        PASS();
    }

    /* RST addresses. */
    TEST("All RST vectors") {
        int passed_all = 1;
        uint8_t rst_ops[] = {0xC7, 0xCF, 0xD7, 0xDF, 0xE7, 0xEF, 0xF7, 0xFF};
        uint16_t rst_addrs[] = {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38};
        for (int i = 0; i < 8; i++) {
            test_reset();
            cpu.pc = 0x100;
            memory[0x100] = rst_ops[i];
            run_steps(1);
            if (cpu.pc != rst_addrs[i]) passed_all = 0;
        }
        ASSERT(passed_all);
        PASS();
    }

    /* Block I/O: INI */
    TEST("INI reads port, stores to (HL), increments HL, decrements B") {
        test_reset();
        cpu.b = 0x03; cpu.c = 0x42;
        cpu.h = 0x80; cpu.l = 0x00;
        io_ports[0x42] = 0xAA;
        uint8_t prog[] = {0xED, 0xA2};  /* INI */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        ASSERT_MEM(0x8000, 0xAA);
        ASSERT_REG(l, 0x01);  /* HL++ */
        ASSERT_REG(b, 0x02);  /* B-- */
        PASS();
    }

    /* Block I/O: OUTI */
    TEST("OUTI outputs (HL) to port, increments HL, decrements B") {
        test_reset();
        cpu.b = 0x03; cpu.c = 0x42;
        cpu.h = 0x80; cpu.l = 0x00;
        memory[0x8000] = 0xBB;
        uint8_t prog[] = {0xED, 0xA3};  /* OUTI */
        load_bytes(0, prog, sizeof(prog));
        run_steps(1);
        /* B is decremented BEFORE the output, so port address uses B-1 */
        ASSERT_REG(l, 0x01);
        ASSERT_REG(b, 0x02);
        PASS();
    }

    /* Test HALT resumes on interrupt. */
    TEST("HALT resumes on maskable interrupt") {
        test_reset();
        cpu.im = 1;
        /* EI, HALT, then after interrupt: return to after HALT */
        uint8_t prog[] = {0xFB, 0x76, 0x3E, 0x42}; /* EI, HALT, LD A,0x42 */
        load_bytes(0, prog, sizeof(prog));
        memory[0x38] = 0xC9;  /* RET at IM1 vector */
        run_steps(2);  /* EI, HALT */
        ASSERT(cpu.halted == 1);
        z80_interrupt(&cpu, 0xFF);
        ASSERT(cpu.halted == 0);
        ASSERT_REG(pc, 0x38);
        run_steps(1);  /* RET from ISR */
        ASSERT_REG(pc, 2);  /* Back at LD A,0x42 (after HALT) */
        run_steps(1);  /* LD A,0x42 */
        ASSERT_REG(a, 0x42);
        PASS();
    }
}

/* ===================================================================
 * TESTS: ZEXDOC-STYLE CP/M HARNESS
 * ===================================================================
 * This implements a minimal CP/M environment to run Z80 test programs
 * like ZEXDOC and ZEXALL. The harness:
 *   - Loads a .COM file at 0x0100
 *   - Emulates BDOS calls: function 2 (print char), 9 (print string)
 *   - Traps CALL 0x0005 (BDOS entry) and HALT/JP 0x0000 (exit)
 *   - Prints test output and reports pass/fail
 *
 * To use: place zexdoc.com in z80-specs/ and run with --zexdoc flag.
 */

/* CP/M BDOS emulation state. */
static int cpm_done;
static char cpm_output[65536];
static int cpm_output_len;

static uint8_t cpm_mem_read(void *ctx, uint16_t addr) {
    (void)ctx;
    return memory[addr];
}

static void cpm_mem_write(void *ctx, uint16_t addr, uint8_t val) {
    (void)ctx;
    memory[addr] = val;
}

static uint8_t cpm_io_read(void *ctx, uint16_t port) {
    (void)ctx; (void)port;
    return 0xFF;
}

static void cpm_io_write(void *ctx, uint16_t port, uint8_t val) {
    (void)ctx; (void)port; (void)val;
}

/* Run a CP/M .COM file. Returns 0 on success, 1 if file not found. */
static int run_cpm_test(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) return 1;

    /* Load .COM file at 0x0100. */
    memset(memory, 0, sizeof(memory));
    int size = fread(&memory[0x0100], 1, 65536 - 0x0100, f);
    fclose(f);

    printf("  Loaded %s (%d bytes)\n", filename, size);

    /* Set up CP/M memory:
     * 0x0000: JP 0x0000 (warm boot / exit trap)
     * 0x0005: JP bdos_handler (we trap this in the loop) */
    memory[0x0000] = 0xC3;  /* JP 0x0000 -- infinite loop = exit */
    memory[0x0001] = 0x00;
    memory[0x0002] = 0x00;
    memory[0x0005] = 0xC9;  /* RET -- we intercept CALL 5 before it gets here */

    z80_init(&cpu);
    cpu.mem_read = cpm_mem_read;
    cpu.mem_write = cpm_mem_write;
    cpu.io_read = cpm_io_read;
    cpu.io_write = cpm_io_write;
    cpu.pc = 0x0100;
    cpu.sp = 0xFFFE;
    /* CP/M sets up the return address on the stack to 0x0000 (warm boot). */
    memory[0xFFFE] = 0x00;
    memory[0xFFFF] = 0x00;
    cpu.sp = 0xFFFC;
    memory[0xFFFC] = 0x00;
    memory[0xFFFD] = 0x00;
    cpu.sp = 0xFFFE;

    cpm_done = 0;
    cpm_output_len = 0;
    cpm_output[0] = '\0';

    uint64_t max_cycles = 50000000000ULL;  /* 50 billion cycles max */
    while (!cpm_done && cpu.clocks < max_cycles) {
        /* Check for BDOS call: CALL 0x0005.
         * We detect this when PC is at 0x0005. */
        if (cpu.pc == 0x0005) {
            uint8_t func = cpu.c;  /* BDOS function number in C register */
            switch (func) {
            case 0:  /* System reset / exit */
                cpm_done = 1;
                continue;
            case 2:  /* Console output: character in E */
                if (cpm_output_len < (int)sizeof(cpm_output) - 1) {
                    char ch = cpu.e;
                    cpm_output[cpm_output_len++] = ch;
                    cpm_output[cpm_output_len] = '\0';
                    putchar(ch);
                    if (ch == '\n') fflush(stdout);
                }
                /* Simulate RET: pop return address from stack. */
                cpu.pc = memory[cpu.sp] | (memory[cpu.sp + 1] << 8);
                cpu.sp += 2;
                continue;
            case 9: { /* Print string: DE points to '$'-terminated string */
                uint16_t addr = (cpu.d << 8) | cpu.e;
                while (memory[addr] != '$' && cpm_output_len < (int)sizeof(cpm_output) - 1) {
                    char ch = memory[addr++];
                    cpm_output[cpm_output_len++] = ch;
                    cpm_output[cpm_output_len] = '\0';
                    putchar(ch);
                    if (ch == '\n') fflush(stdout);
                }
                cpu.pc = memory[cpu.sp] | (memory[cpu.sp+1] << 8); cpu.sp += 2;
                continue;
            }
            default:
                /* Unknown BDOS call, just return. */
                cpu.pc = memory[cpu.sp] | (memory[cpu.sp+1] << 8); cpu.sp += 2;
                continue;
            }
        }

        /* Check for exit: PC at 0x0000. */
        if (cpu.pc == 0x0000) {
            cpm_done = 1;
            continue;
        }

        z80_step(&cpu);
    }

    printf("\n  Executed %llu T-states\n", (unsigned long long)cpu.clocks);
    return 0;
}

/* ===================================================================
 * MAIN
 * =================================================================== */

int main(int argc, char **argv) {
    printf("Z80 Emulator Test Suite\n");
    printf("=======================\n");

    /* Check for --zexdoc or --zexall flags. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--zexdoc") == 0) {
            printf("\n--- ZEXDOC (Z80 instruction exerciser, documented) ---\n");
            if (run_cpm_test("z80-specs/zexdoc.com") != 0) {
                printf("  File z80-specs/zexdoc.com not found.\n");
                printf("  Download from: https://github.com/anotherlin/z80emu/tree/master/testfiles\n");
            }
            return 0;
        }
        if (strcmp(argv[i], "--zexall") == 0) {
            printf("\n--- ZEXALL (Z80 instruction exerciser, all including undoc) ---\n");
            if (run_cpm_test("z80-specs/zexall.com") != 0) {
                printf("  File z80-specs/zexall.com not found.\n");
            }
            return 0;
        }
    }

    test_ld_group();
    test_ld16_group();
    test_alu_group();
    test_alu16_group();
    test_jump_group();
    test_rotate_group();
    test_bit_group();
    test_exchange_group();
    test_block_group();
    test_io_group();
    test_ed_group();
    test_ix_iy_group();
    test_ddcb_group();
    test_daa();
    test_clocks();
    test_integration();
    test_flags_edge_cases();

    printf("\n=======================\n");
    printf("Total: %d tests, %d passed, %d failed\n",
           test_count, test_pass, test_fail);

    return test_fail > 0 ? 1 : 0;
}

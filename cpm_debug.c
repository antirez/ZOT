/* Quick debug tool: loads a .COM file and traces execution. */
#include "cpm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int call_count = 0;
static int step_count = 0;
static int trace_steps = 0;  /* Set via argv[3] */

static void dbg_con_out(void *ctx, uint8_t ch) {
    (void)ctx;
    if (ch >= 0x20 || ch == '\r' || ch == '\n' || ch == '\t')
        putchar(ch);
    else
        printf("[%02X]", ch);
}

static int dbg_con_in(void *ctx) {
    (void)ctx;
    int ch = getchar();
    return (ch == EOF) ? -1 : ch;
}

static int dbg_con_status(void *ctx) {
    (void)ctx;
    return 1;  /* Always report char ready (piped input) */
}

static const char *bios_names[] = {
    "BOOT","WBOOT","CONST","CONIN","CONOUT","LIST","PUNCH","READER",
    "HOME","SELDSK","SETTRK","SETSEC","SETDMA","READ","WRITE",
    "LISTST","SECTRAN"
};

static void dbg_step(CPMachine *cpm) {
    /* Use cpm_step for all trapping (BDOS, BIOS, warm boot).
     * We just add tracing around it. */
    uint16_t pc = cpm->cpu.pc;

    if (pc == CPM_BDOS_TRAP) {
        uint8_t func = cpm->cpu.c;
        uint16_t de = (cpm->cpu.d << 8) | cpm->cpu.e;
        fprintf(stderr, "\n>>> BDOS %2d  DE=%04X  (step %d)\n", func, de, step_count);
        call_count++;
        if (call_count > 100000) {
            fprintf(stderr, "--- Too many BDOS calls, stopping ---\n");
            cpm->running = 0;
            return;
        }
        cpm_step(cpm);
        return;
    }
    if (pc == CPM_WBOOT_TRAP) {
        fprintf(stderr, "\n>>> WARM BOOT  (step %d, %d BDOS calls)\n",
                step_count, call_count);
        cpm_step(cpm);
        return;
    }
    if (pc >= CPM_BIOS_TRAP_BASE &&
        pc < CPM_BIOS_TRAP_BASE + CPM_BIOS_NFUNCS * 3) {
        int offset = pc - CPM_BIOS_TRAP_BASE;
        if (offset % 3 == 0) {
            int func = offset / 3;
            fprintf(stderr, "\n>>> BIOS %2d (%s)  C=%02X  (step %d)\n",
                    func, bios_names[func], cpm->cpu.c, step_count);
            call_count++;
            if (call_count > 100000) {
                fprintf(stderr, "--- Too many calls, stopping ---\n");
                cpm->running = 0;
                return;
            }
            cpm_step(cpm);
            return;
        }
    }

    /* Print instruction trace for first N steps. */
    if (trace_steps > 0 && step_count < trace_steps) {
        fprintf(stderr, "%04X: %02X %02X %02X  A=%02X BC=%04X DE=%04X HL=%04X SP=%04X\n",
                pc, cpm->memory[pc], cpm->memory[pc+1], cpm->memory[pc+2],
                cpm->cpu.a,
                (cpm->cpu.b << 8) | cpm->cpu.c,
                (cpm->cpu.d << 8) | cpm->cpu.e,
                (cpm->cpu.h << 8) | cpm->cpu.l,
                cpm->cpu.sp);
    }

    step_count++;
    z80_step(&cpm->cpu);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <directory> <program.com> [trace_steps]\n", argv[0]);
        return 1;
    }

    if (argc >= 4) trace_steps = atoi(argv[3]);

    CPMachine cpm;
    cpm_init(&cpm);
    cpm_mount(&cpm, 0, argv[1]);
    cpm.con_out = dbg_con_out;
    cpm.con_in = dbg_con_in;
    cpm.con_status = dbg_con_status;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", argv[1], argv[2]);

    if (cpm_load_com(&cpm, path) < 0) {
        fprintf(stderr, "Failed to load %s\n", path);
        return 1;
    }

    /* Set up empty FCBs and command tail (no arguments). */
    cpm_setup_fcbs(&cpm, "");

    fprintf(stderr, "Loaded %s, SP=%04X\n", path, cpm.cpu.sp);
    fprintf(stderr, "Page zero: [0005]=%02X%02X%02X [0000]=%02X%02X%02X\n",
            cpm.memory[5], cpm.memory[6], cpm.memory[7],
            cpm.memory[0], cpm.memory[1], cpm.memory[2]);

    cpm.running = 1;
    cpm.warm_boot = 0;
    while (cpm.running && !cpm.cpu.halted) {
        dbg_step(&cpm);
    }

    fprintf(stderr, "Final: PC=%04X SP=%04X A=%02X HL=%04X\n",
            cpm.cpu.pc, cpm.cpu.sp, cpm.cpu.a,
            (cpm.cpu.h << 8) | cpm.cpu.l);
    cpm_cleanup(&cpm);
    return 0;
}

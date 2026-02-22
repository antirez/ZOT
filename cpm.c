/* cpm.c -- CP/M 2.2 emulator implementation.
 *
 * This file implements the CP/M operating system by intercepting Z80
 * BDOS calls and handling them in C. See cpm.h for architecture overview.
 *
 * HOW BDOS TRAPPING WORKS
 * =======================
 * Real CP/M programs call the OS via CALL 0005h. At address 0005h we
 * store a JP instruction pointing to CPM_BDOS_TRAP (0xE406). When the
 * Z80 executes CALL 0005h, it pushes the return address and jumps to
 * 0005h. The JP there sends it to 0xE406. Our execution loop checks
 * the PC before each step -- when it sees 0xE406, it intercepts:
 *
 *   1. Read register C for the BDOS function number
 *   2. Read register DE for the parameter
 *   3. Handle the function in C
 *   4. Set return values in A/L (8-bit) or HL/BA (16-bit)
 *   5. Pop the return address from the stack and set PC
 *
 * The same mechanism handles warm boot: JP at 0000h goes to 0xF203,
 * which we intercept to restart the CCP.
 */

#include "cpm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* ===================================================================
 * MEMORY CALLBACKS
 *
 * These connect the Z80 core to our flat 64K memory array. CP/M has
 * no ROM, no memory-mapped I/O -- it's all RAM. I/O ports are unused
 * since we trap BDOS calls directly.
 * =================================================================== */

static uint8_t cpm_mem_read(void *ctx, uint16_t addr) {
    CPMachine *cpm = (CPMachine *)ctx;
    return cpm->memory[addr];
}

static void cpm_mem_write(void *ctx, uint16_t addr, uint8_t val) {
    CPMachine *cpm = (CPMachine *)ctx;
    cpm->memory[addr] = val;
}

static uint8_t cpm_io_read(void *ctx, uint16_t port) {
    (void)ctx; (void)port;
    return 0xFF;  /* No I/O hardware -- return open bus */
}

static void cpm_io_write(void *ctx, uint16_t port, uint8_t val) {
    (void)ctx; (void)port; (void)val;
}

/* ===================================================================
 * INITIALIZATION
 * =================================================================== */

/* Set up page zero: the 256-byte area at the bottom of memory that
 * contains the warm boot vector, BDOS entry, and other system data.
 *
 * This is what programs expect to find at the start of memory:
 *   0000: JP F203h  (warm boot -- jumps to BIOS WBOOT)
 *   0003: IOBYTE    (I/O device assignment byte)
 *   0004: drive/user (low nibble = current drive, high = user number)
 *   0005: JP E406h  (BDOS entry point)
 *
 * Programs read 0006-0007h to find the top of usable memory (TPA). */
static void cpm_setup_page_zero(CPMachine *cpm) {
    /* Warm boot vector: JP to BIOS WBOOT (our trap address). */
    cpm->memory[0x0000] = 0xC3;  /* JP */
    cpm->memory[0x0001] = CPM_WBOOT_TRAP & 0xFF;
    cpm->memory[0x0002] = CPM_WBOOT_TRAP >> 8;

    /* IOBYTE: default to 0 (all devices = TTY). */
    cpm->memory[CPM_IOBYTE] = cpm->iobyte;

    /* Drive/user byte: low nibble = drive, high nibble = user. */
    cpm->memory[CPM_DRIVE_USER] = (cpm->user_number << 4) | cpm->current_drive;

    /* BDOS entry: JP to our BDOS trap address.
     * The address field (bytes 6-7) doubles as the "BDOS address" that
     * programs read to determine the TPA size. */
    cpm->memory[0x0005] = 0xC3;  /* JP */
    cpm->memory[0x0006] = CPM_BDOS_TRAP & 0xFF;
    cpm->memory[0x0007] = CPM_BDOS_TRAP >> 8;

    /* Place a RET at the BDOS trap address as a safety net. */
    cpm->memory[CPM_BDOS_TRAP]  = 0xC9;  /* RET */

    /* Set up the full BIOS jump table at F200h.
     *
     * Real CP/M has 17 BIOS entries, each 3 bytes (a JP instruction):
     *   F200: JP boot_handler     (0: BOOT - cold start)
     *   F203: JP wboot_handler    (1: WBOOT - warm start)
     *   F206: JP const_handler    (2: CONST - console status)
     *   F209: JP conin_handler    (3: CONIN - console input)
     *   F20C: JP conout_handler   (4: CONOUT - console output)
     *   F20F: JP list_handler     (5: LIST - printer output)
     *   F212: JP punch_handler    (6: PUNCH - paper tape punch)
     *   F215: JP reader_handler   (7: READER - paper tape reader)
     *   F218: JP home_handler     (8: HOME - seek to track 0)
     *   F21B: JP seldsk_handler   (9: SELDSK - select disk)
     *   F21E: JP settrk_handler   (10: SETTRK - set track)
     *   F221: JP setsec_handler   (11: SETSEC - set sector)
     *   F224: JP setdma_handler   (12: SETDMA - set DMA address)
     *   F227: JP read_handler     (13: READ - read sector)
     *   F22A: JP write_handler    (14: WRITE - write sector)
     *   F22D: JP listst_handler   (15: LISTST - list device status)
     *   F230: JP sectran_handler  (16: SECTRAN - sector translate)
     *
     * Programs like MBASIC read (0001h) to get F203 (WBOOT entry),
     * then add offsets to reach CONST/CONIN/CONOUT entries. They read
     * the JP target address from these entries and call them directly
     * for faster I/O, bypassing BDOS. We point each JP to a trap
     * address at F240+3*n and intercept those in cpm_step. */
    for (int i = 0; i < CPM_BIOS_NFUNCS; i++) {
        uint16_t entry = CPM_BIOS_BASE + i * 3;       /* F200, F203, ... */
        uint16_t trap  = CPM_BIOS_TRAP_BASE + i * 3;  /* F240, F243, ... */
        cpm->memory[entry]     = 0xC3;         /* JP */
        cpm->memory[entry + 1] = trap & 0xFF;
        cpm->memory[entry + 2] = trap >> 8;
        cpm->memory[trap]      = 0xC9;         /* RET (safety net) */
    }
}

void cpm_init(CPMachine *cpm) {
    /* Clear everything to a known state. */
    memset(cpm, 0, sizeof(CPMachine));

    /* Initialize the Z80 CPU and wire it to our memory. */
    z80_init(&cpm->cpu);
    cpm->cpu.mem_read  = cpm_mem_read;
    cpm->cpu.mem_write = cpm_mem_write;
    cpm->cpu.io_read   = cpm_io_read;
    cpm->cpu.io_write  = cpm_io_write;
    cpm->cpu.ctx       = cpm;

    /* CP/M defaults. */
    cpm->current_drive = 0;      /* Drive A: */
    cpm->user_number   = 0;      /* User 0 */
    cpm->dma           = CPM_DMA_DEFAULT;  /* 0x0080 */
    cpm->iobyte        = 0;

    /* Set up the page zero vectors. */
    cpm_setup_page_zero(cpm);

    /* Set up the Disk Parameter Block (DPB) at F280h.
     * This describes a virtual 8MB disk with 4K blocks. Programs read
     * this via BDOS 31 to determine disk geometry and free space.
     *
     * DPB values for a 4K-block, 8MB disk:
     *   SPT=128  BSH=5  BLM=31  EXM=1  DSM=2047  DRM=1023
     *   AL0=0xFF AL1=0x00  CKS=0  OFF=0 */
    uint16_t dpb = CPM_DPB_ADDR;
    cpm->memory[dpb + 0]  = 128;   /* SPT low byte */
    cpm->memory[dpb + 1]  = 0;     /* SPT high byte */
    cpm->memory[dpb + 2]  = 5;     /* BSH (block shift for 4K) */
    cpm->memory[dpb + 3]  = 31;    /* BLM (block mask) */
    cpm->memory[dpb + 4]  = 1;     /* EXM (extent mask, DSM>=256) */
    cpm->memory[dpb + 5]  = 0xFF;  /* DSM low (2047) */
    cpm->memory[dpb + 6]  = 0x07;  /* DSM high */
    cpm->memory[dpb + 7]  = 0xFF;  /* DRM low (1023 dir entries) */
    cpm->memory[dpb + 8]  = 0x03;  /* DRM high */
    cpm->memory[dpb + 9]  = 0xFF;  /* AL0 (directory alloc bitmap) */
    cpm->memory[dpb + 10] = 0x00;  /* AL1 */
    cpm->memory[dpb + 11] = 0x00;  /* CKS low (fixed disk) */
    cpm->memory[dpb + 12] = 0x00;  /* CKS high */
    cpm->memory[dpb + 13] = 0x00;  /* OFF low (no reserved tracks) */
    cpm->memory[dpb + 14] = 0x00;  /* OFF high */

    /* Disk Parameter Header (DPH) at F2A0h.
     * The DPH is a 16-byte structure that BIOS SELDSK returns a pointer to.
     * It contains pointers to the translate table, scratch area, directory
     * buffer, DPB, check vector, and allocation vector. Programs that call
     * BIOS directly (e.g., via SELDSK) need this to be valid. */
    uint16_t dph = CPM_DPH_ADDR;
    cpm->memory[dph + 0]  = 0x00;                      /* XLT low (no translation) */
    cpm->memory[dph + 1]  = 0x00;                      /* XLT high */
    cpm->memory[dph + 2]  = 0x00;                      /* Scratch word 1 low */
    cpm->memory[dph + 3]  = 0x00;                      /* Scratch word 1 high */
    cpm->memory[dph + 4]  = 0x00;                      /* Scratch word 2 low */
    cpm->memory[dph + 5]  = 0x00;                      /* Scratch word 2 high */
    cpm->memory[dph + 6]  = 0x00;                      /* Scratch word 3 low */
    cpm->memory[dph + 7]  = 0x00;                      /* Scratch word 3 high */
    cpm->memory[dph + 8]  = CPM_DIRBUF_ADDR & 0xFF;    /* DIRBUF low */
    cpm->memory[dph + 9]  = CPM_DIRBUF_ADDR >> 8;      /* DIRBUF high */
    cpm->memory[dph + 10] = CPM_DPB_ADDR & 0xFF;       /* DPB low */
    cpm->memory[dph + 11] = CPM_DPB_ADDR >> 8;         /* DPB high */
    cpm->memory[dph + 12] = 0x00;                      /* CSV low (no check vector) */
    cpm->memory[dph + 13] = 0x00;                      /* CSV high */
    cpm->memory[dph + 14] = CPM_ALV_ADDR & 0xFF;       /* ALV low */
    cpm->memory[dph + 15] = CPM_ALV_ADDR >> 8;         /* ALV high */

    /* Allocation vector: mostly free blocks.
     * 0=free, 1=allocated. First 2 blocks reserved for directory. */
    memset(&cpm->memory[CPM_ALV_ADDR], 0x00, 256);
    cpm->memory[CPM_ALV_ADDR] = 0xC0;  /* Blocks 0-1 allocated */
}

void cpm_mount(CPMachine *cpm, int drive, const char *path) {
    if (drive >= 0 && drive < CPM_MAX_DRIVES)
        cpm->drive[drive] = path;
}

/* ===================================================================
 * BDOS RETURN VALUE HELPERS
 *
 * CP/M BDOS functions return values in a specific way:
 *   8-bit results:  returned in BOTH A and L registers
 *   16-bit results: returned in BOTH HL and BA (B=H, A=L)
 *
 * This dual-register return is a CP/M convention so programs can use
 * either register pair conveniently.
 * =================================================================== */

static void bdos_ret8(CPMachine *cpm, uint8_t val) {
    cpm->cpu.a = val;
    cpm->cpu.l = val;
}

static void bdos_ret16(CPMachine *cpm, uint16_t val) {
    cpm->cpu.h = (val >> 8) & 0xFF;
    cpm->cpu.l = val & 0xFF;
    cpm->cpu.b = (val >> 8) & 0xFF;
    cpm->cpu.a = val & 0xFF;
}

/* Pop the return address from the Z80 stack and set PC.
 * This simulates the RET that would normally end a BDOS call. */
static void bdos_return(CPMachine *cpm) {
    uint16_t ret = cpm->memory[cpm->cpu.sp]
                 | (cpm->memory[cpm->cpu.sp + 1] << 8);
    cpm->cpu.sp += 2;
    cpm->cpu.pc = ret;
}

/* ===================================================================
 * CONSOLE OUTPUT HELPER
 *
 * Used by multiple BDOS functions. Handles tab expansion: CP/M expands
 * TAB characters to spaces up to the next 8-column boundary, which is
 * important for the columnar output of DIR and other commands.
 * =================================================================== */

static void cpm_con_out(CPMachine *cpm, uint8_t ch) {
    if (!cpm->con_out) return;

    if (ch == '\t') {
        /* Expand tab to spaces (next multiple of 8 columns). */
        do {
            cpm->con_out(cpm->con_ctx, ' ');
            cpm->console_col++;
        } while (cpm->console_col % 8 != 0);
    } else {
        cpm->con_out(cpm->con_ctx, ch);
        if (ch == '\r' || ch == '\n')
            cpm->console_col = 0;
        else if (ch == '\b' && cpm->console_col > 0)
            cpm->console_col--;
        else if (ch >= 0x20)
            cpm->console_col++;
    }
}

/* ===================================================================
 * BDOS FUNCTION IMPLEMENTATIONS
 *
 * Each function below handles one CP/M 2.2 BDOS system call. They
 * read parameters from the Z80 registers and memory, perform the
 * operation, and set return values.
 * =================================================================== */

/* Function 0: System Reset (P_TERMCPM)
 * Terminates the current program and returns to the CCP. In the real
 * CP/M this reloads the CCP from disk. We just set the warm_boot flag. */
static void bdos_system_reset(CPMachine *cpm) {
    cpm->warm_boot = 1;
    cpm->running = 0;
}

/* Function 1: Console Input (C_READ)
 * Reads one character from the keyboard with echo. Blocks until a
 * character is available. Handles special control characters:
 *   Ctrl-C (03h) = warm boot
 *   Ctrl-S (13h) = pause output (wait for another key)
 * Returns the character in A. */
static void bdos_console_input(CPMachine *cpm) {
    if (!cpm->con_in) { bdos_ret8(cpm, 0x1A); return; }

    int ch = cpm->con_in(cpm->con_ctx);
    if (ch < 0) ch = 0x1A;  /* EOF → Ctrl-Z */
    ch &= 0x7F;             /* Strip to 7-bit ASCII */

    if (ch == 0x03) {
        /* Ctrl-C: warm boot. */
        cpm->warm_boot = 1;
        cpm->running = 0;
        return;
    }

    /* Echo the character to the console. */
    cpm_con_out(cpm, ch);
    bdos_ret8(cpm, ch);
}

/* Function 2: Console Output (C_WRITE)
 * Sends the character in E to the console. Tabs are expanded. */
static void bdos_console_output(CPMachine *cpm) {
    cpm_con_out(cpm, cpm->cpu.e);
}

/* Functions 3-5: Auxiliary and Printer I/O
 * These are stubs. Aux input returns Ctrl-Z (EOF), aux and printer
 * output are silently discarded. Real CP/M systems had serial ports
 * and parallel printers; we don't emulate those. */
static void bdos_aux_input(CPMachine *cpm) {
    bdos_ret8(cpm, 0x1A);  /* Ctrl-Z = EOF */
}

static void bdos_aux_output(CPMachine *cpm) { (void)cpm; }
static void bdos_list_output(CPMachine *cpm) { (void)cpm; }

/* Function 6: Direct Console I/O (C_RAWIO)
 * Raw, unbuffered console I/O that bypasses all control character
 * processing (no Ctrl-S pause, no Ctrl-C abort, no echo).
 *
 * If E = 00h-FEh: output that character directly.
 * If E = FFh:     non-blocking input. Returns the character in A,
 *                 or 00h if no character is waiting. */
static void bdos_direct_io(CPMachine *cpm) {
    if (cpm->cpu.e == 0xFF) {
        /* Input mode: non-blocking read. */
        if (cpm->con_status && cpm->con_status(cpm->con_ctx)) {
            int ch = cpm->con_in ? cpm->con_in(cpm->con_ctx) : -1;
            if (ch < 0)
                bdos_ret8(cpm, 0x00);  /* EOF → treat as no char */
            else
                bdos_ret8(cpm, ch & 0x7F);
        } else {
            bdos_ret8(cpm, 0x00);  /* No character ready */
        }
    } else {
        /* Output mode: send character directly, no tab expansion. */
        if (cpm->con_out)
            cpm->con_out(cpm->con_ctx, cpm->cpu.e);
    }
}

/* Function 7: Get I/O Byte (A_STATIN)
 * Returns the IOBYTE, which controls device assignment (which physical
 * device handles CON:, RDR:, PUN:, LST:). Most CP/M programs ignore it. */
static void bdos_get_iobyte(CPMachine *cpm) {
    bdos_ret8(cpm, cpm->iobyte);
}

/* Function 8: Set I/O Byte (A_STATOUT) */
static void bdos_set_iobyte(CPMachine *cpm) {
    cpm->iobyte = cpm->cpu.e;
    cpm->memory[CPM_IOBYTE] = cpm->iobyte;
}

/* Function 9: Print String (C_WRITESTR)
 * Outputs characters starting at address DE until a '$' (24h) delimiter
 * is found. The '$' itself is not printed. This is CP/M's printf --
 * the most common way programs display text. */
static void bdos_print_string(CPMachine *cpm) {
    uint16_t addr = (cpm->cpu.d << 8) | cpm->cpu.e;
    while (1) {
        uint8_t ch = cpm->memory[addr++];
        if (ch == '$') break;
        cpm_con_out(cpm, ch);
    }
}

/* Function 10: Read Console Buffer (C_READSTR)
 * Buffered line input with editing. The buffer at DE has:
 *   byte 0: maximum characters (set by caller)
 *   byte 1: actual characters read (set by us, excludes the CR)
 *   byte 2+: the character data
 *
 * Supports basic line editing:
 *   Ctrl-H / DEL / Backspace: delete character to the left
 *   Ctrl-C: warm boot
 *   CR or LF: accept the line
 *   Regular characters: echo and store */
static void bdos_read_buffer(CPMachine *cpm) {
    uint16_t buf = (cpm->cpu.d << 8) | cpm->cpu.e;
    uint8_t max_len = cpm->memory[buf];
    uint8_t count = 0;
    uint16_t data = buf + 2;

    if (!cpm->con_in) {
        cpm->memory[buf + 1] = 0;
        return;
    }

    while (count < max_len) {
        int ch = cpm->con_in(cpm->con_ctx);
        if (ch < 0) break;
        ch &= 0x7F;

        if (ch == '\r' || ch == '\n') {
            /* End of line. Echo CR+LF. */
            cpm_con_out(cpm, '\r');
            cpm_con_out(cpm, '\n');
            break;
        } else if (ch == 0x03) {
            /* Ctrl-C: warm boot. */
            cpm->warm_boot = 1;
            cpm->running = 0;
            return;
        } else if (ch == 0x08 || ch == 0x7F) {
            /* Backspace / DEL: erase last character. */
            if (count > 0) {
                count--;
                cpm_con_out(cpm, '\b');
                cpm_con_out(cpm, ' ');
                cpm_con_out(cpm, '\b');
            }
        } else if (ch >= 0x20) {
            /* Printable character: store and echo. */
            cpm->memory[data + count] = ch;
            count++;
            cpm_con_out(cpm, ch);
        }
    }

    cpm->memory[buf + 1] = count;
}

/* Function 11: Get Console Status (C_STAT)
 * Non-blocking check for pending console input. Returns 00h if no
 * character is ready, or FFh (some systems return 01h) if one is. */
static void bdos_console_status(CPMachine *cpm) {
    int ready = (cpm->con_status && cpm->con_status(cpm->con_ctx)) ? 0xFF : 0x00;
    bdos_ret8(cpm, ready);
}

/* Function 12: Return Version Number (S_BDOSVER)
 * Returns the CP/M version as a 16-bit value:
 *   L = version (22h for CP/M 2.2)
 *   H = system type (00h for CP/M on 8080/Z80)
 * Programs use this to check which BDOS functions are available. */
static void bdos_version(CPMachine *cpm) {
    bdos_ret16(cpm, 0x0022);  /* H=0x00 (CP/M), L=0x22 (version 2.2) */
}

/* Function 13: Reset Disk System (DRV_ALLRESET)
 * Resets the disk subsystem: all drives go to read-write state,
 * DMA address is reset to 0080h, current drive set to A:.
 * This is called by the CCP at startup. */
static void bdos_reset_disk(CPMachine *cpm) {
    cpm->dma = CPM_DMA_DEFAULT;
    /* Note: we don't change current_drive here per spec -- it says
     * "A: is re-selected" but the CCP typically sets the drive. */
}

/* Function 14: Select Disk (DRV_SET)
 * Sets the current default drive. 0=A:, 1=B:, etc. */
static void bdos_select_disk(CPMachine *cpm) {
    uint8_t drive = cpm->cpu.e;
    if (drive < CPM_MAX_DRIVES) {
        cpm->current_drive = drive;
        cpm->memory[CPM_DRIVE_USER] =
            (cpm->user_number << 4) | cpm->current_drive;
    }
}

/* Function 25: Return Current Disk (DRV_GET)
 * Returns the current default drive number (0=A:, 1=B:, ...). */
static void bdos_current_disk(CPMachine *cpm) {
    bdos_ret8(cpm, cpm->current_drive);
}

/* Function 26: Set DMA Address (F_DMAOFF)
 * Sets the address for disk read/write and search operations.
 * Default is 0080h, reset by function 13. */
static void bdos_set_dma(CPMachine *cpm) {
    cpm->dma = (cpm->cpu.d << 8) | cpm->cpu.e;
}

/* Function 32: Get/Set User Number (F_USERNUM)
 * CP/M supports 16 user areas (0-15) per drive. Each has its own files.
 *   E = FFh: return current user number in A
 *   E = 0-15: set user number */
static void bdos_user_number(CPMachine *cpm) {
    if (cpm->cpu.e == 0xFF) {
        bdos_ret8(cpm, cpm->user_number);
    } else {
        cpm->user_number = cpm->cpu.e & 0x0F;
        cpm->memory[CPM_DRIVE_USER] =
            (cpm->user_number << 4) | cpm->current_drive;
    }
}

/* ===================================================================
 * FILE SYSTEM HELPERS
 *
 * These utility functions handle the mapping between CP/M's file
 * system concepts (drives, FCBs, 8.3 names) and the host OS.
 * =================================================================== */

/* Resolve the drive byte from an FCB. Drive 0 means "default drive"
 * which we translate to the current drive. Returns 0-15. */
static int resolve_fcb_drive(CPMachine *cpm, uint16_t fcb_addr) {
    uint8_t d = cpm->memory[fcb_addr];
    if (d == 0) return cpm->current_drive;
    return (d - 1) & 0x0F;  /* 1=A, 2=B, ... */
}

/* Extract the 11-character filename from an FCB into a null-terminated
 * buffer. Masks off attribute high bits (T1/T2/T3 bit 7). */
static void fcb_get_name(CPMachine *cpm, uint16_t fcb_addr, char *name) {
    for (int i = 0; i < 11; i++)
        name[i] = cpm->memory[fcb_addr + 1 + i] & 0x7F;
    name[11] = '\0';
}

/* Case-insensitive comparison of two filenames (ASCII only). */
static int fname_casecmp(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 0x20;
        if (cb >= 'a' && cb <= 'z') cb -= 0x20;
        if (ca != cb) return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Build a host filesystem path from a CP/M drive + 11-char name.
 * Converts "HELLO   COM" to "/path/to/drive/HELLO.COM".
 * Case-insensitive: if the uppercase path doesn't exist, scans the
 * directory for a match (so "hello.com" on disk is found by "HELLO.COM").
 * Returns 0 on success, -1 if drive not mounted. */
static int make_host_path(CPMachine *cpm, int drive, const char *name11,
                          char *path, int pathsize) {
    if (drive < 0 || drive >= CPM_MAX_DRIVES || !cpm->drive[drive])
        return -1;

    /* Trim trailing spaces from name (first 8 chars) and ext (3 chars). */
    int name_end = 8;
    while (name_end > 0 && name11[name_end - 1] == ' ') name_end--;
    int ext_end = 3;
    while (ext_end > 0 && name11[8 + ext_end - 1] == ' ') ext_end--;

    char fname[14];  /* "FILENAME.EXT\0" */
    int pos = 0;
    for (int i = 0; i < name_end; i++) fname[pos++] = name11[i];
    if (ext_end > 0) {
        fname[pos++] = '.';
        for (int i = 0; i < ext_end; i++) fname[pos++] = name11[8 + i];
    }
    fname[pos] = '\0';

    snprintf(path, pathsize, "%s/%s", cpm->drive[drive], fname);

    /* If the exact (uppercase) path exists, we're done. Otherwise scan
     * the directory for a case-insensitive match. This handles host
     * filesystems where files are lowercase (e.g., "hello.com"). */
    struct stat st;
    if (stat(path, &st) == 0) return 0;

    DIR *dir = opendir(cpm->drive[drive]);
    if (!dir) return 0;  /* Can't scan -- return uppercase path as-is */

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (fname_casecmp(ent->d_name, fname) == 0) {
            snprintf(path, pathsize, "%s/%s", cpm->drive[drive], ent->d_name);
            closedir(dir);
            return 0;
        }
    }
    closedir(dir);
    return 0;  /* No match found -- return uppercase path, fopen will fail */
}

/* Convert a host filename (e.g., "HELLO.COM") to 11-char CP/M format
 * ("HELLO   COM"). Returns 0 on success, -1 if not valid 8.3. */
static int host_to_cpm_name(const char *host_name, char *name11) {
    memset(name11, ' ', 11);

    /* Skip hidden files and . / .. */
    if (host_name[0] == '.') return -1;

    /* Find the dot separating name from extension. */
    const char *dot = strrchr(host_name, '.');
    int name_len = dot ? (int)(dot - host_name) : (int)strlen(host_name);
    int ext_len = dot ? (int)strlen(dot + 1) : 0;

    /* Must fit in 8.3 format. */
    if (name_len == 0 || name_len > 8 || ext_len > 3) return -1;

    /* Copy and uppercase. */
    for (int i = 0; i < name_len; i++) {
        char c = host_name[i];
        if (c >= 'a' && c <= 'z') c -= 0x20;
        name11[i] = c;
    }
    if (dot) {
        for (int i = 0; i < ext_len; i++) {
            char c = dot[1 + i];
            if (c >= 'a' && c <= 'z') c -= 0x20;
            name11[8 + i] = c;
        }
    }
    return 0;
}

/* Match an 11-char FCB pattern against a filename. '?' matches any
 * character. Both pattern and name should be 11 chars, uppercase. */
static int match_fcb_name(const char *pattern, const char *name) {
    for (int i = 0; i < 11; i++) {
        char p = pattern[i] & 0x7F;
        char n = name[i] & 0x7F;
        if (p == '?') continue;
        if (p != n) return 0;
    }
    return 1;
}

/* Find an open file slot matching the given drive and name.
 * Returns the slot index, or -1 if not found. */
static int find_file_slot(CPMachine *cpm, int drive, const char *name) {
    for (int i = 0; i < CPM_MAX_FILES; i++) {
        if (cpm->files[i].active &&
            cpm->files[i].drive == drive &&
            memcmp(cpm->files[i].name, name, 11) == 0)
            return i;
    }
    return -1;
}

/* Allocate a free file slot. Returns the index, or -1 if all full. */
static int alloc_file_slot(CPMachine *cpm) {
    for (int i = 0; i < CPM_MAX_FILES; i++) {
        if (!cpm->files[i].active)
            return i;
    }
    return -1;
}

/* Close and free a file slot. */
static void close_file_slot(CPMachine *cpm, int idx) {
    if (idx >= 0 && idx < CPM_MAX_FILES && cpm->files[idx].active) {
        if (cpm->files[idx].fp)
            fclose((FILE *)cpm->files[idx].fp);
        cpm->files[idx].active = 0;
        cpm->files[idx].fp = NULL;
    }
}

/* Compute the file position in bytes from FCB sequential fields.
 * CP/M stores the position across three FCB fields:
 *   S2: extent high byte (each S2 unit = 524288 bytes)
 *   EX: extent low byte  (each EX unit = 16384 bytes)
 *   CR: current record    (each CR unit = 128 bytes)
 *
 * Total position = S2*524288 + EX*16384 + CR*128 */
static long fcb_seq_pos(CPMachine *cpm, uint16_t fcb_addr) {
    uint8_t cr = cpm->memory[fcb_addr + 32];
    uint8_t ex = cpm->memory[fcb_addr + 12];
    uint8_t s2 = cpm->memory[fcb_addr + 14];
    return (long)s2 * 524288L + (long)ex * 16384L + (long)cr * 128L;
}

/* Advance the sequential position in an FCB by one record (128 bytes).
 * CR goes 0..127, then wraps to 0 and EX increments, etc. */
static void fcb_advance_seq(CPMachine *cpm, uint16_t fcb_addr) {
    uint8_t cr = cpm->memory[fcb_addr + 32];
    uint8_t ex = cpm->memory[fcb_addr + 12];
    uint8_t s2 = cpm->memory[fcb_addr + 14];
    cr++;
    if (cr >= 128) {
        cr = 0;
        ex++;
        if (ex >= 32) {
            ex = 0;
            s2++;
        }
    }
    cpm->memory[fcb_addr + 32] = cr;
    cpm->memory[fcb_addr + 12] = ex;
    cpm->memory[fcb_addr + 14] = s2;
}

/* Set FCB RC (record count) for the current extent based on file size.
 * RC tells how many 128-byte records are in this extent. */
static void fcb_set_rc(CPMachine *cpm, uint16_t fcb_addr, long file_size) {
    long total_records = (file_size + 127) / 128;
    uint8_t ex = cpm->memory[fcb_addr + 12];
    uint8_t s2 = cpm->memory[fcb_addr + 14];
    long extent_num = (long)s2 * 32 + ex;
    long first_record = extent_num * 128;
    long remaining = total_records - first_record;
    if (remaining < 0) remaining = 0;
    if (remaining > 128) remaining = 128;
    cpm->memory[fcb_addr + 15] = (uint8_t)remaining;
}

/* Close the search directory handle if one is open. */
static void close_search(CPMachine *cpm) {
    if (cpm->search_dir) {
        closedir((DIR *)cpm->search_dir);
        cpm->search_dir = NULL;
    }
}

/* ===================================================================
 * BDOS FILE SYSTEM FUNCTIONS (15-23, 30, 33-36, 40)
 * =================================================================== */

/* Forward declaration: search_first delegates to search_next. */
static void bdos_search_next(CPMachine *cpm);

/* Function 15: Open File (F_OPEN)
 * Opens an existing file. The FCB at DE must have drive, filename, and
 * extension set. EX/S1/S2 should be 0. On success, fills RC and the
 * allocation map. Caller should set CR=0 for sequential access.
 *
 * Returns: A = 0 (success) or FFh (file not found). */
static void bdos_open_file(CPMachine *cpm) {
    uint16_t fcb_addr = (cpm->cpu.d << 8) | cpm->cpu.e;
    int drive = resolve_fcb_drive(cpm, fcb_addr);
    char name[12];
    fcb_get_name(cpm, fcb_addr, name);

    char path[512];
    if (make_host_path(cpm, drive, name, path, sizeof(path)) < 0) {
        bdos_ret8(cpm, 0xFF);
        return;
    }

    /* Open for read+write if possible, fall back to read-only. */
    FILE *fp = fopen(path, "r+b");
    if (!fp) fp = fopen(path, "rb");
    if (!fp) {
        bdos_ret8(cpm, 0xFF);
        return;
    }

    /* If already open, close the old handle first. */
    int slot = find_file_slot(cpm, drive, name);
    if (slot >= 0) close_file_slot(cpm, slot);

    /* Allocate a new slot. */
    slot = alloc_file_slot(cpm);
    if (slot < 0) {
        fclose(fp);
        bdos_ret8(cpm, 0xFF);
        return;
    }
    cpm->files[slot].active = 1;
    cpm->files[slot].drive = drive;
    memcpy(cpm->files[slot].name, name, 12);
    cpm->files[slot].fp = fp;

    /* Get file size for RC calculation. */
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    /* Initialize FCB extent fields. EX/S2 stay as caller set them
     * (normally 0 for a fresh open). */
    cpm->memory[fcb_addr + 13] = 0;   /* S1 = 0 */
    fcb_set_rc(cpm, fcb_addr, size);

    /* Fill allocation map (bytes 16-31) with non-zero values.
     * Programs check if these are zero to know if the file is empty.
     * We just fill with sequential block numbers. */
    for (int i = 0; i < 16; i++)
        cpm->memory[fcb_addr + 16 + i] = (i < (size > 0 ? 16 : 0)) ? (i + 1) : 0;

    bdos_ret8(cpm, 0x00);  /* Success: directory code 0 */
}

/* Function 16: Close File (F_CLOSE)
 * Flushes any pending writes and releases the file handle.
 * Returns: A = 0 (success) or FFh (error). */
static void bdos_close_file(CPMachine *cpm) {
    uint16_t fcb_addr = (cpm->cpu.d << 8) | cpm->cpu.e;
    int drive = resolve_fcb_drive(cpm, fcb_addr);
    char name[12];
    fcb_get_name(cpm, fcb_addr, name);

    int slot = find_file_slot(cpm, drive, name);
    if (slot >= 0) {
        close_file_slot(cpm, slot);
        bdos_ret8(cpm, 0x00);
    } else {
        /* Not an error if file wasn't in our table -- it may have
         * been opened and closed in a way we didn't track. */
        bdos_ret8(cpm, 0x00);
    }
}

/* Function 17: Search First (F_SFIRST)
 * Searches the directory of the specified drive for the first file
 * matching the FCB pattern. Wildcards '?' match any character.
 *
 * On success, a 32-byte directory entry is placed at DMA+0 and
 * A returns 0 (indicating slot 0 in the DMA buffer).
 * On failure, A returns FFh. */
static void bdos_search_first(CPMachine *cpm) {
    uint16_t fcb_addr = (cpm->cpu.d << 8) | cpm->cpu.e;

    /* Close any previous search. */
    close_search(cpm);

    /* Save the search pattern: byte 0 = user, bytes 1-11 = name+ext. */
    cpm->search_pattern[0] = cpm->memory[fcb_addr];  /* drive/user */
    for (int i = 0; i < 11; i++)
        cpm->search_pattern[1 + i] = cpm->memory[fcb_addr + 1 + i] & 0x7F;

    /* Resolve the drive. */
    cpm->search_drive = resolve_fcb_drive(cpm, fcb_addr);
    if (cpm->search_drive < 0 || cpm->search_drive >= CPM_MAX_DRIVES ||
        !cpm->drive[cpm->search_drive]) {
        bdos_ret8(cpm, 0xFF);
        return;
    }

    /* Open the host directory. */
    cpm->search_dir = opendir(cpm->drive[cpm->search_drive]);
    if (!cpm->search_dir) {
        bdos_ret8(cpm, 0xFF);
        return;
    }

    /* Delegate to search_next to find the first match. */
    bdos_search_next(cpm);
}

/* Function 18: Search Next (F_SNEXT)
 * Continues a search started by function 17. Iterates through the
 * host directory looking for the next file matching the pattern. */
static void bdos_search_next(CPMachine *cpm) {
    if (!cpm->search_dir) {
        bdos_ret8(cpm, 0xFF);
        return;
    }

    DIR *dir = (DIR *)cpm->search_dir;
    struct dirent *ent;
    char cpm_name[12];
    char *pattern = (char *)&cpm->search_pattern[1];  /* 11-char pattern */

    while ((ent = readdir(dir)) != NULL) {
        /* Convert host filename to CP/M 8.3 format. Skip files that
         * don't fit (hidden files, long names, etc.). */
        if (host_to_cpm_name(ent->d_name, cpm_name) < 0)
            continue;

        /* Check pattern match. */
        if (!match_fcb_name(pattern, cpm_name))
            continue;

        /* Get file size for RC/EX calculation. */
        char path[512];
        snprintf(path, sizeof(path), "%s/%s",
                 cpm->drive[cpm->search_drive], ent->d_name);
        struct stat st;
        long fsize = 0;
        if (stat(path, &st) == 0) fsize = st.st_size;

        /* Build a 32-byte directory entry at the DMA address.
         * Format matches the on-disk directory entry format. */
        uint16_t dma = cpm->dma;
        cpm->memory[dma + 0] = cpm->user_number;   /* User number */
        for (int i = 0; i < 11; i++)
            cpm->memory[dma + 1 + i] = cpm_name[i];

        /* EX and RC: for the last extent of the file. */
        long total_records = (fsize + 127) / 128;
        uint8_t ex = (total_records / 128) & 0x1F;
        uint8_t rc = total_records % 128;
        if (total_records > 0 && rc == 0) { rc = 128; if (ex > 0) ex--; }

        cpm->memory[dma + 12] = ex;     /* EX */
        cpm->memory[dma + 13] = 0;      /* S1 */
        cpm->memory[dma + 14] = 0;      /* S2 */
        cpm->memory[dma + 15] = rc;     /* RC */
        memset(&cpm->memory[dma + 16], 0, 16);  /* Allocation map */

        bdos_ret8(cpm, 0x00);  /* Found: directory code 0 */
        return;
    }

    /* End of directory: close and report not found. */
    close_search(cpm);
    bdos_ret8(cpm, 0xFF);
}

/* Function 19: Delete File (F_DELETE)
 * Deletes all files matching the FCB pattern (wildcards allowed).
 * Returns: A = 0 (at least one deleted) or FFh (none found). */
static void bdos_delete_file(CPMachine *cpm) {
    uint16_t fcb_addr = (cpm->cpu.d << 8) | cpm->cpu.e;
    int drive = resolve_fcb_drive(cpm, fcb_addr);
    char pattern[12];
    fcb_get_name(cpm, fcb_addr, pattern);

    if (drive < 0 || drive >= CPM_MAX_DRIVES || !cpm->drive[drive]) {
        bdos_ret8(cpm, 0xFF);
        return;
    }

    /* Close any open file handles for files we're about to delete. */
    DIR *dir = opendir(cpm->drive[drive]);
    if (!dir) { bdos_ret8(cpm, 0xFF); return; }

    int deleted = 0;
    struct dirent *ent;
    char cpm_name[12];
    while ((ent = readdir(dir)) != NULL) {
        if (host_to_cpm_name(ent->d_name, cpm_name) < 0) continue;
        if (!match_fcb_name(pattern, cpm_name)) continue;

        /* Close if it's open in our file table. */
        int slot = find_file_slot(cpm, drive, cpm_name);
        if (slot >= 0) close_file_slot(cpm, slot);

        /* Delete the host file. */
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", cpm->drive[drive], ent->d_name);
        if (unlink(path) == 0) deleted++;
    }
    closedir(dir);

    bdos_ret8(cpm, deleted > 0 ? 0x00 : 0xFF);
}

/* Function 20: Read Sequential (F_READ)
 * Reads 128 bytes from the file at the current sequential position
 * (determined by EX/S2/CR in the FCB) into the DMA buffer.
 *
 * Returns: A = 00h (success) or 01h (end of file). */
static void bdos_read_sequential(CPMachine *cpm) {
    uint16_t fcb_addr = (cpm->cpu.d << 8) | cpm->cpu.e;
    int drive = resolve_fcb_drive(cpm, fcb_addr);
    char name[12];
    fcb_get_name(cpm, fcb_addr, name);

    int slot = find_file_slot(cpm, drive, name);
    if (slot < 0) { bdos_ret8(cpm, 0x01); return; }

    FILE *fp = (FILE *)cpm->files[slot].fp;
    long pos = fcb_seq_pos(cpm, fcb_addr);
    fseek(fp, pos, SEEK_SET);

    /* Read 128 bytes. Pre-fill with Ctrl-Z (1Ah) which is CP/M's
     * standard EOF padding for text files. */
    uint8_t buf[128];
    memset(buf, 0x1A, 128);
    size_t n = fread(buf, 1, 128, fp);

    if (n == 0) {
        bdos_ret8(cpm, 0x01);  /* EOF */
        return;
    }

    memcpy(&cpm->memory[cpm->dma], buf, 128);
    fcb_advance_seq(cpm, fcb_addr);

    /* Update RC for the new extent if we crossed an extent boundary. */
    fseek(fp, 0, SEEK_END);
    fcb_set_rc(cpm, fcb_addr, ftell(fp));

    bdos_ret8(cpm, 0x00);
}

/* Function 21: Write Sequential (F_WRITE)
 * Writes 128 bytes from the DMA buffer to the file at the current
 * sequential position. Extends the file as needed.
 *
 * Returns: A = 00h (success), 01h (directory full), 02h (disk full). */
static void bdos_write_sequential(CPMachine *cpm) {
    uint16_t fcb_addr = (cpm->cpu.d << 8) | cpm->cpu.e;
    int drive = resolve_fcb_drive(cpm, fcb_addr);
    char name[12];
    fcb_get_name(cpm, fcb_addr, name);

    int slot = find_file_slot(cpm, drive, name);
    if (slot < 0) { bdos_ret8(cpm, 0x01); return; }

    FILE *fp = (FILE *)cpm->files[slot].fp;
    long pos = fcb_seq_pos(cpm, fcb_addr);
    fseek(fp, pos, SEEK_SET);

    size_t n = fwrite(&cpm->memory[cpm->dma], 1, 128, fp);
    fflush(fp);

    if (n < 128) {
        bdos_ret8(cpm, 0x02);  /* Disk full */
        return;
    }

    fcb_advance_seq(cpm, fcb_addr);

    /* Update RC for the new extent. */
    fseek(fp, 0, SEEK_END);
    fcb_set_rc(cpm, fcb_addr, ftell(fp));

    bdos_ret8(cpm, 0x00);
}

/* Function 22: Make File (F_MAKE)
 * Creates a new file. Does NOT check for duplicates -- the caller
 * should delete the file first if it already exists.
 *
 * Returns: A = 0 (success) or FFh (directory full). */
static void bdos_make_file(CPMachine *cpm) {
    uint16_t fcb_addr = (cpm->cpu.d << 8) | cpm->cpu.e;
    int drive = resolve_fcb_drive(cpm, fcb_addr);
    char name[12];
    fcb_get_name(cpm, fcb_addr, name);

    char path[512];
    if (make_host_path(cpm, drive, name, path, sizeof(path)) < 0) {
        bdos_ret8(cpm, 0xFF);
        return;
    }

    /* Create the file (truncate if exists, per CP/M make semantics). */
    FILE *fp = fopen(path, "w+b");
    if (!fp) {
        bdos_ret8(cpm, 0xFF);
        return;
    }

    /* Close any previous handle. */
    int slot = find_file_slot(cpm, drive, name);
    if (slot >= 0) close_file_slot(cpm, slot);

    slot = alloc_file_slot(cpm);
    if (slot < 0) {
        fclose(fp);
        bdos_ret8(cpm, 0xFF);
        return;
    }
    cpm->files[slot].active = 1;
    cpm->files[slot].drive = drive;
    memcpy(cpm->files[slot].name, name, 12);
    cpm->files[slot].fp = fp;

    /* Clear FCB fields for a new file. */
    cpm->memory[fcb_addr + 12] = 0;   /* EX */
    cpm->memory[fcb_addr + 13] = 0;   /* S1 */
    cpm->memory[fcb_addr + 14] = 0;   /* S2 */
    cpm->memory[fcb_addr + 15] = 0;   /* RC = 0 (empty file) */
    memset(&cpm->memory[fcb_addr + 16], 0, 16);  /* Alloc map */

    bdos_ret8(cpm, 0x00);
}

/* Function 23: Rename File (F_RENAME)
 * Renames a file. FCB layout for rename:
 *   bytes 0-15:  old name (byte 0 = drive, 1-11 = old filename)
 *   bytes 16-31: new name (byte 16 ignored, 17-27 = new filename)
 *
 * Returns: A = 0 (success) or FFh (not found). */
static void bdos_rename_file(CPMachine *cpm) {
    uint16_t fcb_addr = (cpm->cpu.d << 8) | cpm->cpu.e;
    int drive = resolve_fcb_drive(cpm, fcb_addr);

    /* Extract old name from bytes 1-11. */
    char old_name[12];
    for (int i = 0; i < 11; i++)
        old_name[i] = cpm->memory[fcb_addr + 1 + i] & 0x7F;
    old_name[11] = '\0';

    /* Extract new name from bytes 17-27. */
    char new_name[12];
    for (int i = 0; i < 11; i++)
        new_name[i] = cpm->memory[fcb_addr + 17 + i] & 0x7F;
    new_name[11] = '\0';

    char old_path[512], new_path[512];
    if (make_host_path(cpm, drive, old_name, old_path, sizeof(old_path)) < 0 ||
        make_host_path(cpm, drive, new_name, new_path, sizeof(new_path)) < 0) {
        bdos_ret8(cpm, 0xFF);
        return;
    }

    /* Close any open handles for the old file. */
    int slot = find_file_slot(cpm, drive, old_name);
    if (slot >= 0) close_file_slot(cpm, slot);

    if (rename(old_path, new_path) == 0)
        bdos_ret8(cpm, 0x00);
    else
        bdos_ret8(cpm, 0xFF);
}

/* Function 30: Set File Attributes (F_ATTRIB)
 * In real CP/M this updates R/O, SYS, and Archive bits in the directory.
 * We just return success since host filesystems don't map to CP/M attrs. */
static void bdos_set_attributes(CPMachine *cpm) {
    (void)cpm;
    bdos_ret8(cpm, 0x00);
}

/* Function 33: Read Random (F_READRAND)
 * Reads 128 bytes from the record specified by R0/R1/R2 in the FCB.
 * Updates EX/S2/CR to match the random position for subsequent
 * sequential I/O.
 *
 * Returns: A = 00h (ok), 01h (unwritten data), 06h (out of range). */
static void bdos_read_random(CPMachine *cpm) {
    uint16_t fcb_addr = (cpm->cpu.d << 8) | cpm->cpu.e;
    int drive = resolve_fcb_drive(cpm, fcb_addr);
    char name[12];
    fcb_get_name(cpm, fcb_addr, name);

    int slot = find_file_slot(cpm, drive, name);
    if (slot < 0) { bdos_ret8(cpm, 0x01); return; }

    /* Read the random record number from R0/R1/R2. */
    uint8_t r0 = cpm->memory[fcb_addr + 33];
    uint8_t r1 = cpm->memory[fcb_addr + 34];
    uint8_t r2 = cpm->memory[fcb_addr + 35];

    if (r2 != 0) {
        bdos_ret8(cpm, 0x06);  /* Record out of range */
        return;
    }

    long record = (long)r0 | ((long)r1 << 8);
    long pos = record * 128;

    /* Update sequential position fields so subsequent sequential I/O
     * starts from this position.
     * CR = record & 0x7F, EX = (record >> 7) & 0x1F, S2 = record >> 12 */
    cpm->memory[fcb_addr + 32] = record & 0x7F;          /* CR */
    cpm->memory[fcb_addr + 12] = (record >> 7) & 0x1F;   /* EX */
    cpm->memory[fcb_addr + 14] = (record >> 12) & 0xFF;  /* S2 */

    FILE *fp = (FILE *)cpm->files[slot].fp;
    fseek(fp, pos, SEEK_SET);

    uint8_t buf[128];
    memset(buf, 0x1A, 128);
    size_t n = fread(buf, 1, 128, fp);

    if (n == 0) {
        bdos_ret8(cpm, 0x01);  /* Reading unwritten data */
        return;
    }

    memcpy(&cpm->memory[cpm->dma], buf, 128);
    bdos_ret8(cpm, 0x00);
}

/* Function 34: Write Random (F_WRITERAND)
 * Writes 128 bytes from DMA to the record at R0/R1/R2.
 * Creates the record if it doesn't exist (file may have gaps).
 *
 * Returns: A = 00h (ok), 02h (disk full), 06h (out of range). */
static void bdos_write_random(CPMachine *cpm) {
    uint16_t fcb_addr = (cpm->cpu.d << 8) | cpm->cpu.e;
    int drive = resolve_fcb_drive(cpm, fcb_addr);
    char name[12];
    fcb_get_name(cpm, fcb_addr, name);

    int slot = find_file_slot(cpm, drive, name);
    if (slot < 0) { bdos_ret8(cpm, 0x05); return; }

    uint8_t r0 = cpm->memory[fcb_addr + 33];
    uint8_t r1 = cpm->memory[fcb_addr + 34];
    uint8_t r2 = cpm->memory[fcb_addr + 35];

    if (r2 != 0) {
        bdos_ret8(cpm, 0x06);
        return;
    }

    long record = (long)r0 | ((long)r1 << 8);
    long pos = record * 128;

    /* Update sequential position fields. */
    cpm->memory[fcb_addr + 32] = record & 0x7F;
    cpm->memory[fcb_addr + 12] = (record >> 7) & 0x1F;
    cpm->memory[fcb_addr + 14] = (record >> 12) & 0xFF;

    FILE *fp = (FILE *)cpm->files[slot].fp;
    fseek(fp, pos, SEEK_SET);

    size_t n = fwrite(&cpm->memory[cpm->dma], 1, 128, fp);
    fflush(fp);

    if (n < 128) {
        bdos_ret8(cpm, 0x02);
        return;
    }

    bdos_ret8(cpm, 0x00);
}

/* Function 35: Compute File Size (F_SIZE)
 * Sets R0/R1/R2 in the FCB to the file size in 128-byte records.
 * The file does not need to be open. */
static void bdos_file_size(CPMachine *cpm) {
    uint16_t fcb_addr = (cpm->cpu.d << 8) | cpm->cpu.e;
    int drive = resolve_fcb_drive(cpm, fcb_addr);
    char name[12];
    fcb_get_name(cpm, fcb_addr, name);

    char path[512];
    if (make_host_path(cpm, drive, name, path, sizeof(path)) < 0) {
        cpm->memory[fcb_addr + 33] = 0;
        cpm->memory[fcb_addr + 34] = 0;
        cpm->memory[fcb_addr + 35] = 0;
        return;
    }

    struct stat st;
    long records = 0;
    if (stat(path, &st) == 0)
        records = (st.st_size + 127) / 128;

    cpm->memory[fcb_addr + 33] = records & 0xFF;         /* R0 */
    cpm->memory[fcb_addr + 34] = (records >> 8) & 0xFF;  /* R1 */
    cpm->memory[fcb_addr + 35] = (records >> 16) & 0xFF; /* R2 */
}

/* Function 36: Set Random Record (F_RANDREC)
 * Converts the current sequential position (EX/S2/CR) to a random
 * record number in R0/R1/R2. Used by programs that switch between
 * sequential and random I/O. */
static void bdos_set_random_record(CPMachine *cpm) {
    uint16_t fcb_addr = (cpm->cpu.d << 8) | cpm->cpu.e;
    uint8_t cr = cpm->memory[fcb_addr + 32];
    uint8_t ex = cpm->memory[fcb_addr + 12];
    uint8_t s2 = cpm->memory[fcb_addr + 14];

    long record = (long)s2 * 4096 + (long)ex * 128 + cr;
    cpm->memory[fcb_addr + 33] = record & 0xFF;
    cpm->memory[fcb_addr + 34] = (record >> 8) & 0xFF;
    cpm->memory[fcb_addr + 35] = (record >> 16) & 0xFF;
}

/* Function 40: Write Random with Zero Fill (F_WRITEZF)
 * Same as function 34, but any newly allocated blocks are zeroed.
 * Since we use host files (which handle sparse allocation), this
 * behaves identically to function 34 for us. */
static void bdos_write_random_zero(CPMachine *cpm) {
    bdos_write_random(cpm);  /* Same behavior on host filesystem */
}

/* ===================================================================
 * BDOS DISPATCHER
 *
 * This is the central dispatch for all BDOS system calls. It reads
 * the function number from register C and calls the appropriate handler.
 * =================================================================== */

void cpm_bdos(CPMachine *cpm) {
    uint8_t func = cpm->cpu.c;

    switch (func) {
        case 0:  bdos_system_reset(cpm);    break;
        case 1:  bdos_console_input(cpm);   break;
        case 2:  bdos_console_output(cpm);  break;
        case 3:  bdos_aux_input(cpm);       break;
        case 4:  bdos_aux_output(cpm);      break;
        case 5:  bdos_list_output(cpm);     break;
        case 6:  bdos_direct_io(cpm);       break;
        case 7:  bdos_get_iobyte(cpm);      break;
        case 8:  bdos_set_iobyte(cpm);      break;
        case 9:  bdos_print_string(cpm);    break;
        case 10: bdos_read_buffer(cpm);     break;
        case 11: bdos_console_status(cpm);  break;
        case 12: bdos_version(cpm);         break;
        case 13: bdos_reset_disk(cpm);      break;
        case 14: bdos_select_disk(cpm);     break;
        case 25: bdos_current_disk(cpm);    break;
        case 26: bdos_set_dma(cpm);         break;
        case 32: bdos_user_number(cpm);     break;

        case 15: bdos_open_file(cpm);         break;
        case 16: bdos_close_file(cpm);        break;
        case 17: bdos_search_first(cpm);      break;
        case 18: bdos_search_next(cpm);       break;
        case 19: bdos_delete_file(cpm);       break;
        case 20: bdos_read_sequential(cpm);   break;
        case 21: bdos_write_sequential(cpm);  break;
        case 22: bdos_make_file(cpm);         break;
        case 23: bdos_rename_file(cpm);       break;
        case 30: bdos_set_attributes(cpm);    break;
        case 33: bdos_read_random(cpm);       break;
        case 34: bdos_write_random(cpm);      break;
        case 35: bdos_file_size(cpm);         break;
        case 36: bdos_set_random_record(cpm); break;
        case 40: bdos_write_random_zero(cpm); break;

        case 24: {  /* Login vector: bitmap of mounted drives */
            uint16_t vec = 0;
            for (int i = 0; i < CPM_MAX_DRIVES; i++)
                if (cpm->drive[i]) vec |= (1 << i);
            bdos_ret16(cpm, vec);
            break;
        }
        case 27:  /* Alloc vector address */
            bdos_ret16(cpm, CPM_ALV_ADDR);
            break;
        case 28:  /* Write protect disk -- no-op */
            break;
        case 29:  /* R/O vector */
            bdos_ret16(cpm, 0x0000);  /* No drives are read-only */
            break;
        case 31:  /* Get DPB address */
            bdos_ret16(cpm, CPM_DPB_ADDR);
            break;
        case 37:  /* Reset drive */
            bdos_ret8(cpm, 0x00);
            break;

        default:
            /* Unknown function: silently ignore. */
            break;
    }

    /* Pop the return address and resume execution.
     * The CALL 0005h pushed a return address, then the JP instruction
     * at 0005h brought us here. We need to pop that return address
     * so the program continues after its CALL 0005h instruction.
     *
     * Exception: function 0 (system reset) doesn't return -- we've
     * already set warm_boot and stopped the loop. */
    if (func != 0) {
        bdos_return(cpm);
    }
}

/* ===================================================================
 * BIOS FUNCTION HANDLER
 *
 * Programs like MBASIC bypass the BDOS for console I/O by reading the
 * BIOS jump table at F200h and calling the handler addresses directly.
 * This is faster because it skips the BDOS overhead. The 17 BIOS
 * functions are:
 *
 *   0: BOOT    Cold start (→ warm boot for us)
 *   1: WBOOT   Warm start (reload CCP)
 *   2: CONST   Console status: A=FFh if char ready, 00h if not
 *   3: CONIN   Console input: blocks until char ready, returns in A
 *   4: CONOUT  Console output: character in C register
 *   5: LIST    Printer output: character in C (stub)
 *   6: PUNCH   Paper tape punch: character in C (stub)
 *   7: READER  Paper tape reader: returns char in A (stub, returns EOF)
 *   8: HOME    Seek to track 0 (no-op)
 *   9: SELDSK  Select disk: C=drive, returns HL=DPH or 0 if invalid
 *  10: SETTRK  Set track number: BC=track (no-op)
 *  11: SETSEC  Set sector number: BC=sector (no-op)
 *  12: SETDMA  Set DMA address: BC=address (no-op at BIOS level)
 *  13: READ    Read sector (stub, returns success)
 *  14: WRITE   Write sector (stub, returns success)
 *  15: LISTST  List device status: A=FFh (always ready)
 *  16: SECTRAN Sector translation: BC=logical, DE=table, HL=physical
 * =================================================================== */

static void cpm_bios(CPMachine *cpm, int func) {
    switch (func) {
        case 0:  /* BOOT (cold start) */
        case 1:  /* WBOOT (warm start) */
            cpm->warm_boot = 1;
            cpm->running = 0;
            return;  /* Don't pop return address -- execution stops */

        case 2:  /* CONST -- console status.
                  * Returns A=FFh if a character is waiting, 00h if not.
                  * Many programs poll this in a tight loop. */
            cpm->cpu.a = (cpm->con_status &&
                          cpm->con_status(cpm->con_ctx)) ? 0xFF : 0x00;
            break;

        case 3:  /* CONIN -- console input (blocking).
                  * Waits for a keypress and returns it in A with bit 7
                  * stripped (standard ASCII). */
            if (cpm->con_in) {
                int ch = cpm->con_in(cpm->con_ctx);
                cpm->cpu.a = (ch < 0) ? 0x1A : (ch & 0x7F);
            } else {
                cpm->cpu.a = 0x1A;  /* No input → EOF (Ctrl-Z) */
            }
            break;

        case 4:  /* CONOUT -- console output.
                  * Character to print is in register C. No tab expansion
                  * at the BIOS level (that's a BDOS responsibility). */
            if (cpm->con_out)
                cpm->con_out(cpm->con_ctx, cpm->cpu.c);
            break;

        case 5:  /* LIST -- printer output (stub, silently ignore) */
        case 6:  /* PUNCH -- paper tape punch (stub, silently ignore) */
            break;

        case 7:  /* READER -- paper tape reader.
                  * Returns character in A. We always return EOF. */
            cpm->cpu.a = 0x1A;
            break;

        case 8:  /* HOME -- seek to track 0 (no-op for us) */
            break;

        case 9:  /* SELDSK -- select disk.
                  * C = disk number (0=A, 1=B, ...).
                  * Returns HL = address of 16-byte DPH, or HL=0 if the
                  * disk is not valid. We return our single shared DPH
                  * for any mounted drive. */
            if (cpm->cpu.c < CPM_MAX_DRIVES &&
                cpm->drive[cpm->cpu.c]) {
                cpm->cpu.h = CPM_DPH_ADDR >> 8;
                cpm->cpu.l = CPM_DPH_ADDR & 0xFF;
            } else {
                cpm->cpu.h = 0;
                cpm->cpu.l = 0;
            }
            break;

        case 10: /* SETTRK -- set track number (no-op) */
        case 11: /* SETSEC -- set sector number (no-op) */
        case 12: /* SETDMA -- set DMA address (no-op at BIOS level) */
            break;

        case 13: /* READ -- read disk sector (stub, return success) */
        case 14: /* WRITE -- write disk sector (stub, return success) */
            cpm->cpu.a = 0;
            break;

        case 15: /* LISTST -- list device status (always ready) */
            cpm->cpu.a = 0xFF;
            break;

        case 16: /* SECTRAN -- sector translation.
                  * BC = logical sector, DE = translate table address.
                  * Returns HL = physical sector. We use identity (no
                  * translation needed for our virtual disk). */
            cpm->cpu.h = cpm->cpu.b;
            cpm->cpu.l = cpm->cpu.c;
            break;

        default:
            break;
    }

    /* Pop the return address from the stack and resume, just like
     * bdos_return does. The program CALLed the BIOS entry, which JP'd
     * to our trap address. The CALL pushed the return address. */
    bdos_return(cpm);
}

/* ===================================================================
 * EXECUTION ENGINE
 * =================================================================== */

void cpm_step(CPMachine *cpm) {
    /* Check for trap addresses BEFORE executing the instruction.
     * This way we intercept the call without running whatever is
     * at the trap address in memory. */
    uint16_t pc = cpm->cpu.pc;

    if (pc == CPM_BDOS_TRAP) {
        /* BDOS system call. The Z80 got here via:
         *   CALL 0005h  →  JP E406h  →  we intercept here */
        cpm_bdos(cpm);
        return;
    }

    if (pc == CPM_WBOOT_TRAP) {
        /* Warm boot. The Z80 got here via:
         *   JP 0000h  →  JP F203h  →  we intercept here
         * (or via RET to 0000h → JP F203h)
         * Note: F203 now contains JP F243 (BIOS WBOOT trap), but we
         * intercept here before it executes. */
        cpm->warm_boot = 1;
        cpm->running = 0;
        return;
    }

    /* BIOS direct call traps. Programs like MBASIC read the BIOS jump
     * table at F200h to extract handler addresses (the JP targets at
     * F240+), then CALL those addresses directly for fast console I/O.
     * We intercept when PC lands on any of these trap targets. */
    if (pc >= CPM_BIOS_TRAP_BASE &&
        pc < CPM_BIOS_TRAP_BASE + CPM_BIOS_NFUNCS * 3) {
        int offset = pc - CPM_BIOS_TRAP_BASE;
        if (offset % 3 == 0) {
            cpm_bios(cpm, offset / 3);
            return;
        }
    }

    /* Normal Z80 instruction execution. */
    z80_step(&cpm->cpu);
}

int cpm_run(CPMachine *cpm) {
    cpm->running = 1;
    cpm->warm_boot = 0;

    while (cpm->running && !cpm->cpu.halted) {
        cpm_step(cpm);
    }

    return cpm->warm_boot;
}

/* ===================================================================
 * .COM FILE LOADING
 * =================================================================== */

int cpm_load_com(CPMachine *cpm, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    /* .COM files load at 0100h and can fill up to the top of the TPA. */
    size_t max_size = CPM_TPA_TOP - CPM_TPA_BASE;
    size_t n = fread(&cpm->memory[CPM_TPA_BASE], 1, max_size, f);
    fclose(f);

    if (n == 0) return -1;

    /* Set up the stack just below the CCP area.
     * Push 0x0000 as the return address: when the program does RET,
     * it goes to 0x0000, which has JP F203h (warm boot trap). */
    cpm->cpu.sp = CPM_CCP_BASE;
    cpm->cpu.sp -= 2;
    cpm->memory[cpm->cpu.sp]     = 0x00;
    cpm->memory[cpm->cpu.sp + 1] = 0x00;

    /* Entry point. */
    cpm->cpu.pc = CPM_TPA_BASE;

    /* Reset DMA to default. */
    cpm->dma = CPM_DMA_DEFAULT;

    return 0;
}

/* ===================================================================
 * FCB AND COMMAND TAIL SETUP
 *
 * When the CCP runs a .COM program, it parses the command line to fill:
 *   005Ch: FCB #1 (first filename argument)
 *   006Ch: FCB #2 (second filename argument)
 *   0080h: command tail (length byte + the raw argument text)
 *
 * FCB format (first 16 bytes):
 *   byte 0:    drive code (0 = default, 1 = A:, 2 = B:, ...)
 *   bytes 1-8: filename, uppercase, space-padded
 *   bytes 9-11: extension, uppercase, space-padded
 *   bytes 12-15: EX, S1, S2, RC (all zeroed)
 * =================================================================== */

/* Parse one filename from the command line into an FCB.
 * Returns a pointer to the next position in the command line. */
static const char *parse_fcb(const char *cmd, uint8_t *fcb) {
    /* Initialize FCB to blank. */
    fcb[0] = 0;  /* Default drive */
    memset(fcb + 1, ' ', 11);   /* Blank filename + extension */
    memset(fcb + 12, 0, 4);     /* EX, S1, S2, RC = 0 */

    /* Skip leading spaces. */
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') return cmd;

    /* Check for drive prefix (e.g., "B:"). */
    if (cmd[1] == ':' && ((cmd[0] >= 'A' && cmd[0] <= 'P') ||
                          (cmd[0] >= 'a' && cmd[0] <= 'p'))) {
        fcb[0] = (cmd[0] & 0xDF) - 'A' + 1;  /* 1=A, 2=B, ... */
        cmd += 2;
    }

    /* Parse filename (up to 8 characters). */
    int i = 0;
    while (*cmd && *cmd != ' ' && *cmd != '.' && i < 8) {
        uint8_t ch = *cmd++;
        if (ch >= 'a' && ch <= 'z') ch -= 0x20;  /* Uppercase */
        if (ch == '*') {
            /* Star wildcard: fill remaining with '?'. */
            while (i < 8) fcb[1 + i++] = '?';
            break;
        }
        fcb[1 + i++] = ch;
    }

    /* Skip excess filename characters. */
    while (*cmd && *cmd != ' ' && *cmd != '.') cmd++;

    /* Parse extension (up to 3 characters). */
    if (*cmd == '.') {
        cmd++;
        i = 0;
        while (*cmd && *cmd != ' ' && i < 3) {
            uint8_t ch = *cmd++;
            if (ch >= 'a' && ch <= 'z') ch -= 0x20;
            if (ch == '*') {
                while (i < 3) fcb[9 + i++] = '?';
                break;
            }
            fcb[9 + i++] = ch;
        }
        /* Skip excess extension characters. */
        while (*cmd && *cmd != ' ') cmd++;
    }

    return cmd;
}

void cpm_setup_fcbs(CPMachine *cpm, const char *cmdline) {
    if (!cmdline) cmdline = "";

    /* Parse first argument into FCB at 005Ch. */
    uint8_t fcb1[16], fcb2[16];
    const char *rest = parse_fcb(cmdline, fcb1);

    /* Parse second argument into FCB at 006Ch. */
    parse_fcb(rest, fcb2);

    /* Copy FCBs into Z80 memory.
     * Note: FCB2 at 006Ch overlaps bytes 16-31 of FCB1. This is by
     * design in CP/M -- programs must copy FCB2 before opening FCB1. */
    memcpy(&cpm->memory[CPM_FCB1], fcb1, 16);
    memset(&cpm->memory[CPM_FCB1 + 16], 0, 16);  /* Clear allocation map */
    cpm->memory[CPM_FCB1 + 32] = 0;              /* CR = 0 */
    memcpy(&cpm->memory[CPM_FCB2], fcb2, 16);

    /* Set up the command tail at 0080h.
     * Byte at 0080h = length, 0081h onward = the tail text.
     * The tail includes the leading space before arguments. */
    int tail_len = 0;
    if (cmdline[0]) {
        /* Include the full command tail with leading space. */
        const char *tail = cmdline;
        /* Skip past the command name (already handled by CCP). */
        tail_len = strlen(tail);
        if (tail_len > 127) tail_len = 127;
        cpm->memory[0x0080] = tail_len;
        memcpy(&cpm->memory[0x0081], tail, tail_len);
    } else {
        cpm->memory[0x0080] = 0;
    }
}

/* ===================================================================
 * RESOURCE CLEANUP
 * =================================================================== */

void cpm_cleanup(CPMachine *cpm) {
    /* Close all open files. */
    for (int i = 0; i < CPM_MAX_FILES; i++) {
        if (cpm->files[i].active)
            close_file_slot(cpm, i);
    }
    /* Close any open search directory. */
    close_search(cpm);
}

/* ===================================================================
 * CCP (Console Command Processor)
 *
 * The CCP is CP/M's command-line shell. It prints a prompt ("A>"),
 * reads commands, executes built-in commands (DIR, ERA, TYPE, REN,
 * USER, SAVE), and loads .COM files for transient commands.
 *
 * Unlike real CP/M where the CCP is Z80 code living at DC00h, ours
 * is implemented entirely in C. This means it can't be overwritten
 * by large programs, and doesn't need reloading from disk.
 *
 * The CCP uses the console I/O callbacks directly (not through the
 * Z80 BDOS path) since it runs as native C code.
 * =================================================================== */

/* --- CCP Console Helpers --- */

/* Print a string to the console. Uses cpm_con_out for tab expansion. */
static void ccp_print(CPMachine *cpm, const char *s) {
    while (*s) cpm_con_out(cpm, (uint8_t)*s++);
}

/* Read a line from the console with backspace editing.
 * Input is uppercased as typed (CP/M convention).
 * Returns character count, or -1 on EOF/Ctrl-C (signals CCP exit). */
static int ccp_readline(CPMachine *cpm, char *buf, int max) {
    int len = 0;
    while (len < max - 1) {
        if (!cpm->con_in) return -1;
        int ch = cpm->con_in(cpm->con_ctx);
        if (ch < 0) return -1;       /* EOF */
        ch &= 0x7F;

        if (ch == '\r' || ch == '\n') {
            ccp_print(cpm, "\r\n");
            break;
        }
        if (ch == 0x03) return -1;    /* Ctrl-C: exit CCP */
        if (ch == 0x08 || ch == 0x7F) {
            /* Backspace: erase last character. */
            if (len > 0) { len--; ccp_print(cpm, "\b \b"); }
            continue;
        }
        if (ch >= 0x20) {
            if (ch >= 'a' && ch <= 'z') ch -= 0x20;  /* Uppercase */
            buf[len++] = ch;
            cpm_con_out(cpm, ch);
        }
    }
    buf[len] = '\0';
    return len;
}

/* --- Built-in: DIR --- */

/* List directory entries matching a pattern. Default is *.* (all files).
 * Output format: 4 entries per line, "A: FILENAME TYP : FILENAME TYP". */
static void ccp_dir(CPMachine *cpm, const char *args) {
    /* Parse optional filespec into a pattern. */
    char pattern[12];
    memset(pattern, '?', 11);  /* Default: match everything */
    pattern[11] = '\0';
    int drive = cpm->current_drive;

    while (*args == ' ') args++;

    if (*args) {
        /* Parse drive prefix. */
        const char *p = args;
        if (p[1] == ':' && p[0] >= 'A' && p[0] <= 'P') {
            drive = p[0] - 'A';
            p += 2;
        }

        /* Parse filename into pattern. */
        memset(pattern, ' ', 11);
        int i = 0;
        while (*p && *p != ' ' && *p != '.' && i < 8) {
            if (*p == '*') { while (i < 8) pattern[i++] = '?'; p++; break; }
            char c = *p++;
            if (c >= 'a' && c <= 'z') c -= 0x20;
            pattern[i++] = c;
        }
        while (*p && *p != ' ' && *p != '.') p++;

        if (*p == '.') {
            p++;
            i = 0;
            while (*p && *p != ' ' && i < 3) {
                if (*p == '*') { while (i < 3) pattern[8 + i++] = '?'; p++; break; }
                char c = *p++;
                if (c >= 'a' && c <= 'z') c -= 0x20;
                pattern[8 + i++] = c;
            }
        } else {
            /* No extension specified: match all extensions. */
            memset(pattern + 8, '?', 3);
        }
    }

    if (drive < 0 || drive >= CPM_MAX_DRIVES || !cpm->drive[drive]) {
        ccp_print(cpm, "NO FILE\r\n");
        return;
    }

    DIR *dir = opendir(cpm->drive[drive]);
    if (!dir) { ccp_print(cpm, "NO FILE\r\n"); return; }

    int count = 0;
    struct dirent *ent;
    char cpm_name[12];

    while ((ent = readdir(dir)) != NULL) {
        if (host_to_cpm_name(ent->d_name, cpm_name) < 0) continue;
        if (!match_fcb_name(pattern, cpm_name)) continue;

        /* Print 4 entries per line. */
        if (count % 4 == 0) {
            if (count > 0) ccp_print(cpm, "\r\n");
            cpm_con_out(cpm, 'A' + drive);
            ccp_print(cpm, ": ");
        } else {
            ccp_print(cpm, " : ");
        }

        /* "FILENAME TYP" */
        for (int i = 0; i < 8; i++) cpm_con_out(cpm, cpm_name[i]);
        cpm_con_out(cpm, ' ');
        for (int i = 0; i < 3; i++) cpm_con_out(cpm, cpm_name[8 + i]);
        count++;
    }
    closedir(dir);

    if (count == 0) ccp_print(cpm, "NO FILE");
    ccp_print(cpm, "\r\n");
}

/* --- Built-in: ERA --- */

/* Erase (delete) files matching a pattern. Wildcards OK.
 * Prompts "ALL (Y/N)?" for *.* */
static void ccp_era(CPMachine *cpm, const char *args) {
    while (*args == ' ') args++;
    if (!*args) { ccp_print(cpm, "NO FILE\r\n"); return; }

    uint8_t fcb[36];
    parse_fcb(args, fcb);

    int drive = fcb[0] ? (fcb[0] - 1) : cpm->current_drive;
    if (drive < 0 || drive >= CPM_MAX_DRIVES || !cpm->drive[drive]) {
        ccp_print(cpm, "NO FILE\r\n");
        return;
    }

    /* Check for *.* (all wildcards): prompt for confirmation. */
    int all_wild = 1;
    for (int i = 1; i <= 11; i++)
        if (fcb[i] != '?') { all_wild = 0; break; }

    if (all_wild) {
        ccp_print(cpm, "ALL (Y/N)?");
        if (!cpm->con_in) return;
        int ch = cpm->con_in(cpm->con_ctx);
        ccp_print(cpm, "\r\n");
        if (ch < 0 || ((ch & 0xDF) != 'Y')) return;
    }

    DIR *dir = opendir(cpm->drive[drive]);
    if (!dir) { ccp_print(cpm, "NO FILE\r\n"); return; }

    int deleted = 0;
    struct dirent *ent;
    char cpm_name[12];
    char *pattern = (char *)&fcb[1];  /* 11-char pattern */

    while ((ent = readdir(dir)) != NULL) {
        if (host_to_cpm_name(ent->d_name, cpm_name) < 0) continue;
        if (!match_fcb_name(pattern, cpm_name)) continue;

        /* Close if open, then delete. */
        int slot = find_file_slot(cpm, drive, cpm_name);
        if (slot >= 0) close_file_slot(cpm, slot);

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", cpm->drive[drive], ent->d_name);
        if (unlink(path) == 0) deleted++;
    }
    closedir(dir);

    if (deleted == 0) ccp_print(cpm, "NO FILE\r\n");
}

/* --- Built-in: TYPE --- */

/* Display the contents of a text file. Stops at Ctrl-Z (1Ah) or EOF. */
static void ccp_type(CPMachine *cpm, const char *args) {
    while (*args == ' ') args++;
    if (!*args) return;

    uint8_t fcb[36];
    parse_fcb(args, fcb);

    int drive = fcb[0] ? (fcb[0] - 1) : cpm->current_drive;
    char name[12];
    memcpy(name, &fcb[1], 11);
    name[11] = '\0';

    char path[512];
    if (make_host_path(cpm, drive, name, path, sizeof(path)) < 0) {
        ccp_print(cpm, "NO FILE\r\n");
        return;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) { ccp_print(cpm, "NO FILE\r\n"); return; }

    int ch;
    while ((ch = fgetc(fp)) != EOF) {
        if (ch == 0x1A) break;  /* Ctrl-Z: CP/M text EOF marker */
        cpm_con_out(cpm, (uint8_t)ch);
    }
    fclose(fp);
}

/* --- Built-in: REN --- */

/* Rename a file: REN NEWNAME.TYP=OLDNAME.TYP */
static void ccp_ren(CPMachine *cpm, const char *args) {
    while (*args == ' ') args++;

    /* Find the '=' separator between new and old names. */
    const char *eq = strchr(args, '=');
    if (!eq) { ccp_print(cpm, "NO FILE\r\n"); return; }

    /* Parse new name (left of =). */
    char new_str[64];
    int nlen = (int)(eq - args);
    if (nlen >= (int)sizeof(new_str)) nlen = sizeof(new_str) - 1;
    memcpy(new_str, args, nlen);
    new_str[nlen] = '\0';

    /* Parse old name (right of =). */
    const char *old_str = eq + 1;
    while (*old_str == ' ') old_str++;

    uint8_t new_fcb[36], old_fcb[36];
    parse_fcb(new_str, new_fcb);
    parse_fcb(old_str, old_fcb);

    int drive = old_fcb[0] ? (old_fcb[0] - 1) : cpm->current_drive;
    char old_name[12], new_name[12];
    memcpy(old_name, &old_fcb[1], 11); old_name[11] = '\0';
    memcpy(new_name, &new_fcb[1], 11); new_name[11] = '\0';

    char old_path[512], new_path[512];
    if (make_host_path(cpm, drive, old_name, old_path, sizeof(old_path)) < 0 ||
        make_host_path(cpm, drive, new_name, new_path, sizeof(new_path)) < 0) {
        ccp_print(cpm, "NO FILE\r\n");
        return;
    }

    /* Check if new name already exists. */
    struct stat st;
    if (stat(new_path, &st) == 0) {
        ccp_print(cpm, "FILE EXISTS\r\n");
        return;
    }
    if (stat(old_path, &st) != 0) {
        ccp_print(cpm, "NO FILE\r\n");
        return;
    }

    rename(old_path, new_path);
}

/* --- Built-in: USER --- */

/* Set the current user number (0-15). */
static void ccp_user(CPMachine *cpm, const char *args) {
    while (*args == ' ') args++;
    int n = atoi(args);
    if (n >= 0 && n <= 15) {
        cpm->user_number = n;
        cpm->memory[CPM_DRIVE_USER] =
            (cpm->user_number << 4) | cpm->current_drive;
    }
}

/* --- Built-in: SAVE --- */

/* Save n pages (256 bytes each) from the TPA to a file.
 * Usage: SAVE n FILENAME.TYP */
static void ccp_save(CPMachine *cpm, const char *args) {
    while (*args == ' ') args++;

    /* Parse page count (decimal number). */
    int pages = 0;
    while (*args >= '0' && *args <= '9')
        pages = pages * 10 + (*args++ - '0');
    while (*args == ' ') args++;

    if (pages <= 0 || !*args) return;

    uint8_t fcb[36];
    parse_fcb(args, fcb);

    int drive = fcb[0] ? (fcb[0] - 1) : cpm->current_drive;
    char name[12];
    memcpy(name, &fcb[1], 11);
    name[11] = '\0';

    char path[512];
    if (make_host_path(cpm, drive, name, path, sizeof(path)) < 0) {
        ccp_print(cpm, "NO SPACE\r\n");
        return;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) { ccp_print(cpm, "NO SPACE\r\n"); return; }

    int bytes = pages * 256;
    if (bytes > CPM_TPA_TOP - CPM_TPA_BASE)
        bytes = CPM_TPA_TOP - CPM_TPA_BASE;

    fwrite(&cpm->memory[CPM_TPA_BASE], 1, bytes, fp);
    fclose(fp);
}

/* --- .COM File Loader ---
 *
 * When a command isn't built-in, the CCP tries to load COMMAND.COM
 * from the specified (or current) drive, set up FCBs and the command
 * tail, and run the program via the Z80 engine. */

static int ccp_load_com(CPMachine *cpm, const char *cmd_name, int cmd_drive,
                        const char *args) {
    /* Build the 11-char CP/M filename: command name + COM extension. */
    char name[12];
    memset(name, ' ', 11);
    for (int i = 0; i < 8 && cmd_name[i]; i++) {
        char c = cmd_name[i];
        if (c >= 'a' && c <= 'z') c -= 0x20;
        name[i] = c;
    }
    name[8] = 'C'; name[9] = 'O'; name[10] = 'M';
    name[11] = '\0';

    int drive = (cmd_drive >= 0) ? cmd_drive : cpm->current_drive;

    char path[512];
    if (make_host_path(cpm, drive, name, path, sizeof(path)) < 0)
        return 0;

    if (cpm_load_com(cpm, path) < 0)
        return 0;

    /* Set up FCBs and command tail from the argument portion. */
    while (*args == ' ') args++;
    cpm_setup_fcbs(cpm, args);

    cpm->dma = CPM_DMA_DEFAULT;

    /* Run the Z80 program. It returns on warm boot (RET to 0000h),
     * BDOS function 0, or HALT. */
    cpm_run(cpm);

    /* Clean up after the program: close any leaked file handles,
     * restore page zero vectors, reset DMA. */
    for (int i = 0; i < CPM_MAX_FILES; i++)
        if (cpm->files[i].active)
            close_file_slot(cpm, i);
    close_search(cpm);

    cpm->dma = CPM_DMA_DEFAULT;
    cpm_setup_page_zero(cpm);

    return 1;
}

/* --- Main CCP Loop --- */

void cpm_ccp(CPMachine *cpm) {
    /* Initialize: reset DMA and page zero. */
    cpm->dma = CPM_DMA_DEFAULT;
    cpm_setup_page_zero(cpm);

    char line[128];
    while (1) {
        /* Print prompt: CR/LF + drive letter + ">". */
        ccp_print(cpm, "\r\n");
        cpm_con_out(cpm, 'A' + cpm->current_drive);
        cpm_con_out(cpm, '>');

        /* Read command line. */
        int len = ccp_readline(cpm, line, sizeof(line));
        if (len < 0) break;     /* EOF or Ctrl-C: exit CCP */
        if (len == 0) continue;  /* Empty line: re-prompt */

        char *p = line;
        while (*p == ' ') p++;
        if (!*p) continue;

        /* Bare drive switch? E.g., "B:" with nothing after it. */
        if (p[0] >= 'A' && p[0] <= 'P' && p[1] == ':' &&
            (p[2] == '\0' || p[2] == ' ')) {
            int d = p[0] - 'A';
            if (d >= 0 && d < CPM_MAX_DRIVES && cpm->drive[d]) {
                cpm->current_drive = d;
                cpm->memory[CPM_DRIVE_USER] =
                    (cpm->user_number << 4) | cpm->current_drive;
            }
            continue;
        }

        /* Check for drive prefix on command (e.g., "B:PROGRAM"). */
        int cmd_drive = -1;
        if (p[1] == ':' && p[0] >= 'A' && p[0] <= 'P') {
            cmd_drive = p[0] - 'A';
            p += 2;
        }

        /* Extract the command name (up to space, dot, or end of line).
         * The extension (if any) is ignored for command dispatch. */
        char cmd_name[9];
        int ci = 0;
        while (*p && *p != ' ' && *p != '.' && ci < 8)
            cmd_name[ci++] = *p++;
        cmd_name[ci] = '\0';
        while (*p && *p != ' ') p++;  /* Skip past any extension */

        /* Everything after the command name is the argument string. */
        char *args = p;

        /* Built-in commands are only recognized without a drive prefix.
         * "A:DIR" tries to load DIR.COM, not run built-in DIR. */
        if (cmd_drive < 0) {
            if (strcmp(cmd_name, "DIR") == 0)  { ccp_dir(cpm, args);  continue; }
            if (strcmp(cmd_name, "ERA") == 0)  { ccp_era(cpm, args);  continue; }
            if (strcmp(cmd_name, "TYPE") == 0) { ccp_type(cpm, args); continue; }
            if (strcmp(cmd_name, "REN") == 0)  { ccp_ren(cpm, args);  continue; }
            if (strcmp(cmd_name, "USER") == 0) { ccp_user(cpm, args); continue; }
            if (strcmp(cmd_name, "SAVE") == 0) { ccp_save(cpm, args); continue; }
        }

        /* Not a built-in: try loading COMMAND.COM from disk. */
        if (!ccp_load_com(cpm, cmd_name, cmd_drive, args)) {
            ccp_print(cpm, cmd_name);
            ccp_print(cpm, "?\r\n");
        }
    }
}

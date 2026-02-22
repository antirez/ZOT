/* cpm.h -- CP/M 2.2 emulator interface.
 *
 * This implements a CP/M 2.2 operating system emulator on top of our Z80
 * core. Instead of running actual CP/M code for the BDOS and CCP, we
 * implement them in C and intercept Z80 calls to the BDOS entry point.
 *
 * CP/M ARCHITECTURE (how the real thing works)
 * =============================================
 * CP/M is a simple single-tasking OS for 8-bit microcomputers. It has
 * three layers:
 *
 *   CCP  (Console Command Processor) -- the command line ("A>")
 *   BDOS (Basic Disk Operating System) -- ~38 system calls for file I/O,
 *         console I/O, and disk management
 *   BIOS (Basic I/O System) -- hardware abstraction (serial ports, disks)
 *
 * Programs are .COM files that load at address 0100h and run in the TPA
 * (Transient Program Area). They make OS calls via CALL 0005h with the
 * function number in register C and a parameter in DE. That's the entire
 * system call interface -- elegantly simple.
 *
 * OUR APPROACH
 * ============
 * When the Z80 reaches the BDOS entry point, we intercept and handle the
 * system call in C code. The file system maps CP/M drives (A:, B:, ...)
 * to host directories. Console I/O passes through callbacks. The CCP is
 * also implemented in C, so we don't need any CP/M binary images.
 *
 * MEMORY MAP
 * ==========
 * We set up a standard 64K CP/M system layout:
 *
 *   0000-0002   JP to warm boot (BIOS WBOOT entry)
 *   0003        IOBYTE (I/O device mapping)
 *   0004        Current drive (low nibble) / user number (high nibble)
 *   0005-0007   JP to BDOS entry point
 *   005C-007F   Default FCBs (filled by CCP for .COM programs)
 *   0080-00FF   Default DMA buffer / command tail
 *   0100-DBFF   TPA (where .COM programs load and run)
 *   DC00-E3FF   CCP area (not used -- our CCP is in C)
 *   E400-F1FF   BDOS area (trap target, no real code here)
 *   F200-FFFF   BIOS area (trap target, no real code here)
 */

#ifndef CPM_H
#define CPM_H

#include "z80.h"
#include <stdint.h>

/* ===================================================================
 * MEMORY LAYOUT CONSTANTS
 *
 * These match a standard 64K CP/M 2.2 system. Programs commonly read
 * bytes 0006-0007h to find the BDOS entry address, which tells them
 * how large the TPA is (they subtract 0100h to get usable memory).
 * =================================================================== */

#define CPM_BIOS_BASE   0xF200  /* BIOS starts here */
#define CPM_BDOS_BASE   0xE400  /* BDOS starts here */
#define CPM_CCP_BASE    0xDC00  /* CCP starts here */
#define CPM_TPA_BASE    0x0100  /* Programs load here */
#define CPM_TPA_TOP     CPM_CCP_BASE  /* Top of TPA */

/* Trap addresses: the JP instructions at page zero redirect here.
 * Our execution loop intercepts when PC reaches these addresses,
 * before the Z80 executes whatever is there. */
#define CPM_WBOOT_TRAP  (CPM_BIOS_BASE + 3)   /* 0xF203 */
#define CPM_BDOS_TRAP   (CPM_BDOS_BASE + 6)   /* 0xE406 */

/* Page zero layout addresses. */
#define CPM_IOBYTE      0x0003  /* I/O byte */
#define CPM_DRIVE_USER  0x0004  /* Drive (low nibble) + user (high nibble) */
#define CPM_FCB1        0x005C  /* Default FCB #1 (first command argument) */
#define CPM_FCB2        0x006C  /* Default FCB #2 (second argument, overlaps FCB1) */
#define CPM_DMA_DEFAULT 0x0080  /* Default DMA buffer */

/* Maximum drives A: through P:. */
#define CPM_MAX_DRIVES  16

/* Maximum simultaneously open files we track. */
#define CPM_MAX_FILES   16

/* BIOS trap targets. Each BIOS jump table entry at F200+3*n is a JP
 * instruction pointing to F240+3*n. Programs like MBASIC read these JP
 * targets and call them directly for faster I/O. We intercept at the
 * trap addresses in cpm_step. */
#define CPM_BIOS_TRAP_BASE (CPM_BIOS_BASE + 0x40)  /* 0xF240 */
#define CPM_BIOS_NFUNCS    17  /* Number of BIOS functions (BOOT..SECTRAN) */

/* Disk Parameter Block, Disk Parameter Header, and Allocation Vector.
 * We place these in the BIOS area where they won't conflict with
 * anything. Programs read the DPB via BDOS 31 to learn disk geometry.
 *
 * Layout in BIOS area:
 *   F200-F232  BIOS jump table (17 entries * 3 bytes)
 *   F240-F272  BIOS trap targets (17 entries * 3 bytes)
 *   F280-F28E  DPB (15 bytes)
 *   F2A0-F2AF  DPH (16 bytes)
 *   F2B0-F32F  DIRBUF (128 bytes)
 *   F340-F43F  ALV (256 bytes) */
#define CPM_DPB_ADDR    (CPM_BIOS_BASE + 0x80)    /* 0xF280 */
#define CPM_DPH_ADDR    (CPM_BIOS_BASE + 0xA0)    /* 0xF2A0 */
#define CPM_DIRBUF_ADDR (CPM_BIOS_BASE + 0xB0)    /* 0xF2B0 */
#define CPM_ALV_ADDR    (CPM_BIOS_BASE + 0x140)   /* 0xF340 */

/* ===================================================================
 * CP/M MACHINE STATE
 * =================================================================== */

typedef struct CPMachine {
    Z80 cpu;                        /* Z80 processor core */
    uint8_t memory[65536];          /* Full 64K address space */

    /* Drive mapping: each drive (0=A, 1=B, ..., 15=P) maps to a host
     * directory path. NULL means the drive is not mounted. */
    const char *drive[CPM_MAX_DRIVES];

    /* System state. These mirror what the real CP/M BDOS maintains. */
    uint8_t current_drive;          /* Current default drive (0=A, 1=B, ...) */
    uint8_t user_number;            /* Current user number (0-15) */
    uint16_t dma;                   /* DMA buffer address (default 0x0080) */
    uint8_t iobyte;                 /* I/O device mapping byte */

    /* Console state: we track the column for tab expansion.
     * CP/M expands TAB (09h) to spaces up to the next 8-column boundary. */
    int console_col;

    /* Directory search state for BDOS functions 17 (search first) and
     * 18 (search next). We iterate through the host directory across
     * calls. The search_dir is a DIR* handle (void* to avoid pulling
     * <dirent.h> into this header). */
    void *search_dir;               /* DIR* from opendir(), or NULL */
    uint8_t search_pattern[12];     /* FCB pattern: [0]=user, [1-11]=name+ext */
    int search_drive;               /* Resolved drive number for search */

    /* Open file table. Maps CP/M files to host FILE* handles across
     * multiple BDOS calls. We identify files by drive + 11-char name
     * rather than by FCB address, since programs may copy FCBs around. */
    struct {
        int active;         /* 1 = slot in use, 0 = free */
        int drive;          /* resolved drive number (0=A, ...) */
        char name[12];      /* 11 chars + null (e.g., "HELLO   COM") */
        void *fp;           /* FILE* (void* avoids stdio.h here) */
    } files[CPM_MAX_FILES];

    /* Console I/O callbacks. The host environment provides these.
     * For a terminal: con_out writes to stdout, con_in reads from stdin.
     * For tests: con_out captures to a buffer, con_in returns canned data. */
    void (*con_out)(void *ctx, uint8_t ch);   /* Output one character */
    int  (*con_in)(void *ctx);                /* Read one char (blocking) */
    int  (*con_status)(void *ctx);            /* 1 = char ready, 0 = not */
    void *con_ctx;                            /* Opaque context pointer */

    /* Execution control. */
    int running;                    /* 1 while Z80 loop is active */
    int warm_boot;                  /* 1 when warm boot was triggered */
} CPMachine;

/* ===================================================================
 * API
 * =================================================================== */

/* Initialize the CP/M machine: clear memory, reset Z80 CPU, set up
 * page zero with trap vectors. Call this once, then mount drives and
 * set console callbacks before running any programs. */
void cpm_init(CPMachine *cpm);

/* Clean up resources: close all open files and search directories.
 * Call this when done with the CP/M machine, or before re-init. */
void cpm_cleanup(CPMachine *cpm);

/* Mount a host directory as a CP/M drive.
 *   drive: 0=A, 1=B, ..., 15=P
 *   path:  host directory path (must exist, not copied -- keep it alive) */
void cpm_mount(CPMachine *cpm, int drive, const char *path);

/* Execute one Z80 instruction, checking for BDOS and warm boot traps.
 * This is the core execution primitive. */
void cpm_step(CPMachine *cpm);

/* Handle a BDOS system call. Called internally when PC reaches the
 * BDOS trap address. Reads function number from C, parameter from DE. */
void cpm_bdos(CPMachine *cpm);

/* Run the Z80 until it halts or triggers a warm boot.
 * Returns 1 if warm boot was requested, 0 if halted. */
int cpm_run(CPMachine *cpm);

/* Load a .COM file into memory at 0100h. Sets up the stack and pushes
 * a warm-boot return address. Does NOT set up FCBs or command tail
 * (the CCP does that). Returns 0 on success, -1 on error. */
int cpm_load_com(CPMachine *cpm, const char *path);

/* Set up the default FCBs at 005Ch/006Ch and the command tail at 0080h
 * from a command line string (e.g., "FILE1.TXT FILE2.TXT"). */
void cpm_setup_fcbs(CPMachine *cpm, const char *cmdline);

/* Run the CCP (Console Command Processor) -- the interactive command
 * line. Prints prompt, reads commands, dispatches to built-in commands
 * or loads .COM files. */
void cpm_ccp(CPMachine *cpm);

#endif /* CPM_H */

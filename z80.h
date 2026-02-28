/* z80.h -- Z80 CPU emulator interface.
 *
 * This is a minimal, readable Z80 emulator designed for accuracy
 * sufficient to run ZX Spectrum software. Each call to z80_step()
 * executes one instruction and advances cpu->clocks by the number
 * of T-states (clock cycles) that instruction takes on real hardware.
 *
 * The Z80 runs at 3.5 MHz in a ZX Spectrum, so one T-state = ~286 ns.
 *
 * REGISTER LAYOUT
 * ===============
 * The Z80 has a main register set and an alternate (shadow) set.
 * Main:    A, F, B, C, D, E, H, L
 * Shadow:  A', F', B', C', D', E', H', L'
 * Index:   IX, IY (16-bit, used for indexed addressing)
 * Control: SP (stack pointer), PC (program counter)
 * Special: I (interrupt vector base), R (memory refresh counter)
 * Flags:   IFF1, IFF2 (interrupt flip-flops), IM (interrupt mode 0/1/2)
 *
 * Registers are paired for 16-bit operations:
 *   BC = B<<8 | C,  DE = D<<8 | E,  HL = H<<8 | L,  AF = A<<8 | F
 *
 * FLAGS (bits of the F register)
 * ==============================
 * Bit 7: S  - Sign flag (copy of bit 7 of result)
 * Bit 6: Z  - Zero flag (set if result is zero)
 * Bit 5: Y  - Undocumented (copy of bit 5 of result)
 * Bit 4: H  - Half-carry (carry from bit 3 to bit 4)
 * Bit 3: X  - Undocumented (copy of bit 3 of result)
 * Bit 2: PV - Parity/Overflow (parity for logic ops, overflow for arith)
 * Bit 1: N  - Subtract flag (set if last op was subtraction)
 * Bit 0: C  - Carry flag
 */

#ifndef Z80_H
#define Z80_H

#include <stdint.h>

/* Flag bit positions in the F register. */
#define Z80_CF  0x01  /* Carry */
#define Z80_NF  0x02  /* Subtract */
#define Z80_PF  0x04  /* Parity/Overflow */
#define Z80_XF  0x08  /* Undocumented bit 3 */
#define Z80_HF  0x10  /* Half-carry */
#define Z80_YF  0x20  /* Undocumented bit 5 */
#define Z80_ZF  0x40  /* Zero */
#define Z80_SF  0x80  /* Sign */

/* The Z80 CPU state.
 *
 * Memory access is done through function pointers so the emulator
 * is decoupled from any specific memory map. The host system sets
 * these to point at functions that implement its address decoding
 * (RAM, ROM, I/O ports, memory-mapped devices, etc.).
 *
 * For a ZX Spectrum:
 *   - 0x0000-0x3FFF: 16K ROM
 *   - 0x4000-0x7FFF: 16K RAM (includes screen memory at 0x4000)
 *   - 0x8000-0xFFFF: 32K RAM (48K model)
 */
typedef struct Z80 {
    /* Main registers. We store them individually for clarity.
     * In register pair BC, B is the high byte and C is the low byte. */
    uint8_t a, f;
    uint8_t b, c;
    uint8_t d, e;
    uint8_t h, l;

    /* Shadow (alternate) registers, swapped with EX AF,AF' and EXX. */
    uint8_t a_, f_;
    uint8_t b_, c_;
    uint8_t d_, e_;
    uint8_t h_, l_;

    /* Index registers (16-bit). */
    uint16_t ix, iy;

    /* Stack pointer and program counter. */
    uint16_t sp, pc;

    /* Interrupt vector base (I) and memory refresh counter (R).
     * R increments on each instruction fetch (lower 7 bits only,
     * bit 7 is preserved from the last LD R,A). */
    uint8_t i, r;

    /* Interrupt flip-flops and interrupt mode.
     * IFF1 controls whether maskable interrupts are accepted.
     * IFF2 stores the previous IFF1 state (used by NMI/RETN) and is
     * observable via the PV flag in LD A,I / LD A,R.
     * IM is 0, 1, or 2. */
    uint8_t iff1, iff2;
    uint8_t im;
    /* EI blocks maskable interrupt acceptance until after one full
     * subsequent instruction has executed. */
    uint8_t ei_delay;

    /* HALT state: CPU is halted, executing NOPs until an interrupt. */
    uint8_t halted;

    /* Total T-states elapsed. The caller can use this for timing. */
    uint64_t clocks;

    /* Memory read/write callbacks. The void *ctx is passed through
     * so the host can access its own state (e.g., RAM array). */
    uint8_t (*mem_read)(void *ctx, uint16_t addr);
    void (*mem_write)(void *ctx, uint16_t addr, uint8_t val);

    /* I/O port read/write callbacks. The Z80 has a separate 16-bit
     * I/O address space (though typically only the low 8 bits matter). */
    uint8_t (*io_read)(void *ctx, uint16_t port);
    void (*io_write)(void *ctx, uint16_t port, uint8_t val);

    /* Opaque context pointer passed to all callbacks. */
    void *ctx;
} Z80;

/* Initialize the CPU to its reset state.
 * After calling this, set the memory/IO callbacks before stepping. */
void z80_init(Z80 *cpu);

/* Execute one instruction. Returns the number of T-states consumed.
 * Also adds that count to cpu->clocks. */
int z80_step(Z80 *cpu);

/* Request a maskable interrupt. The data byte is used in IM 0 (as the
 * opcode to execute, typically RST 38h = 0xFF) and IM 2 (as the low
 * byte of the interrupt vector table address). */
int z80_interrupt(Z80 *cpu, uint8_t data);

/* Request a non-maskable interrupt (NMI). */
int z80_nmi(Z80 *cpu);

#endif /* Z80_H */

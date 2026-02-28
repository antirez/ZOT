/* z80.c -- Z80 CPU emulator implementation.
 *
 * This file implements the full Z80 instruction set including:
 *   - All 256 unprefixed opcodes
 *   - CB-prefixed bit/rotate/shift instructions
 *   - ED-prefixed extended instructions
 *   - DD-prefixed (IX) and FD-prefixed (IY) instructions
 *   - DDCB/FDCB double-prefixed indexed bit operations
 *   - Undocumented opcodes (SLL, IX/IY half-registers, etc.)
 *
 * DESIGN NOTES
 * ============
 * The emulator executes one complete instruction per z80_step() call,
 * returning the T-state count. This is simpler than cycle-stepping
 * (where each T-state is a separate call) but sufficient for systems
 * like the ZX Spectrum where mid-instruction timing is rarely needed.
 *
 * Flag computation follows the real Z80 behavior including the
 * undocumented X (bit 3) and Y (bit 5) flags, which are copies of
 * the corresponding bits of the result (or operand, depending on
 * the instruction).
 *
 * The R register (memory refresh) is incremented once per instruction
 * fetch for unprefixed opcodes, and once per prefix byte for prefixed
 * opcodes. Only the low 7 bits are incremented; bit 7 is preserved.
 */

#include "z80.h"
#include <string.h>

/* ===================================================================
 * HELPER MACROS
 * =================================================================== */

/* Read/write memory through callbacks. */
#define RB(addr)      cpu->mem_read(cpu->ctx, (addr))
#define WB(addr, val) cpu->mem_write(cpu->ctx, (addr), (val))

/* Read a 16-bit word from memory (little-endian: low byte first). */
#define RW(addr)      ((uint16_t)RB(addr) | ((uint16_t)RB((uint16_t)((addr)+1)) << 8))

/* Write a 16-bit word to memory (little-endian). Uses temps to eval once. */
#define WW(addr, val) do { uint16_t _a=(addr), _v=(val); \
    WB(_a, _v & 0xFF); WB((uint16_t)(_a+1), _v >> 8); } while(0)

/* I/O port access. */
#define IN(port)       cpu->io_read(cpu->ctx, (port))
#define OUT(port, val) cpu->io_write(cpu->ctx, (port), (val))

/* Fetch the next byte from PC and advance PC. */
#define FETCH() RB(cpu->pc++)

/* Fetch a 16-bit immediate (little-endian) and advance PC by 2. */
#define FETCH16() (cpu->pc += 2, RW(cpu->pc - 2))

/* Register pair getters/setters. In register pairs, the first register
 * is the high byte (B in BC, D in DE, etc.), while memory words are
 * little-endian. */
#define BC() ((uint16_t)((cpu->b << 8) | cpu->c))
#define DE() ((uint16_t)((cpu->d << 8) | cpu->e))
#define HL() ((uint16_t)((cpu->h << 8) | cpu->l))
#define AF() ((uint16_t)((cpu->a << 8) | cpu->f))

/* SET macros use a temporary to evaluate (v) only once.
 * This is important because (v) may have side effects (e.g., alu_add16). */
#define SET_BC(v) do { uint16_t _v=(v); cpu->b = _v >> 8; cpu->c = _v & 0xFF; } while(0)
#define SET_DE(v) do { uint16_t _v=(v); cpu->d = _v >> 8; cpu->e = _v & 0xFF; } while(0)
#define SET_HL(v) do { uint16_t _v=(v); cpu->h = _v >> 8; cpu->l = _v & 0xFF; } while(0)
#define SET_AF(v) do { uint16_t _v=(v); cpu->a = _v >> 8; cpu->f = _v & 0xFF; } while(0)

/* Push/pop 16-bit values on the stack. Final memory layout matches Z80
 * stack semantics: low byte at [SP], high byte at [SP+1]. */
#define PUSH(v) do { cpu->sp -= 2; WW(cpu->sp, (v)); } while(0)
#define POP()   (cpu->sp += 2, RW(cpu->sp - 2))

/* Increment the R register (lower 7 bits only, bit 7 preserved). */
#define INC_R() (cpu->r = (cpu->r & 0x80) | ((cpu->r + 1) & 0x7F))

/* ===================================================================
 * FLAG COMPUTATION TABLES
 * =================================================================== */

/* Precomputed flags for the SZ53P set: Sign, Zero, Y, X, and Parity.
 * Many instructions set flags based solely on the 8-bit result, so
 * we precompute S, Z, Y (bit 5), X (bit 3), and P (parity) for all
 * 256 possible byte values. H, N, and C are instruction-specific. */
static uint8_t sz53p_table[256];

/* Parity lookup: 1 if even parity (even number of 1 bits), 0 if odd. */
static uint8_t parity_table[256];

/* Initialize the lookup tables. Called once from z80_init(). */
static int tables_initialized = 0;
static void init_tables(void) {
    if (tables_initialized) return;
    tables_initialized = 1;

    for (int i = 0; i < 256; i++) {
        /* Count bits for parity. */
        int bits = 0;
        for (int b = 0; b < 8; b++)
            if (i & (1 << b)) bits++;
        parity_table[i] = (bits % 2 == 0) ? Z80_PF : 0;

        /* S, Z, Y, X, P flags from the byte value. */
        sz53p_table[i] = (i & Z80_SF)           /* S = bit 7 */
                        | (i == 0 ? Z80_ZF : 0)  /* Z = value is zero */
                        | (i & Z80_YF)           /* Y = bit 5 */
                        | (i & Z80_XF)           /* X = bit 3 */
                        | parity_table[i];        /* P = even parity */
    }
}

/* ===================================================================
 * ALU OPERATIONS
 * ===================================================================
 * These implement the core arithmetic/logic operations and set flags
 * according to Z80 behavior. Each function modifies cpu->f.
 */

/* 8-bit addition: A = A + val (+ carry if with_carry).
 *
 * Flags affected:
 *   S - set if result is negative (bit 7)
 *   Z - set if result is zero
 *   H - set if carry from bit 3 to bit 4
 *   PV - set if signed overflow
 *   N - reset
 *   C - set if carry from bit 7
 *   X, Y - from the result
 */
static inline void alu_add(Z80 *cpu, uint8_t val, int with_carry) {
    int carry = with_carry ? (cpu->f & Z80_CF) : 0;
    int result = cpu->a + val + carry;
    int half = (cpu->a & 0x0F) + (val & 0x0F) + carry;

    /* Signed overflow: operands have same sign, result has different sign. */
    int overflow = ((cpu->a ^ val) ^ 0x80) & (cpu->a ^ result) & 0x80;

    cpu->a = result & 0xFF;
    cpu->f = (sz53p_table[cpu->a] & ~Z80_PF)  /* S, Z, X, Y (clear parity) */
           | (overflow ? Z80_PF : 0)           /* PV = overflow */
           | (half & 0x10 ? Z80_HF : 0)       /* H = half-carry */
           | (result & 0x100 ? Z80_CF : 0);    /* C = carry */
    /* N is implicitly 0 because sz53p_table doesn't include N. */
}

/* 8-bit subtraction: A = A - val (- carry if with_carry).
 *
 * Flags: same as ADD but N is set, and half-carry/carry are borrows.
 */
static inline void alu_sub(Z80 *cpu, uint8_t val, int with_carry) {
    int carry = with_carry ? (cpu->f & Z80_CF) : 0;
    int result = cpu->a - val - carry;
    int half = (cpu->a & 0x0F) - (val & 0x0F) - carry;

    /* Signed overflow: A and val have different signs, result sign differs from A. */
    int overflow = (cpu->a ^ val) & (cpu->a ^ result) & 0x80;

    cpu->a = result & 0xFF;
    cpu->f = (sz53p_table[cpu->a] & ~Z80_PF)
           | (overflow ? Z80_PF : 0)
           | (half & 0x10 ? Z80_HF : 0)
           | (result & 0x100 ? Z80_CF : 0)
           | Z80_NF;                           /* N = 1 (subtraction) */
}

/* CP (compare): same as SUB but doesn't store the result.
 * The X and Y flags come from the operand, not the result. */
static inline void alu_cp(Z80 *cpu, uint8_t val) {
    int result = cpu->a - val;
    int half = (cpu->a & 0x0F) - (val & 0x0F);
    int overflow = (cpu->a ^ val) & (cpu->a ^ result) & 0x80;

    cpu->f = (sz53p_table[result & 0xFF] & ~(Z80_XF | Z80_YF | Z80_PF))
           | (val & (Z80_XF | Z80_YF))      /* X, Y from operand, not result */
           | (overflow ? Z80_PF : 0)
           | (half & 0x10 ? Z80_HF : 0)
           | (result & 0x100 ? Z80_CF : 0)
           | Z80_NF;
}

/* AND: A = A & val. H is set, C and N are reset. PV = parity. */
static inline void alu_and(Z80 *cpu, uint8_t val) {
    cpu->a &= val;
    cpu->f = sz53p_table[cpu->a] | Z80_HF;
}

/* XOR: A = A ^ val. All of H, N, C are reset. PV = parity. */
static inline void alu_xor(Z80 *cpu, uint8_t val) {
    cpu->a ^= val;
    cpu->f = sz53p_table[cpu->a];
}

/* OR: A = A | val. All of H, N, C are reset. PV = parity. */
static inline void alu_or(Z80 *cpu, uint8_t val) {
    cpu->a |= val;
    cpu->f = sz53p_table[cpu->a];
}

/* 8-bit increment. Preserves C flag, sets H, S, Z, PV, X, Y.
 * Returns the incremented value. */
static inline uint8_t alu_inc(Z80 *cpu, uint8_t val) {
    uint8_t result = val + 1;
    cpu->f = (cpu->f & Z80_CF)          /* Preserve carry */
           | (sz53p_table[result] & ~Z80_PF) /* S, Z, X, Y */
           | ((val & 0x0F) == 0x0F ? Z80_HF : 0)  /* H: half-carry */
           | (val == 0x7F ? Z80_PF : 0); /* PV: overflow (0x7F -> 0x80) */
    /* N is implicitly 0. */
    return result;
}

/* 8-bit decrement. Preserves C flag, sets H, N, S, Z, PV, X, Y. */
static inline uint8_t alu_dec(Z80 *cpu, uint8_t val) {
    uint8_t result = val - 1;
    cpu->f = (cpu->f & Z80_CF)
           | (sz53p_table[result] & ~Z80_PF)
           | ((val & 0x0F) == 0x00 ? Z80_HF : 0)  /* H: half-borrow */
           | (val == 0x80 ? Z80_PF : 0) /* PV: overflow (0x80 -> 0x7F) */
           | Z80_NF;                     /* N = 1 (subtraction) */
    return result;
}

/* 16-bit addition: dest = dest + val.
 * Only affects H, C, N, X, Y flags. S, Z, PV are preserved.
 * X and Y come from the high byte of the result. */
static inline uint16_t alu_add16(Z80 *cpu, uint16_t dest, uint16_t val) {
    uint32_t result = dest + val;
    int half = (dest & 0x0FFF) + (val & 0x0FFF);

    cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_PF)) /* Preserve S, Z, PV */
           | ((result >> 8) & (Z80_XF | Z80_YF))    /* X, Y from high byte */
           | (half & 0x1000 ? Z80_HF : 0)           /* H from bit 11 carry */
           | (result & 0x10000 ? Z80_CF : 0);        /* C from bit 15 carry */
    /* N is implicitly 0. */
    return result & 0xFFFF;
}

/* ===================================================================
 * ROTATE AND SHIFT HELPERS
 * =================================================================== */

/* RLCA: Rotate A left, old bit 7 to carry and bit 0.
 * Only affects C, H (reset), N (reset), X, Y. S, Z, P are preserved. */
static inline void op_rlca(Z80 *cpu) {
    uint8_t old = cpu->a;
    cpu->a = (old << 1) | (old >> 7);
    cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_PF))
           | (cpu->a & (Z80_XF | Z80_YF))
           | (old >> 7);  /* C = old bit 7 */
}

/* RRCA: Rotate A right, old bit 0 to carry and bit 7. */
static inline void op_rrca(Z80 *cpu) {
    uint8_t old = cpu->a;
    cpu->a = (old >> 1) | (old << 7);
    cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_PF))
           | (cpu->a & (Z80_XF | Z80_YF))
           | (old & Z80_CF);  /* C = old bit 0 */
}

/* RLA: Rotate A left through carry. */
static inline void op_rla(Z80 *cpu) {
    uint8_t old = cpu->a;
    cpu->a = (old << 1) | (cpu->f & Z80_CF);
    cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_PF))
           | (cpu->a & (Z80_XF | Z80_YF))
           | (old >> 7);
}

/* RRA: Rotate A right through carry. */
static inline void op_rra(Z80 *cpu) {
    uint8_t old = cpu->a;
    cpu->a = (old >> 1) | ((cpu->f & Z80_CF) << 7);
    cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_PF))
           | (cpu->a & (Z80_XF | Z80_YF))
           | (old & Z80_CF);
}

/* CB-prefix rotate/shift operations. These set full flags (S,Z,H=0,PV=P,N=0,C). */

/* RLC: Rotate left, old bit 7 to carry and bit 0. */
static inline uint8_t op_rlc(Z80 *cpu, uint8_t val) {
    uint8_t result = (val << 1) | (val >> 7);
    cpu->f = sz53p_table[result] | (val >> 7);
    return result;
}

/* RRC: Rotate right, old bit 0 to carry and bit 7. */
static inline uint8_t op_rrc(Z80 *cpu, uint8_t val) {
    uint8_t result = (val >> 1) | (val << 7);
    cpu->f = sz53p_table[result] | (val & 0x01);
    return result;
}

/* RL: Rotate left through carry. */
static inline uint8_t op_rl(Z80 *cpu, uint8_t val) {
    uint8_t result = (val << 1) | (cpu->f & Z80_CF);
    cpu->f = sz53p_table[result] | (val >> 7);
    return result;
}

/* RR: Rotate right through carry. */
static inline uint8_t op_rr(Z80 *cpu, uint8_t val) {
    uint8_t result = (val >> 1) | ((cpu->f & Z80_CF) << 7);
    cpu->f = sz53p_table[result] | (val & 0x01);
    return result;
}

/* SLA: Shift left arithmetic (bit 0 = 0, old bit 7 to carry). */
static inline uint8_t op_sla(Z80 *cpu, uint8_t val) {
    uint8_t result = val << 1;
    cpu->f = sz53p_table[result] | (val >> 7);
    return result;
}

/* SRA: Shift right arithmetic (bit 7 preserved, old bit 0 to carry). */
static inline uint8_t op_sra(Z80 *cpu, uint8_t val) {
    uint8_t result = (val >> 1) | (val & 0x80);
    cpu->f = sz53p_table[result] | (val & 0x01);
    return result;
}

/* SLL: Undocumented shift left (bit 0 = 1). Sometimes called SL1 or SLS.
 * Same as SLA but sets bit 0. */
static inline uint8_t op_sll(Z80 *cpu, uint8_t val) {
    uint8_t result = (val << 1) | 0x01;
    cpu->f = sz53p_table[result] | (val >> 7);
    return result;
}

/* SRL: Shift right logical (bit 7 = 0, old bit 0 to carry). */
static inline uint8_t op_srl(Z80 *cpu, uint8_t val) {
    uint8_t result = val >> 1;
    cpu->f = sz53p_table[result] | (val & 0x01);
    return result;
}

/* BIT: Test bit n of val. Sets Z if the bit is 0.
 * H is set, N is reset, C is preserved.
 * S is set if bit 7 is tested and the bit is set.
 * PV is set like Z (same as Z flag).
 * X and Y flags come from the value being tested (for register operands)
 * or from an internal temporary register (for (HL)/(IX+d)/(IY+d)). */
static inline void op_bit(Z80 *cpu, int bit, uint8_t val) {
    uint8_t result = val & (1 << bit);
    cpu->f = (cpu->f & Z80_CF)  /* Preserve carry */
           | Z80_HF             /* H is always set */
           | (result & Z80_SF)  /* S from the tested bit (only matters for bit 7) */
           | (result ? 0 : Z80_ZF | Z80_PF) /* Z and PV: set if bit is 0 */
           | (val & (Z80_XF | Z80_YF)); /* X, Y from the operand value */
}

/* BIT for (HL)/(IX+d)/(IY+d): X and Y come from high byte of address. */
static inline void op_bit_mem(Z80 *cpu, int bit, uint8_t val, uint16_t addr) {
    uint8_t result = val & (1 << bit);
    cpu->f = (cpu->f & Z80_CF)
           | Z80_HF
           | (result & Z80_SF)
           | (result ? 0 : Z80_ZF | Z80_PF)
           | ((addr >> 8) & (Z80_XF | Z80_YF)); /* X, Y from addr high byte */
}

/* ===================================================================
 * CONDITION CODE EVALUATION
 * =================================================================== */

/* The Z80 encodes conditions in 3-bit fields within opcodes:
 *   000 = NZ (not zero)    100 = PO (parity odd / no overflow)
 *   001 = Z  (zero)        101 = PE (parity even / overflow)
 *   010 = NC (no carry)    110 = P  (positive / sign clear)
 *   011 = C  (carry)       111 = M  (minus / sign set)
 */
static inline int check_condition(Z80 *cpu, int cc) {
    switch (cc) {
    case 0: return !(cpu->f & Z80_ZF);  /* NZ */
    case 1: return  (cpu->f & Z80_ZF);  /* Z */
    case 2: return !(cpu->f & Z80_CF);  /* NC */
    case 3: return  (cpu->f & Z80_CF);  /* C */
    case 4: return !(cpu->f & Z80_PF);  /* PO */
    case 5: return  (cpu->f & Z80_PF);  /* PE */
    case 6: return !(cpu->f & Z80_SF);  /* P */
    case 7: return  (cpu->f & Z80_SF);  /* M */
    }
    return 0;
}

/* ===================================================================
 * REGISTER DECODE HELPERS
 * ===================================================================
 * Many Z80 opcodes encode a register in a 3-bit field:
 *   000=B, 001=C, 010=D, 011=E, 100=H, 101=L, 110=(HL), 111=A
 *
 * We provide read/write helpers that handle this encoding.
 * When using IX/IY prefixes, H/L are replaced with IXH/IXL or IYH/IYL
 * (undocumented), and (HL) is replaced with (IX+d) or (IY+d).
 */

static inline uint8_t read_reg8(Z80 *cpu, int r) {
    switch (r) {
    case 0: return cpu->b;
    case 1: return cpu->c;
    case 2: return cpu->d;
    case 3: return cpu->e;
    case 4: return cpu->h;
    case 5: return cpu->l;
    case 6: return RB(HL());
    case 7: return cpu->a;
    }
    return 0;
}

static inline void write_reg8(Z80 *cpu, int r, uint8_t val) {
    switch (r) {
    case 0: cpu->b = val; break;
    case 1: cpu->c = val; break;
    case 2: cpu->d = val; break;
    case 3: cpu->e = val; break;
    case 4: cpu->h = val; break;
    case 5: cpu->l = val; break;
    case 6: WB(HL(), val); break;
    case 7: cpu->a = val; break;
    }
}

/* Read/write 16-bit register pairs. Encoding varies by instruction group:
 *   For PUSH/POP:    00=BC, 01=DE, 10=HL, 11=AF
 *   For most others: 00=BC, 01=DE, 10=HL, 11=SP */
static inline uint16_t read_rp(Z80 *cpu, int rp) {
    switch (rp) {
    case 0: return BC();
    case 1: return DE();
    case 2: return HL();
    case 3: return cpu->sp;
    }
    return 0;
}

static inline void write_rp(Z80 *cpu, int rp, uint16_t val) {
    switch (rp) {
    case 0: SET_BC(val); break;
    case 1: SET_DE(val); break;
    case 2: SET_HL(val); break;
    case 3: cpu->sp = val; break;
    }
}

/* ===================================================================
 * DAA - Decimal Adjust Accumulator
 * ===================================================================
 * This is one of the most complex Z80 instructions. It adjusts A
 * after a BCD (Binary Coded Decimal) addition or subtraction so that
 * the result is a valid BCD value.
 *
 * The adjustment depends on the current value of A, and the H (half-carry),
 * N (subtract), and C (carry) flags from the previous operation.
 *
 * After ADD/ADC (N=0):
 *   If low nibble > 9 or H is set: add 0x06 to A
 *   If high nibble > 9 or C is set: add 0x60 to A, set C
 *
 * After SUB/SBC (N=1):
 *   If H is set: subtract 0x06 from A
 *   If C is set: subtract 0x60 from A
 */
static inline void op_daa(Z80 *cpu) {
    uint8_t a = cpu->a;
    uint8_t correction = 0;
    int carry = cpu->f & Z80_CF;

    if (cpu->f & Z80_HF || (a & 0x0F) > 9) {
        correction |= 0x06;
    }
    if (carry || a > 0x99) {
        correction |= 0x60;
        carry = 1;
    }

    if (cpu->f & Z80_NF) {
        /* After subtraction. */
        int half = (cpu->f & Z80_HF) && (a & 0x0F) < 0x06;
        cpu->a -= correction;
        cpu->f = sz53p_table[cpu->a]
               | (carry ? Z80_CF : 0)
               | Z80_NF
               | (half ? Z80_HF : 0);
    } else {
        /* After addition. */
        int half = (a & 0x0F) > 9;
        cpu->a += correction;
        cpu->f = sz53p_table[cpu->a]
               | (carry ? Z80_CF : 0)
               | (half ? Z80_HF : 0);
    }
}

/* ===================================================================
 * CB-PREFIX EXECUTION
 * ===================================================================
 * CB-prefixed opcodes: bit rotations, shifts, BIT/RES/SET.
 *
 * Opcode layout: CB xx where xx encodes:
 *   Bits 7-6: operation group
 *     00 = rotate/shift (further decoded by bits 5-3)
 *     01 = BIT n, r
 *     10 = RES n, r
 *     11 = SET n, r
 *   Bits 5-3: bit number (for BIT/RES/SET) or shift type (for group 00)
 *   Bits 2-0: register (B,C,D,E,H,L,(HL),A)
 */
static int exec_cb(Z80 *cpu) {
    uint8_t op = FETCH();
    INC_R();

    int group = op >> 6;        /* 0=rot/shift, 1=BIT, 2=RES, 3=SET */
    int bit_or_op = (op >> 3) & 7;  /* bit number or shift operation */
    int reg = op & 7;           /* register index */

    int tstates = (reg == 6) ? 15 : 8;  /* (HL) takes 15, reg takes 8 */

    if (group == 0) {
        /* Rotate/shift group. */
        uint8_t val = read_reg8(cpu, reg);
        uint8_t result;
        switch (bit_or_op) {
        case 0: result = op_rlc(cpu, val); break;
        case 1: result = op_rrc(cpu, val); break;
        case 2: result = op_rl(cpu, val); break;
        case 3: result = op_rr(cpu, val); break;
        case 4: result = op_sla(cpu, val); break;
        case 5: result = op_sra(cpu, val); break;
        case 6: result = op_sll(cpu, val); break;  /* Undocumented */
        case 7: result = op_srl(cpu, val); break;
        default: result = val; break;
        }
        write_reg8(cpu, reg, result);
    } else if (group == 1) {
        /* BIT n, r */
        uint8_t val = read_reg8(cpu, reg);
        if (reg == 6) {
            tstates = 12;
            op_bit_mem(cpu, bit_or_op, val, HL());
        } else {
            op_bit(cpu, bit_or_op, val);
        }
    } else if (group == 2) {
        /* RES n, r */
        uint8_t val = read_reg8(cpu, reg);
        write_reg8(cpu, reg, val & ~(1 << bit_or_op));
    } else {
        /* SET n, r */
        uint8_t val = read_reg8(cpu, reg);
        write_reg8(cpu, reg, val | (1 << bit_or_op));
    }

    return tstates;
}

/* ===================================================================
 * DDCB/FDCB PREFIX EXECUTION
 * ===================================================================
 * These are indexed bit operations: the displacement byte comes BEFORE
 * the opcode byte (unusual!). Format: DD CB dd oo or FD CB dd oo.
 *
 * The operation always uses (IX+d) or (IY+d) as the operand.
 * For non-BIT instructions, the result is also stored in a register
 * (undocumented behavior) unless the register field is 6 ((HL)).
 *
 * All operations take 23 T-states (except BIT which takes 20).
 */
static int exec_ddfdcb(Z80 *cpu, uint16_t ixiy) {
    int8_t d = (int8_t)FETCH();    /* Displacement */
    uint8_t op = FETCH();          /* Opcode */

    uint16_t addr = ixiy + d;
    uint8_t val = RB(addr);

    int group = op >> 6;
    int bit_or_op = (op >> 3) & 7;
    int reg = op & 7;

    if (group == 0) {
        /* Rotate/shift */
        uint8_t result;
        switch (bit_or_op) {
        case 0: result = op_rlc(cpu, val); break;
        case 1: result = op_rrc(cpu, val); break;
        case 2: result = op_rl(cpu, val); break;
        case 3: result = op_rr(cpu, val); break;
        case 4: result = op_sla(cpu, val); break;
        case 5: result = op_sra(cpu, val); break;
        case 6: result = op_sll(cpu, val); break;
        case 7: result = op_srl(cpu, val); break;
        default: result = val; break;
        }
        WB(addr, result);
        /* Undocumented: also store in register if not (HL). */
        if (reg != 6) write_reg8(cpu, reg, result);
        return 23;
    } else if (group == 1) {
        /* BIT n, (IX+d)/(IY+d) */
        op_bit_mem(cpu, bit_or_op, val, addr);
        return 20;
    } else if (group == 2) {
        /* RES n, (IX+d)/(IY+d) */
        uint8_t result = val & ~(1 << bit_or_op);
        WB(addr, result);
        if (reg != 6) write_reg8(cpu, reg, result);
        return 23;
    } else {
        /* SET n, (IX+d)/(IY+d) */
        uint8_t result = val | (1 << bit_or_op);
        WB(addr, result);
        if (reg != 6) write_reg8(cpu, reg, result);
        return 23;
    }
}

/* Forward declaration: used by DD/FD fallback re-decoding. */
static int exec_opcode(Z80 *cpu, uint8_t op);

/* ===================================================================
 * ED-PREFIX EXECUTION
 * ===================================================================
 * ED-prefixed opcodes include:
 *   - Block transfer: LDI, LDD, LDIR, LDDR
 *   - Block search: CPI, CPD, CPIR, CPDR
 *   - Block I/O: INI, IND, INIR, INDR, OUTI, OUTD, OTIR, OTDR
 *   - 16-bit ADC/SBC with HL
 *   - LD (nn),rp and LD rp,(nn) for all pairs
 *   - Interrupt control: IM 0/1/2, RETI, RETN
 *   - Special: NEG, RLD, RRD, LD I,A, LD R,A, LD A,I, LD A,R
 *   - IN r,(C) and OUT (C),r
 */
static int exec_ed(Z80 *cpu) {
    uint8_t op = FETCH();
    INC_R();

    switch (op) {

    /* ====================== IN r,(C) ====================== */
    /* ED 40-47,48-4F,50-57,58-5F,60-67,68-6F,78-7F
     * Read from port BC into register. Flags: S,Z,H=0,PV=parity,N=0.
     * If r=6, the value is read but only flags are affected (undocumented). */
    case 0x40: case 0x48: case 0x50: case 0x58:
    case 0x60: case 0x68: case 0x70: case 0x78: {
        int r = (op >> 3) & 7;
        uint8_t val = IN(BC());
        cpu->f = sz53p_table[val] | (cpu->f & Z80_CF);
        if (r != 6) write_reg8(cpu, r, val);
        return 12;
    }

    /* ====================== OUT (C),r ====================== */
    /* ED 41-47,49-4F,51-57,59-5F,61-67,69-6F,79-7F
     * Write register to port BC.
     * If r=6, outputs 0 (undocumented). */
    case 0x41: case 0x49: case 0x51: case 0x59:
    case 0x61: case 0x69: case 0x71: case 0x79: {
        int r = (op >> 3) & 7;
        uint8_t val = (r == 6) ? 0 : read_reg8(cpu, r);
        OUT(BC(), val);
        return 12;
    }

    /* ====================== SBC HL,rp ====================== */
    /* ED 42,52,62,72 -- 16-bit subtraction with carry.
     * Full flag set: S,Z,H,PV=overflow,N=1,C. */
    case 0x42: case 0x52: case 0x62: case 0x72: {
        int rp = (op >> 4) & 3;
        uint16_t val = read_rp(cpu, rp);
        int carry = cpu->f & Z80_CF;
        uint32_t result = HL() - val - carry;
        int half = (HL() & 0x0FFF) - (val & 0x0FFF) - carry;
        int overflow = (HL() ^ val) & (HL() ^ result) & 0x8000;

        SET_HL(result & 0xFFFF);
        cpu->f = ((result >> 8) & (Z80_SF | Z80_XF | Z80_YF)) /* S,X,Y from high byte */
               | (HL() == 0 ? Z80_ZF : 0)
               | (half & 0x1000 ? Z80_HF : 0)
               | (overflow ? Z80_PF : 0)
               | (result & 0x10000 ? Z80_CF : 0)
               | Z80_NF;
        return 15;
    }

    /* ====================== ADC HL,rp ====================== */
    /* ED 4A,5A,6A,7A -- 16-bit addition with carry. Full flags. */
    case 0x4A: case 0x5A: case 0x6A: case 0x7A: {
        int rp = (op >> 4) & 3;
        uint16_t val = read_rp(cpu, rp);
        int carry = cpu->f & Z80_CF;
        uint32_t result = HL() + val + carry;
        int half = (HL() & 0x0FFF) + (val & 0x0FFF) + carry;
        int overflow = ((HL() ^ val) ^ 0x8000) & (HL() ^ result) & 0x8000;

        SET_HL(result & 0xFFFF);
        cpu->f = ((result >> 8) & (Z80_SF | Z80_XF | Z80_YF))
               | (HL() == 0 ? Z80_ZF : 0)
               | (half & 0x1000 ? Z80_HF : 0)
               | (overflow ? Z80_PF : 0)
               | (result & 0x10000 ? Z80_CF : 0);
        return 15;
    }

    /* ====================== LD (nn),rp ====================== */
    /* ED 43,53,63,73 -- Store register pair to memory. */
    case 0x43: case 0x53: case 0x63: case 0x73: {
        int rp = (op >> 4) & 3;
        uint16_t addr = FETCH16();
        WW(addr, read_rp(cpu, rp));
        return 20;
    }

    /* ====================== LD rp,(nn) ====================== */
    /* ED 4B,5B,6B,7B -- Load register pair from memory. */
    case 0x4B: case 0x5B: case 0x6B: case 0x7B: {
        int rp = (op >> 4) & 3;
        uint16_t addr = FETCH16();
        write_rp(cpu, rp, RW(addr));
        return 20;
    }

    /* ====================== NEG ====================== */
    /* ED 44 (and undocumented mirrors ED 4C,54,5C,64,6C,74,7C)
     * Negate A: A = 0 - A. */
    case 0x44: case 0x4C: case 0x54: case 0x5C:
    case 0x64: case 0x6C: case 0x74: case 0x7C: {
        uint8_t old = cpu->a;
        cpu->a = 0;
        alu_sub(cpu, old, 0);
        return 8;
    }

    /* ====================== RETN ====================== */
    /* ED 45 (and undocumented mirrors ED 55,65,75)
     * Return from NMI. Restores IFF1 from IFF2. */
    case 0x45: case 0x55: case 0x65: case 0x75:
        cpu->iff1 = cpu->iff2;
        cpu->pc = POP();
        return 14;

    /* ====================== RETI ====================== */
    /* ED 4D (and undocumented mirrors ED 5D,6D,7D)
     * Return from interrupt. Same as RETN functionally,
     * but signals external devices that the ISR is done. */
    case 0x4D: case 0x5D: case 0x6D: case 0x7D:
        cpu->iff1 = cpu->iff2;
        cpu->pc = POP();
        return 14;

    /* ====================== IM modes ====================== */
    case 0x46: case 0x66: cpu->im = 0; return 8;  /* IM 0 */
    case 0x56: case 0x76: cpu->im = 1; return 8;  /* IM 1 */
    case 0x5E: case 0x7E: cpu->im = 2; return 8;  /* IM 2 */
    /* Undocumented: ED 4E, 6E also set IM 0. */
    case 0x4E: case 0x6E: cpu->im = 0; return 8;

    /* ====================== LD I,A / LD R,A ====================== */
    case 0x47: cpu->i = cpu->a; return 9;         /* LD I,A */
    case 0x4F: cpu->r = cpu->a; return 9;         /* LD R,A */

    /* ====================== LD A,I / LD A,R ====================== */
    /* These set flags: S,Z from the value, H=0, PV=IFF2, N=0, C preserved. */
    case 0x57: /* LD A,I */
        cpu->a = cpu->i;
        cpu->f = (cpu->f & Z80_CF)
               | (sz53p_table[cpu->a] & ~Z80_PF)
               | (cpu->iff2 ? Z80_PF : 0);
        return 9;

    case 0x5F: /* LD A,R */
        cpu->a = cpu->r;
        cpu->f = (cpu->f & Z80_CF)
               | (sz53p_table[cpu->a] & ~Z80_PF)
               | (cpu->iff2 ? Z80_PF : 0);
        return 9;

    /* ====================== RRD ====================== */
    /* Rotate right BCD digit between A and (HL).
     * Low nibble of (HL) -> low nibble of A
     * Low nibble of A -> high nibble of (HL)
     * High nibble of (HL) -> low nibble of (HL)
     *
     * Example: A=0x12, (HL)=0x34 -> A=0x14, (HL)=0x23 */
    case 0x67: {
        uint8_t val = RB(HL());
        uint8_t new_mem = (cpu->a << 4) | (val >> 4);
        cpu->a = (cpu->a & 0xF0) | (val & 0x0F);
        WB(HL(), new_mem);
        cpu->f = sz53p_table[cpu->a] | (cpu->f & Z80_CF);
        return 18;
    }

    /* ====================== RLD ====================== */
    /* Rotate left BCD digit between A and (HL).
     * High nibble of (HL) -> low nibble of A
     * Low nibble of A -> low nibble of (HL)
     * Low nibble of (HL) -> high nibble of (HL)
     *
     * Example: A=0x12, (HL)=0x34 -> A=0x13, (HL)=0x42 */
    case 0x6F: {
        uint8_t val = RB(HL());
        uint8_t new_mem = (val << 4) | (cpu->a & 0x0F);
        cpu->a = (cpu->a & 0xF0) | (val >> 4);
        WB(HL(), new_mem);
        cpu->f = sz53p_table[cpu->a] | (cpu->f & Z80_CF);
        return 18;
    }

    /* ====================== BLOCK TRANSFER ====================== */

    /* LDI: (DE) = (HL), HL++, DE++, BC--.
     * PV is set if BC != 0 after decrement. H=0, N=0.
     * X and Y flags come from A + (HL) (the byte transferred). */
    case 0xA0: {
        uint8_t val = RB(HL());
        WB(DE(), val);
        SET_HL(HL() + 1);
        SET_DE(DE() + 1);
        SET_BC(BC() - 1);
        uint8_t n = cpu->a + val;
        cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_CF))
               | (n & Z80_XF)           /* X = bit 3 of A + val */
               | ((n << 4) & Z80_YF)    /* Y = bit 1 of A + val */
               | (BC() ? Z80_PF : 0);
        return 16;
    }

    /* LDD: same but HL--, DE--. */
    case 0xA8: {
        uint8_t val = RB(HL());
        WB(DE(), val);
        SET_HL(HL() - 1);
        SET_DE(DE() - 1);
        SET_BC(BC() - 1);
        uint8_t n = cpu->a + val;
        cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_CF))
               | (n & Z80_XF)
               | ((n << 4) & Z80_YF)
               | (BC() ? Z80_PF : 0);
        return 16;
    }

    /* LDIR: Repeat LDI until BC=0. 21 T-states per iteration, 16 for last. */
    case 0xB0: {
        uint8_t val = RB(HL());
        WB(DE(), val);
        SET_HL(HL() + 1);
        SET_DE(DE() + 1);
        SET_BC(BC() - 1);
        uint8_t n = cpu->a + val;
        if (BC()) {
            cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_CF))
                   | (n & Z80_XF) | ((n << 4) & Z80_YF) | Z80_PF;
            cpu->pc -= 2;  /* Repeat instruction */
            return 21;
        } else {
            cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_CF))
                   | (n & Z80_XF) | ((n << 4) & Z80_YF);
            return 16;
        }
    }

    /* LDDR: Repeat LDD until BC=0. */
    case 0xB8: {
        uint8_t val = RB(HL());
        WB(DE(), val);
        SET_HL(HL() - 1);
        SET_DE(DE() - 1);
        SET_BC(BC() - 1);
        uint8_t n = cpu->a + val;
        if (BC()) {
            cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_CF))
                   | (n & Z80_XF) | ((n << 4) & Z80_YF) | Z80_PF;
            cpu->pc -= 2;
            return 21;
        } else {
            cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_CF))
                   | (n & Z80_XF) | ((n << 4) & Z80_YF);
            return 16;
        }
    }

    /* ====================== BLOCK SEARCH ====================== */

    /* CPI: Compare A with (HL), HL++, BC--.
     * Z=1 if A==(HL), S from result, H from half-borrow.
     * PV=1 if BC!=0 after dec. C preserved. N=1.
     * X and Y: tricky -- from A - (HL) - H. */
    case 0xA1: {
        uint8_t val = RB(HL());
        int result = cpu->a - val;
        int half = (cpu->a & 0x0F) - (val & 0x0F);
        SET_HL(HL() + 1);
        SET_BC(BC() - 1);
        uint8_t n = result - ((half & 0x10) ? 1 : 0);
        cpu->f = (cpu->f & Z80_CF)
               | ((result & 0x80) ? Z80_SF : 0)
               | ((result & 0xFF) == 0 ? Z80_ZF : 0)
               | (half & 0x10 ? Z80_HF : 0)
               | (n & Z80_XF)
               | ((n << 4) & Z80_YF)
               | (BC() ? Z80_PF : 0)
               | Z80_NF;
        return 16;
    }

    /* CPD: same but HL--. */
    case 0xA9: {
        uint8_t val = RB(HL());
        int result = cpu->a - val;
        int half = (cpu->a & 0x0F) - (val & 0x0F);
        SET_HL(HL() - 1);
        SET_BC(BC() - 1);
        uint8_t n = result - ((half & 0x10) ? 1 : 0);
        cpu->f = (cpu->f & Z80_CF)
               | ((result & 0x80) ? Z80_SF : 0)
               | ((result & 0xFF) == 0 ? Z80_ZF : 0)
               | (half & 0x10 ? Z80_HF : 0)
               | (n & Z80_XF)
               | ((n << 4) & Z80_YF)
               | (BC() ? Z80_PF : 0)
               | Z80_NF;
        return 16;
    }

    /* CPIR: Repeat CPI until BC=0 or match found. */
    case 0xB1: {
        uint8_t val = RB(HL());
        int result = cpu->a - val;
        int half = (cpu->a & 0x0F) - (val & 0x0F);
        SET_HL(HL() + 1);
        SET_BC(BC() - 1);
        uint8_t n = result - ((half & 0x10) ? 1 : 0);
        cpu->f = (cpu->f & Z80_CF)
               | ((result & 0x80) ? Z80_SF : 0)
               | ((result & 0xFF) == 0 ? Z80_ZF : 0)
               | (half & 0x10 ? Z80_HF : 0)
               | (n & Z80_XF)
               | ((n << 4) & Z80_YF)
               | (BC() ? Z80_PF : 0)
               | Z80_NF;
        if (BC() && (result & 0xFF)) {
            cpu->pc -= 2;
            return 21;
        }
        return 16;
    }

    /* CPDR: Repeat CPD until BC=0 or match found. */
    case 0xB9: {
        uint8_t val = RB(HL());
        int result = cpu->a - val;
        int half = (cpu->a & 0x0F) - (val & 0x0F);
        SET_HL(HL() - 1);
        SET_BC(BC() - 1);
        uint8_t n = result - ((half & 0x10) ? 1 : 0);
        cpu->f = (cpu->f & Z80_CF)
               | ((result & 0x80) ? Z80_SF : 0)
               | ((result & 0xFF) == 0 ? Z80_ZF : 0)
               | (half & 0x10 ? Z80_HF : 0)
               | (n & Z80_XF)
               | ((n << 4) & Z80_YF)
               | (BC() ? Z80_PF : 0)
               | Z80_NF;
        if (BC() && (result & 0xFF)) {
            cpu->pc -= 2;
            return 21;
        }
        return 16;
    }

    /* ====================== BLOCK I/O ====================== */

    /* INI: (HL) = IN(C), HL++, B--. */
    case 0xA2: {
        uint8_t val = IN(BC());
        WB(HL(), val);
        SET_HL(HL() + 1);
        cpu->b = alu_dec(cpu, cpu->b);
        /* N is set (from dec), Z from B. Other flags are complex/undocumented. */
        return 16;
    }

    /* IND: (HL) = IN(C), HL--, B--. */
    case 0xAA: {
        uint8_t val = IN(BC());
        WB(HL(), val);
        SET_HL(HL() - 1);
        cpu->b = alu_dec(cpu, cpu->b);
        return 16;
    }

    /* INIR: Repeat INI until B=0. */
    case 0xB2: {
        uint8_t val = IN(BC());
        WB(HL(), val);
        SET_HL(HL() + 1);
        cpu->b = alu_dec(cpu, cpu->b);
        if (cpu->b) {
            cpu->pc -= 2;
            return 21;
        }
        return 16;
    }

    /* INDR: Repeat IND until B=0. */
    case 0xBA: {
        uint8_t val = IN(BC());
        WB(HL(), val);
        SET_HL(HL() - 1);
        cpu->b = alu_dec(cpu, cpu->b);
        if (cpu->b) {
            cpu->pc -= 2;
            return 21;
        }
        return 16;
    }

    /* OUTI: OUT(C) = (HL), HL++, B--. */
    case 0xA3: {
        uint8_t val = RB(HL());
        cpu->b = alu_dec(cpu, cpu->b);
        OUT(BC(), val);
        SET_HL(HL() + 1);
        return 16;
    }

    /* OUTD: OUT(C) = (HL), HL--, B--. */
    case 0xAB: {
        uint8_t val = RB(HL());
        cpu->b = alu_dec(cpu, cpu->b);
        OUT(BC(), val);
        SET_HL(HL() - 1);
        return 16;
    }

    /* OTIR: Repeat OUTI until B=0. */
    case 0xB3: {
        uint8_t val = RB(HL());
        cpu->b = alu_dec(cpu, cpu->b);
        OUT(BC(), val);
        SET_HL(HL() + 1);
        if (cpu->b) {
            cpu->pc -= 2;
            return 21;
        }
        return 16;
    }

    /* OTDR: Repeat OUTD until B=0. */
    case 0xBB: {
        uint8_t val = RB(HL());
        cpu->b = alu_dec(cpu, cpu->b);
        OUT(BC(), val);
        SET_HL(HL() - 1);
        if (cpu->b) {
            cpu->pc -= 2;
            return 21;
        }
        return 16;
    }

    default:
        /* Undocumented ED opcodes act as NOPs (2 bytes, 8 T-states). */
        return 8;
    }
}

/* ===================================================================
 * DD/FD PREFIX EXECUTION (IX/IY instructions)
 * ===================================================================
 * DD-prefixed opcodes use IX in place of HL.
 * FD-prefixed opcodes use IY in place of HL.
 *
 * Most opcodes mirror the unprefixed versions with these substitutions:
 *   - HL -> IX/IY (16-bit operations)
 *   - H -> IXH/IYH, L -> IXL/IYL (undocumented 8-bit operations)
 *   - (HL) -> (IX+d)/(IY+d) (indexed memory access with displacement)
 *
 * If the opcode doesn't reference HL/H/L, the prefix is ignored
 * and the base instruction executes normally.
 *
 * DDCB and FDCB are handled by exec_ddfdcb().
 */
static int exec_ddfd(Z80 *cpu, uint16_t *ixiy) {
    uint8_t op = FETCH();
    INC_R();

    /* DDCB / FDCB: indexed bit operations. */
    if (op == 0xCB) {
        return exec_ddfdcb(cpu, *ixiy);
    }

    /* For instructions that access (HL), we compute (IX+d)/(IY+d).
     * The displacement byte is read for any opcode that would normally
     * access (HL). We detect this based on the opcode pattern. */

    switch (op) {

    /* ====================== 8-bit loads ====================== */
    /* LD r, (IX+d) -- opcodes 0x46,0x4E,0x56,0x5E,0x66,0x6E,0x7E */
    case 0x46: case 0x4E: case 0x56: case 0x5E:
    case 0x66: case 0x6E: case 0x7E: {
        int r = (op >> 3) & 7;
        int8_t d = (int8_t)FETCH();
        write_reg8(cpu, r, RB(*ixiy + d));
        return 19;
    }

    /* LD (IX+d), r -- opcodes 0x70-0x75,0x77 */
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x77: {
        int r = op & 7;
        int8_t d = (int8_t)FETCH();
        WB(*ixiy + d, read_reg8(cpu, r));
        return 19;
    }

    /* LD (IX+d), n */
    case 0x36: {
        int8_t d = (int8_t)FETCH();
        uint8_t n = FETCH();
        WB(*ixiy + d, n);
        return 19;
    }

    /* LD r,r' with IXH/IXL substitution (undocumented).
     * Only when r or r' is 4 (H) or 5 (L) and the other is not 6 ((HL)). */
    case 0x44: write_reg8(cpu, 0, *ixiy >> 8); return 8;     /* LD B,IXH */
    case 0x45: write_reg8(cpu, 0, *ixiy & 0xFF); return 8;   /* LD B,IXL */
    case 0x4C: write_reg8(cpu, 1, *ixiy >> 8); return 8;     /* LD C,IXH */
    case 0x4D: write_reg8(cpu, 1, *ixiy & 0xFF); return 8;   /* LD C,IXL */
    case 0x54: write_reg8(cpu, 2, *ixiy >> 8); return 8;     /* LD D,IXH */
    case 0x55: write_reg8(cpu, 2, *ixiy & 0xFF); return 8;   /* LD D,IXL */
    case 0x5C: write_reg8(cpu, 3, *ixiy >> 8); return 8;     /* LD E,IXH */
    case 0x5D: write_reg8(cpu, 3, *ixiy & 0xFF); return 8;   /* LD E,IXL */
    case 0x7C: cpu->a = *ixiy >> 8; return 8;                /* LD A,IXH */
    case 0x7D: cpu->a = *ixiy & 0xFF; return 8;              /* LD A,IXL */

    case 0x60: *ixiy = (*ixiy & 0x00FF) | (cpu->b << 8); return 8;  /* LD IXH,B */
    case 0x61: *ixiy = (*ixiy & 0x00FF) | (cpu->c << 8); return 8;  /* LD IXH,C */
    case 0x62: *ixiy = (*ixiy & 0x00FF) | (cpu->d << 8); return 8;  /* LD IXH,D */
    case 0x63: *ixiy = (*ixiy & 0x00FF) | (cpu->e << 8); return 8;  /* LD IXH,E */
    case 0x64: break; /* LD IXH,IXH = NOP */
    case 0x65: *ixiy = (*ixiy & 0x00FF) | ((*ixiy & 0xFF) << 8); return 8; /* LD IXH,IXL */
    case 0x67: *ixiy = (*ixiy & 0x00FF) | (cpu->a << 8); return 8;  /* LD IXH,A */

    case 0x68: *ixiy = (*ixiy & 0xFF00) | cpu->b; return 8;         /* LD IXL,B */
    case 0x69: *ixiy = (*ixiy & 0xFF00) | cpu->c; return 8;         /* LD IXL,C */
    case 0x6A: *ixiy = (*ixiy & 0xFF00) | cpu->d; return 8;         /* LD IXL,D */
    case 0x6B: *ixiy = (*ixiy & 0xFF00) | cpu->e; return 8;         /* LD IXL,E */
    case 0x6C: *ixiy = (*ixiy & 0xFF00) | (*ixiy >> 8); return 8;   /* LD IXL,IXH */
    case 0x6D: break; /* LD IXL,IXL = NOP */
    case 0x6F: *ixiy = (*ixiy & 0xFF00) | cpu->a; return 8;         /* LD IXL,A */

    /* ====================== 16-bit loads ====================== */
    case 0x21: *ixiy = FETCH16(); return 14;              /* LD IX,nn */
    case 0x22: { uint16_t a = FETCH16(); WW(a, *ixiy); return 20; } /* LD (nn),IX */
    case 0x2A: { uint16_t a = FETCH16(); *ixiy = RW(a); return 20; } /* LD IX,(nn) */
    case 0xF9: cpu->sp = *ixiy; return 10;               /* LD SP,IX */

    /* ====================== 16-bit arithmetic ====================== */
    case 0x09: *ixiy = alu_add16(cpu, *ixiy, BC()); return 15;   /* ADD IX,BC */
    case 0x19: *ixiy = alu_add16(cpu, *ixiy, DE()); return 15;   /* ADD IX,DE */
    case 0x29: *ixiy = alu_add16(cpu, *ixiy, *ixiy); return 15;  /* ADD IX,IX */
    case 0x39: *ixiy = alu_add16(cpu, *ixiy, cpu->sp); return 15; /* ADD IX,SP */

    case 0x23: (*ixiy)++; return 10;                      /* INC IX */
    case 0x2B: (*ixiy)--; return 10;                      /* DEC IX */

    /* ====================== 8-bit INC/DEC IXH/IXL ====================== */
    case 0x24: /* INC IXH */
        *ixiy = (*ixiy & 0x00FF) | (alu_inc(cpu, *ixiy >> 8) << 8);
        return 8;
    case 0x25: /* DEC IXH */
        *ixiy = (*ixiy & 0x00FF) | (alu_dec(cpu, *ixiy >> 8) << 8);
        return 8;
    case 0x2C: /* INC IXL */
        *ixiy = (*ixiy & 0xFF00) | alu_inc(cpu, *ixiy & 0xFF);
        return 8;
    case 0x2D: /* DEC IXL */
        *ixiy = (*ixiy & 0xFF00) | alu_dec(cpu, *ixiy & 0xFF);
        return 8;

    /* ====================== INC/DEC (IX+d) ====================== */
    case 0x34: {
        int8_t d = (int8_t)FETCH();
        uint16_t addr = *ixiy + d;
        WB(addr, alu_inc(cpu, RB(addr)));
        return 23;
    }
    case 0x35: {
        int8_t d = (int8_t)FETCH();
        uint16_t addr = *ixiy + d;
        WB(addr, alu_dec(cpu, RB(addr)));
        return 23;
    }

    /* ====================== LD IXH/IXL, n ====================== */
    case 0x26: /* LD IXH,n */
        *ixiy = (*ixiy & 0x00FF) | (FETCH() << 8);
        return 11;
    case 0x2E: /* LD IXL,n */
        *ixiy = (*ixiy & 0xFF00) | FETCH();
        return 11;

    /* ====================== PUSH/POP IX ====================== */
    case 0xE5: PUSH(*ixiy); return 15;                   /* PUSH IX */
    case 0xE1: *ixiy = POP(); return 14;                 /* POP IX */

    /* ====================== EX (SP),IX ====================== */
    case 0xE3: {
        uint16_t tmp = RW(cpu->sp);
        WW(cpu->sp, *ixiy);
        *ixiy = tmp;
        return 23;
    }

    /* ====================== JP (IX) ====================== */
    case 0xE9: cpu->pc = *ixiy; return 8;

    /* ====================== ALU with (IX+d) ====================== */
    case 0x86: { int8_t d=(int8_t)FETCH(); alu_add(cpu, RB(*ixiy+d), 0); return 19; }
    case 0x8E: { int8_t d=(int8_t)FETCH(); alu_add(cpu, RB(*ixiy+d), 1); return 19; }
    case 0x96: { int8_t d=(int8_t)FETCH(); alu_sub(cpu, RB(*ixiy+d), 0); return 19; }
    case 0x9E: { int8_t d=(int8_t)FETCH(); alu_sub(cpu, RB(*ixiy+d), 1); return 19; }
    case 0xA6: { int8_t d=(int8_t)FETCH(); alu_and(cpu, RB(*ixiy+d)); return 19; }
    case 0xAE: { int8_t d=(int8_t)FETCH(); alu_xor(cpu, RB(*ixiy+d)); return 19; }
    case 0xB6: { int8_t d=(int8_t)FETCH(); alu_or(cpu, RB(*ixiy+d)); return 19; }
    case 0xBE: { int8_t d=(int8_t)FETCH(); alu_cp(cpu, RB(*ixiy+d)); return 19; }

    /* ====================== ALU with IXH/IXL (undocumented) ====================== */
    case 0x84: alu_add(cpu, *ixiy >> 8, 0); return 8;     /* ADD A,IXH */
    case 0x85: alu_add(cpu, *ixiy & 0xFF, 0); return 8;   /* ADD A,IXL */
    case 0x8C: alu_add(cpu, *ixiy >> 8, 1); return 8;     /* ADC A,IXH */
    case 0x8D: alu_add(cpu, *ixiy & 0xFF, 1); return 8;   /* ADC A,IXL */
    case 0x94: alu_sub(cpu, *ixiy >> 8, 0); return 8;     /* SUB IXH */
    case 0x95: alu_sub(cpu, *ixiy & 0xFF, 0); return 8;   /* SUB IXL */
    case 0x9C: alu_sub(cpu, *ixiy >> 8, 1); return 8;     /* SBC A,IXH */
    case 0x9D: alu_sub(cpu, *ixiy & 0xFF, 1); return 8;   /* SBC A,IXL */
    case 0xA4: alu_and(cpu, *ixiy >> 8); return 8;        /* AND IXH */
    case 0xA5: alu_and(cpu, *ixiy & 0xFF); return 8;      /* AND IXL */
    case 0xAC: alu_xor(cpu, *ixiy >> 8); return 8;        /* XOR IXH */
    case 0xAD: alu_xor(cpu, *ixiy & 0xFF); return 8;      /* XOR IXL */
    case 0xB4: alu_or(cpu, *ixiy >> 8); return 8;         /* OR IXH */
    case 0xB5: alu_or(cpu, *ixiy & 0xFF); return 8;       /* OR IXL */
    case 0xBC: alu_cp(cpu, *ixiy >> 8); return 8;         /* CP IXH */
    case 0xBD: alu_cp(cpu, *ixiy & 0xFF); return 8;       /* CP IXL */

    default:
        /* If this opcode doesn't use HL/H/L/(HL), DD/FD acts as an
         * ignored prefix and adds 4 T-states.
         *
         * Collapse long DD/FD prefix chains iteratively so pathological
         * input can't build deep recursion. Keep only the last prefix as
         * effective, as on real Z80 hardware. */
        if (op == 0xDD || op == 0xFD) {
            int prefixes = 1; /* Includes the currently-executing prefix. */
            uint16_t *last_prefix = (op == 0xDD) ? &cpu->ix : &cpu->iy;

            do {
                op = FETCH();
                INC_R();
                prefixes++;
                if (op == 0xDD || op == 0xFD)
                    last_prefix = (op == 0xDD) ? &cpu->ix : &cpu->iy;
            } while (op == 0xDD || op == 0xFD);

            /* We fetched one non-prefix byte too far: rewind PC/R so the
             * next exec_ddfd() re-reads it exactly once. */
            cpu->pc--;
            cpu->r = (cpu->r & 0x80) | ((cpu->r - 1) & 0x7F);

            /* exec_ddfd(last_prefix) accounts for one prefix. Add 4T for
             * each older ignored prefix in the collapsed chain. */
            return (prefixes - 1) * 4 + exec_ddfd(cpu, last_prefix);
        }
        return 4 + exec_opcode(cpu, op);
    }

    return 8; /* Fallthrough for NOP-like cases (LD IXH,IXH etc.) */
}

/* ===================================================================
 * MAIN OPCODE EXECUTION
 * ===================================================================
 * Execute a fetched opcode (possibly a prefix) and return its T-states.
 * This helper does not fetch the first opcode byte and does not update
 * cpu->clocks.
 */
static int exec_opcode(Z80 *cpu, uint8_t op) {
    int tstates = 0;

    /* Prefixed instructions. */
    if (op == 0xCB) {
        return exec_cb(cpu);
    }
    if (op == 0xED) {
        return exec_ed(cpu);
    }
    if (op == 0xDD) {
        return exec_ddfd(cpu, &cpu->ix);
    }
    if (op == 0xFD) {
        return exec_ddfd(cpu, &cpu->iy);
    }

    /* Decode the unprefixed opcode.
     *
     * Z80 opcodes follow patterns based on octal grouping:
     *   Bits 7-6 (x): major group
     *   Bits 5-3 (y): typically destination register or condition
     *   Bits 2-0 (z): typically source register or sub-operation
     *
     * Additional breakdown of y:
     *   Bits 5-4 (p): register pair index
     *   Bit 3 (q): sub-selector within pair operations
     */
    int x = (op >> 6) & 3;
    int y = (op >> 3) & 7;
    int z = op & 7;
    int p = (op >> 4) & 3;
    int q = (op >> 3) & 1;

    switch (x) {
    case 0:
        switch (z) {
        case 0:
            switch (y) {
            case 0: /* NOP */
                tstates = 4;
                break;
            case 1: /* EX AF,AF' */
                { uint8_t t;
                  t = cpu->a; cpu->a = cpu->a_; cpu->a_ = t;
                  t = cpu->f; cpu->f = cpu->f_; cpu->f_ = t;
                }
                tstates = 4;
                break;
            case 2: /* DJNZ d -- Decrement B, jump if not zero.
                      * This is a common loop instruction: set B to count,
                      * DJNZ loops back until B reaches 0. */
                { int8_t d = (int8_t)FETCH();
                  cpu->b--;
                  if (cpu->b) {
                      cpu->pc += d;
                      tstates = 13;
                  } else {
                      tstates = 8;
                  }
                }
                break;
            case 3: /* JR d -- Relative jump (unconditional). */
                { int8_t d = (int8_t)FETCH();
                  cpu->pc += d;
                  tstates = 12;
                }
                break;
            case 4: case 5: case 6: case 7:
                /* JR cc, d -- Conditional relative jump.
                 * cc: 4=NZ, 5=Z, 6=NC, 7=C (mapped to conditions 0-3) */
                { int8_t d = (int8_t)FETCH();
                  if (check_condition(cpu, y - 4)) {
                      cpu->pc += d;
                      tstates = 12;
                  } else {
                      tstates = 7;
                  }
                }
                break;
            }
            break;

        case 1:
            if (q == 0) {
                /* LD rp, nn -- Load 16-bit immediate into register pair. */
                write_rp(cpu, p, FETCH16());
                tstates = 10;
            } else {
                /* ADD HL, rp -- 16-bit addition. */
                SET_HL(alu_add16(cpu, HL(), read_rp(cpu, p)));
                tstates = 11;
            }
            break;

        case 2:
            /* Indirect loads. */
            if (q == 0) {
                switch (p) {
                case 0: WB(BC(), cpu->a); tstates = 7; break;   /* LD (BC),A */
                case 1: WB(DE(), cpu->a); tstates = 7; break;   /* LD (DE),A */
                case 2: { uint16_t a = FETCH16(); WW(a, HL()); tstates = 16; break; } /* LD (nn),HL */
                case 3: { uint16_t a = FETCH16(); WB(a, cpu->a); tstates = 13; break; } /* LD (nn),A */
                }
            } else {
                switch (p) {
                case 0: cpu->a = RB(BC()); tstates = 7; break;  /* LD A,(BC) */
                case 1: cpu->a = RB(DE()); tstates = 7; break;  /* LD A,(DE) */
                case 2: { uint16_t a = FETCH16(); SET_HL(RW(a)); tstates = 16; break; } /* LD HL,(nn) */
                case 3: { uint16_t a = FETCH16(); cpu->a = RB(a); tstates = 13; break; } /* LD A,(nn) */
                }
            }
            break;

        case 3:
            /* INC/DEC 16-bit register pair. No flags affected. */
            if (q == 0) {
                write_rp(cpu, p, read_rp(cpu, p) + 1);  /* INC rp */
            } else {
                write_rp(cpu, p, read_rp(cpu, p) - 1);  /* DEC rp */
            }
            tstates = 6;
            break;

        case 4:
            /* INC r -- 8-bit increment. */
            write_reg8(cpu, y, alu_inc(cpu, read_reg8(cpu, y)));
            tstates = (y == 6) ? 11 : 4;
            break;

        case 5:
            /* DEC r -- 8-bit decrement. */
            write_reg8(cpu, y, alu_dec(cpu, read_reg8(cpu, y)));
            tstates = (y == 6) ? 11 : 4;
            break;

        case 6:
            /* LD r, n -- Load 8-bit immediate. */
            write_reg8(cpu, y, FETCH());
            tstates = (y == 6) ? 10 : 7;
            break;

        case 7:
            /* Miscellaneous single-byte operations. */
            switch (y) {
            case 0: op_rlca(cpu); break;  /* RLCA */
            case 1: op_rrca(cpu); break;  /* RRCA */
            case 2: op_rla(cpu); break;   /* RLA */
            case 3: op_rra(cpu); break;   /* RRA */
            case 4: op_daa(cpu); break;   /* DAA */
            case 5: /* CPL -- Complement A (flip all bits).
                      * Sets H and N, preserves other flags. X, Y from result. */
                cpu->a = ~cpu->a;
                cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_PF | Z80_CF))
                       | (cpu->a & (Z80_XF | Z80_YF))
                       | Z80_HF | Z80_NF;
                break;
            case 6: /* SCF -- Set carry flag.
                      * C=1, H=0, N=0. X, Y from A. S, Z, PV preserved. */
                cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_PF))
                       | (cpu->a & (Z80_XF | Z80_YF))
                       | Z80_CF;
                break;
            case 7: /* CCF -- Complement carry flag.
                      * C is flipped, H = old C, N=0. X, Y from A. */
                cpu->f = (cpu->f & (Z80_SF | Z80_ZF | Z80_PF))
                       | (cpu->a & (Z80_XF | Z80_YF))
                       | ((cpu->f & Z80_CF) ? Z80_HF : 0)
                       | ((cpu->f & Z80_CF) ? 0 : Z80_CF);
                break;
            }
            tstates = 4;
            break;
        }
        break;

    case 1:
        /* x=1: LD r, r' (register to register loads).
         * Special case: LD (HL),(HL) (opcode 0x76) = HALT. */
        if (y == 6 && z == 6) {
            /* HALT: CPU stops executing, stays at this address.
             * NOP instructions are executed internally. The CPU
             * resumes when an interrupt (or NMI) occurs. */
            cpu->halted = 1;
            cpu->pc--; /* Keep PC on the HALT instruction */
            tstates = 4;
        } else {
            write_reg8(cpu, y, read_reg8(cpu, z));
            tstates = (y == 6 || z == 6) ? 7 : 4;
        }
        break;

    case 2:
        /* x=2: ALU A, r -- 8-bit ALU operations with register.
         * y selects the operation, z selects the source register. */
        { uint8_t val = read_reg8(cpu, z);
          switch (y) {
          case 0: alu_add(cpu, val, 0); break;  /* ADD A,r */
          case 1: alu_add(cpu, val, 1); break;  /* ADC A,r */
          case 2: alu_sub(cpu, val, 0); break;  /* SUB r */
          case 3: alu_sub(cpu, val, 1); break;  /* SBC A,r */
          case 4: alu_and(cpu, val); break;      /* AND r */
          case 5: alu_xor(cpu, val); break;      /* XOR r */
          case 6: alu_or(cpu, val); break;       /* OR r */
          case 7: alu_cp(cpu, val); break;       /* CP r */
          }
        }
        tstates = (z == 6) ? 7 : 4;
        break;

    case 3:
        switch (z) {
        case 0:
            /* RET cc -- Conditional return. */
            if (check_condition(cpu, y)) {
                cpu->pc = POP();
                tstates = 11;
            } else {
                tstates = 5;
            }
            break;

        case 1:
            if (q == 0) {
                /* POP rp2 -- Pop 16-bit register pair.
                 * Here rp2 uses AF instead of SP: BC,DE,HL,AF. */
                switch (p) {
                case 0: SET_BC(POP()); break;
                case 1: SET_DE(POP()); break;
                case 2: SET_HL(POP()); break;
                case 3: SET_AF(POP()); break;
                }
                tstates = 10;
            } else {
                switch (p) {
                case 0: /* RET */
                    cpu->pc = POP();
                    tstates = 10;
                    break;
                case 1: /* EXX -- Exchange BC,DE,HL with shadow registers. */
                    { uint8_t t;
                      t = cpu->b; cpu->b = cpu->b_; cpu->b_ = t;
                      t = cpu->c; cpu->c = cpu->c_; cpu->c_ = t;
                      t = cpu->d; cpu->d = cpu->d_; cpu->d_ = t;
                      t = cpu->e; cpu->e = cpu->e_; cpu->e_ = t;
                      t = cpu->h; cpu->h = cpu->h_; cpu->h_ = t;
                      t = cpu->l; cpu->l = cpu->l_; cpu->l_ = t;
                    }
                    tstates = 4;
                    break;
                case 2: /* JP (HL) -- Jump to address in HL.
                         * Despite the mnemonic, this is NOT indirect:
                         * it loads HL into PC, not (HL). */
                    cpu->pc = HL();
                    tstates = 4;
                    break;
                case 3: /* LD SP,HL */
                    cpu->sp = HL();
                    tstates = 6;
                    break;
                }
            }
            break;

        case 2:
            /* JP cc, nn -- Conditional absolute jump. */
            { uint16_t addr = FETCH16();
              if (check_condition(cpu, y)) {
                  cpu->pc = addr;
              }
            }
            tstates = 10;
            break;

        case 3:
            switch (y) {
            case 0: /* JP nn -- Unconditional absolute jump. */
                cpu->pc = FETCH16();
                tstates = 10;
                break;
            case 1: /* CB prefix -- already handled above. */
                break;
            case 2: /* OUT (n),A -- Output A to port (A<<8 | n). */
                { uint8_t port = FETCH();
                  OUT((cpu->a << 8) | port, cpu->a);
                }
                tstates = 11;
                break;
            case 3: /* IN A,(n) -- Input from port (A<<8 | n). */
                { uint8_t port = FETCH();
                  cpu->a = IN((cpu->a << 8) | port);
                }
                tstates = 11;
                break;
            case 4: /* EX (SP),HL -- Exchange top of stack with HL. */
                { uint16_t tmp = RW(cpu->sp);
                  WW(cpu->sp, HL());
                  SET_HL(tmp);
                }
                tstates = 19;
                break;
            case 5: /* EX DE,HL -- Exchange DE and HL. */
                { uint8_t t;
                  t = cpu->d; cpu->d = cpu->h; cpu->h = t;
                  t = cpu->e; cpu->e = cpu->l; cpu->l = t;
                }
                tstates = 4;
                break;
            case 6: /* DI -- Disable interrupts.
                      * Both IFF1 and IFF2 are reset. */
                cpu->iff1 = 0;
                cpu->iff2 = 0;
                cpu->ei_delay = 0;
                tstates = 4;
                break;
            case 7: /* EI -- Enable interrupts.
                      * Interrupts are not accepted until after the
                      * NEXT instruction (to allow a RET or similar). */
                cpu->iff1 = 1;
                cpu->iff2 = 1;
                cpu->ei_delay = 1;
                tstates = 4;
                break;
            }
            break;

        case 4:
            /* CALL cc, nn -- Conditional subroutine call. */
            { uint16_t addr = FETCH16();
              if (check_condition(cpu, y)) {
                  PUSH(cpu->pc);
                  cpu->pc = addr;
                  tstates = 17;
              } else {
                  tstates = 10;
              }
            }
            break;

        case 5:
            if (q == 0) {
                /* PUSH rp2 -- Push register pair (BC,DE,HL,AF). */
                switch (p) {
                case 0: PUSH(BC()); break;
                case 1: PUSH(DE()); break;
                case 2: PUSH(HL()); break;
                case 3: PUSH(AF()); break;
                }
                tstates = 11;
            } else {
                switch (p) {
                case 0: /* CALL nn -- Unconditional subroutine call. */
                    { uint16_t addr = FETCH16();
                      PUSH(cpu->pc);
                      cpu->pc = addr;
                    }
                    tstates = 17;
                    break;
                case 1: /* DD prefix -- already handled above. */
                    break;
                case 2: /* ED prefix -- already handled above. */
                    break;
                case 3: /* FD prefix -- already handled above. */
                    break;
                }
            }
            break;

        case 6:
            /* ALU A, n -- 8-bit ALU operation with immediate. */
            { uint8_t val = FETCH();
              switch (y) {
              case 0: alu_add(cpu, val, 0); break;
              case 1: alu_add(cpu, val, 1); break;
              case 2: alu_sub(cpu, val, 0); break;
              case 3: alu_sub(cpu, val, 1); break;
              case 4: alu_and(cpu, val); break;
              case 5: alu_xor(cpu, val); break;
              case 6: alu_or(cpu, val); break;
              case 7: alu_cp(cpu, val); break;
              }
            }
            tstates = 7;
            break;

        case 7:
            /* RST y*8 -- Restart (fast CALL to fixed address).
             * Used heavily by the ZX Spectrum ROM for system calls.
             * RST 0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38. */
            PUSH(cpu->pc);
            cpu->pc = y * 8;
            tstates = 11;
            break;
        }
        break;
    }

    return tstates;
}

int z80_step(Z80 *cpu) {
    int tstates;

    /* EI enables maskable interrupts only after one full subsequent
     * instruction has completed. */
    if (cpu->ei_delay)
        cpu->ei_delay = 0;

    /* If halted, execute NOPs until an interrupt wakes us up. */
    if (cpu->halted) {
        INC_R();
        tstates = 4;
        cpu->clocks += tstates;
        return tstates;
    }

    uint8_t op = FETCH();
    INC_R();
    tstates = exec_opcode(cpu, op);
    cpu->clocks += tstates;
    return tstates;
}

/* ===================================================================
 * CPU INITIALIZATION
 * =================================================================== */

void z80_init(Z80 *cpu) {
    init_tables();
    memset(cpu, 0, sizeof(Z80));
    /* Power-on/reset values are not fully deterministic on real hardware.
     * We use common emulator defaults for compatibility: PC=0, SP=0xFFFF,
     * A=0xFF, F=0xFF, other fields zeroed. The host ROM will quickly
     * initialize the rest of the machine state. */
    cpu->sp = 0xFFFF;
    cpu->a = 0xFF;
    cpu->f = 0xFF;
}

/* ===================================================================
 * INTERRUPT HANDLING
 * =================================================================== */

/* Process a maskable interrupt request.
 * Returns the number of T-states consumed, or 0 if interrupts are disabled.
 *
 * The Z80 supports three interrupt modes:
 *   IM 0: The interrupting device places an instruction on the data bus.
 *          Typically RST 38h (0xFF), but any opcode is possible.
 *   IM 1: Always executes RST 38h regardless of the data byte.
 *          This is what the ZX Spectrum uses.
 *   IM 2: Vectored interrupt. The I register provides the high byte
 *          of a pointer table address, and the data byte provides the
 *          low byte. The CPU reads a 16-bit address from that table
 *          entry and jumps to it.
 */
int z80_interrupt(Z80 *cpu, uint8_t data) {
    if (!cpu->iff1 || cpu->ei_delay) return 0;

    /* Acknowledge the interrupt. If halted, advance PC past the HALT
     * instruction so that after the ISR returns, execution continues
     * with the instruction following HALT (not HALT itself). */
    cpu->iff1 = 0;
    cpu->iff2 = 0;
    cpu->ei_delay = 0;
    if (cpu->halted) {
        cpu->halted = 0;
        cpu->pc++;  /* Skip past the HALT opcode */
    }

    switch (cpu->im) {
    case 0:
        /* IM 0: execute the opcode supplied on the data bus.
         * Interrupt acknowledge is an M1 cycle (increments R) and adds
         * 2 T-states over normal instruction timing.
         *
         * This is exact for the common RST flow. For exotic multi-byte
         * IM 0 opcodes, the post-ack execution timing depends on how the
         * external device drives subsequent bytes on real hardware. */
        INC_R();
        return exec_opcode(cpu, data) + 2;

    case 1:
        /* IM 1: Always RST 38h. */
        INC_R();
        PUSH(cpu->pc);
        cpu->pc = 0x0038;
        return 13;

    case 2:
        /* IM 2: Vectored. Read jump address from (I << 8 | data). */
        INC_R();
        PUSH(cpu->pc);
        { uint16_t vector_addr = (cpu->i << 8) | data;
          cpu->pc = RW(vector_addr);
        }
        return 19;

    default:
        return 0;
    }
}

/* Process a non-maskable interrupt (NMI).
 * NMI is edge-triggered and cannot be disabled. It always vectors to 0x0066.
 * IFF1 is saved to IFF2 (so it can be restored by RETN), then IFF1 is reset. */
int z80_nmi(Z80 *cpu) {
    if (cpu->halted) {
        cpu->halted = 0;
        cpu->pc++;  /* Skip past HALT */
    }
    cpu->iff2 = cpu->iff1;
    cpu->iff1 = 0;
    cpu->ei_delay = 0;
    PUSH(cpu->pc);
    cpu->pc = 0x0066;
    return 11;
}

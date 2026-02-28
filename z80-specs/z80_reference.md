# Z80 CPU Comprehensive Reference for Emulator Development

## Table of Contents

1. [Register Layout](#1-register-layout)
2. [Flag Register Details](#2-flag-register-details)
3. [Complete Opcode Tables](#3-complete-opcode-tables)
4. [CB-Prefixed Opcodes](#4-cb-prefixed-opcodes)
5. [ED-Prefixed Opcodes](#5-ed-prefixed-opcodes)
6. [DD-Prefixed Opcodes (IX)](#6-dd-prefixed-opcodes-ix)
7. [FD-Prefixed Opcodes (IY)](#7-fd-prefixed-opcodes-iy)
8. [DD CB / FD CB Prefixed Opcodes](#8-dd-cb--fd-cb-prefixed-opcodes)
9. [Undocumented Opcodes](#9-undocumented-opcodes)
10. [Flag Behavior Per Instruction](#10-flag-behavior-per-instruction)
11. [Undocumented Flags (Bits 3 and 5)](#11-undocumented-flags-bits-3-and-5)
12. [MEMPTR/WZ Register](#12-memptrwz-register)
13. [Interrupt Handling](#13-interrupt-handling)
14. [HALT Behavior](#14-halt-behavior)
15. [DAA Algorithm](#15-daa-algorithm)
16. [R Register Behavior](#16-r-register-behavior)
17. [Opcode Decoding Algorithm](#17-opcode-decoding-algorithm)
18. [Test Suites](#18-test-suites)

---

## 1. Register Layout

The Z80 CPU contains 208 bits of read/write memory organized as registers:

### Main Register Set
```
  7       0  7       0
+----------+----------+
|    A     |    F     |   Accumulator and Flags
+----------+----------+
|    B     |    C     |   General Purpose
+----------+----------+
|    D     |    E     |   General Purpose
+----------+----------+
|    H     |    L     |   General Purpose
+----------+----------+
```

### Alternate Register Set
```
  7       0  7       0
+----------+----------+
|    A'    |    F'    |   Alternate Accumulator and Flags
+----------+----------+
|    B'    |    C'    |   Alternate General Purpose
+----------+----------+
|    D'    |    E'    |   Alternate General Purpose
+----------+----------+
|    H'    |    L'    |   Alternate General Purpose
+----------+----------+
```

### Special Purpose Registers
```
  15                 0
+---------------------+
|         IX          |   Index Register X (16-bit)
+---------------------+
|         IY          |   Index Register Y (16-bit)
+---------------------+
|         SP          |   Stack Pointer (16-bit)
+---------------------+
|         PC          |   Program Counter (16-bit)
+---------------------+

  7       0
+----------+
|    I     |   Interrupt Page Address Register (8-bit)
+----------+
|    R     |   Memory Refresh Register (8-bit, 7-bit counter)
+----------+
```

### Internal State (not directly accessible as registers)
```
IFF1    - Interrupt Flip-Flop 1 (controls maskable interrupt acceptance)
IFF2    - Interrupt Flip-Flop 2 (temporary storage of IFF1 during NMI)
IM      - Interrupt Mode (0, 1, or 2)
MEMPTR  - Internal WZ register (16-bit, undocumented, affects flag bits 3/5)
Q       - Internal ALU latch (affects SCF/CCF flag behavior)
```

### Register Pair Usage
| Pair | High | Low | Primary Use |
|------|------|-----|-------------|
| AF   | A    | F   | Accumulator + Flags |
| BC   | B    | C   | Counter / Port address |
| DE   | D    | E   | Destination address |
| HL   | H    | L   | Memory pointer (indirect addressing) |
| IX   | IXH* | IXL* | Index register (base+displacement) |
| IY   | IYH* | IYL* | Index register (base+displacement) |

(*) IXH, IXL, IYH, IYL are undocumented but functional 8-bit halves.

---

## 2. Flag Register Details

The F register (and F') is organized as follows:

```
Bit:  7    6    5    4    3    2    1    0
      S    Z    Y    H    X   P/V   N    C
```

| Bit | Name | Also Called | Description |
|-----|------|-------------|-------------|
| 7   | S    | Sign        | Set if result is negative (bit 7 of result = 1) |
| 6   | Z    | Zero        | Set if result is zero |
| 5   | Y    | F5, YF      | Undocumented: usually copy of bit 5 of result |
| 4   | H    | Half Carry  | Set if carry from bit 3 to bit 4 (8-bit) or bit 11 to 12 (16-bit) |
| 3   | X    | F3, XF      | Undocumented: usually copy of bit 3 of result |
| 2   | P/V  | Parity/Overflow | Parity (even=1) for logic ops; Overflow for arithmetic |
| 1   | N    | Add/Subtract | Set for subtract operations; cleared for add. Used by DAA |
| 0   | C    | Carry       | Set if carry out of bit 7 (8-bit) or bit 15 (16-bit) |

### Flag Notation in Tables Below
| Symbol | Meaning |
|--------|---------|
| *      | Affected according to operation result |
| 0      | Reset to 0 |
| 1      | Set to 1 |
| -      | Not affected |
| P      | Parity flag (even parity = 1) |
| V      | Overflow flag |
| ?      | Unknown/undefined |

---

## 3. Complete Opcode Tables -- Unprefixed (00-FF)

### Notation
- `d8` = 8-bit immediate data (1 byte follows opcode)
- `d16` = 16-bit immediate data (2 bytes follow opcode, little-endian)
- `a16` = 16-bit address (2 bytes follow opcode, little-endian)
- `r8` = 8-bit signed relative offset (-128 to +127)
- `cc` = condition code: NZ, Z, NC, C, PO, PE, P, M
- T-states shown as `taken/not-taken` for conditional instructions

### Row 0x (00-0F)

| Hex | Mnemonic | Bytes | T-states | S | Z | H | P/V | N | C |
|-----|----------|-------|----------|---|---|---|-----|---|---|
| 00  | NOP | 1 | 4 | - | - | - | - | - | - |
| 01  | LD BC,d16 | 3 | 10 | - | - | - | - | - | - |
| 02  | LD (BC),A | 1 | 7 | - | - | - | - | - | - |
| 03  | INC BC | 1 | 6 | - | - | - | - | - | - |
| 04  | INC B | 1 | 4 | * | * | * | V | 0 | - |
| 05  | DEC B | 1 | 4 | * | * | * | V | 1 | - |
| 06  | LD B,d8 | 2 | 7 | - | - | - | - | - | - |
| 07  | RLCA | 1 | 4 | - | - | 0 | - | 0 | * |
| 08  | EX AF,AF' | 1 | 4 | - | - | - | - | - | - |
| 09  | ADD HL,BC | 1 | 11 | - | - | * | - | 0 | * |
| 0A  | LD A,(BC) | 1 | 7 | - | - | - | - | - | - |
| 0B  | DEC BC | 1 | 6 | - | - | - | - | - | - |
| 0C  | INC C | 1 | 4 | * | * | * | V | 0 | - |
| 0D  | DEC C | 1 | 4 | * | * | * | V | 1 | - |
| 0E  | LD C,d8 | 2 | 7 | - | - | - | - | - | - |
| 0F  | RRCA | 1 | 4 | - | - | 0 | - | 0 | * |

### Row 1x (10-1F)

| Hex | Mnemonic | Bytes | T-states | S | Z | H | P/V | N | C |
|-----|----------|-------|----------|---|---|---|-----|---|---|
| 10  | DJNZ r8 | 2 | 13/8 | - | - | - | - | - | - |
| 11  | LD DE,d16 | 3 | 10 | - | - | - | - | - | - |
| 12  | LD (DE),A | 1 | 7 | - | - | - | - | - | - |
| 13  | INC DE | 1 | 6 | - | - | - | - | - | - |
| 14  | INC D | 1 | 4 | * | * | * | V | 0 | - |
| 15  | DEC D | 1 | 4 | * | * | * | V | 1 | - |
| 16  | LD D,d8 | 2 | 7 | - | - | - | - | - | - |
| 17  | RLA | 1 | 4 | - | - | 0 | - | 0 | * |
| 18  | JR r8 | 2 | 12 | - | - | - | - | - | - |
| 19  | ADD HL,DE | 1 | 11 | - | - | * | - | 0 | * |
| 1A  | LD A,(DE) | 1 | 7 | - | - | - | - | - | - |
| 1B  | DEC DE | 1 | 6 | - | - | - | - | - | - |
| 1C  | INC E | 1 | 4 | * | * | * | V | 0 | - |
| 1D  | DEC E | 1 | 4 | * | * | * | V | 1 | - |
| 1E  | LD E,d8 | 2 | 7 | - | - | - | - | - | - |
| 1F  | RRA | 1 | 4 | - | - | 0 | - | 0 | * |

### Row 2x (20-2F)

| Hex | Mnemonic | Bytes | T-states | S | Z | H | P/V | N | C |
|-----|----------|-------|----------|---|---|---|-----|---|---|
| 20  | JR NZ,r8 | 2 | 12/7 | - | - | - | - | - | - |
| 21  | LD HL,d16 | 3 | 10 | - | - | - | - | - | - |
| 22  | LD (a16),HL | 3 | 16 | - | - | - | - | - | - |
| 23  | INC HL | 1 | 6 | - | - | - | - | - | - |
| 24  | INC H | 1 | 4 | * | * | * | V | 0 | - |
| 25  | DEC H | 1 | 4 | * | * | * | V | 1 | - |
| 26  | LD H,d8 | 2 | 7 | - | - | - | - | - | - |
| 27  | DAA | 1 | 4 | * | * | * | P | - | * |
| 28  | JR Z,r8 | 2 | 12/7 | - | - | - | - | - | - |
| 29  | ADD HL,HL | 1 | 11 | - | - | * | - | 0 | * |
| 2A  | LD HL,(a16) | 3 | 16 | - | - | - | - | - | - |
| 2B  | DEC HL | 1 | 6 | - | - | - | - | - | - |
| 2C  | INC L | 1 | 4 | * | * | * | V | 0 | - |
| 2D  | DEC L | 1 | 4 | * | * | * | V | 1 | - |
| 2E  | LD L,d8 | 2 | 7 | - | - | - | - | - | - |
| 2F  | CPL | 1 | 4 | - | - | 1 | - | 1 | - |

### Row 3x (30-3F)

| Hex | Mnemonic | Bytes | T-states | S | Z | H | P/V | N | C |
|-----|----------|-------|----------|---|---|---|-----|---|---|
| 30  | JR NC,r8 | 2 | 12/7 | - | - | - | - | - | - |
| 31  | LD SP,d16 | 3 | 10 | - | - | - | - | - | - |
| 32  | LD (a16),A | 3 | 13 | - | - | - | - | - | - |
| 33  | INC SP | 1 | 6 | - | - | - | - | - | - |
| 34  | INC (HL) | 1 | 11 | * | * | * | V | 0 | - |
| 35  | DEC (HL) | 1 | 11 | * | * | * | V | 1 | - |
| 36  | LD (HL),d8 | 2 | 10 | - | - | - | - | - | - |
| 37  | SCF | 1 | 4 | - | - | 0 | - | 0 | 1 |
| 38  | JR C,r8 | 2 | 12/7 | - | - | - | - | - | - |
| 39  | ADD HL,SP | 1 | 11 | - | - | * | - | 0 | * |
| 3A  | LD A,(a16) | 3 | 13 | - | - | - | - | - | - |
| 3B  | DEC SP | 1 | 6 | - | - | - | - | - | - |
| 3C  | INC A | 1 | 4 | * | * | * | V | 0 | - |
| 3D  | DEC A | 1 | 4 | * | * | * | V | 1 | - |
| 3E  | LD A,d8 | 2 | 7 | - | - | - | - | - | - |
| 3F  | CCF | 1 | 4 | - | - | * | - | 0 | * |

**Note on CCF**: H = previous carry flag. C = inverted previous carry.

### Row 4x-7x (40-7F) -- LD and HALT

This is the register-to-register LD block plus HALT.
The encoding follows: `LD dst,src` where dst = bits 5-3, src = bits 2-0.

Register encoding for bits 2-0 and 5-3:
| Code | Register |
|------|----------|
| 0    | B |
| 1    | C |
| 2    | D |
| 3    | E |
| 4    | H |
| 5    | L |
| 6    | (HL) |
| 7    | A |

All LD r,r' instructions: 1 byte, 4 T-states, no flags affected.
LD r,(HL) and LD (HL),r: 1 byte, 7 T-states, no flags affected.
Exception: 76 = HALT (not LD (HL),(HL)), 1 byte, 4 T-states, no flags.

| Hex | Mnemonic | Bytes | T-states |
|-----|----------|-------|----------|
| 40  | LD B,B | 1 | 4 |
| 41  | LD B,C | 1 | 4 |
| 42  | LD B,D | 1 | 4 |
| 43  | LD B,E | 1 | 4 |
| 44  | LD B,H | 1 | 4 |
| 45  | LD B,L | 1 | 4 |
| 46  | LD B,(HL) | 1 | 7 |
| 47  | LD B,A | 1 | 4 |
| 48  | LD C,B | 1 | 4 |
| 49  | LD C,C | 1 | 4 |
| 4A  | LD C,D | 1 | 4 |
| 4B  | LD C,E | 1 | 4 |
| 4C  | LD C,H | 1 | 4 |
| 4D  | LD C,L | 1 | 4 |
| 4E  | LD C,(HL) | 1 | 7 |
| 4F  | LD C,A | 1 | 4 |
| 50  | LD D,B | 1 | 4 |
| 51  | LD D,C | 1 | 4 |
| 52  | LD D,D | 1 | 4 |
| 53  | LD D,E | 1 | 4 |
| 54  | LD D,H | 1 | 4 |
| 55  | LD D,L | 1 | 4 |
| 56  | LD D,(HL) | 1 | 7 |
| 57  | LD D,A | 1 | 4 |
| 58  | LD E,B | 1 | 4 |
| 59  | LD E,C | 1 | 4 |
| 5A  | LD E,D | 1 | 4 |
| 5B  | LD E,E | 1 | 4 |
| 5C  | LD E,H | 1 | 4 |
| 5D  | LD E,L | 1 | 4 |
| 5E  | LD E,(HL) | 1 | 7 |
| 5F  | LD E,A | 1 | 4 |
| 60  | LD H,B | 1 | 4 |
| 61  | LD H,C | 1 | 4 |
| 62  | LD H,D | 1 | 4 |
| 63  | LD H,E | 1 | 4 |
| 64  | LD H,H | 1 | 4 |
| 65  | LD H,L | 1 | 4 |
| 66  | LD H,(HL) | 1 | 7 |
| 67  | LD H,A | 1 | 4 |
| 68  | LD L,B | 1 | 4 |
| 69  | LD L,C | 1 | 4 |
| 6A  | LD L,D | 1 | 4 |
| 6B  | LD L,E | 1 | 4 |
| 6C  | LD L,H | 1 | 4 |
| 6D  | LD L,L | 1 | 4 |
| 6E  | LD L,(HL) | 1 | 7 |
| 6F  | LD L,A | 1 | 4 |
| 70  | LD (HL),B | 1 | 7 |
| 71  | LD (HL),C | 1 | 7 |
| 72  | LD (HL),D | 1 | 7 |
| 73  | LD (HL),E | 1 | 7 |
| 74  | LD (HL),H | 1 | 7 |
| 75  | LD (HL),L | 1 | 7 |
| 76  | HALT | 1 | 4 |
| 77  | LD (HL),A | 1 | 7 |
| 78  | LD A,B | 1 | 4 |
| 79  | LD A,C | 1 | 4 |
| 7A  | LD A,D | 1 | 4 |
| 7B  | LD A,E | 1 | 4 |
| 7C  | LD A,H | 1 | 4 |
| 7D  | LD A,L | 1 | 4 |
| 7E  | LD A,(HL) | 1 | 7 |
| 7F  | LD A,A | 1 | 4 |

### Row 8x-Bx (80-BF) -- ALU Operations

Encoding: bits 5-3 select operation, bits 2-0 select source register.
All register ops: 1 byte, 4 T-states. (HL) ops: 1 byte, 7 T-states.

| Hex Range | Mnemonic | S | Z | H | P/V | N | C |
|-----------|----------|---|---|---|-----|---|---|
| 80-87 | ADD A,r | * | * | * | V | 0 | * |
| 88-8F | ADC A,r | * | * | * | V | 0 | * |
| 90-97 | SUB r | * | * | * | V | 1 | * |
| 98-9F | SBC A,r | * | * | * | V | 1 | * |
| A0-A7 | AND r | * | * | 1 | P | 0 | 0 |
| A8-AF | XOR r | * | * | 0 | P | 0 | 0 |
| B0-B7 | OR r | * | * | 0 | P | 0 | 0 |
| B8-BF | CP r | * | * | * | V | 1 | * |

Full register mapping (low 3 bits):
| Low bits | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|----------|---|---|---|---|---|---|---|---|
| Register | B | C | D | E | H | L | (HL) | A |

Detailed ALU block:
| Hex | Mnemonic | Bytes | T-states |
|-----|----------|-------|----------|
| 80  | ADD A,B | 1 | 4 |
| 81  | ADD A,C | 1 | 4 |
| 82  | ADD A,D | 1 | 4 |
| 83  | ADD A,E | 1 | 4 |
| 84  | ADD A,H | 1 | 4 |
| 85  | ADD A,L | 1 | 4 |
| 86  | ADD A,(HL) | 1 | 7 |
| 87  | ADD A,A | 1 | 4 |
| 88  | ADC A,B | 1 | 4 |
| 89  | ADC A,C | 1 | 4 |
| 8A  | ADC A,D | 1 | 4 |
| 8B  | ADC A,E | 1 | 4 |
| 8C  | ADC A,H | 1 | 4 |
| 8D  | ADC A,L | 1 | 4 |
| 8E  | ADC A,(HL) | 1 | 7 |
| 8F  | ADC A,A | 1 | 4 |
| 90  | SUB B | 1 | 4 |
| 91  | SUB C | 1 | 4 |
| 92  | SUB D | 1 | 4 |
| 93  | SUB E | 1 | 4 |
| 94  | SUB H | 1 | 4 |
| 95  | SUB L | 1 | 4 |
| 96  | SUB (HL) | 1 | 7 |
| 97  | SUB A | 1 | 4 |
| 98  | SBC A,B | 1 | 4 |
| 99  | SBC A,C | 1 | 4 |
| 9A  | SBC A,D | 1 | 4 |
| 9B  | SBC A,E | 1 | 4 |
| 9C  | SBC A,H | 1 | 4 |
| 9D  | SBC A,L | 1 | 4 |
| 9E  | SBC A,(HL) | 1 | 7 |
| 9F  | SBC A,A | 1 | 4 |
| A0  | AND B | 1 | 4 |
| A1  | AND C | 1 | 4 |
| A2  | AND D | 1 | 4 |
| A3  | AND E | 1 | 4 |
| A4  | AND H | 1 | 4 |
| A5  | AND L | 1 | 4 |
| A6  | AND (HL) | 1 | 7 |
| A7  | AND A | 1 | 4 |
| A8  | XOR B | 1 | 4 |
| A9  | XOR C | 1 | 4 |
| AA  | XOR D | 1 | 4 |
| AB  | XOR E | 1 | 4 |
| AC  | XOR H | 1 | 4 |
| AD  | XOR L | 1 | 4 |
| AE  | XOR (HL) | 1 | 7 |
| AF  | XOR A | 1 | 4 |
| B0  | OR B | 1 | 4 |
| B1  | OR C | 1 | 4 |
| B2  | OR D | 1 | 4 |
| B3  | OR E | 1 | 4 |
| B4  | OR H | 1 | 4 |
| B5  | OR L | 1 | 4 |
| B6  | OR (HL) | 1 | 7 |
| B7  | OR A | 1 | 4 |
| B8  | CP B | 1 | 4 |
| B9  | CP C | 1 | 4 |
| BA  | CP D | 1 | 4 |
| BB  | CP E | 1 | 4 |
| BC  | CP H | 1 | 4 |
| BD  | CP L | 1 | 4 |
| BE  | CP (HL) | 1 | 7 |
| BF  | CP A | 1 | 4 |

### Row Cx-Fx (C0-FF) -- Control, Stack, I/O, Immediate ALU

| Hex | Mnemonic | Bytes | T-states | S | Z | H | P/V | N | C |
|-----|----------|-------|----------|---|---|---|-----|---|---|
| C0  | RET NZ | 1 | 11/5 | - | - | - | - | - | - |
| C1  | POP BC | 1 | 10 | - | - | - | - | - | - |
| C2  | JP NZ,a16 | 3 | 10 | - | - | - | - | - | - |
| C3  | JP a16 | 3 | 10 | - | - | - | - | - | - |
| C4  | CALL NZ,a16 | 3 | 17/10 | - | - | - | - | - | - |
| C5  | PUSH BC | 1 | 11 | - | - | - | - | - | - |
| C6  | ADD A,d8 | 2 | 7 | * | * | * | V | 0 | * |
| C7  | RST 00h | 1 | 11 | - | - | - | - | - | - |
| C8  | RET Z | 1 | 11/5 | - | - | - | - | - | - |
| C9  | RET | 1 | 10 | - | - | - | - | - | - |
| CA  | JP Z,a16 | 3 | 10 | - | - | - | - | - | - |
| CB  | *CB prefix* | - | - | - | - | - | - | - | - |
| CC  | CALL Z,a16 | 3 | 17/10 | - | - | - | - | - | - |
| CD  | CALL a16 | 3 | 17 | - | - | - | - | - | - |
| CE  | ADC A,d8 | 2 | 7 | * | * | * | V | 0 | * |
| CF  | RST 08h | 1 | 11 | - | - | - | - | - | - |
| D0  | RET NC | 1 | 11/5 | - | - | - | - | - | - |
| D1  | POP DE | 1 | 10 | - | - | - | - | - | - |
| D2  | JP NC,a16 | 3 | 10 | - | - | - | - | - | - |
| D3  | OUT (d8),A | 2 | 11 | - | - | - | - | - | - |
| D4  | CALL NC,a16 | 3 | 17/10 | - | - | - | - | - | - |
| D5  | PUSH DE | 1 | 11 | - | - | - | - | - | - |
| D6  | SUB d8 | 2 | 7 | * | * | * | V | 1 | * |
| D7  | RST 10h | 1 | 11 | - | - | - | - | - | - |
| D8  | RET C | 1 | 11/5 | - | - | - | - | - | - |
| D9  | EXX | 1 | 4 | - | - | - | - | - | - |
| DA  | JP C,a16 | 3 | 10 | - | - | - | - | - | - |
| DB  | IN A,(d8) | 2 | 11 | - | - | - | - | - | - |
| DC  | CALL C,a16 | 3 | 17/10 | - | - | - | - | - | - |
| DD  | *DD prefix* | - | - | - | - | - | - | - | - |
| DE  | SBC A,d8 | 2 | 7 | * | * | * | V | 1 | * |
| DF  | RST 18h | 1 | 11 | - | - | - | - | - | - |
| E0  | RET PO | 1 | 11/5 | - | - | - | - | - | - |
| E1  | POP HL | 1 | 10 | - | - | - | - | - | - |
| E2  | JP PO,a16 | 3 | 10 | - | - | - | - | - | - |
| E3  | EX (SP),HL | 1 | 19 | - | - | - | - | - | - |
| E4  | CALL PO,a16 | 3 | 17/10 | - | - | - | - | - | - |
| E5  | PUSH HL | 1 | 11 | - | - | - | - | - | - |
| E6  | AND d8 | 2 | 7 | * | * | 1 | P | 0 | 0 |
| E7  | RST 20h | 1 | 11 | - | - | - | - | - | - |
| E8  | RET PE | 1 | 11/5 | - | - | - | - | - | - |
| E9  | JP (HL) | 1 | 4 | - | - | - | - | - | - |
| EA  | JP PE,a16 | 3 | 10 | - | - | - | - | - | - |
| EB  | EX DE,HL | 1 | 4 | - | - | - | - | - | - |
| EC  | CALL PE,a16 | 3 | 17/10 | - | - | - | - | - | - |
| ED  | *ED prefix* | - | - | - | - | - | - | - | - |
| EE  | XOR d8 | 2 | 7 | * | * | 0 | P | 0 | 0 |
| EF  | RST 28h | 1 | 11 | - | - | - | - | - | - |
| F0  | RET P | 1 | 11/5 | - | - | - | - | - | - |
| F1  | POP AF | 1 | 10 | * | * | * | * | * | * |
| F2  | JP P,a16 | 3 | 10 | - | - | - | - | - | - |
| F3  | DI | 1 | 4 | - | - | - | - | - | - |
| F4  | CALL P,a16 | 3 | 17/10 | - | - | - | - | - | - |
| F5  | PUSH AF | 1 | 11 | - | - | - | - | - | - |
| F6  | OR d8 | 2 | 7 | * | * | 0 | P | 0 | 0 |
| F7  | RST 30h | 1 | 11 | - | - | - | - | - | - |
| F8  | RET M | 1 | 11/5 | - | - | - | - | - | - |
| F9  | LD SP,HL | 1 | 6 | - | - | - | - | - | - |
| FA  | JP M,a16 | 3 | 10 | - | - | - | - | - | - |
| FB  | EI | 1 | 4 | - | - | - | - | - | - |
| FC  | CALL M,a16 | 3 | 17/10 | - | - | - | - | - | - |
| FD  | *FD prefix* | - | - | - | - | - | - | - | - |
| FE  | CP d8 | 2 | 7 | * | * | * | V | 1 | * |
| FF  | RST 38h | 1 | 11 | - | - | - | - | - | - |

**Note on POP AF**: All flags are loaded from stack (not computed).
**Note on JP (HL)**: This is really `JP HL` -- it loads PC from HL, NOT from memory at (HL).

---

## 4. CB-Prefixed Opcodes

All CB-prefixed instructions are 2 bytes. The second byte encodes:
- Bits 7-6: operation group (00=rotate/shift, 01=BIT, 10=RES, 11=SET)
- Bits 5-3: bit number (for BIT/RES/SET) or sub-operation (for rotate/shift)
- Bits 2-0: register (B=0, C=1, D=2, E=3, H=4, L=5, (HL)=6, A=7)

### Rotate/Shift Operations (CB 00-3F)

Register variants: 2 bytes, 8 T-states.
(HL) variants: 2 bytes, 15 T-states.

| Hex Range | Mnemonic | Operation | S | Z | H | P/V | N | C |
|-----------|----------|-----------|---|---|---|-----|---|---|
| CB 00-07 | RLC r | Rotate left, old bit 7 to C and bit 0 | * | * | 0 | P | 0 | * |
| CB 08-0F | RRC r | Rotate right, old bit 0 to C and bit 7 | * | * | 0 | P | 0 | * |
| CB 10-17 | RL r | Rotate left through carry | * | * | 0 | P | 0 | * |
| CB 18-1F | RR r | Rotate right through carry | * | * | 0 | P | 0 | * |
| CB 20-27 | SLA r | Shift left arithmetic, bit 0 = 0 | * | * | 0 | P | 0 | * |
| CB 28-2F | SRA r | Shift right arithmetic, bit 7 unchanged | * | * | 0 | P | 0 | * |
| CB 30-37 | SLL r | **UNDOCUMENTED** Shift left, bit 0 = 1 | * | * | 0 | P | 0 | * |
| CB 38-3F | SRL r | Shift right logical, bit 7 = 0 | * | * | 0 | P | 0 | * |

Detailed opcodes:
| Hex | Mnemonic | Bytes | T-states |
|-----|----------|-------|----------|
| CB 00 | RLC B | 2 | 8 |
| CB 01 | RLC C | 2 | 8 |
| CB 02 | RLC D | 2 | 8 |
| CB 03 | RLC E | 2 | 8 |
| CB 04 | RLC H | 2 | 8 |
| CB 05 | RLC L | 2 | 8 |
| CB 06 | RLC (HL) | 2 | 15 |
| CB 07 | RLC A | 2 | 8 |
| CB 08 | RRC B | 2 | 8 |
| CB 09 | RRC C | 2 | 8 |
| CB 0A | RRC D | 2 | 8 |
| CB 0B | RRC E | 2 | 8 |
| CB 0C | RRC H | 2 | 8 |
| CB 0D | RRC L | 2 | 8 |
| CB 0E | RRC (HL) | 2 | 15 |
| CB 0F | RRC A | 2 | 8 |
| CB 10 | RL B | 2 | 8 |
| CB 11 | RL C | 2 | 8 |
| CB 12 | RL D | 2 | 8 |
| CB 13 | RL E | 2 | 8 |
| CB 14 | RL H | 2 | 8 |
| CB 15 | RL L | 2 | 8 |
| CB 16 | RL (HL) | 2 | 15 |
| CB 17 | RL A | 2 | 8 |
| CB 18 | RR B | 2 | 8 |
| CB 19 | RR C | 2 | 8 |
| CB 1A | RR D | 2 | 8 |
| CB 1B | RR E | 2 | 8 |
| CB 1C | RR H | 2 | 8 |
| CB 1D | RR L | 2 | 8 |
| CB 1E | RR (HL) | 2 | 15 |
| CB 1F | RR A | 2 | 8 |
| CB 20 | SLA B | 2 | 8 |
| CB 21 | SLA C | 2 | 8 |
| CB 22 | SLA D | 2 | 8 |
| CB 23 | SLA E | 2 | 8 |
| CB 24 | SLA H | 2 | 8 |
| CB 25 | SLA L | 2 | 8 |
| CB 26 | SLA (HL) | 2 | 15 |
| CB 27 | SLA A | 2 | 8 |
| CB 28 | SRA B | 2 | 8 |
| CB 29 | SRA C | 2 | 8 |
| CB 2A | SRA D | 2 | 8 |
| CB 2B | SRA E | 2 | 8 |
| CB 2C | SRA H | 2 | 8 |
| CB 2D | SRA L | 2 | 8 |
| CB 2E | SRA (HL) | 2 | 15 |
| CB 2F | SRA A | 2 | 8 |
| CB 30 | SLL B | 2 | 8 | *UNDOCUMENTED* |
| CB 31 | SLL C | 2 | 8 | *UNDOCUMENTED* |
| CB 32 | SLL D | 2 | 8 | *UNDOCUMENTED* |
| CB 33 | SLL E | 2 | 8 | *UNDOCUMENTED* |
| CB 34 | SLL H | 2 | 8 | *UNDOCUMENTED* |
| CB 35 | SLL L | 2 | 8 | *UNDOCUMENTED* |
| CB 36 | SLL (HL) | 2 | 15 | *UNDOCUMENTED* |
| CB 37 | SLL A | 2 | 8 | *UNDOCUMENTED* |
| CB 38 | SRL B | 2 | 8 |
| CB 39 | SRL C | 2 | 8 |
| CB 3A | SRL D | 2 | 8 |
| CB 3B | SRL E | 2 | 8 |
| CB 3C | SRL H | 2 | 8 |
| CB 3D | SRL L | 2 | 8 |
| CB 3E | SRL (HL) | 2 | 15 |
| CB 3F | SRL A | 2 | 8 |

### BIT Test Operations (CB 40-7F)

Register variants: 2 bytes, 8 T-states.
(HL) variants: 2 bytes, 12 T-states.

Flags: S = undocumented (set if bit 7 tested and is set), Z = set if tested bit is 0, H = 1, P/V = same as Z, N = 0, C = not affected.

| Hex Range | Mnemonic |
|-----------|----------|
| CB 40-47 | BIT 0,r |
| CB 48-4F | BIT 1,r |
| CB 50-57 | BIT 2,r |
| CB 58-5F | BIT 3,r |
| CB 60-67 | BIT 4,r |
| CB 68-6F | BIT 5,r |
| CB 70-77 | BIT 6,r |
| CB 78-7F | BIT 7,r |

Where r follows: B=0, C=1, D=2, E=3, H=4, L=5, (HL)=6, A=7.

### RES (Reset Bit) Operations (CB 80-BF)

Register variants: 2 bytes, 8 T-states.
(HL) variants: 2 bytes, 15 T-states.
No flags affected.

| Hex Range | Mnemonic |
|-----------|----------|
| CB 80-87 | RES 0,r |
| CB 88-8F | RES 1,r |
| CB 90-97 | RES 2,r |
| CB 98-9F | RES 3,r |
| CB A0-A7 | RES 4,r |
| CB A8-AF | RES 5,r |
| CB B0-B7 | RES 6,r |
| CB B8-BF | RES 7,r |

### SET (Set Bit) Operations (CB C0-FF)

Register variants: 2 bytes, 8 T-states.
(HL) variants: 2 bytes, 15 T-states.
No flags affected.

| Hex Range | Mnemonic |
|-----------|----------|
| CB C0-C7 | SET 0,r |
| CB C8-CF | SET 1,r |
| CB D0-D7 | SET 2,r |
| CB D8-DF | SET 3,r |
| CB E0-E7 | SET 4,r |
| CB E8-EF | SET 5,r |
| CB F0-F7 | SET 6,r |
| CB F8-FF | SET 7,r |

---

## 5. ED-Prefixed Opcodes

### Documented ED Instructions

| Hex | Mnemonic | Bytes | T-states | S | Z | H | P/V | N | C |
|-----|----------|-------|----------|---|---|---|-----|---|---|
| ED 40 | IN B,(C) | 2 | 12 | * | * | 0 | P | 0 | - |
| ED 41 | OUT (C),B | 2 | 12 | - | - | - | - | - | - |
| ED 42 | SBC HL,BC | 2 | 15 | * | * | * | V | 1 | * |
| ED 43 | LD (a16),BC | 4 | 20 | - | - | - | - | - | - |
| ED 44 | NEG | 2 | 8 | * | * | * | V | 1 | * |
| ED 45 | RETN | 2 | 14 | - | - | - | - | - | - |
| ED 46 | IM 0 | 2 | 8 | - | - | - | - | - | - |
| ED 47 | LD I,A | 2 | 9 | - | - | - | - | - | - |
| ED 48 | IN C,(C) | 2 | 12 | * | * | 0 | P | 0 | - |
| ED 49 | OUT (C),C | 2 | 12 | - | - | - | - | - | - |
| ED 4A | ADC HL,BC | 2 | 15 | * | * | * | V | 0 | * |
| ED 4B | LD BC,(a16) | 4 | 20 | - | - | - | - | - | - |
| ED 4C | NEG* | 2 | 8 | * | * | * | V | 1 | * | *undoc duplicate*
| ED 4D | RETI | 2 | 14 | - | - | - | - | - | - |
| ED 4E | IM 0* | 2 | 8 | - | - | - | - | - | - | *undoc duplicate*
| ED 4F | LD R,A | 2 | 9 | - | - | - | - | - | - |
| ED 50 | IN D,(C) | 2 | 12 | * | * | 0 | P | 0 | - |
| ED 51 | OUT (C),D | 2 | 12 | - | - | - | - | - | - |
| ED 52 | SBC HL,DE | 2 | 15 | * | * | * | V | 1 | * |
| ED 53 | LD (a16),DE | 4 | 20 | - | - | - | - | - | - |
| ED 54 | NEG* | 2 | 8 | * | * | * | V | 1 | * | *undoc duplicate*
| ED 55 | RETN* | 2 | 14 | - | - | - | - | - | - | *undoc duplicate*
| ED 56 | IM 1 | 2 | 8 | - | - | - | - | - | - |
| ED 57 | LD A,I | 2 | 9 | * | * | 0 | IFF2 | 0 | - |
| ED 58 | IN E,(C) | 2 | 12 | * | * | 0 | P | 0 | - |
| ED 59 | OUT (C),E | 2 | 12 | - | - | - | - | - | - |
| ED 5A | ADC HL,DE | 2 | 15 | * | * | * | V | 0 | * |
| ED 5B | LD DE,(a16) | 4 | 20 | - | - | - | - | - | - |
| ED 5C | NEG* | 2 | 8 | * | * | * | V | 1 | * | *undoc duplicate*
| ED 5D | RETN* | 2 | 14 | - | - | - | - | - | - | *undoc duplicate*
| ED 5E | IM 2 | 2 | 8 | - | - | - | - | - | - |
| ED 5F | LD A,R | 2 | 9 | * | * | 0 | IFF2 | 0 | - |
| ED 60 | IN H,(C) | 2 | 12 | * | * | 0 | P | 0 | - |
| ED 61 | OUT (C),H | 2 | 12 | - | - | - | - | - | - |
| ED 62 | SBC HL,HL | 2 | 15 | * | * | * | V | 1 | * |
| ED 63 | LD (a16),HL | 4 | 20 | - | - | - | - | - | - |
| ED 64 | NEG* | 2 | 8 | * | * | * | V | 1 | * | *undoc duplicate*
| ED 65 | RETN* | 2 | 14 | - | - | - | - | - | - | *undoc duplicate*
| ED 66 | IM 0* | 2 | 8 | - | - | - | - | - | - | *undoc duplicate*
| ED 67 | RRD | 2 | 18 | * | * | 0 | P | 0 | - |
| ED 68 | IN L,(C) | 2 | 12 | * | * | 0 | P | 0 | - |
| ED 69 | OUT (C),L | 2 | 12 | - | - | - | - | - | - |
| ED 6A | ADC HL,HL | 2 | 15 | * | * | * | V | 0 | * |
| ED 6B | LD HL,(a16) | 4 | 20 | - | - | - | - | - | - |
| ED 6C | NEG* | 2 | 8 | * | * | * | V | 1 | * | *undoc duplicate*
| ED 6D | RETN* | 2 | 14 | - | - | - | - | - | - | *undoc duplicate*
| ED 6E | IM 0* | 2 | 8 | - | - | - | - | - | - | *undoc duplicate*
| ED 6F | RLD | 2 | 18 | * | * | 0 | P | 0 | - |
| ED 70 | IN (C)* | 2 | 12 | * | * | 0 | P | 0 | - | *undoc: reads port, sets flags, discards result*
| ED 71 | OUT (C),0* | 2 | 12 | - | - | - | - | - | - | *undoc: outputs 0 to port*
| ED 72 | SBC HL,SP | 2 | 15 | * | * | * | V | 1 | * |
| ED 73 | LD (a16),SP | 4 | 20 | - | - | - | - | - | - |
| ED 74 | NEG* | 2 | 8 | * | * | * | V | 1 | * | *undoc duplicate*
| ED 75 | RETN* | 2 | 14 | - | - | - | - | - | - | *undoc duplicate*
| ED 76 | IM 1* | 2 | 8 | - | - | - | - | - | - | *undoc duplicate*
| ED 77 | NOP* | 2 | 8 | - | - | - | - | - | - | *undoc: ED NOP*
| ED 78 | IN A,(C) | 2 | 12 | * | * | 0 | P | 0 | - |
| ED 79 | OUT (C),A | 2 | 12 | - | - | - | - | - | - |
| ED 7A | ADC HL,SP | 2 | 15 | * | * | * | V | 0 | * |
| ED 7B | LD SP,(a16) | 4 | 20 | - | - | - | - | - | - |
| ED 7C | NEG* | 2 | 8 | * | * | * | V | 1 | * | *undoc duplicate*
| ED 7D | RETN* | 2 | 14 | - | - | - | - | - | - | *undoc duplicate*
| ED 7E | IM 2* | 2 | 8 | - | - | - | - | - | - | *undoc duplicate*
| ED 7F | NOP* | 2 | 8 | - | - | - | - | - | - | *undoc: ED NOP*

### Block Transfer and Search Instructions

| Hex | Mnemonic | Bytes | T-states | S | Z | H | P/V | N | C |
|-----|----------|-------|----------|---|---|---|-----|---|---|
| ED A0 | LDI | 2 | 16 | - | - | 0 | * | 0 | - |
| ED A1 | CPI | 2 | 16 | * | * | * | * | 1 | - |
| ED A2 | INI | 2 | 16 | ? | * | ? | ? | 1 | - |
| ED A3 | OUTI | 2 | 16 | ? | * | ? | ? | 1 | - |
| ED A8 | LDD | 2 | 16 | - | - | 0 | * | 0 | - |
| ED A9 | CPD | 2 | 16 | * | * | * | * | 1 | - |
| ED AA | IND | 2 | 16 | ? | * | ? | ? | 1 | - |
| ED AB | OUTD | 2 | 16 | ? | * | ? | ? | 1 | - |
| ED B0 | LDIR | 2 | 21/16 | - | - | 0 | 0 | 0 | - |
| ED B1 | CPIR | 2 | 21/16 | * | * | * | * | 1 | - |
| ED B2 | INIR | 2 | 21/16 | ? | 1 | ? | ? | 1 | - |
| ED B3 | OTIR | 2 | 21/16 | ? | 1 | ? | ? | 1 | - |
| ED B8 | LDDR | 2 | 21/16 | - | - | 0 | 0 | 0 | - |
| ED B9 | CPDR | 2 | 21/16 | * | * | * | * | 1 | - |
| ED BA | INDR | 2 | 21/16 | ? | 1 | ? | ? | 1 | - |
| ED BB | OTDR | 2 | 21/16 | ? | 1 | ? | ? | 1 | - |

**Notes on block instructions:**
- LDI/LDD: P/V = 1 if BC-1 != 0 (i.e., more bytes to transfer)
- CPI/CPD: P/V = 1 if BC-1 != 0; Z = 1 if A == (HL) after comparison
- LDIR/LDDR: 21 T-states if BC != 0 (repeating), 16 if BC == 0 (done)
- CPIR/CPDR: 21 T-states if BC != 0 AND A != (HL), 16 otherwise
- INIR/OTIR/INDR/OTDR: 21 T-states if B != 0 (repeating), 16 if B == 0 (done)
- For block instructions, Z is set when B decrements to 0 (for IN/OUT variants)

### Undocumented ED Ranges
- ED 00-3F: Behave as NOP (2 bytes, 8 T-states)
- ED 80-9F: Behave as NOP (2 bytes, 8 T-states)
- ED C0-FF: Behave as NOP (2 bytes, 8 T-states)

---

## 6. DD-Prefixed Opcodes (IX)

The DD prefix substitutes IX for HL in most instructions. Additionally, H and L references in non-(HL) contexts become IXH and IXL (undocumented).

### Rules for DD prefix:
1. (HL) becomes (IX+d) where d is a signed byte displacement following the opcode
2. HL becomes IX in 16-bit operations
3. H becomes IXH and L becomes IXL in 8-bit operations (UNDOCUMENTED)
4. Instructions that do NOT change with DD prefix: EX DE,HL remains as-is; all ED-prefixed instructions are unaffected

### DD-Prefixed Instruction Table (Documented)

| Hex | Mnemonic | Bytes | T-states | Notes |
|-----|----------|-------|----------|-------|
| DD 09 | ADD IX,BC | 2 | 15 | |
| DD 19 | ADD IX,DE | 2 | 15 | |
| DD 21 | LD IX,d16 | 4 | 14 | |
| DD 22 | LD (a16),IX | 4 | 20 | |
| DD 23 | INC IX | 2 | 10 | |
| DD 29 | ADD IX,IX | 2 | 15 | |
| DD 2A | LD IX,(a16) | 4 | 20 | |
| DD 2B | DEC IX | 2 | 10 | |
| DD 34 | INC (IX+d) | 3 | 23 | |
| DD 35 | DEC (IX+d) | 3 | 23 | |
| DD 36 | LD (IX+d),d8 | 4 | 19 | |
| DD 39 | ADD IX,SP | 2 | 15 | |
| DD 46 | LD B,(IX+d) | 3 | 19 | |
| DD 4E | LD C,(IX+d) | 3 | 19 | |
| DD 56 | LD D,(IX+d) | 3 | 19 | |
| DD 5E | LD E,(IX+d) | 3 | 19 | |
| DD 66 | LD H,(IX+d) | 3 | 19 | |
| DD 6E | LD L,(IX+d) | 3 | 19 | |
| DD 70 | LD (IX+d),B | 3 | 19 | |
| DD 71 | LD (IX+d),C | 3 | 19 | |
| DD 72 | LD (IX+d),D | 3 | 19 | |
| DD 73 | LD (IX+d),E | 3 | 19 | |
| DD 74 | LD (IX+d),H | 3 | 19 | |
| DD 75 | LD (IX+d),L | 3 | 19 | |
| DD 77 | LD (IX+d),A | 3 | 19 | |
| DD 7E | LD A,(IX+d) | 3 | 19 | |
| DD 86 | ADD A,(IX+d) | 3 | 19 | |
| DD 8E | ADC A,(IX+d) | 3 | 19 | |
| DD 96 | SUB (IX+d) | 3 | 19 | |
| DD 9E | SBC A,(IX+d) | 3 | 19 | |
| DD A6 | AND (IX+d) | 3 | 19 | |
| DD AE | XOR (IX+d) | 3 | 19 | |
| DD B6 | OR (IX+d) | 3 | 19 | |
| DD BE | CP (IX+d) | 3 | 19 | |
| DD CB | *DDCB prefix* | - | - | See Section 8 |
| DD E1 | POP IX | 2 | 14 | |
| DD E3 | EX (SP),IX | 2 | 23 | |
| DD E5 | PUSH IX | 2 | 15 | |
| DD E9 | JP (IX) | 2 | 8 | |
| DD F9 | LD SP,IX | 2 | 10 | |

### DD-Prefixed Undocumented Instructions (IXH/IXL)

These access the high and low bytes of IX as 8-bit registers.
All are 2 bytes (DD + opcode).

| Hex | Mnemonic | Bytes | T-states |
|-----|----------|-------|----------|
| DD 24 | INC IXH | 2 | 8 |
| DD 25 | DEC IXH | 2 | 8 |
| DD 26 | LD IXH,d8 | 3 | 11 |
| DD 2C | INC IXL | 2 | 8 |
| DD 2D | DEC IXL | 2 | 8 |
| DD 2E | LD IXL,d8 | 3 | 11 |
| DD 44 | LD B,IXH | 2 | 8 |
| DD 45 | LD B,IXL | 2 | 8 |
| DD 4C | LD C,IXH | 2 | 8 |
| DD 4D | LD C,IXL | 2 | 8 |
| DD 54 | LD D,IXH | 2 | 8 |
| DD 55 | LD D,IXL | 2 | 8 |
| DD 5C | LD E,IXH | 2 | 8 |
| DD 5D | LD E,IXL | 2 | 8 |
| DD 60 | LD IXH,B | 2 | 8 |
| DD 61 | LD IXH,C | 2 | 8 |
| DD 62 | LD IXH,D | 2 | 8 |
| DD 63 | LD IXH,E | 2 | 8 |
| DD 64 | LD IXH,IXH | 2 | 8 |
| DD 65 | LD IXH,IXL | 2 | 8 |
| DD 67 | LD IXH,A | 2 | 8 |
| DD 68 | LD IXL,B | 2 | 8 |
| DD 69 | LD IXL,C | 2 | 8 |
| DD 6A | LD IXL,D | 2 | 8 |
| DD 6B | LD IXL,E | 2 | 8 |
| DD 6C | LD IXL,IXH | 2 | 8 |
| DD 6D | LD IXL,IXL | 2 | 8 |
| DD 6F | LD IXL,A | 2 | 8 |
| DD 7C | LD A,IXH | 2 | 8 |
| DD 7D | LD A,IXL | 2 | 8 |
| DD 84 | ADD A,IXH | 2 | 8 |
| DD 85 | ADD A,IXL | 2 | 8 |
| DD 8C | ADC A,IXH | 2 | 8 |
| DD 8D | ADC A,IXL | 2 | 8 |
| DD 94 | SUB IXH | 2 | 8 |
| DD 95 | SUB IXL | 2 | 8 |
| DD 9C | SBC A,IXH | 2 | 8 |
| DD 9D | SBC A,IXL | 2 | 8 |
| DD A4 | AND IXH | 2 | 8 |
| DD A5 | AND IXL | 2 | 8 |
| DD AC | XOR IXH | 2 | 8 |
| DD AD | XOR IXL | 2 | 8 |
| DD B4 | OR IXH | 2 | 8 |
| DD B5 | OR IXL | 2 | 8 |
| DD BC | CP IXH | 2 | 8 |
| DD BD | CP IXL | 2 | 8 |

Flag effects for IXH/IXL operations are the same as for the equivalent H/L operations.

**Important**: Instructions that reference BOTH (IX+d) AND H or L in the same instruction (like LD H,(IX+d)) use the original H/L registers, not IXH/IXL. The IXH/IXL substitution only applies when the instruction does NOT involve (IX+d).

### Unaffected DD Instructions
Any DD-prefixed opcode that does not normally reference HL, H, L, or (HL) simply behaves as if the DD prefix is a NOP (4 extra T-states consumed for the prefix fetch). The following instruction executes normally.

---

## 7. FD-Prefixed Opcodes (IY)

FD-prefixed instructions are **identical** to DD-prefixed instructions with IY substituted for IX, IYH for IXH, and IYL for IXL.

All byte counts, T-states, and flag effects are the same. Simply replace:
- IX -> IY
- IXH -> IYH
- IXL -> IYL
- DD -> FD

---

## 8. DD CB / FD CB Prefixed Opcodes

### Encoding Format

These instructions are 4 bytes long:
```
DD CB dd oo    (for IX)
FD CB dd oo    (for IY)
```
Where:
- `dd` = signed 8-bit displacement
- `oo` = opcode byte (same encoding as CB-prefixed table)

**CRITICAL**: The displacement byte comes BEFORE the opcode byte (unlike normal DD/FD+opcode+displacement order).

### Timing
- All rotate/shift/RES/SET operations: 23 T-states
- All BIT test operations: 20 T-states

### Documented Operations (when low 3 bits of opcode = 110, i.e., (HL) encoding)

These are the "official" DDCB/FDCB instructions that operate on (IX+d) or (IY+d):

| Opcode byte | Mnemonic | T-states |
|-------------|----------|----------|
| 06 | RLC (IX+d) | 23 |
| 0E | RRC (IX+d) | 23 |
| 16 | RL (IX+d) | 23 |
| 1E | RR (IX+d) | 23 |
| 26 | SLA (IX+d) | 23 |
| 2E | SRA (IX+d) | 23 |
| 36 | SLL (IX+d) | 23 | *UNDOCUMENTED (SLL itself is undocumented)*
| 3E | SRL (IX+d) | 23 |
| 46 | BIT 0,(IX+d) | 20 |
| 4E | BIT 1,(IX+d) | 20 |
| 56 | BIT 2,(IX+d) | 20 |
| 5E | BIT 3,(IX+d) | 20 |
| 66 | BIT 4,(IX+d) | 20 |
| 6E | BIT 5,(IX+d) | 20 |
| 76 | BIT 6,(IX+d) | 20 |
| 7E | BIT 7,(IX+d) | 20 |
| 86 | RES 0,(IX+d) | 23 |
| 8E | RES 1,(IX+d) | 23 |
| 96 | RES 2,(IX+d) | 23 |
| 9E | RES 3,(IX+d) | 23 |
| A6 | RES 4,(IX+d) | 23 |
| AE | RES 5,(IX+d) | 23 |
| B6 | RES 6,(IX+d) | 23 |
| BE | RES 7,(IX+d) | 23 |
| C6 | SET 0,(IX+d) | 23 |
| CE | SET 1,(IX+d) | 23 |
| D6 | SET 2,(IX+d) | 23 |
| DE | SET 3,(IX+d) | 23 |
| E6 | SET 4,(IX+d) | 23 |
| EE | SET 5,(IX+d) | 23 |
| F6 | SET 6,(IX+d) | 23 |
| FE | SET 7,(IX+d) | 23 |

### Undocumented DD CB / FD CB Operations

When the low 3 bits of the opcode byte are NOT 110 (i.e., when they would normally specify a register), the instruction performs the operation on (IX+d) AND stores the result in the specified register.

The register encoding for the low 3 bits:
| Bits 2-0 | Register |
|----------|----------|
| 000 | B |
| 001 | C |
| 010 | D |
| 011 | E |
| 100 | H |
| 101 | L |
| 110 | (IX+d) only (documented) |
| 111 | A |

Example undocumented instructions:
```
DD CB dd 00 = RLC (IX+d) -> B  (rotate (IX+d) left, store in both (IX+d) and B)
DD CB dd 01 = RLC (IX+d) -> C
DD CB dd 02 = RLC (IX+d) -> D
...
DD CB dd 07 = RLC (IX+d) -> A
DD CB dd C0 = SET 0,(IX+d) -> B
DD CB dd C1 = SET 0,(IX+d) -> C
...
DD CB dd C7 = SET 0,(IX+d) -> A
```

**Exception**: BIT instructions (opcode bits 7-6 = 01) do NOT store the result in a register. All BIT n,(IX+d) variants behave the same regardless of the low 3 bits. T-states = 20 for all BIT variants.

---

## 9. Undocumented Opcodes

### 9.1. SLL (Shift Left Logical) -- CB 30-37

SLL shifts the operand left by one bit position. Unlike SLA (which sets bit 0 to 0), SLL sets bit 0 to 1. The old bit 7 goes into the carry flag.

```
Operation:  C <- [7 <- 6 <- 5 <- 4 <- 3 <- 2 <- 1 <- 0] <- 1
```

This effectively computes: result = (operand * 2) + 1

Opcodes: CB 30 (B), CB 31 (C), CB 32 (D), CB 33 (E), CB 34 (H), CB 35 (L), CB 36 (HL), CB 37 (A)

### 9.2. IXH/IXL/IYH/IYL Operations

The DD and FD prefixes affect 8-bit register references to H and L, giving access to the high and low bytes of IX and IY. See Section 6 for the complete list.

**Rules:**
- If an instruction references H or L (but NOT (HL)), DD prefix changes H->IXH and L->IXL
- If an instruction references (HL), DD prefix changes it to (IX+d) -- H and L themselves are NOT substituted
- Both substitutions cannot occur in the same instruction

### 9.3. Undocumented ED Duplicates

Many ED opcodes in the 40-7F range are duplicates of documented instructions:
- ED 4C, 54, 5C, 64, 6C, 74, 7C all behave as NEG
- ED 4E, 66, 6E all behave as IM 0
- ED 76 behaves as IM 1
- ED 7E behaves as IM 2
- ED 45, 55, 5D, 65, 6D, 75, 7D all behave as RETN
- ED 70: IN F,(C) -- reads port, affects flags, discards value
- ED 71: OUT (C),0 -- outputs 0 to port (some CMOS variants output 0xFF)

### 9.4. Multiple DD/FD Prefixes

If multiple DD or FD prefixes appear in a row, each one is treated as a separate instruction that:
- Consumes 4 T-states
- Increments R by 1
- The last prefix in the chain is the effective one

Example: DD DD FD 21 nn nn = LD IY,d16 (the DD prefixes are effectively NOPs, only the final FD takes effect)

---

## 10. Flag Behavior Per Instruction

### 10.1. 8-bit Arithmetic

**ADD A,r / ADD A,d8 / ADC A,r / ADC A,d8:**
| Flag | Effect |
|------|--------|
| S | Set if result is negative (bit 7 = 1) |
| Z | Set if result is zero |
| H | Set if carry from bit 3 |
| P/V | Set if overflow (signed result out of range -128..+127) |
| N | Reset to 0 |
| C | Set if carry from bit 7 |

**SUB r / SUB d8 / SBC A,r / SBC A,d8 / CP r / CP d8:**
| Flag | Effect |
|------|--------|
| S | Set if result is negative (bit 7 = 1) |
| Z | Set if result is zero |
| H | Set if borrow from bit 4 |
| P/V | Set if overflow |
| N | Set to 1 |
| C | Set if borrow (operand > A for SUB; operand+carry > A for SBC) |

**NOTE on CP**: CP computes A - operand but does NOT store the result. A is unchanged. Flags reflect the subtraction result. However, the undocumented flags (bits 3 and 5) are taken from the OPERAND, not the result.

**INC r / INC (HL) / INC (IX+d):**
| Flag | Effect |
|------|--------|
| S | Set if result is negative |
| Z | Set if result is zero |
| H | Set if carry from bit 3 |
| P/V | Set if previous value was 0x7F (overflow 127->-128) |
| N | Reset to 0 |
| C | Not affected |

**DEC r / DEC (HL) / DEC (IX+d):**
| Flag | Effect |
|------|--------|
| S | Set if result is negative |
| Z | Set if result is zero |
| H | Set if borrow from bit 4 |
| P/V | Set if previous value was 0x80 (overflow -128->127) |
| N | Set to 1 |
| C | Not affected |

**NEG (A = 0 - A):**
| Flag | Effect |
|------|--------|
| S | Set if result is negative |
| Z | Set if result is zero (A was 0) |
| H | Set if borrow from bit 4 |
| P/V | Set if A was 0x80 before NEG |
| N | Set to 1 |
| C | Set if A was NOT 0 before NEG |

### 10.2. 16-bit Arithmetic

**ADD HL,rr / ADD IX,rr / ADD IY,rr:**
| Flag | Effect |
|------|--------|
| S | Not affected |
| Z | Not affected |
| H | Set if carry from bit 11 |
| P/V | Not affected |
| N | Reset to 0 |
| C | Set if carry from bit 15 |

**ADC HL,rr:**
| Flag | Effect |
|------|--------|
| S | Set if result is negative (bit 15 = 1) |
| Z | Set if result is zero |
| H | Set if carry from bit 11 |
| P/V | Set if overflow |
| N | Reset to 0 |
| C | Set if carry from bit 15 |

**SBC HL,rr:**
| Flag | Effect |
|------|--------|
| S | Set if result is negative (bit 15 = 1) |
| Z | Set if result is zero |
| H | Set if borrow from bit 12 |
| P/V | Set if overflow |
| N | Set to 1 |
| C | Set if borrow |

**INC rr / DEC rr / INC IX / DEC IX / INC IY / DEC IY:**
No flags affected.

### 10.3. Logical Operations

**AND r / AND d8:**
| Flag | Effect |
|------|--------|
| S | Set if bit 7 of result is set |
| Z | Set if result is zero |
| H | Set to 1 |
| P/V | Set if parity is even |
| N | Reset to 0 |
| C | Reset to 0 |

**OR r / OR d8 / XOR r / XOR d8:**
| Flag | Effect |
|------|--------|
| S | Set if bit 7 of result is set |
| Z | Set if result is zero |
| H | Reset to 0 |
| P/V | Set if parity is even |
| N | Reset to 0 |
| C | Reset to 0 |

### 10.4. Rotate and Shift

**RLCA, RRCA, RLA, RRA (accumulator rotates):**
| Flag | Effect |
|------|--------|
| S | Not affected |
| Z | Not affected |
| H | Reset to 0 |
| P/V | Not affected |
| N | Reset to 0 |
| C | Set according to rotated bit |

**RLC, RRC, RL, RR, SLA, SRA, SLL, SRL (CB-prefixed):**
| Flag | Effect |
|------|--------|
| S | Set if result bit 7 is set |
| Z | Set if result is zero |
| H | Reset to 0 |
| P/V | Set if parity is even |
| N | Reset to 0 |
| C | Set according to shifted/rotated bit |

**RLD:**
Rotates the low nibble of (HL) into the high nibble of (HL), the old high nibble of (HL) into the low nibble of A, and the old low nibble of A into the low nibble of (HL).
```
A: [7..4][3..0]    (HL): [7..4][3..0]
A: [7..4][(HL)7..4]  (HL): [(HL)3..0][A3..0]
```
Flags: S*, Z*, H=0, P/V=parity, N=0, C unchanged.

**RRD:**
Reverse of RLD.
```
A: [7..4][(HL)3..0]  (HL): [A3..0][(HL)7..4]
```
Flags: S*, Z*, H=0, P/V=parity, N=0, C unchanged.

### 10.5. Bit Operations

**BIT n,r:**
| Flag | Effect |
|------|--------|
| S | Set if n=7 and tested bit is set |
| Z | Set if tested bit is zero |
| H | Set to 1 |
| P/V | Same as Z flag |
| N | Reset to 0 |
| C | Not affected |

**SET n,r / RES n,r:**
No flags affected.

### 10.6. Jump, Call, Return
No flags affected (except conditional variants check flags without modifying them).

### 10.7. Input/Output

**IN r,(C):**
| Flag | Effect |
|------|--------|
| S | Set if input data bit 7 is set |
| Z | Set if input data is zero |
| H | Reset to 0 |
| P/V | Set if parity of input data is even |
| N | Reset to 0 |
| C | Not affected |

**IN A,(d8):**
No flags affected.

**OUT (C),r / OUT (d8),A:**
No flags affected.

### 10.8. Special Instructions

**DAA:** S*, Z*, H*, P/V=parity, N unchanged, C* (see Section 15)

**CPL (complement A):** H=1, N=1, others unchanged.

**SCF (set carry):** H=0, N=0, C=1, others unchanged.

**CCF (complement carry):** H=previous C, N=0, C=inverted C, others unchanged.

**LD A,I / LD A,R:**
| Flag | Effect |
|------|--------|
| S | Set if I/R bit 7 is set |
| Z | Set if I/R is zero |
| H | Reset to 0 |
| P/V | Contents of IFF2 |
| N | Reset to 0 |
| C | Not affected |

**Warning**: If an interrupt occurs during LD A,I or LD A,R, the P/V flag may be reset to 0 even if IFF2 is set. This is a known Z80 bug.

---

## 11. Undocumented Flags (Bits 3 and 5)

Bits 3 (XF/F3) and 5 (YF/F5) of the F register are not used by any documented instruction for condition testing, but they do have predictable behavior.

### 11.1. General Rule

For most instructions that affect flags, XF and YF are copies of bits 3 and 5 of the 8-bit result.

### 11.2. Special Cases

**16-bit ADD/ADC/SBC (ADD HL,rr etc.):**
XF = bit 11 of result (bit 3 of high byte)
YF = bit 13 of result (bit 5 of high byte)

**CP r / CP d8:**
XF and YF are taken from the OPERAND, not the result. This is the major exception.

**BIT n,r (register):**
XF = bit 3 of the register being tested
YF = bit 5 of the register being tested

**BIT n,(HL):**
XF = bit 11 of MEMPTR (WZ register) = bit 3 of WZ high byte
YF = bit 13 of MEMPTR (WZ register) = bit 5 of WZ high byte

**BIT n,(IX+d) / BIT n,(IY+d):**
XF = bit 3 of high byte of (IX+d address), i.e., bit 3 of WZ high byte
YF = bit 5 of high byte of (IX+d address), i.e., bit 5 of WZ high byte

**SCF:**
On Zilog NMOS Z80: If the previous instruction modified flags, then XF = A.3, YF = A.5. If the previous instruction did NOT modify flags, then XF = XF | A.3, YF = YF | A.5.

The "Q register" model: XF and YF are bits 3 and 5 of `(Q XOR F) OR A`, where Q = F after a flag-modifying instruction, or Q = 0 after a non-flag-modifying instruction.

**CCF:**
Same as SCF for XF/YF behavior.

**RLCA, RRCA, RLA, RRA:**
XF = bit 3 of A (after rotation)
YF = bit 5 of A (after rotation)

**LDI/LDD:**
XF = bit 3 of (A + transferred byte)
YF = bit 1 of (A + transferred byte)  (**NOTE: bit 1, not bit 5!**)

**CPI/CPD:**
Let n = A - (HL) - H (where H is the half-carry from A - (HL)):
XF = bit 3 of n
YF = bit 1 of n (**NOTE: bit 1, not bit 5!**)

**LDIR/LDDR:** Same as LDI/LDD for the last iteration.

**CPIR/CPDR:** Same as CPI/CPD for the last iteration. If repeating (not done), MEMPTR = PC+1.

**INI/IND/OUTI/OUTD and repeating variants:**
Complex flag behavior. XF and YF come from the result of internal operations involving the transferred byte and the B counter.

**Block instructions when interrupted (LDIR/LDDR/CPIR/CPDR/etc.):**
When a block instruction is interrupted mid-execution:
YF = bit 13 of PC (PC.13, where PC is the address of the instruction prefix)
XF = bit 11 of PC (PC.11)

---

## 12. MEMPTR/WZ Register

MEMPTR (also called WZ) is an internal 16-bit register that is not directly accessible. It affects the undocumented flags primarily through BIT n,(HL) instructions.

### MEMPTR Update Rules by Instruction

| Instruction | MEMPTR value after execution |
|-------------|------------------------------|
| LD A,(a16) | a16 + 1 |
| LD (a16),A | low = (a16 + 1) & 0xFF, high = A |
| LD A,(BC) | BC + 1 |
| LD A,(DE) | DE + 1 |
| LD (BC),A | low = (BC + 1) & 0xFF, high = A |
| LD (DE),A | low = (DE + 1) & 0xFF, high = A |
| LD rr,(a16) | a16 + 1 |
| LD (a16),rr | a16 + 1 |
| EX (SP),HL/IX/IY | value read from stack (the new value of HL/IX/IY) |
| ADD/ADC/SBC HL,rr | HL (before operation) + 1 |
| RLD / RRD | HL + 1 |
| JP a16 | a16 (the jump target) |
| JP cc,a16 | a16 (even if condition is false) |
| JR / DJNZ (taken) | target address (PC + offset) |
| CALL a16 / CALL cc,a16 | a16 |
| RET / RETI / RETN | address popped from stack |
| RST n | n (the restart address) |
| IN A,(port) | (A << 8) + port + 1 |
| IN r,(C) | BC + 1 |
| OUT (port),A | low = (port + 1) & 0xFF, high = A |
| OUT (C),r | BC + 1 |
| INI / OUTI | BC + 1 (after B is decremented) |
| IND / OUTD | BC - 1 (after B is decremented) |
| INIR / OTIR | BC + 1 (on each iteration) |
| INDR / OTDR | BC - 1 (on each iteration) |
| CPI | MEMPTR + 1 |
| CPD | MEMPTR - 1 |
| CPIR (repeating) | PC + 1 |
| CPDR (repeating) | PC + 1 |
| CPIR/CPDR (done) | same as CPI/CPD |
| LDI/LDD/LDIR/LDDR | not affected |
| BIT n,(IX+d) | IX + d (the effective address) |
| (IX+d) operations | IX + d |

---

## 13. Interrupt Handling

### 13.1. Interrupt Enable Flip-Flops

**IFF1**: Controls whether maskable interrupts are accepted.
- When IFF1 = 1: maskable interrupts are enabled
- When IFF1 = 0: maskable interrupts are disabled

**IFF2**: Stores the previous state of IFF1 during NMI processing.
- The LD A,I and LD A,R instructions copy IFF2 to the P/V flag
- RETN copies IFF2 back to IFF1

### 13.2. DI and EI Instructions

**DI**: Sets both IFF1 and IFF2 to 0 (disables maskable interrupts).

**EI**: Sets both IFF1 and IFF2 to 1 (enables maskable interrupts).
- **CRITICAL**: Interrupts are NOT accepted immediately after EI. The instruction following EI executes before any pending interrupt is serviced. This one-instruction delay allows safe `EI; RET` sequences.

### 13.3. Non-Maskable Interrupt (NMI)

Triggered by a falling edge on the /NMI pin (high-to-low transition).

**NMI Processing sequence:**
1. Current PC is pushed onto the stack
2. IFF1 is copied to IFF2 (preserving the interrupt enable state)
3. IFF1 is reset to 0 (disabling maskable interrupts during NMI handler)
4. PC is loaded with 0x0066
5. Total: 11 T-states

**RETN instruction (return from NMI):**
1. IFF1 is restored from IFF2
2. PC is popped from stack
3. Total: 14 T-states

The NMI cannot be disabled. The CPU does not accept a second NMI until at least one instruction of the NMI handler has executed.

### 13.4. Maskable Interrupt -- Mode 0 (IM 0)

Default mode after RESET.

**Processing:**
1. An interrupt acknowledge cycle occurs
2. The interrupting device places an instruction byte on the data bus
3. The CPU executes that instruction (usually RST n)
4. IFF1 and IFF2 are reset to 0
5. If the instruction is RST, it takes 13 T-states total (11 for RST + 2 extra for acknowledge)
6. If no device responds, 0xFF is typically read (= RST 38h)

Practical emulator note: adding a flat +2 T-state acknowledge overhead is exact
for RST-based IM 0 interrupts; very uncommon non-RST IM 0 flows may need
device-specific bus timing to be cycle-exact.

### 13.5. Maskable Interrupt -- Mode 1 (IM 1)

Most commonly used mode.

**Processing:**
1. An interrupt acknowledge cycle occurs (any data on bus is ignored)
2. IFF1 and IFF2 are reset to 0
3. Current PC is pushed onto the stack
4. PC is loaded with 0x0038
5. Total: 13 T-states

This is equivalent to executing RST 38h.

### 13.6. Maskable Interrupt -- Mode 2 (IM 2)

Most powerful mode -- vectored interrupts.

**Processing:**
1. An interrupt acknowledge cycle occurs
2. The interrupting device places a vector byte on the data bus
3. IFF1 and IFF2 are reset to 0
4. Current PC is pushed onto the stack
5. A 16-bit address is formed: high byte = I register, low byte = vector from data bus
6. PC is loaded with the 16-bit value read from the address formed in step 5
7. Total: 19 T-states

**Note**: The IM 2 low byte comes from the interrupting device's data bus value.
Practical implementations often use even-aligned vector tables, but the CPU
forms the lookup address from all 8 bits.

The I register holds the high byte of the interrupt vector table. This allows up to 128 different interrupt service routines (256 byte table / 2 bytes per entry).

### 13.7. RETI Instruction

**Purpose**: Return from maskable interrupt service routine.
- Functionally identical to RET (pops PC from stack)
- At CPU level, RETI and RETN both restore IFF1 from IFF2
- RETI additionally signals daisy-chained I/O devices that the interrupt is complete
- The opcode sequence ED 4D is recognized by external hardware

### 13.8. Interrupt Timing Summary

| Event | T-states |
|-------|----------|
| NMI acknowledge + push + jump | 11 |
| INT IM 0 (RST instruction) | 13 |
| INT IM 1 | 13 |
| INT IM 2 | 19 |
| RETN | 14 |
| RETI | 14 |

---

## 14. HALT Behavior

The HALT instruction (opcode 0x76) suspends CPU operation.

**During HALT:**
1. The CPU repeatedly executes NOP instructions internally
2. Each NOP takes 4 T-states (maintaining memory refresh via the R register)
3. The PC remains pointing to the HALT instruction (or technically PC+1, but it re-reads the HALT)
4. The CPU continues to respond to bus requests (BUSRQ)

**Exiting HALT:**
1. A maskable interrupt (if enabled) exits HALT and services the interrupt. After RETI/RET, execution continues at the instruction AFTER HALT.
2. A non-maskable interrupt exits HALT. After RETN, execution continues at the instruction AFTER HALT.
3. A RESET exits HALT.

**Important**: If interrupts are disabled (DI) and HALT is executed, the CPU will remain halted forever (until NMI or RESET).

---

## 15. DAA Algorithm

The DAA (Decimal Adjust Accumulator) instruction corrects the result in A after a BCD addition or subtraction. It uses the N flag to determine which operation was performed.

### Complete Correction Table

#### After Addition (N = 0): ADD, ADC, INC

| C before | Upper nibble (A[7:4]) | H before | Lower nibble (A[3:0]) | Correction | C after |
|----------|----------------------|----------|----------------------|------------|---------|
| 0 | 0-9 | 0 | 0-9 | +0x00 | 0 |
| 0 | 0-8 | 0 | A-F | +0x06 | 0 |
| 0 | 0-9 | 1 | 0-3 | +0x06 | 0 |
| 0 | A-F | 0 | 0-9 | +0x60 | 1 |
| 0 | 9-F | 0 | A-F | +0x66 | 1 |
| 0 | A-F | 1 | 0-3 | +0x66 | 1 |
| 1 | 0-2 | 0 | 0-9 | +0x60 | 1 |
| 1 | 0-2 | 0 | A-F | +0x66 | 1 |
| 1 | 0-3 | 1 | 0-3 | +0x66 | 1 |

#### After Subtraction (N = 1): SUB, SBC, DEC, NEG

| C before | Upper nibble (A[7:4]) | H before | Lower nibble (A[3:0]) | Correction | C after |
|----------|----------------------|----------|----------------------|------------|---------|
| 0 | 0-9 | 0 | 0-9 | +0x00 | 0 |
| 0 | 0-8 | 1 | 6-F | -0x06 | 0 |
| 1 | 7-F | 0 | 0-9 | -0x60 | 1 |
| 1 | 6-F | 1 | 6-F | -0x66 | 1 |

### DAA Flag Effects

| Flag | Effect |
|------|--------|
| S | Set if result bit 7 is set |
| Z | Set if result is zero |
| H | See below |
| P/V | Set if parity of result is even |
| N | Not affected (preserved from previous operation) |
| C | See table above |

**H flag after DAA:**
- After addition: H = 1 if lower nibble correction caused carry from bit 3
- After subtraction: H = 1 if lower nibble correction caused borrow from bit 4
- Simple rule: For addition, H = (A[3:0] > 9 OR H_before). For subtraction, H = (H_before AND A[3:0] < 6)... In practice, just use the before/after values.

### Practical Implementation

```c
void daa(Z80 *cpu) {
    uint8_t a = cpu->A;
    uint8_t correction = 0;
    bool carry = cpu->flags.C;

    if (cpu->flags.H || (!cpu->flags.N && (a & 0x0F) > 9)) {
        correction |= 0x06;
    }
    if (cpu->flags.C || (!cpu->flags.N && a > 0x99)) {
        correction |= 0x60;
        carry = true;
    }

    if (cpu->flags.N) {
        a -= correction;
    } else {
        a += correction;
    }

    cpu->flags.S = (a & 0x80) != 0;
    cpu->flags.Z = (a == 0);
    cpu->flags.H = /* see note */;
    cpu->flags.PV = parity(a);
    cpu->flags.C = carry;
    // N is preserved
    cpu->A = a;
}
```

**Note on H flag**: For addition, H is set if the lower nibble of the original A value plus the lower nibble correction produced a carry. For subtraction, H is set if the original H was set and the lower nibble is >= 6 -- this is complex. Many emulators use a lookup table approach.

---

## 16. R Register Behavior

The R (Memory Refresh) register is an 8-bit register, but only the lower 7 bits (bits 0-6) are automatically incremented. Bit 7 can only be changed by the `LD R,A` instruction.

### Increment Rules

- R is incremented by 1 after each M1 (opcode fetch) cycle
- Prefix bytes (DD, FD, ED, CB) each cause their own M1 cycle, so they increment R
- A normal unprefixed instruction: R increments by 1
- A CB/ED-prefixed instruction: R increments by 2 (one for prefix, one for opcode)
- A DD/FD-prefixed instruction: R increments by 2 (one for prefix, one for opcode)
- A DDCB/FDCB-prefixed instruction: R increments by 2 (one for DD/FD, one for CB; the displacement and final opcode do not cause M1 cycles)

### Bit 7 Behavior

When R overflows from bit 6, the counter wraps from 127 to 0 in the low 7 bits. Bit 7 stays at whatever value was last loaded via `LD R,A`.

```
LD A,0x80
LD R,A     ; R = 0x80 (bit 7 = 1)
; After 127 instructions: R = 0xFF
; After 128 instructions: R = 0x80 (wraps, bit 7 preserved)
```

---

## 17. Opcode Decoding Algorithm

Z80 opcodes can be decoded algorithmically by splitting the opcode byte into bit fields:

```
Bit:  7  6  5  4  3  2  1  0
      [  x  ] [  y  ] [  z  ]
               [p ] [q]
```

Where:
- x = bits 7-6 (2 bits, values 0-3)
- y = bits 5-3 (3 bits, values 0-7)
- z = bits 2-0 (3 bits, values 0-7)
- p = bits 5-4 (2 bits, values 0-3) = y >> 1
- q = bit 3 (1 bit, values 0-1) = y & 1

### Register Tables

**r[y] and r[z] (8-bit registers):**
| Index | Register |
|-------|----------|
| 0 | B |
| 1 | C |
| 2 | D |
| 3 | E |
| 4 | H |
| 5 | L |
| 6 | (HL) |
| 7 | A |

**rp[p] (register pairs):**
| Index | Pair |
|-------|------|
| 0 | BC |
| 1 | DE |
| 2 | HL |
| 3 | SP |

**rp2[p] (register pairs for PUSH/POP):**
| Index | Pair |
|-------|------|
| 0 | BC |
| 1 | DE |
| 2 | HL |
| 3 | AF |

**cc[y] (condition codes):**
| Index | Condition | Flag Test |
|-------|-----------|-----------|
| 0 | NZ | Z = 0 |
| 1 | Z  | Z = 1 |
| 2 | NC | C = 0 |
| 3 | C  | C = 1 |
| 4 | PO | P/V = 0 |
| 5 | PE | P/V = 1 |
| 6 | P  | S = 0 |
| 7 | M  | S = 1 |

**alu[y] (ALU operations):**
| Index | Operation |
|-------|-----------|
| 0 | ADD A, |
| 1 | ADC A, |
| 2 | SUB |
| 3 | SBC A, |
| 4 | AND |
| 5 | XOR |
| 6 | OR |
| 7 | CP |

### Unprefixed Decoding

**x = 0:**
| y | z | Instruction |
|---|---|-------------|
| - | 0 | NOP (y=0), EX AF,AF' (y=1), DJNZ d (y=2), JR d (y=3), JR cc[y-4],d (y=4..7) |
| - | 1 | q=0: LD rp[p],d16; q=1: ADD HL,rp[p] |
| - | 2 | q=0: LD (BC/DE),A or LD (d16),HL/A; q=1: LD A,(BC/DE) or LD HL/A,(d16) |
| - | 3 | q=0: INC rp[p]; q=1: DEC rp[p] |
| - | 4 | INC r[y] |
| - | 5 | DEC r[y] |
| - | 6 | LD r[y],d8 |
| - | 7 | RLCA(0), RRCA(1), RLA(2), RRA(3), DAA(4), CPL(5), SCF(6), CCF(7) |

**x = 1:**
| Condition | Instruction |
|-----------|-------------|
| y=6 AND z=6 | HALT |
| otherwise | LD r[y],r[z] |

**x = 2:**
alu[y] r[z]

**x = 3:**
| y | z | Instruction |
|---|---|-------------|
| - | 0 | RET cc[y] |
| - | 1 | q=0: POP rp2[p]; q=1: RET(0), EXX(1), JP (HL)(2), LD SP,HL(3) |
| - | 2 | JP cc[y],d16 |
| - | 3 | JP d16(0), CB prefix(1), OUT (d8),A(2), IN A,(d8)(3), EX (SP),HL(4), EX DE,HL(5), DI(6), EI(7) |
| - | 4 | CALL cc[y],d16 |
| - | 5 | q=0: PUSH rp2[p]; q=1: CALL d16(0), DD prefix(1), ED prefix(2), FD prefix(3) |
| - | 6 | alu[y] d8 |
| - | 7 | RST y*8 |

### CB-Prefixed Decoding

| x | Instruction |
|---|-------------|
| 0 | rot[y] r[z] (RLC/RRC/RL/RR/SLA/SRA/SLL/SRL) |
| 1 | BIT y,r[z] |
| 2 | RES y,r[z] |
| 3 | SET y,r[z] |

### ED-Prefixed Decoding

**x = 1:**
| y | z | Instruction |
|---|---|-------------|
| - | 0 | IN r[y],(C) (y=6: result discarded) |
| - | 1 | OUT (C),r[y] (y=6: outputs 0) |
| - | 2 | q=0: SBC HL,rp[p]; q=1: ADC HL,rp[p] |
| - | 3 | q=0: LD (d16),rp[p]; q=1: LD rp[p],(d16) |
| - | 4 | NEG |
| - | 5 | q=0: RETN; q=1: RETI |
| - | 6 | IM im[y] (im = 0,0,1,2,0,0,1,2) |
| - | 7 | LD I,A(0), LD R,A(1), LD A,I(2), LD A,R(3), RRD(4), RLD(5), NOP(6), NOP(7) |

**x = 2, z <= 3, y >= 4:**
Block instructions: LDI, CPI, INI, OUTI, LDD, CPD, IND, OUTD, LDIR, CPIR, INIR, OTIR, LDDR, CPDR, INDR, OTDR

All other ED opcodes: NOP (no operation).

### DD/FD Prefix Decoding

Same as unprefixed but with HL->IX/IY, H->IXH/IYH, L->IXL/IYL, (HL)->(IX+d)/(IY+d).

### DD CB / FD CB Decoding

Same as CB-prefixed but r[z] is always (IX+d)/(IY+d) for the memory operation. If z != 6, the result is additionally stored in r[z].

---

## 18. Test Suites

### 18.1. ZEXALL / ZEXDOC

**Author**: Frank D. Cringle
**Repository**: https://github.com/agn453/ZEXALL
**License**: GPL v2

**ZEXDOC**: Tests documented Z80 instructions with documented flag behavior only (masks out bits 3 and 5 of F register).

**ZEXALL**: Tests all Z80 instructions including undocumented flag bits 3 and 5.

**How it works:**
- Runs under CP/M (uses BDOS calls for console I/O)
- For each instruction group, it exercises the instruction with many different operand/flag combinations
- Computes a CRC32 of all results
- Compares against known-good CRC values from real Z80 hardware
- Reports PASS or FAIL for each test group

**How to use:**
1. Implement a minimal CP/M BDOS interface (trap CALL 5 for console output)
2. Load ZEXDOC.COM or ZEXALL.COM at address 0x0100
3. Start execution at 0x0100
4. Capture output -- each line shows test name and OK or FAIL with CRC
5. Start with ZEXDOC (easier to pass), then graduate to ZEXALL

**68 tests** covering: ld, add, adc, sub, sbc, and, or, xor, cp, inc, dec, bit, set, res, rl, rr, sla, sra, srl, push, pop, ex, call, ret, jp, jr, djnz, cpi, cpd, cpir, cpdr, ldi, ldd, ldir, lddr, in, out, daa, cpl, neg, ccf, scf, rlca, rrca, rla, rra, rld, rrd, etc.

**Runtime**: Several hours at real Z80 speed. Most emulators run it with turbo/no-delay mode.

### 18.2. z80test

**Author**: Patrik Rak
**Repository**: https://github.com/raxoft/z80test
**License**: MIT
**Latest Release**: v1.2a (December 2023)

Seven test programs of increasing strictness:

| Test | What it checks |
|------|---------------|
| z80full | All flags and registers |
| z80doc | Registers + documented flags only |
| z80flags | All flags, ignores register values |
| z80docflags | Documented flags only, ignores registers |
| z80ccf | Flag behavior after CCF instruction |
| z80ccfscr | Visual representation of SCF/CCF flag patterns |
| z80memptr | Flags after BIT n,(HL) (tests MEMPTR) |

**Target platform**: ZX Spectrum 48K
**Reference hardware**: Zilog Z80 NMOS CPU

**How to use:**
1. Implement ZX Spectrum memory map (ROM at 0-16383, RAM at 16384-65535)
2. Implement basic screen output (or intercept the RST 10h print routine)
3. Load the .tap or .sna file
4. Run and observe output

**Recommended progression:**
1. z80docflags (easiest -- only documented flags)
2. z80doc (adds register value checks)
3. z80flags (adds undocumented flags)
4. z80full (complete check)
5. z80memptr (MEMPTR register behavior)
6. z80ccf (SCF/CCF edge cases)

### 18.3. Other Useful Tests

**FUSE tests** (Free Unix Spectrum Emulator):
- https://fuse-emulator.sourceforge.net/
- Contains per-instruction test cases with expected register/flag states
- Input: initial register state + memory contents
- Output: expected register state + memory contents after execution
- Very useful for debugging individual instructions

**z80-tests by raddad772**:
- Single-instruction test cases
- Good for early development when individual instructions fail

### 18.4. Recommended Testing Strategy

1. **First**: Get basic instruction execution working (LD, ADD, SUB, JP, CALL, RET)
2. **Then**: Run ZEXDOC -- fix failures one test group at a time
3. **Then**: Run z80docflags / z80doc from z80test
4. **Then**: Implement undocumented flags and run ZEXALL
5. **Then**: Run z80full / z80flags from z80test
6. **Finally**: Implement MEMPTR and run z80memptr, z80ccf

---

## Appendix A: Condition Code Summary

| Code | Hex bits 5-3 | Test |
|------|-------------|------|
| NZ | 000 | Z flag = 0 |
| Z  | 001 | Z flag = 1 |
| NC | 010 | C flag = 0 |
| C  | 011 | C flag = 1 |
| PO | 100 | P/V flag = 0 |
| PE | 101 | P/V flag = 1 |
| P  | 110 | S flag = 0 (positive) |
| M  | 111 | S flag = 1 (minus/negative) |

## Appendix B: RST Addresses

| Opcode | Address |
|--------|---------|
| C7 | 0x0000 |
| CF | 0x0008 |
| D7 | 0x0010 |
| DF | 0x0018 |
| E7 | 0x0020 |
| EF | 0x0028 |
| F7 | 0x0030 |
| FF | 0x0038 |

## Appendix C: Parity Lookup

Parity is EVEN (P/V = 1) when the number of set bits in the byte is even.
Parity is ODD (P/V = 0) when the number of set bits is odd.

Pre-compute a 256-entry lookup table:
```c
uint8_t parity_table[256];
for (int i = 0; i < 256; i++) {
    int bits = 0;
    int v = i;
    while (v) { bits += v & 1; v >>= 1; }
    parity_table[i] = (bits % 2 == 0) ? 1 : 0;
}
```

## Appendix D: Overflow Detection

For 8-bit addition: overflow occurs when both operands have the same sign but the result has a different sign.
```c
bool overflow_add(uint8_t a, uint8_t b, uint8_t result) {
    return ((a ^ result) & (b ^ result) & 0x80) != 0;
}
```

For 8-bit subtraction: overflow occurs when the operands have different signs and the result has a different sign from the first operand.
```c
bool overflow_sub(uint8_t a, uint8_t b, uint8_t result) {
    return ((a ^ b) & (a ^ result) & 0x80) != 0;
}
```

## Appendix E: Half-Carry Detection

For 8-bit addition:
```c
bool half_carry_add(uint8_t a, uint8_t b, uint8_t carry) {
    return ((a & 0x0F) + (b & 0x0F) + carry) > 0x0F;
}
```

For 8-bit subtraction:
```c
bool half_carry_sub(uint8_t a, uint8_t b, uint8_t carry) {
    return ((int)(a & 0x0F) - (int)(b & 0x0F) - (int)carry) < 0;
}
```

For 16-bit addition (half-carry is from bit 11):
```c
bool half_carry_add16(uint16_t a, uint16_t b, uint8_t carry) {
    return ((a & 0x0FFF) + (b & 0x0FFF) + carry) > 0x0FFF;
}
```

---

## Sources

- [Z80 Opcode Table - CLRHome](https://clrhome.org/table/)
- [Z80 / R800 Instruction Set - Grauw](https://map.grauw.nl/resources/z80instr.php)
- [Decoding Z80 Opcodes - z80.info](http://www.z80.info/decoding.htm)
- [Z80 Opcodes - Pastraiser](https://pastraiser.com/cpu/Z80/Z80_opcodes.html)
- [Z80 Opcodes - MSX Wiki](https://www.msx.org/wiki/Z80_Opcodes)
- [Z80 Undocumented Instructions - z80.info](http://www.z80.info/z80undoc.htm)
- [The Undocumented Z80 Documented - Sean Young (PDF)](http://www.z80.info/zip/z80-documented.pdf)
- [Z80 Flag Affection - z80.info](http://www.z80.info/z80sflag.htm)
- [Undocumented Flags - Z80Decoder Wiki](https://github.com/hoglet67/Z80Decoder/wiki/Undocumented-Flags)
- [Z80 MEMPTR - drhelius gist](https://gist.github.com/drhelius/8497817)
- [The Z80 Flags - jnz.dk](https://jnz.dk/z80/flags.html)
- [The Z80 Interrupts - jnz.dk](https://jnz.dk/z80/interrupts.html)
- [The Z80 Registers - jnz.dk](https://jnz.dk/z80/registers.html)
- [The Z80 DAA - jnz.dk](https://jnz.dk/z80/daa.html)
- [Interrupt Behaviour of the Z80 CPU - z80.info](http://z80.info/interrup.htm)
- [Interrupt Mechanism - SMS Power!](https://www.smspower.org/Development/InterruptMechanism)
- [Z80 Interrupts - redcode Wiki](https://github.com/redcode/Z80/wiki/Interrupts)
- [Z80 Instruction Timing - floooh](https://floooh.github.io/2021/12/06/z80-instruction-timing.html)
- [Z80 CPU User Manual - Zilog (PDF)](https://www.zilog.com/docs/z80/um0080.pdf)
- [ZEXALL - GitHub](https://github.com/agn453/ZEXALL/)
- [z80test - GitHub](https://github.com/raxoft/z80test)
- [Z80 WikiTI Instruction Set](https://wikiti.brandonw.net/index.php?title=Z80_Instruction_Set)
- [Z80 Undocumented Instructions - World of Spectrum](https://worldofspectrum.org/z88forever/dn327/z80undoc.htm)
- [SCF/CCF Unstable Behaviour - Z80Decoder Wiki](https://github.com/hoglet67/Z80Decoder/wiki/Unstable-CCF-SCF-Behaviour)
- [Z80 MEMPTR - redcode Wiki](https://github.com/redcode/Z80/wiki/MEMPTR)

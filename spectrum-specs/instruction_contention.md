# ZX Spectrum 48K -- Per-Instruction Contention Breakdowns

This table documents the exact contention pattern for each Z80 instruction
on the 48K Spectrum. Notation: `address:cycles` means contention is checked
at the start, then that many T-states elapse. `[bracketed]` sections only
apply when a condition is met (branch taken, counter nonzero, etc.).

## Unprefixed Instructions

| Instruction(s) | Pattern |
|---|---|
| NOP, LD r,r', ALU A,r, INC/DEC r, EXX, EX AF/AF', EX DE/HL, DAA, CPL, CCF, SCF, DI, EI, RLA, RRA, RLCA, RRCA, JP (HL) | `pc:4` |
| INC/DEC dd, LD SP,HL | `pc:6` |
| ADD HL,dd | `pc:11` |
| LD r,n / ALU A,n | `pc:4, pc+1:3` |
| LD r,(HL) / ALU A,(HL) | `pc:4, hl:3` |
| LD (HL),r | `pc:4, hl:3` |
| LD (HL),n | `pc:4, pc+1:3, hl:3` |
| INC/DEC (HL) | `pc:4, hl:3, hl:1, hl(w):3` |
| LD dd,nn / JP nn / JP cc,nn | `pc:4, pc+1:3, pc+2:3` |
| LD A,(nn) / LD (nn),A | `pc:4, pc+1:3, pc+2:3, nn:3` |
| LD HL,(nn) / LD (nn),HL | `pc:4, pc+1:3, pc+2:3, nn:3, nn+1:3` |
| LD A,(BC) / LD A,(DE) / LD (BC),A / LD (DE),A | `pc:4, rr:3` |
| POP dd / RET | `pc:4, sp:3, sp+1:3` |
| RET cc | `pc:5, [sp:3, sp+1:3]` |
| PUSH dd / RST n | `pc:5, sp-1:3, sp-2:3` |
| CALL nn | `pc:4, pc+1:3, pc+2:3, [pc+2:1, sp-1:3, sp-2:3]` |
| CALL cc,nn | `pc:4, pc+1:3, pc+2:3, [pc+2:1, sp-1:3, sp-2:3]` |
| JR n | `pc:4, pc+1:3, [pc+1:1x5]` |
| JR cc,n | `pc:4, pc+1:3, [pc+1:1x5]` |
| DJNZ n | `pc:5, pc+1:3, [pc+1:1x5]` |
| IN A,(n) / OUT (n),A | `pc:4, pc+1:3, I/O` |
| EX (SP),HL | `pc:4, sp:3, sp+1:4, sp(w):3, sp+1(w):3, sp+1(w):1x2` |

## CB-Prefixed Instructions

| Instruction(s) | Pattern |
|---|---|
| SRL r, BIT b,r, SET b,r, RES b,r | `pc:4, pc+1:4` |
| SRL (HL), SET/RES b,(HL) | `pc:4, pc+1:4, hl:3, hl:1, hl(w):3` |
| BIT b,(HL) | `pc:4, pc+1:4, hl:3, hl:1` |

## ED-Prefixed Instructions

| Instruction(s) | Pattern |
|---|---|
| NEG, IM 0/1/2, LD A,I, LD I,A, LD A,R, LD R,A | `pc:4, pc+1:4` (some +1) |
| LD A,I / LD A,R / LD I,A / LD R,A | `pc:4, pc+1:5` |
| ADC HL,dd / SBC HL,dd | `pc:4, pc+1:11` |
| LD dd,(nn) / LD (nn),dd (ED) | `pc:4, pc+1:4, pc+2:3, pc+3:3, nn:3, nn+1:3` |
| IN r,(C) / OUT (C),r | `pc:4, pc+1:4, I/O` |
| LDI/LDD | `pc:4, pc+1:4, hl:3, de:3, de:1x2` |
| LDIR/LDDR | `pc:4, pc+1:4, hl:3, de:3, de:1x2, [de:1x5]` |
| CPI/CPD | `pc:4, pc+1:4, hl:3, hl:1x5` |
| CPIR/CPDR | `pc:4, pc+1:4, hl:3, hl:1x5, [hl:1x5]` |
| INI/IND | `pc:4, pc+1:5, I/O, hl:3` |
| INIR/INDR | `pc:4, pc+1:5, I/O, hl:3, [hl:1x5]` |
| OUTI/OUTD | `pc:4, pc+1:5, hl:3, I/O` |
| OTIR/OTDR | `pc:4, pc+1:5, hl:3, I/O, [hl:1x5]` |
| RLD / RRD | `pc:4, pc+1:4, hl:3, hl:1x4, hl(w):3` |
| RETI / RETN | `pc:4, pc+1:4, sp:3, sp+1:3` |

## DD/FD-Prefixed (IX/IY) Instructions

| Instruction(s) | Pattern |
|---|---|
| LD r,(IX+d) / ALU A,(IX+d) | `pc:4, pc+1:4, pc+2:3, pc+2:1x5, ix+d:3` |
| LD (IX+d),r | `pc:4, pc+1:4, pc+2:3, pc+2:1x5, ix+d:3` |
| LD (IX+d),n | `pc:4, pc+1:4, pc+2:3, pc+3:3, pc+3:1x2, ix+d:3` |
| INC/DEC (IX+d) | `pc:4, pc+1:4, pc+2:3, pc+2:1x5, ix+d:3, ix+d:1, ix+d(w):3` |
| ADD IX,dd | `pc:4, pc+1:11` |
| EX (SP),IX | `pc:4, pc+1:4, sp:3, sp+1:4, sp(w):3, sp+1(w):3, sp+1(w):1x2` |

## DDCB/FDCB-Prefixed (Indexed Bit Operations)

| Instruction(s) | Pattern |
|---|---|
| SRL (IX+d) / SET/RES b,(IX+d) | `pc:4, pc+1:4, pc+2:3, pc+3:3, pc+3:1x2, ix+d:3, ix+d:1, ix+d(w):3` |
| BIT b,(IX+d) | `pc:4, pc+1:4, pc+2:3, pc+3:3, pc+3:1x2, ix+d:3, ix+d:1` |

## Notation Key

- `pc:4` -- Opcode fetch from PC, 4 T-states, contention checked if PC in 0x4000-0x7FFF
- `hl:3` -- Memory access at HL address, 3 T-states, contention checked if HL in 0x4000-0x7FFF
- `hl:1x5` -- 5 internal cycles of 1 T-state each with HL on address bus, contention on each
- `hl(w):3` -- Memory write at HL, 3 T-states
- `sp:3` -- Memory read from SP, 3 T-states
- `nn:3` -- Memory access at absolute address nn
- `I/O` -- I/O operation (see I/O contention rules)
- `[...]` -- Conditional (only when branch taken / counter not exhausted)

## Sources

- World of Spectrum FAQ: 48K Reference
- Sinclair Wiki: Contended Memory
- Scratchpad Wiki: Contended Memory

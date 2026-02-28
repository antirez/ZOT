# ZX Spectrum 48K I/O Ports

## Partial Port Decoding

The Spectrum uses partial port decoding -- not all 16 address lines are checked.
Devices respond to patterns of address bits.

## ULA Port (0xFE -- any even port)

The ULA responds when bit 0 of the port address is 0.
ANY even-numbered port triggers the ULA (0x00, 0x02, 0x04, ..., 0xFE all work).

### Read (IN from even port)

| Bit | Function               | Notes                           |
|-----|------------------------|---------------------------------|
| 0   | Keyboard column 0     | 0 = pressed, 1 = not pressed    |
| 1   | Keyboard column 1     | 0 = pressed, 1 = not pressed    |
| 2   | Keyboard column 2     | 0 = pressed, 1 = not pressed    |
| 3   | Keyboard column 3     | 0 = pressed, 1 = not pressed    |
| 4   | Keyboard column 4     | 0 = pressed, 1 = not pressed    |
| 5   | Unused                 | Always 1                        |
| 6   | EAR input              | EAR line, with board-level coupling to MIC/speaker outputs (Issue-dependent) |
| 7   | Unused                 | Always 1                        |

The high byte of the port address selects keyboard half-rows (see keyboard.md).

### Write (OUT to even port)

| Bit | Function          | Notes                              |
|-----|-------------------|------------------------------------|
| 0   | Border blue       | Border color = bits 0-2            |
| 1   | Border red        |                                    |
| 2   | Border green      |                                    |
| 3   | MIC output        | Active LOW, used for tape saving   |
| 4   | Speaker (beeper)  | Toggle for sound output            |
| 5-7 | Unused            |                                    |

## Kempston Joystick (port 0x1F)

Responds when bits A7, A6, A5 are all 0.
Port pattern: `---- ---- 000- ----`

Reading returns: `000FUDLR`
| Bit | Function | Notes            |
|-----|----------|------------------|
| 0   | Right    | Active HIGH (1 = pressed) |
| 1   | Left     | Active HIGH      |
| 2   | Down     | Active HIGH      |
| 3   | Up       | Active HIGH      |
| 4   | Fire     | Active HIGH      |
| 5-7 | 0        |                  |

Note: opposite polarity from keyboard (active HIGH vs keyboard's active LOW).

## Floating Bus (unattached ports)

Reading a port that no device responds to returns whatever data is on the bus.
During ULA display fetch cycles, this is the bitmap or attribute byte being
fetched. During border/idle periods, returns 0xFF.

The floating bus is used by some games (notably Arkanoid) for display sync.

## Port Decoding Summary

| Device     | Condition                | Port example |
|------------|--------------------------|-------------|
| ULA        | Bit 0 = 0 (even)        | 0xFE        |
| Kempston   | Bits 7,6,5 = 0          | 0x1F        |
| ZX Printer | Bit 2 = 0               | 0xFB        |
| No device  | None of the above match  | 0xFF        |

## Implementation Notes

```c
uint8_t io_read(z80_t *cpu, uint16_t port) {
    if (!(port & 0x01)) {
        // ULA port: keyboard + EAR
        return keyboard_read(port);
    }
    if (!(port & 0xE0)) {
        // Kempston joystick
        return kempston_state;
    }
    // Floating bus / unattached: return 0xFF
    return 0xFF;
}

void io_write(z80_t *cpu, uint16_t port, uint8_t value) {
    if (!(port & 0x01)) {
        // ULA port
        border_color = value & 0x07;
        speaker_state = (value >> 4) & 1;
        mic_state = (value >> 3) & 1;
    }
}
```

## Sources

- World of Spectrum FAQ: Ports
- Sinclair Wiki: I/O Ports, Floating Bus

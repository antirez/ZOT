# ZX Spectrum 48K Keyboard

## Matrix Layout

The keyboard is a 40-key membrane keyboard organized as 8 half-rows of 5 keys.
Each half-row is selected by setting one of the address lines A8-A15 LOW when
reading port 0xFE.

| Port Address | Line | Bit 0      | Bit 1        | Bit 2 | Bit 3 | Bit 4 |
|-------------|------|------------|--------------|-------|-------|-------|
| 0xFEFE      | A8   | CAPS SHIFT | Z            | X     | C     | V     |
| 0xFDFE      | A9   | A          | S            | D     | F     | G     |
| 0xFBFE      | A10  | Q          | W            | E     | R     | T     |
| 0xF7FE      | A11  | 1          | 2            | 3     | 4     | 5     |
| 0xEFFE      | A12  | 0          | 9            | 8     | 7     | 6     |
| 0xDFFE      | A13  | P          | O            | I     | U     | Y     |
| 0xBFFE      | A14  | ENTER      | L            | K     | J     | H     |
| 0x7FFE      | A15  | SPACE      | SYMBOL SHIFT | M     | N     | B     |

## Reading the Keyboard

- Issue `IN A,(0xFE)` with the desired half-row selection in the upper address byte
- A **0** bit means the key IS pressed
- A **1** bit means the key is NOT pressed
- Bits 0-4: key data
- Bit 5: unused (always 1)
- Bit 6: EAR input (from tape)
- Bit 7: unused (always 1)

## Multiple Half-Row Selection

Multiple half-rows can be read simultaneously by setting multiple address lines
LOW. The results are ANDed together. For example, reading port 0x00FE (all
address lines low) reads all keys at once.

## Physical Layout

The left side of the keyboard (CAPS SHIFT through 5, T, G, V) uses address
lines A8-A11. The right side (6 through 0, Y, H, B through SPACE) uses A12-A15.

## Implementation Notes

For emulation, maintain an 8-byte array where each byte represents one half-row.
Each bit (0-4) represents one key. Set bit to 0 when pressed, 1 when released.

When reading port 0xFE:
1. Check which address lines are LOW (bits 8-15 of port address)
2. AND together the corresponding half-row bytes
3. OR with 0xA0 (bits 5 and 7 always high)
4. Handle bit 6 (EAR) separately

```c
uint8_t keyboard_read(uint16_t port) {
    uint8_t result = 0x1F;  // All keys released (bits 0-4 high)
    uint8_t high = port >> 8;

    for (int row = 0; row < 8; row++) {
        if (!(high & (1 << row))) {      // Address line LOW = row selected
            result &= keyboard_state[row]; // AND the row's key state
        }
    }

    result |= 0xA0;  // Bits 5,7 always 1
    // Bit 6: EAR input (set to 1 if no tape input)
    result |= 0x40;

    return result;
}
```

## Sources

- World of Spectrum FAQ: Keyboard
- Sinclair Wiki: Keyboard

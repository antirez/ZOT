# ZX Spectrum 48K Floating Bus

## What It Is

When the Z80 reads from a port that no device responds to, the data bus is
electrically floating. On the 48K Spectrum, the ULA's data bus connects to
the CPU's data bus through 470-ohm resistors (not tri-state buffers).

If the ULA is currently fetching screen data, the fetched byte "leaks" through
and is read by the CPU. If the ULA is idle (border, retrace), 0xFF is returned.

## Timing Pattern

Within each 8 T-state cycle during active display, the ULA fetches:

| T-state offset | ULA activity     | Byte returned        |
|----------------|------------------|----------------------|
| 0              | Fetch bitmap N   | Bitmap byte          |
| 1              | Fetch attr N     | Attribute byte       |
| 2              | Fetch bitmap N+1 | Next bitmap byte     |
| 3              | Fetch attr N+1   | Next attribute byte  |
| 4-7            | Idle             | 0xFF                 |

This repeats 16 times per scanline (128 T-states). During border/retrace,
always returns 0xFF.

## How Games Use It

Games place a distinctive attribute value (e.g., 0x68) in a known screen
position, then execute a tight loop:

```z80
sync_loop:
    IN A,(0xFF)
    CP 0x68          ; our marker attribute
    JR NZ, sync_loop ; keep looping until we see it
    ; Now we know exactly where the beam is
```

This provides display synchronization without interrupts.

## Games That Require It

- Arkanoid: loops forever without floating bus (waiting for a value)
- Cobra (original): same floating bus sync technique
- Sidewize: polls port 0x40FF, jerky without it
- Short Circuit: display sync via floating bus
- DigiSynth: requires exact floating bus behavior

## Implementation Priority

LOW for initial emulator. Most games work fine without it.
Can be added later -- when reading unattached ports, check current T-state
and return ULA fetch data if in active display, 0xFF otherwise.

Simple stub: always return 0xFF from unattached ports. This breaks ~5-10
known commercial games but works for everything else.

## Sources

- Sinclair Wiki: Floating Bus
- Floating Bus Technical Guide (k1.spdns.de)
- Spectrum for Everyone: Memory Contention and Floating Bus

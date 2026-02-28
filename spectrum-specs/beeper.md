# ZX Spectrum 48K Beeper / Sound

## Hardware

The Spectrum 48K has no dedicated sound chip -- it uses a simple 1-bit beeper
driven directly by the CPU via port 0xFE.

## Port 0xFE Bit 4: Speaker Output

Writing to any even port address controls the speaker:
- Bit 4 = 1: speaker cone pushed out
- Bit 4 = 0: speaker cone pulled in

Sound is produced by alternating bit 4 at the desired frequency.

## Generating Tones

For a tone of frequency F Hz, toggle bit 4 every:
```
delay = 3,500,000 / (2 * F)  T-states
```

Examples:
- Middle A (440 Hz): 3,500,000 / 880 = 3977 T-states between toggles
- Middle C (~262 Hz): 3,500,000 / 524 = 6679 T-states between toggles

## EAR/MIC Interaction

- Bit 3 (MIC output): used for tape saving (active LOW)
- Bit 6 (EAR input, on read): from EAR socket (tape loading)

On Issue 3 hardware (most common 48K):
- MIC output has minimal effect on EAR input
- If bit 4 (speaker) is 1, EAR input bit 6 cannot be pulled low

Emulator note: a practical digital approximation is `EAR_read_bit = external_EAR OR speaker_bit`.
Exact analog behavior differs between board revisions and input signal levels.

## Emulator Implementation

For audio output, sample bit 4 state at regular intervals matching the host
audio sample rate.

At 44,100 Hz: sample every 3,500,000 / 44,100 = ~79.4 T-states.

Approach:
1. Maintain a frame audio buffer (69,888 / 79.4 = ~880 samples per frame)
2. Track current speaker state (0 or 1) and the T-state of each change
3. At end of frame (or periodically), convert to PCM samples
4. Apply simple low-pass filtering to smooth the 1-bit output

```c
// Simple approach: record speaker state changes
typedef struct {
    uint32_t tstates;  // When the change happened
    uint8_t  level;    // New speaker level (0 or 1)
} beeper_event_t;

beeper_event_t beeper_events[MAX_EVENTS];
int beeper_event_count;

void beeper_write(uint32_t frame_tstate, uint8_t port_value) {
    uint8_t new_level = (port_value >> 4) & 1;
    if (new_level != current_speaker_level) {
        current_speaker_level = new_level;
        beeper_events[beeper_event_count].tstates = frame_tstate;
        beeper_events[beeper_event_count].level = new_level;
        beeper_event_count++;
    }
}

// At end of frame, convert events to PCM samples
void beeper_render_frame(int16_t *audio_buf, int num_samples) {
    int event_idx = 0;
    uint8_t level = initial_speaker_level;

    for (int i = 0; i < num_samples; i++) {
        uint32_t sample_tstate = (uint32_t)(i * 69888.0 / num_samples);

        while (event_idx < beeper_event_count &&
               beeper_events[event_idx].tstates <= sample_tstate) {
            level = beeper_events[event_idx].level;
            event_idx++;
        }

        audio_buf[i] = level ? 16384 : -16384;  // Convert to signed PCM
    }
}
```

## Sources

- World of Spectrum FAQ: Sound
- Sinclair Wiki: Beeper

/* tzx.h -- TZX/TAP tape file player for ZX Spectrum emulator.
 *
 * TZX files store Spectrum tape data as symbolic pulse sequences with
 * exact T-state timing for every pilot tone, sync pulse, and data bit.
 * This player feeds the EAR bit directly from TZX pulse data -- perfect
 * loading with no audio processing needed.
 *
 * TAP files are also supported. TAP is a simpler format: each block is
 * just a 2-byte length followed by raw data bytes. All blocks use
 * standard ROM loader timing (pilot 2168, sync 667/735, bits 855/1710).
 *
 * Usage:
 *   TZXPlayer tape;
 *   tzx_load(&tape, data, len);
 *   tzx_play(&tape, cpu.clocks);
 *
 *   // In main loop, before each zx_tick():
 *   zx_set_ear(zx, tzx_update(&tape, zx->cpu.clocks));
 */

#ifndef TZX_H
#define TZX_H

#include <stdint.h>

/* Phase state machine for pulse generation. Each data block progresses
 * through PILOT -> SYNC1 -> SYNC2 -> DATA -> PAUSE. Pure tone and
 * pulse sequence blocks use their own phases. */
typedef enum {
    TZX_PHASE_IDLE,     /* No block active, parse next block */
    TZX_PHASE_PILOT,    /* Pilot tone pulses */
    TZX_PHASE_SYNC1,    /* First sync pulse */
    TZX_PHASE_SYNC2,    /* Second sync pulse */
    TZX_PHASE_DATA,     /* Data bits (two pulses per bit) */
    TZX_PHASE_PAUSE,    /* Post-block silence */
    TZX_PHASE_TONE,     /* Pure tone (block 0x12) */
    TZX_PHASE_PULSES,   /* Pulse sequence (block 0x13) */
    TZX_PHASE_DIRECT,   /* Direct recording (block 0x15) */
} TZXPhase;

typedef struct {
    /* File data (caller-owned, must remain valid during playback). */
    const uint8_t *data;
    int len;

    /* Current parse position in file data (offset to next block).
     * For TZX: starts at 10 (past header). For TAP: starts at 0. */
    int offset;

    /* File format: 0 = TZX, 1 = TAP. */
    int is_tap;

    /* Playback state. */
    int playing;
    uint8_t level;          /* Current EAR output (0 or 1) */
    uint64_t edge_clock;    /* Absolute cpu.clocks of next EAR transition */

    /* Phase state machine. */
    TZXPhase phase;

    /* Standard/turbo data block timing parameters. */
    uint16_t pilot_pulse;   /* Pilot pulse length in T-states */
    uint16_t sync1;         /* First sync pulse */
    uint16_t sync2;         /* Second sync pulse */
    uint16_t zero_pulse;    /* Zero bit pulse length */
    uint16_t one_pulse;     /* One bit pulse length */
    int pilot_remaining;    /* Pilot pulses left to emit */

    /* Data state (shared by DATA and DIRECT phases). */
    const uint8_t *block_data;  /* Pointer to current block's data bytes */
    int data_len;               /* Total data bytes in block */
    int data_pos;               /* Current byte index */
    int bit_pos;                /* Current bit (7=MSB down to 0=LSB) */
    int pulse_half;             /* 0=first half, 1=second half of bit */
    int used_bits_last;         /* Valid bits in last byte (1-8) */

    /* Pause after current block. */
    uint32_t pause_tstates;

    /* Pure tone (block 0x12). */
    uint16_t tone_pulse;
    int tone_remaining;

    /* Pulse sequence (block 0x13). */
    const uint8_t *pulse_lengths;   /* Array of 2-byte LE pulse lengths */
    int pulse_count;
    int pulse_idx;

    /* Direct recording (block 0x15). */
    uint16_t sample_tstates;

    /* Loop support (single nesting level). */
    int loop_start_offset;
    int loop_count;
} TZXPlayer;

/* Load a TZX or TAP file. The data pointer must remain valid while
 * playing. Auto-detects format (TZX has "ZXTape!" header, else TAP).
 * Returns 0 on success, -1 on error. */
int tzx_load(TZXPlayer *p, const uint8_t *data, int len);

/* Start playback from the beginning. cpu_clocks is the current value
 * of cpu.clocks, used to synchronize the first edge transition. */
void tzx_play(TZXPlayer *p, uint64_t cpu_clocks);

/* Stop playback. Sets EAR level to 0. */
void tzx_stop(TZXPlayer *p);

/* Returns 1 if tape is currently playing. */
int tzx_is_playing(TZXPlayer *p);

/* Update the EAR output based on current CPU clock. Call before each
 * zx_tick(). Returns the current EAR level (0 or 1).
 * This is the hot path -- called once per Z80 instruction. */
uint8_t tzx_update(TZXPlayer *p, uint64_t cpu_clocks);

#endif /* TZX_H */

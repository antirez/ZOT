/* tzx.c -- TZX/TAP tape file player implementation.
 *
 * TZX FORMAT OVERVIEW
 * ===================
 * A TZX file starts with a 10-byte header ("ZXTape!" + 0x1A + version),
 * followed by a sequence of blocks. Each block starts with an ID byte
 * that determines its type and layout.
 *
 * The most important block types store tape data as pulse sequences:
 *
 *   0x10 Standard Speed Data -- standard ROM loader timing
 *   0x11 Turbo Speed Data    -- custom timing for turbo loaders
 *   0x12 Pure Tone           -- repeated pulse of fixed length
 *   0x13 Pulse Sequence      -- arbitrary sequence of pulses
 *   0x14 Pure Data            -- data bits without pilot/sync
 *   0x15 Direct Recording    -- raw 1-bit samples at given rate
 *
 * Standard tape data (blocks 0x10/0x11) follows this pulse pattern:
 *   PILOT (many identical pulses) -> SYNC1 -> SYNC2 -> DATA bits -> PAUSE
 *
 * Each data bit produces two identical pulses (one full wave):
 *   - Zero bit: two short pulses (855 T-states each at standard speed)
 *   - One bit:  two long pulses (1710 T-states each at standard speed)
 *
 * TAP FORMAT
 * ==========
 * TAP is simpler: just a sequence of data blocks, each preceded by a
 * 2-byte little-endian length. All blocks use standard ROM timing.
 * There's no header or block type IDs.
 *
 * PULSE GENERATION
 * ================
 * The player maintains a level (0 or 1) that represents the EAR bit.
 * At each "edge" (transition), the level toggles and we compute how
 * many T-states until the next edge. The main loop in tzx_update()
 * advances through edges as cpu.clocks catches up.
 */

#include "tzx.h"
#include <string.h>
#include <stdio.h>

/* ===================================================================
 * HELPER: READ LITTLE-ENDIAN VALUES
 * =================================================================== */

static inline uint16_t r16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t r24(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
}

static inline uint32_t r32(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

/* Standard ROM loader timing constants (T-states at 3.5 MHz). */
#define PILOT_PULSE     2168    /* Pilot tone pulse length */
#define SYNC1_PULSE     667     /* First sync pulse */
#define SYNC2_PULSE     735     /* Second sync pulse */
#define ZERO_PULSE      855     /* Zero bit pulse length */
#define ONE_PULSE       1710    /* One bit pulse length */
#define PILOT_HEADER    8063    /* Pilot pulses for header block */
#define PILOT_DATA      3223    /* Pilot pulses for data block */
#define PAUSE_MS        1000    /* Default pause between blocks (ms) */
#define TSTATES_PER_MS  3500    /* T-states per millisecond at 3.5 MHz */

/* ===================================================================
 * FORWARD DECLARATIONS
 * =================================================================== */

static int tzx_parse_next_block(TZXPlayer *p);
static uint32_t tzx_next_pulse(TZXPlayer *p);

/* ===================================================================
 * PUBLIC API
 * =================================================================== */

int tzx_load(TZXPlayer *p, const uint8_t *data, int len) {
    memset(p, 0, sizeof(TZXPlayer));
    p->data = data;
    p->len = len;

    /* Detect format: TZX starts with "ZXTape!\x1A". */
    if (len >= 10 && memcmp(data, "ZXTape!\x1A", 8) == 0) {
        p->is_tap = 0;
        p->offset = 10;    /* Skip TZX header */
        printf("TZX: version %d.%02d, %d bytes\n",
               data[8], data[9], len);
    } else if (len >= 2) {
        p->is_tap = 1;
        p->offset = 0;
        printf("TAP: %d bytes\n", len);
    } else {
        return -1;
    }
    return 0;
}

void tzx_play(TZXPlayer *p, uint64_t cpu_clocks) {
    if (!p->data) return;

    /* Reset to beginning. */
    p->offset = p->is_tap ? 0 : 10;
    p->playing = 1;
    p->level = 0;
    p->edge_clock = cpu_clocks;
    p->phase = TZX_PHASE_IDLE;
    p->loop_count = 0;
    p->loop_start_offset = 0;
}

void tzx_stop(TZXPlayer *p) {
    p->playing = 0;
    p->level = 0;
}

int tzx_is_playing(TZXPlayer *p) {
    return p->playing;
}

uint8_t tzx_update(TZXPlayer *p, uint64_t cpu_clocks) {
    /* Hot path: advance through edges until we're caught up.
     * Each iteration toggles the level and gets the duration of the
     * next pulse (time until the next toggle). */
    while (p->playing && cpu_clocks >= p->edge_clock) {
        p->level ^= 1;
        uint32_t pulse = tzx_next_pulse(p);
        if (!pulse) {
            /* End of tape or stop command. */
            p->playing = 0;
            p->level = 0;
            break;
        }
        p->edge_clock += pulse;
    }
    return p->level;
}

/* ===================================================================
 * BLOCK PARSING -- TAP FORMAT
 * =================================================================== */

/* Parse the next TAP block. Each block is:
 *   2 bytes: data length N (little-endian)
 *   N bytes: data (flag byte + payload + checksum)
 * All blocks use standard ROM loader timing. */
static int tzx_parse_tap_block(TZXPlayer *p) {
    if (p->offset + 2 > p->len) return 0;  /* End of tape */

    uint16_t block_len = r16(p->data + p->offset);
    if (block_len == 0 || p->offset + 2 + block_len > p->len) return 0;

    p->pilot_pulse = PILOT_PULSE;
    p->sync1 = SYNC1_PULSE;
    p->sync2 = SYNC2_PULSE;
    p->zero_pulse = ZERO_PULSE;
    p->one_pulse = ONE_PULSE;

    p->block_data = p->data + p->offset + 2;
    p->data_len = block_len;

    /* The first byte is the flag byte: < 0x80 = header, >= 0x80 = data.
     * Headers get a longer pilot tone so the user can hear the
     * characteristic "bzzz" before loading starts. */
    p->pilot_remaining = (p->block_data[0] < 0x80) ? PILOT_HEADER : PILOT_DATA;

    p->data_pos = 0;
    p->bit_pos = 7;
    p->pulse_half = 0;
    p->used_bits_last = 8;
    p->pause_tstates = (uint32_t)PAUSE_MS * TSTATES_PER_MS;
    p->phase = TZX_PHASE_PILOT;
    p->offset += 2 + block_len;

    printf("TAP: block %d bytes (flag 0x%02X)\n",
           block_len, p->block_data[0]);
    return 1;
}

/* ===================================================================
 * BLOCK PARSING -- TZX FORMAT
 * =================================================================== */

/* Parse the next TZX block. Sets up the phase state machine and
 * block parameters. Returns 1 if a block was parsed and pulses are
 * ready, 0 if end of tape or unrecoverable error. */
static int tzx_parse_next_block(TZXPlayer *p) {
    /* TAP files use a completely different block format. */
    if (p->is_tap) return tzx_parse_tap_block(p);

    for (;;) {
        if (p->offset >= p->len) return 0;  /* End of tape */

        uint8_t id = p->data[p->offset];
        const uint8_t *b = p->data + p->offset + 1;
        int remaining = p->len - p->offset - 1;

        switch (id) {

        /* -----------------------------------------------------------
         * Block 0x10: Standard Speed Data
         * -----------------------------------------------------------
         * Uses fixed ROM loader timing. The pilot count depends on the
         * flag byte: headers (flag < 0x80) get 8063 pilot pulses,
         * data blocks (flag >= 0x80) get 3223. */
        case 0x10: {
            if (remaining < 4) return 0;
            uint16_t pause_ms = r16(b);
            uint16_t data_len = r16(b + 2);
            if (remaining < 4 + data_len) return 0;

            p->pilot_pulse = PILOT_PULSE;
            p->sync1 = SYNC1_PULSE;
            p->sync2 = SYNC2_PULSE;
            p->zero_pulse = ZERO_PULSE;
            p->one_pulse = ONE_PULSE;
            p->block_data = b + 4;
            p->data_len = data_len;
            p->pilot_remaining = (data_len > 0 && p->block_data[0] < 0x80)
                                 ? PILOT_HEADER : PILOT_DATA;
            p->data_pos = 0;
            p->bit_pos = 7;
            p->pulse_half = 0;
            p->used_bits_last = 8;
            p->pause_tstates = (uint32_t)pause_ms * TSTATES_PER_MS;
            p->phase = TZX_PHASE_PILOT;
            p->offset += 1 + 4 + data_len;
            return 1;
        }

        /* -----------------------------------------------------------
         * Block 0x11: Turbo Speed Data
         * -----------------------------------------------------------
         * Like 0x10 but all timing parameters come from the block
         * header, allowing turbo loaders with custom pulse widths. */
        case 0x11: {
            if (remaining < 18) return 0;
            p->pilot_pulse = r16(b);
            p->sync1 = r16(b + 2);
            p->sync2 = r16(b + 4);
            p->zero_pulse = r16(b + 6);
            p->one_pulse = r16(b + 8);
            p->pilot_remaining = r16(b + 10);
            p->used_bits_last = b[12];
            if (p->used_bits_last == 0) p->used_bits_last = 8;
            uint16_t pause_ms = r16(b + 13);
            uint32_t data_len = r24(b + 15);
            if (remaining < (int)(18 + data_len)) return 0;

            p->block_data = b + 18;
            p->data_len = (int)data_len;
            p->data_pos = 0;
            p->bit_pos = 7;
            p->pulse_half = 0;
            p->pause_tstates = (uint32_t)pause_ms * TSTATES_PER_MS;
            p->phase = TZX_PHASE_PILOT;
            p->offset += 1 + 18 + (int)data_len;
            return 1;
        }

        /* -----------------------------------------------------------
         * Block 0x12: Pure Tone
         * -----------------------------------------------------------
         * N identical pulses -- used by custom loaders for pilot tones
         * or clock sync. No data, no pause. */
        case 0x12: {
            if (remaining < 4) return 0;
            p->tone_pulse = r16(b);
            p->tone_remaining = r16(b + 2);
            p->phase = TZX_PHASE_TONE;
            p->offset += 1 + 4;
            return 1;
        }

        /* -----------------------------------------------------------
         * Block 0x13: Pulse Sequence
         * -----------------------------------------------------------
         * A sequence of individually-timed pulses. Each pulse toggles
         * the EAR level. Used for custom sync patterns. */
        case 0x13: {
            if (remaining < 1) return 0;
            int n = b[0];
            if (remaining < 1 + n * 2) return 0;
            p->pulse_lengths = b + 1;
            p->pulse_count = n;
            p->pulse_idx = 0;
            p->phase = TZX_PHASE_PULSES;
            p->offset += 1 + 1 + n * 2;
            return 1;
        }

        /* -----------------------------------------------------------
         * Block 0x14: Pure Data
         * -----------------------------------------------------------
         * Data bits without pilot/sync. Starts directly in DATA phase.
         * Used by turbo loaders that handle their own pilot via
         * blocks 0x12/0x13. */
        case 0x14: {
            if (remaining < 10) return 0;
            p->zero_pulse = r16(b);
            p->one_pulse = r16(b + 2);
            p->used_bits_last = b[4];
            if (p->used_bits_last == 0) p->used_bits_last = 8;
            uint16_t pause_ms = r16(b + 5);
            uint32_t data_len = r24(b + 7);
            if (remaining < (int)(10 + data_len)) return 0;

            p->block_data = b + 10;
            p->data_len = (int)data_len;
            p->data_pos = 0;
            p->bit_pos = 7;
            p->pulse_half = 0;
            p->pause_tstates = (uint32_t)pause_ms * TSTATES_PER_MS;
            p->phase = TZX_PHASE_DATA;
            p->offset += 1 + 10 + (int)data_len;
            return 1;
        }

        /* -----------------------------------------------------------
         * Block 0x15: Direct Recording
         * -----------------------------------------------------------
         * Raw 1-bit samples at a given sample rate. The level is set
         * directly from each bit (not toggled like normal pulses). */
        case 0x15: {
            if (remaining < 8) return 0;
            p->sample_tstates = r16(b);
            uint16_t pause_ms = r16(b + 2);
            p->used_bits_last = b[4];
            if (p->used_bits_last == 0) p->used_bits_last = 8;
            uint32_t data_len = r24(b + 5);
            if (remaining < (int)(8 + data_len)) return 0;

            p->block_data = b + 8;
            p->data_len = (int)data_len;
            p->data_pos = 0;
            p->bit_pos = 7;
            p->pause_tstates = (uint32_t)pause_ms * TSTATES_PER_MS;
            p->phase = TZX_PHASE_DIRECT;
            p->offset += 1 + 8 + (int)data_len;
            return 1;
        }

        /* -----------------------------------------------------------
         * Block 0x20: Pause / Stop the Tape
         * -----------------------------------------------------------
         * If duration is 0, stop the tape (user must press play).
         * Otherwise, insert silence for the given number of ms. */
        case 0x20: {
            if (remaining < 2) return 0;
            uint16_t pause_ms = r16(b);
            p->offset += 1 + 2;
            if (pause_ms == 0) {
                printf("TZX: Stop the tape\n");
                p->playing = 0;
                return 0;
            }
            p->pause_tstates = (uint32_t)pause_ms * TSTATES_PER_MS;
            p->phase = TZX_PHASE_PAUSE;
            return 1;
        }

        /* -----------------------------------------------------------
         * Block 0x21: Group Start
         * -----------------------------------------------------------
         * Groups are informational only. Print the name and continue. */
        case 0x21: {
            if (remaining < 1) return 0;
            int name_len = b[0];
            if (remaining < 1 + name_len) return 0;
            printf("TZX: [%.*s]\n", name_len, (const char *)(b + 1));
            p->offset += 1 + 1 + name_len;
            continue;
        }

        /* Block 0x22: Group End -- no body, just continue. */
        case 0x22:
            p->offset += 1;
            continue;

        /* -----------------------------------------------------------
         * Block 0x24: Loop Start
         * -----------------------------------------------------------
         * Record the offset of the block *after* this one, and the
         * number of repetitions. Only one nesting level is supported
         * (which covers all known TZX files). */
        case 0x24: {
            if (remaining < 2) return 0;
            p->loop_count = r16(b);
            p->offset += 1 + 2;
            p->loop_start_offset = p->offset;
            continue;
        }

        /* -----------------------------------------------------------
         * Block 0x25: Loop End
         * -----------------------------------------------------------
         * Decrement loop counter. If iterations remain, jump back. */
        case 0x25:
            p->offset += 1;
            if (p->loop_count > 1) {
                p->loop_count--;
                p->offset = p->loop_start_offset;
            }
            continue;

        /* -----------------------------------------------------------
         * Block 0x2A: Stop the Tape if in 48K Mode
         * -----------------------------------------------------------
         * Since we're always 48K, this always stops the tape. */
        case 0x2A: {
            if (remaining < 4) return 0;
            p->offset += 1 + 4;
            printf("TZX: Stop (48K mode)\n");
            p->playing = 0;
            return 0;
        }

        /* -----------------------------------------------------------
         * Block 0x2B: Set Signal Level
         * -----------------------------------------------------------
         * Force the EAR level to a specific value. Used by some TZX
         * files to ensure correct polarity before data blocks. */
        case 0x2B: {
            if (remaining < 5) return 0;
            p->level = b[4] ? 1 : 0;
            p->offset += 1 + 5;
            continue;
        }

        /* -----------------------------------------------------------
         * Block 0x30: Text Description
         * -----------------------------------------------------------
         * Informational text. Print and skip. */
        case 0x30: {
            if (remaining < 1) return 0;
            int text_len = b[0];
            if (remaining < 1 + text_len) return 0;
            printf("TZX: %.*s\n", text_len, (const char *)(b + 1));
            p->offset += 1 + 1 + text_len;
            continue;
        }

        /* -----------------------------------------------------------
         * Block 0x32: Archive Info
         * -----------------------------------------------------------
         * Structured metadata (title, author, year, etc.). */
        case 0x32: {
            if (remaining < 2) return 0;
            uint16_t block_len = r16(b);
            if (remaining < 2 + (int)block_len) return 0;

            /* Parse and print the text strings. */
            if (block_len >= 1) {
                int num_strings = b[2];
                int pos = 3;
                for (int i = 0; i < num_strings; i++) {
                    if (pos + 2 > 2 + (int)block_len) break;
                    uint8_t type_id = b[pos];
                    uint8_t text_len = b[pos + 1];
                    pos += 2;
                    if (pos + text_len > 2 + (int)block_len) break;

                    const char *label;
                    switch (type_id) {
                        case 0x00: label = "Title"; break;
                        case 0x01: label = "Publisher"; break;
                        case 0x02: label = "Author"; break;
                        case 0x03: label = "Year"; break;
                        case 0x04: label = "Language"; break;
                        case 0x05: label = "Type"; break;
                        case 0x06: label = "Price"; break;
                        case 0x07: label = "Loader"; break;
                        case 0x08: label = "Origin"; break;
                        case 0xFF: label = "Comment"; break;
                        default:   label = "Info"; break;
                    }
                    printf("TZX: %s: %.*s\n", label, text_len,
                           (const char *)(b + pos));
                    pos += text_len;
                }
            }
            p->offset += 1 + 2 + (int)block_len;
            continue;
        }

        /* -----------------------------------------------------------
         * Unknown / unsupported blocks
         * -----------------------------------------------------------
         * Try to compute the block size and skip it. Many TZX block
         * types have predictable sizes even if we don't process them. */
        default: {
            int body_size = -1;
            switch (id) {
                case 0x23: body_size = 2; break;
                case 0x26: if (remaining >= 2) body_size = 2 + r16(b) * 2; break;
                case 0x27: body_size = 0; break;
                case 0x28: if (remaining >= 2) body_size = 2 + r16(b); break;
                case 0x31: if (remaining >= 2) body_size = 2 + b[1]; break;
                case 0x33: if (remaining >= 1) body_size = 1 + b[0] * 3; break;
                case 0x35: if (remaining >= 20) body_size = 20 + (int)r32(b + 16); break;
                case 0x5A: body_size = 9; break;
                default: break;
            }
            if (body_size >= 0 && 1 + body_size <= p->len - p->offset) {
                printf("TZX: Skipping block 0x%02X (%d bytes)\n",
                       id, body_size);
                p->offset += 1 + body_size;
                continue;
            }
            printf("TZX: Unknown block 0x%02X at offset %d, stopping\n",
                   id, p->offset);
            p->playing = 0;
            return 0;
        }

        }  /* switch (id) */
    }  /* for (;;) */
}

/* ===================================================================
 * PULSE GENERATION
 * =================================================================== */

/* Return the T-state duration of the next pulse, advancing through
 * the current block's phase state machine. Returns 0 when the tape
 * ends or playback should stop.
 *
 * For most phases, the caller (tzx_update) toggles the level before
 * calling this. For DIRECT recording and PAUSE, this function
 * overrides the level directly. */
static uint32_t tzx_next_pulse(TZXPlayer *p) {
    for (;;) {
        switch (p->phase) {

        /* No block active: try to parse the next one. */
        case TZX_PHASE_IDLE:
            if (!tzx_parse_next_block(p)) return 0;
            continue;

        /* Pilot tone: emit identical pulses until count exhausted. */
        case TZX_PHASE_PILOT:
            if (p->pilot_remaining > 0) {
                p->pilot_remaining--;
                return p->pilot_pulse;
            }
            p->phase = TZX_PHASE_SYNC1;
            continue;

        /* First sync pulse. */
        case TZX_PHASE_SYNC1:
            p->phase = TZX_PHASE_SYNC2;
            return p->sync1;

        /* Second sync pulse. After this, data bits follow. */
        case TZX_PHASE_SYNC2:
            p->phase = TZX_PHASE_DATA;
            return p->sync2;

        /* Data bits: each bit produces two identical pulses.
         * A zero bit uses short pulses, a one bit uses long pulses.
         * We track which half of the current bit we're emitting. */
        case TZX_PHASE_DATA: {
            if (p->data_len == 0) {
                /* No data in this block, skip to pause. */
                if (p->pause_tstates > 0) {
                    p->phase = TZX_PHASE_PAUSE;
                } else {
                    p->phase = TZX_PHASE_IDLE;
                }
                continue;
            }

            int bit = (p->block_data[p->data_pos] >> p->bit_pos) & 1;
            uint32_t pulse_len = bit ? p->one_pulse : p->zero_pulse;

            if (p->pulse_half == 0) {
                /* First half of bit: emit pulse, come back for second. */
                p->pulse_half = 1;
                return pulse_len;
            }

            /* Second half done. Advance to next bit. */
            p->pulse_half = 0;
            p->bit_pos--;

            /* Check if we've exhausted all valid bits. For the last byte,
             * only used_bits_last bits are valid (counting from MSB).
             * e.g. used_bits_last=6 means bits 7..2 are valid. */
            int stop_bit = (p->data_pos == p->data_len - 1)
                           ? (8 - p->used_bits_last) : 0;

            if (p->bit_pos < stop_bit) {
                /* This byte is done. */
                if (p->data_pos >= p->data_len - 1) {
                    /* All data consumed. Move to pause or next block. */
                    if (p->pause_tstates > 0) {
                        p->phase = TZX_PHASE_PAUSE;
                    } else {
                        p->phase = TZX_PHASE_IDLE;
                    }
                    return pulse_len;
                }
                /* Next byte. */
                p->bit_pos = 7;
                p->data_pos++;
            }
            return pulse_len;
        }

        /* Post-block silence. Force EAR low for the pause duration,
         * then continue to the next block. */
        case TZX_PHASE_PAUSE: {
            uint32_t d = p->pause_tstates;
            p->level = 0;
            p->phase = TZX_PHASE_IDLE;
            return d;
        }

        /* Pure tone: repeated pulses of fixed length. */
        case TZX_PHASE_TONE:
            if (p->tone_remaining > 0) {
                p->tone_remaining--;
                return p->tone_pulse;
            }
            p->phase = TZX_PHASE_IDLE;
            continue;

        /* Pulse sequence: individually-timed pulses. */
        case TZX_PHASE_PULSES:
            if (p->pulse_idx < p->pulse_count) {
                uint16_t len = r16(p->pulse_lengths + p->pulse_idx * 2);
                p->pulse_idx++;
                return len;
            }
            p->phase = TZX_PHASE_IDLE;
            continue;

        /* Direct recording: each bit directly sets the level.
         * Unlike normal data, the level is NOT toggled by the caller --
         * we override it here based on the sample bit value. */
        case TZX_PHASE_DIRECT: {
            if (p->data_len == 0) {
                if (p->pause_tstates > 0) {
                    p->phase = TZX_PHASE_PAUSE;
                } else {
                    p->phase = TZX_PHASE_IDLE;
                }
                continue;
            }

            /* Set level directly from the sample bit. */
            p->level = (p->block_data[p->data_pos] >> p->bit_pos) & 1;

            /* Advance to next sample bit. */
            int stop_bit = (p->data_pos == p->data_len - 1)
                           ? (8 - p->used_bits_last) : 0;
            p->bit_pos--;

            if (p->bit_pos < stop_bit) {
                if (p->data_pos >= p->data_len - 1) {
                    /* All samples consumed. */
                    if (p->pause_tstates > 0) {
                        p->phase = TZX_PHASE_PAUSE;
                    } else {
                        p->phase = TZX_PHASE_IDLE;
                    }
                    return p->sample_tstates;
                }
                p->bit_pos = 7;
                p->data_pos++;
            }
            return p->sample_tstates;
        }

        }  /* switch (p->phase) */
    }  /* for (;;) */
}

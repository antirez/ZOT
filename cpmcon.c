/* cpmcon.c -- CP/M 2.2 Console Frontend
 *
 * This is the terminal-based front end for the CP/M emulator. It sets
 * up a CPMachine, mounts host directories as CP/M drives, puts the
 * terminal into raw mode for interactive use, and runs the CCP.
 *
 * Usage:
 *   cpmcon [directory]
 *
 * The directory (default: current directory) is mounted as drive A:.
 * You can set additional drives with -B, -C, etc.:
 *   cpmcon -A /path/to/a -B /path/to/b
 *
 * Terminal translation (-t flag):
 *   auto   -- (default) translates ADM-3A/Kaypro codes to ANSI while
 *             passing through native ANSI sequences untouched
 *   adm3a  -- full translation of ADM-3A/Kaypro/TeleVideo/VT52 codes
 *   ansi   -- no translation, raw pass-through for ANSI programs
 *
 * Terminal is set to raw mode so CP/M programs get unbuffered input.
 * Ctrl-\ quits immediately.
 */

#include "cpm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>

/* ===================================================================
 * TERMINAL RAW MODE
 *
 * CP/M expects character-at-a-time console input with no OS-level
 * line editing. We switch the terminal to raw mode and restore it
 * on exit (including abnormal exits via signals).
 * =================================================================== */

static struct termios orig_termios;
static int raw_mode_active = 0;

static void disable_raw_mode(void) {
    if (raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_mode_active = 0;
    }
}

static void enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) < 0) return;

    struct termios raw = orig_termios;
    /* Input: no break, no CR-to-NL, no parity, no strip, no flow control. */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /* Output: keep output processing for \n → \r\n. */
    raw.c_oflag |= OPOST;
    /* Control: 8-bit chars. */
    raw.c_cflag |= CS8;
    /* Local: no echo, no canonical mode, no signals, no ext processing. */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    /* Read returns after 1 byte, no timeout. */
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode_active = 1;
    atexit(disable_raw_mode);
}

/* Signal handler: restore terminal on Ctrl-\ (SIGQUIT). */
static void signal_handler(int sig) {
    disable_raw_mode();
    fprintf(stderr, "\n[cpmcon: signal %d, exiting]\n", sig);
    _exit(128 + sig);
}

/* ===================================================================
 * TERMINAL TRANSLATION LAYER
 *
 * CP/M programs typically emit terminal codes for vintage terminals
 * like the ADM-3A, Kaypro, TeleVideo, or VT52. Modern terminals
 * understand ANSI/VT100 escape sequences. This layer translates
 * vintage terminal codes to ANSI on the fly.
 *
 * Three modes are available:
 *
 *   auto  -- (default) Translates the core ADM-3A/Kaypro codes that
 *            don't conflict with ANSI. Programs that send native ANSI
 *            (ESC [) sequences will have them passed through. This mode
 *            works for both WordStar (Kaypro) and Turbo Pascal (ANSI).
 *
 *   adm3a -- Full translation of ADM-3A, Kaypro, TeleVideo, and VT52
 *            codes. Use this for programs that definitely target these
 *            vintage terminals. Some VT52 ESC codes (e.g., ESC D, ESC H)
 *            conflict with ANSI C1 codes, so this mode may break programs
 *            that send native ANSI escape sequences.
 *
 *   ansi  -- No translation at all. Every byte passes through raw to
 *            the host terminal. Use for programs already configured for
 *            ANSI/VT100 (e.g., Turbo Pascal with ANSI terminal driver).
 *
 * The "auto" and "adm3a" modes both translate:
 *   Single-byte: 0x0B (cursor up), 0x0C (cursor right), 0x17 (clear
 *     to EOS), 0x18 (clear to EOL), 0x1A (clear screen), 0x1E (home)
 *   Multi-byte: ESC = row col (cursor address), ESC B digit / ESC C
 *     digit (Kaypro attributes)
 *
 * The "adm3a" mode additionally translates:
 *   ESC E/R (insert/delete line), ESC T/Y (TeleVideo clear ops),
 *   ESC G digit (TeleVideo attribute), ESC A/D/H/I/J/K (VT52),
 *   ESC p/q/(/) (attribute shortcuts), ESC star/+/: (clear screen)
 * =================================================================== */

/* Terminal emulation mode, selectable via -t flag. */
enum {
    TERM_MODE_AUTO,    /* Safe ADM-3A subset + ANSI pass-through */
    TERM_MODE_ADM3A,   /* Full ADM-3A/Kaypro/TeleVideo/VT52 */
    TERM_MODE_ANSI,    /* Raw pass-through, no translation */
};

static int term_mode = TERM_MODE_AUTO;

/* State machine for multi-byte escape sequences. */
enum {
    TERM_NORMAL,       /* Not in an escape sequence */
    TERM_ESC,          /* Received ESC, waiting for command byte */
    TERM_CUR_ROW,      /* ESC = received, waiting for row byte */
    TERM_CUR_COL,      /* ESC = row received, waiting for col byte */
    TERM_ATTR_ON,      /* ESC B (Kaypro), waiting for attribute digit */
    TERM_ATTR_OFF,     /* ESC C (Kaypro), waiting for attribute digit */
    TERM_TV_ATTR,      /* ESC G (TeleVideo), waiting for attribute */
};

static int term_state = TERM_NORMAL;
static uint8_t term_saved_row;   /* Saved row during cursor addressing */

/* Write a string to stdout (used for ANSI escape sequences). */
static void term_write(const char *s) {
    write(STDOUT_FILENO, s, strlen(s));
}

/* ===================================================================
 * CONSOLE I/O CALLBACKS
 *
 * These connect the CP/M emulator to the host terminal. The con_out
 * callback translates vintage terminal codes to ANSI/VT100 according
 * to the selected terminal mode.
 * =================================================================== */

static void con_out(void *ctx, uint8_t ch) {
    (void)ctx;
    char buf[32];

    /* ANSI mode: no translation at all. */
    if (term_mode == TERM_MODE_ANSI) {
        write(STDOUT_FILENO, &ch, 1);
        return;
    }

    /* ---------------------------------------------------------------
     * Multi-byte escape sequence handling.
     * When we've already received ESC (or ESC + command byte), the
     * state machine routes subsequent bytes here.
     * --------------------------------------------------------------- */
    switch (term_state) {
    case TERM_ESC:
        /* Dispatch on the byte following ESC. */
        term_state = TERM_NORMAL;

        /* --- Codes handled in both auto and adm3a modes ---
         * These are core ADM-3A/Kaypro codes that don't conflict with
         * ANSI sequences (ANSI CSI uses ESC [ not bare ESC + letter). */
        switch (ch) {
        case '=':   /* ADM-3A/Kaypro: ESC = row col (cursor address) */
            term_state = TERM_CUR_ROW;
            return;
        case 'B':   /* Kaypro: ESC B digit (enable attribute) */
            term_state = TERM_ATTR_ON;
            return;
        case 'C':   /* Kaypro: ESC C digit (disable attribute) */
            term_state = TERM_ATTR_OFF;
            return;
        case '[':   /* ANSI CSI -- pass through to host terminal */
            term_write("\033[");
            return;
        }

        /* --- Extended codes: only in full adm3a mode ---
         * These include TeleVideo, VT52, and other codes that may
         * conflict with ANSI C1 codes (ESC + uppercase letter). */
        if (term_mode == TERM_MODE_ADM3A) {
            switch (ch) {
            /* TeleVideo attribute (3-byte) */
            case 'G':   term_state = TERM_TV_ATTR; return;

            /* Line editing */
            case 'E':   term_write("\033[L"); return;  /* Insert line */
            case 'R':   term_write("\033[M"); return;  /* Delete line */

            /* Clear operations */
            case 'T':   /* Clear to end of line */
            case 't':   term_write("\033[K"); return;
            case 'Y':   term_write("\033[J"); return;  /* Clear to EOS */
            case '*':   /* Clear screen + home */
            case '+':
            case ':':   term_write("\033[2J\033[H"); return;

            /* Attribute shortcuts */
            case 'p':   term_write("\033[7m"); return;  /* Reverse on */
            case 'q':   term_write("\033[0m"); return;  /* Reverse off */
            case '(':   term_write("\033[2m"); return;  /* Dim on */
            case ')':   term_write("\033[0m"); return;  /* Dim off */

            /* VT52 cursor/clear (no Kaypro conflict) */
            case 'A':   term_write("\033[A"); return;   /* Cursor up */
            case 'D':   term_write("\033[D"); return;   /* Cursor left */
            case 'H':   term_write("\033[H"); return;   /* Home cursor */
            case 'I':   term_write("\033M");  return;   /* Reverse LF */
            case 'J':   term_write("\033[J"); return;   /* Clear to EOS */
            case 'K':   term_write("\033[K"); return;   /* Clear to EOL */
            }
        }
        /* Unknown or unsupported escape in this mode -- drop. */
        return;

    case TERM_CUR_ROW:
        /* Save row byte, wait for column. */
        term_saved_row = ch;
        term_state = TERM_CUR_COL;
        return;

    case TERM_CUR_COL:
        /* ADM-3A cursor addressing: row and col are offset by 0x20
         * (space character). Row 0 = 0x20, col 0 = 0x20.
         * ANSI uses 1-based coordinates: ESC [ row ; col H */
        {
            int row = (term_saved_row & 0x7F) - 0x20 + 1;
            int col = (ch & 0x7F) - 0x20 + 1;
            if (row < 1) row = 1;
            if (col < 1) col = 1;
            snprintf(buf, sizeof(buf), "\033[%d;%dH", row, col);
            term_write(buf);
        }
        term_state = TERM_NORMAL;
        return;

    case TERM_ATTR_ON:
        /* Kaypro ESC B <digit>: enable display attribute.
         *   '0' = reverse video     → ANSI ESC[7m
         *   '1' = reduced intensity → ANSI ESC[2m (dim)
         *   '2' = blink             → ANSI ESC[5m
         *   '3' = underline         → ANSI ESC[4m */
        term_state = TERM_NORMAL;
        switch (ch) {
        case '0': term_write("\033[7m"); return;
        case '1': term_write("\033[2m"); return;
        case '2': term_write("\033[5m"); return;
        case '3': term_write("\033[4m"); return;
        default:  return;  /* Unknown attribute digit, ignore */
        }

    case TERM_ATTR_OFF:
        /* Kaypro ESC C <digit>: disable display attribute.
         * We reset all attributes since ANSI doesn't reliably support
         * turning off individual attributes on all terminals. */
        term_state = TERM_NORMAL;
        term_write("\033[0m");
        return;

    case TERM_TV_ATTR:
        /* TeleVideo ESC G <attr>: set display attribute.
         *   '0' = normal   '1' = underline   '2' = reverse
         *   '4' = dim      '8' = bold */
        term_state = TERM_NORMAL;
        switch (ch) {
        case '0': term_write("\033[0m"); return;
        case '1': term_write("\033[4m"); return;
        case '2': term_write("\033[7m"); return;
        case '4': term_write("\033[2m"); return;
        case '8': term_write("\033[1m"); return;
        default:  term_write("\033[0m"); return;
        }

    default:
        break;  /* TERM_NORMAL -- fall through to single-byte handling */
    }

    /* ---------------------------------------------------------------
     * Single-byte control code translation (TERM_NORMAL state).
     *
     * ADM-3A uses several ASCII control characters for cursor movement
     * and screen clearing. We translate them to ANSI equivalents.
     * Characters that already work on modern terminals (BEL, BS, TAB,
     * LF, CR) pass through unchanged.
     *
     * These translations are used in both auto and adm3a modes. They
     * don't conflict with ANSI because ANSI programs use ESC [ ...
     * sequences for cursor movement, not bare control characters.
     * --------------------------------------------------------------- */
    switch (ch) {
    case 0x07:  /* BEL -- terminal bell, pass through */
    case 0x08:  /* BS  -- backspace / cursor left, pass through */
    case 0x09:  /* TAB -- horizontal tab, pass through */
    case 0x0A:  /* LF  -- line feed / cursor down, pass through */
    case 0x0D:  /* CR  -- carriage return, pass through */
        write(STDOUT_FILENO, &ch, 1);
        return;

    case 0x0B:  /* VT -- cursor up (ADM-3A) */
        term_write("\033[A");
        return;

    case 0x0C:  /* FF -- cursor right (ADM-3A) */
        term_write("\033[C");
        return;

    case 0x17:  /* ETB -- clear to end of screen (Kaypro) */
        term_write("\033[J");
        return;

    case 0x18:  /* CAN -- clear to end of line (Kaypro) */
        term_write("\033[K");
        return;

    case 0x1A:  /* SUB -- clear screen + home cursor (ADM-3A) */
        term_write("\033[2J\033[H");
        return;

    case 0x1B:  /* ESC -- begin escape sequence */
        term_state = TERM_ESC;
        return;

    case 0x1E:  /* RS -- home cursor (ADM-3A) */
        term_write("\033[H");
        return;

    default:
        /* Printable character or other control -- pass through. */
        write(STDOUT_FILENO, &ch, 1);
        return;
    }
}

/* ---------------------------------------------------------------
 * INPUT TRANSLATION
 *
 * Modern terminals send ANSI escape sequences for arrow keys and
 * other special keys, but CP/M programs expect single-byte codes.
 * The de facto standard for CP/M editors (WordStar, Turbo Pascal,
 * dBASE, etc.) is the "WordStar diamond":
 *
 *   Ctrl-E = up    Ctrl-X = down
 *   Ctrl-S = left  Ctrl-D = right
 *   Ctrl-G = delete char at cursor
 *
 * We translate ANSI arrow sequences to these keys in auto and adm3a
 * modes. In ansi mode, no input translation is performed.
 *
 * We also translate 0x7F (what modern terminals send for Backspace)
 * to 0x08 (what CP/M expects for Backspace).
 *
 * To distinguish a real ESC keypress from ESC-as-sequence-leader,
 * we use a short timeout: arrow key bytes arrive within microseconds,
 * while a human pressing ESC produces an isolated byte.
 * --------------------------------------------------------------- */

/* Input buffer for bytes remaining after partial sequence parsing. */
static uint8_t in_buf[8];
static int in_len = 0, in_pos = 0;

/* Check if stdin has data within the given timeout (microseconds). */
static int input_ready(int timeout_us) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, timeout_us};
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

static int con_in(void *ctx) {
    (void)ctx;

    for (;;) {
        /* Return any buffered bytes from a previous partial sequence. */
        if (in_pos < in_len) return in_buf[in_pos++];
        in_len = in_pos = 0;

        uint8_t ch;
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n <= 0) return -1;

        /* Ctrl-\ to quit (since we disabled ISIG). */
        if (ch == 0x1C) {
            disable_raw_mode();
            fprintf(stderr, "\n[cpmcon: quit]\n");
            exit(0);
        }

        /* No input translation in ANSI mode. */
        if (term_mode == TERM_MODE_ANSI) return ch;

        /* Modern terminals send 0x7F for Backspace. CP/M expects 0x08. */
        if (ch == 0x7F) return 0x08;

        /* Detect ANSI escape sequences and translate arrow keys. */
        if (ch == 0x1B) {
            /* Wait briefly for more bytes (20ms). Arrow key sequences
             * arrive as a burst; a lone ESC keypress is isolated. */
            if (!input_ready(20000)) return 0x1B;  /* Bare ESC */

            uint8_t ch2;
            if (read(STDIN_FILENO, &ch2, 1) != 1) return 0x1B;

            if (ch2 == '[') {
                /* ESC [ -- ANSI CSI sequence. Read the final byte. */
                if (!input_ready(20000)) {
                    /* Incomplete: buffer '[', return ESC. */
                    in_buf[0] = '[';
                    in_len = 1;
                    return 0x1B;
                }

                uint8_t ch3;
                if (read(STDIN_FILENO, &ch3, 1) != 1) {
                    in_buf[0] = '[';
                    in_len = 1;
                    return 0x1B;
                }

                /* Arrow keys → WordStar control keys. */
                switch (ch3) {
                case 'A': return 0x05;  /* Up    → Ctrl-E */
                case 'B': return 0x18;  /* Down  → Ctrl-X */
                case 'C': return 0x04;  /* Right → Ctrl-D */
                case 'D': return 0x13;  /* Left  → Ctrl-S */
                }

                /* Extended CSI sequence (digits, semicolons, then a
                 * final byte 0x40-0x7E). Consume the entire sequence
                 * to prevent garbage reaching the CP/M program. */
                while (ch3 < 0x40 || ch3 > 0x7E) {
                    if (!input_ready(20000)) break;
                    if (read(STDIN_FILENO, &ch3, 1) != 1) break;
                }

                /* Translate the final byte of common extended sequences
                 * that we recognize, discard the rest. */
                continue;  /* Swallow unknown CSI; read next input. */
            }

            if (ch2 == 'O') {
                /* ESC O -- SS3 sequence (some terminals use this for
                 * arrow keys in application mode: ESC O A/B/C/D). */
                if (!input_ready(20000)) {
                    in_buf[0] = 'O';
                    in_len = 1;
                    return 0x1B;
                }
                uint8_t ch3;
                if (read(STDIN_FILENO, &ch3, 1) == 1) {
                    switch (ch3) {
                    case 'A': return 0x05;  /* Up    → Ctrl-E */
                    case 'B': return 0x18;  /* Down  → Ctrl-X */
                    case 'C': return 0x04;  /* Right → Ctrl-D */
                    case 'D': return 0x13;  /* Left  → Ctrl-S */
                    default:
                        in_buf[0] = 'O';
                        in_buf[1] = ch3;
                        in_len = 2;
                        return 0x1B;
                    }
                }
                in_buf[0] = 'O';
                in_len = 1;
                return 0x1B;
            }

            /* ESC + something else: buffer and return ESC. The CP/M
             * program will see the ESC and the next byte separately. */
            in_buf[0] = ch2;
            in_len = 1;
            return 0x1B;
        }

        return ch;
    }
}

static int con_status(void *ctx) {
    (void)ctx;
    /* Report character ready if we have buffered input from a
     * partial escape sequence, or if stdin has data. */
    if (in_pos < in_len) return 1;
    return input_ready(0);
}

/* ===================================================================
 * USAGE AND ARGUMENT PARSING
 * =================================================================== */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] [directory]\n"
        "\n"
        "CP/M 2.2 Emulator Console\n"
        "\n"
        "Options:\n"
        "  -A path   Mount path as drive A: (default: directory or .)\n"
        "  -B path   Mount path as drive B:\n"
        "  -C path   ... through -P for drives C:-P:\n"
        "  -t mode   Terminal translation mode:\n"
        "              auto  - (default) ADM-3A/Kaypro with ANSI pass-through\n"
        "              adm3a - full ADM-3A/Kaypro/TeleVideo/VT52 translation\n"
        "              ansi  - no translation (for VT100/ANSI programs)\n"
        "  -h        Show this help\n"
        "\n"
        "If no -A is given, the first non-option argument (or current\n"
        "directory) is mounted as A:.\n"
        "\n"
        "Keyboard:\n"
        "  Ctrl-\\    Quit immediately\n"
        "  Ctrl-C   Warm boot (return to CCP prompt)\n"
        "\n", prog);
}

/* ===================================================================
 * MAIN
 * =================================================================== */

int main(int argc, char **argv) {
    CPMachine cpm;
    cpm_init(&cpm);

    /* Parse arguments: -A path, -B path, -t mode, etc. */
    int mounted[CPM_MAX_DRIVES] = {0};
    const char *default_dir = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] >= 'A' && argv[i][1] <= 'P'
            && argv[i][2] == '\0') {
            int drive = argv[i][1] - 'A';
            if (i + 1 < argc) {
                cpm_mount(&cpm, drive, argv[++i]);
                mounted[drive] = 1;
            } else {
                fprintf(stderr, "Error: -%c requires a path argument\n",
                        argv[i][1]);
                return 1;
            }
        } else if (strcmp(argv[i], "-t") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -t requires a mode (auto, adm3a, ansi)\n");
                return 1;
            }
            i++;
            if (strcmp(argv[i], "auto") == 0) {
                term_mode = TERM_MODE_AUTO;
            } else if (strcmp(argv[i], "adm3a") == 0) {
                term_mode = TERM_MODE_ADM3A;
            } else if (strcmp(argv[i], "ansi") == 0) {
                term_mode = TERM_MODE_ANSI;
            } else {
                fprintf(stderr, "Error: unknown terminal mode '%s'"
                        " (use auto, adm3a, or ansi)\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            default_dir = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    /* If drive A: wasn't explicitly mounted, use the default directory. */
    if (!mounted[0]) {
        if (!default_dir) default_dir = ".";
        cpm_mount(&cpm, 0, default_dir);
    }

    /* Connect console I/O. */
    cpm.con_out    = con_out;
    cpm.con_in     = con_in;
    cpm.con_status = con_status;
    cpm.con_ctx    = NULL;

    /* Set up signals. */
    signal(SIGQUIT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Switch to raw mode. */
    if (isatty(STDIN_FILENO)) {
        enable_raw_mode();
    }

    /* Print banner. */
    const char *mode_names[] = {"auto", "adm3a", "ansi"};
    fprintf(stderr, "CP/M 2.2 Emulator [%dK TPA, terminal: %s]\r\n",
            (CPM_TPA_TOP - CPM_TPA_BASE) / 1024, mode_names[term_mode]);
    fprintf(stderr, "Ctrl-\\ to quit\r\n");

    /* Run the CCP in a loop. When a program warm-boots, we restart
     * the CCP. The CCP only exits on EOF (which shouldn't happen
     * with a terminal) or Ctrl-\ (handled in con_in). */
    while (1) {
        cpm_ccp(&cpm);
        /* CCP returned -- this happens on Ctrl-C (warm boot from
         * within CCP readline) or EOF. For a terminal, just restart. */
        if (!isatty(STDIN_FILENO)) break;  /* Pipe input: don't loop */
    }

    cpm_cleanup(&cpm);
    return 0;
}

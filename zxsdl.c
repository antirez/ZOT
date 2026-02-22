/* zxsdl.c -- Simple SDL2 frontend for the ZX Spectrum 48K emulator.
 *
 * Usage: zxsdl [file.z80 | file.tzx | file.tap]
 *
 * If no file is given, boots into the Spectrum ROM (BASIC prompt).
 * .z80 snapshots load and run immediately.
 * .tzx/.tap tape files load but don't auto-play -- type LOAD "" then F3.
 *
 * Controls:
 *   Physical keyboard maps to the Spectrum keyboard (see table below).
 *   Arrow keys = Kempston joystick, Tab = fire.
 *   F2 = reset, F3 = play/restart tape, F4 = stop tape,
 *   F5 = toggle 2x/3x scale, F11 = toggle fullscreen.
 *   ESC = quit.
 *
 * Compile:
 *   cc -O2 -o zxsdl zxsdl.c spectrum.c z80.c tzx.c $(sdl2-config --cflags --libs)
 */

#include "spectrum.h"
#include "tzx.h"
#include "rom.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================
 * KEYBOARD MAPPING
 * =================================================================== */

/* Map SDL scancodes to Spectrum keyboard matrix (row, bit).
 * Row = -1 means no mapping. */
typedef struct {
    int row;
    int bit;
} ZXKeyMap;

/* We also need CAPS SHIFT and SYMBOL SHIFT for some keys. */
#define CS_ROW 0
#define CS_BIT 0
#define SS_ROW 7
#define SS_BIT 1

static ZXKeyMap sdl_to_zx(SDL_Scancode sc) {
    ZXKeyMap k = {-1, -1};
    switch (sc) {
        /* Row 0: CAPS SHIFT, Z, X, C, V */
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT:  k.row = 0; k.bit = 0; break;
        case SDL_SCANCODE_Z:       k.row = 0; k.bit = 1; break;
        case SDL_SCANCODE_X:       k.row = 0; k.bit = 2; break;
        case SDL_SCANCODE_C:       k.row = 0; k.bit = 3; break;
        case SDL_SCANCODE_V:       k.row = 0; k.bit = 4; break;

        /* Row 1: A, S, D, F, G */
        case SDL_SCANCODE_A:       k.row = 1; k.bit = 0; break;
        case SDL_SCANCODE_S:       k.row = 1; k.bit = 1; break;
        case SDL_SCANCODE_D:       k.row = 1; k.bit = 2; break;
        case SDL_SCANCODE_F:       k.row = 1; k.bit = 3; break;
        case SDL_SCANCODE_G:       k.row = 1; k.bit = 4; break;

        /* Row 2: Q, W, E, R, T */
        case SDL_SCANCODE_Q:       k.row = 2; k.bit = 0; break;
        case SDL_SCANCODE_W:       k.row = 2; k.bit = 1; break;
        case SDL_SCANCODE_E:       k.row = 2; k.bit = 2; break;
        case SDL_SCANCODE_R:       k.row = 2; k.bit = 3; break;
        case SDL_SCANCODE_T:       k.row = 2; k.bit = 4; break;

        /* Row 3: 1, 2, 3, 4, 5 */
        case SDL_SCANCODE_1:       k.row = 3; k.bit = 0; break;
        case SDL_SCANCODE_2:       k.row = 3; k.bit = 1; break;
        case SDL_SCANCODE_3:       k.row = 3; k.bit = 2; break;
        case SDL_SCANCODE_4:       k.row = 3; k.bit = 3; break;
        case SDL_SCANCODE_5:       k.row = 3; k.bit = 4; break;

        /* Row 4: 0, 9, 8, 7, 6 */
        case SDL_SCANCODE_0:       k.row = 4; k.bit = 0; break;
        case SDL_SCANCODE_9:       k.row = 4; k.bit = 1; break;
        case SDL_SCANCODE_8:       k.row = 4; k.bit = 2; break;
        case SDL_SCANCODE_7:       k.row = 4; k.bit = 3; break;
        case SDL_SCANCODE_6:       k.row = 4; k.bit = 4; break;

        /* Row 5: P, O, I, U, Y */
        case SDL_SCANCODE_P:       k.row = 5; k.bit = 0; break;
        case SDL_SCANCODE_O:       k.row = 5; k.bit = 1; break;
        case SDL_SCANCODE_I:       k.row = 5; k.bit = 2; break;
        case SDL_SCANCODE_U:       k.row = 5; k.bit = 3; break;
        case SDL_SCANCODE_Y:       k.row = 5; k.bit = 4; break;

        /* Row 6: ENTER, L, K, J, H */
        case SDL_SCANCODE_RETURN:  k.row = 6; k.bit = 0; break;
        case SDL_SCANCODE_L:       k.row = 6; k.bit = 1; break;
        case SDL_SCANCODE_K:       k.row = 6; k.bit = 2; break;
        case SDL_SCANCODE_J:       k.row = 6; k.bit = 3; break;
        case SDL_SCANCODE_H:       k.row = 6; k.bit = 4; break;

        /* Row 7: SPACE, SYM SHIFT, M, N, B */
        case SDL_SCANCODE_SPACE:   k.row = 7; k.bit = 0; break;
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL:   k.row = 7; k.bit = 1; break; /* SYM SHIFT */
        case SDL_SCANCODE_M:       k.row = 7; k.bit = 2; break;
        case SDL_SCANCODE_N:       k.row = 7; k.bit = 3; break;
        case SDL_SCANCODE_B:       k.row = 7; k.bit = 4; break;

        /* Backspace = CAPS SHIFT + 0 (DELETE on Spectrum) */
        case SDL_SCANCODE_BACKSPACE: k.row = 4; k.bit = 0; break;

        default: break;
    }
    return k;
}

/* Some PC keys need to press Spectrum key combos. */
static int needs_caps_shift(SDL_Scancode sc) {
    return sc == SDL_SCANCODE_BACKSPACE;
}

/* ===================================================================
 * FILE EXTENSION CHECK
 * =================================================================== */

/* Case-insensitive check if a filename ends with the given suffix. */
static int ends_with_ci(const char *s, const char *suffix) {
    size_t slen = strlen(s);
    size_t xlen = strlen(suffix);
    if (slen < xlen) return 0;
    const char *end = s + slen - xlen;
    for (size_t i = 0; i < xlen; i++) {
        char a = end[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

/* Is this a tape file (.tzx or .tap)? */
static int is_tape_file(const char *path) {
    return ends_with_ci(path, ".tzx") || ends_with_ci(path, ".tap");
}

/* ===================================================================
 * MAIN
 * =================================================================== */

int main(int argc, char **argv) {
    /* Initialize emulator. */
    ZXSpectrum *zx = (ZXSpectrum *)malloc(sizeof(ZXSpectrum));
    zx_init(zx, zx_spectrum_rom);

    /* Tape player state. tape_data is owned by us (must free). */
    TZXPlayer tape;
    memset(&tape, 0, sizeof(tape));
    uint8_t *tape_data = NULL;

    /* Load file if provided. */
    if (argc >= 2 && argv[1][0] != '-') {
        FILE *f = fopen(argv[1], "rb");
        if (!f) {
            fprintf(stderr, "Cannot open %s\n", argv[1]);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *data = (uint8_t *)malloc(fsize);
        fread(data, 1, fsize, f);
        fclose(f);

        if (is_tape_file(argv[1])) {
            /* Load as TZX/TAP tape. Don't free data -- TZXPlayer
             * holds a pointer to it for the duration of playback. */
            if (tzx_load(&tape, data, (int)fsize) != 0) {
                fprintf(stderr, "Failed to load tape: %s\n", argv[1]);
                free(data);
                free(zx);
                return 1;
            }
            tape_data = data;
            printf("Tape loaded: %s (type LOAD \"\" then press F3)\n", argv[1]);
        } else {
            /* Load as .z80 snapshot. */
            if (zx_load_z80(zx, data, (int)fsize) != 0) {
                fprintf(stderr, "Failed to load .z80 snapshot\n");
                free(data);
                free(zx);
                return 1;
            }
            free(data);
            printf("Loaded: %s\n", argv[1]);
        }
    }

    /* Initialize SDL. */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        free(tape_data);
        free(zx);
        return 1;
    }

    int scale = 3;
    SDL_Window *window = SDL_CreateWindow(
        "ZX Spectrum 48K",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        ZX_FB_WIDTH * scale, ZX_FB_HEIGHT * scale,
        SDL_WINDOW_RESIZABLE
    );
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(renderer, ZX_FB_WIDTH, ZX_FB_HEIGHT);

    SDL_Texture *texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
        ZX_FB_WIDTH, ZX_FB_HEIGHT);

    /* Set up audio. */
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = ZX_AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = NULL;  /* Use queue-based audio */

    SDL_AudioDeviceID audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_dev > 0)
        SDL_PauseAudioDevice(audio_dev, 0);

    /* Framebuffer for per-scanline rendering. */
    uint8_t *fb = (uint8_t *)malloc(ZX_FB_WIDTH * ZX_FB_HEIGHT * 3);
    zx_set_framebuffer(zx, fb);

    /* Main loop: paced by audio queue depth.
     * We target keeping ~2 frames worth of audio buffered. If the queue
     * has more than that, we sleep instead of producing another frame.
     * This naturally locks us to 50 Hz (44100 / 882 samples-per-frame). */
    int running = 1;
    int fullscreen = 0;
    const Uint32 audio_frame_bytes = ZX_AUDIO_SAMPLES * sizeof(int16_t);
    const Uint32 audio_target = audio_frame_bytes * 3; /* ~3 frames buffered */

    while (running) {

        /* Process events. */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = 0;
                break;

            case SDL_KEYDOWN:
                if (ev.key.repeat) break;

                switch (ev.key.keysym.scancode) {
                case SDL_SCANCODE_ESCAPE:
                    running = 0;
                    break;
                case SDL_SCANCODE_F2:
                    zx_init(zx, zx_spectrum_rom);
                    zx_set_framebuffer(zx, fb);
                    break;
                case SDL_SCANCODE_F3:
                    /* Play/restart tape from beginning. */
                    if (tape.data) {
                        tzx_play(&tape, zx->cpu.clocks);
                        printf("Tape: playing\n");
                    }
                    break;
                case SDL_SCANCODE_F4:
                    /* Stop tape. */
                    if (tzx_is_playing(&tape)) {
                        tzx_stop(&tape);
                        zx_set_ear(zx, 0);
                        printf("Tape: stopped\n");
                    }
                    break;
                case SDL_SCANCODE_F5:
                    scale = (scale == 3) ? 2 : 3;
                    SDL_SetWindowSize(window,
                        ZX_FB_WIDTH * scale, ZX_FB_HEIGHT * scale);
                    break;
                case SDL_SCANCODE_F11:
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(window,
                        fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    break;
                default: {
                    /* Kempston joystick: arrow keys */
                    switch (ev.key.keysym.scancode) {
                    case SDL_SCANCODE_UP:    zx_joy_down(zx, ZX_JOY_UP); break;
                    case SDL_SCANCODE_DOWN:  zx_joy_down(zx, ZX_JOY_DOWN); break;
                    case SDL_SCANCODE_LEFT:  zx_joy_down(zx, ZX_JOY_LEFT); break;
                    case SDL_SCANCODE_RIGHT: zx_joy_down(zx, ZX_JOY_RIGHT); break;
                    case SDL_SCANCODE_TAB:   zx_joy_down(zx, ZX_JOY_FIRE); break;
                    default: break;
                    }

                    /* Spectrum keyboard */
                    ZXKeyMap k = sdl_to_zx(ev.key.keysym.scancode);
                    if (k.row >= 0) {
                        zx_key_down(zx, k.row, k.bit);
                        if (needs_caps_shift(ev.key.keysym.scancode))
                            zx_key_down(zx, CS_ROW, CS_BIT);
                    }
                    break;
                }
                }
                break;

            case SDL_KEYUP: {
                /* Kempston joystick */
                switch (ev.key.keysym.scancode) {
                case SDL_SCANCODE_UP:    zx_joy_up(zx, ZX_JOY_UP); break;
                case SDL_SCANCODE_DOWN:  zx_joy_up(zx, ZX_JOY_DOWN); break;
                case SDL_SCANCODE_LEFT:  zx_joy_up(zx, ZX_JOY_LEFT); break;
                case SDL_SCANCODE_RIGHT: zx_joy_up(zx, ZX_JOY_RIGHT); break;
                case SDL_SCANCODE_TAB:   zx_joy_up(zx, ZX_JOY_FIRE); break;
                default: break;
                }

                /* Spectrum keyboard */
                ZXKeyMap k = sdl_to_zx(ev.key.keysym.scancode);
                if (k.row >= 0) {
                    zx_key_up(zx, k.row, k.bit);
                    if (needs_caps_shift(ev.key.keysym.scancode)) {
                        const Uint8 *state = SDL_GetKeyboardState(NULL);
                        if (!state[SDL_SCANCODE_LSHIFT] && !state[SDL_SCANCODE_RSHIFT])
                            zx_key_up(zx, CS_ROW, CS_BIT);
                    }
                }
                break;
            }

            case SDL_DROPFILE: {
                /* Drag and drop file onto window. */
                char *path = ev.drop.file;
                FILE *f = fopen(path, "rb");
                if (f) {
                    fseek(f, 0, SEEK_END);
                    long fsize = ftell(f);
                    fseek(f, 0, SEEK_SET);
                    uint8_t *data = (uint8_t *)malloc(fsize);
                    fread(data, 1, fsize, f);
                    fclose(f);

                    if (is_tape_file(path)) {
                        /* Load tape file. Reset Spectrum so user can
                         * type LOAD "" from a clean state. */
                        zx_init(zx, zx_spectrum_rom);
                        zx_set_framebuffer(zx, fb);
                        tzx_stop(&tape);
                        if (tape_data) free(tape_data);
                        tape_data = NULL;

                        if (tzx_load(&tape, data, (int)fsize) == 0) {
                            tape_data = data;
                            printf("Tape loaded: %s (type LOAD \"\" then press F3)\n", path);
                        } else {
                            fprintf(stderr, "Failed to load tape: %s\n", path);
                            free(data);
                        }
                    } else {
                        /* Load .z80 snapshot. */
                        zx_init(zx, zx_spectrum_rom);
                        zx_set_framebuffer(zx, fb);
                        if (zx_load_z80(zx, data, (int)fsize) == 0)
                            printf("Loaded: %s\n", path);
                        else
                            fprintf(stderr, "Failed to load: %s\n", path);
                        free(data);
                    }
                }
                SDL_free(path);
                break;
            }
            }
        }

        /* Wait if the audio queue is full enough -- this paces us to 50 Hz. */
        if (audio_dev > 0) {
            while (SDL_GetQueuedAudioSize(audio_dev) > audio_target) {
                SDL_Delay(1);
            }
        }

        /* Run one emulation frame. When tape is playing, we use the
         * per-instruction loop so we can update EAR between every
         * Z80 instruction -- the ROM tape loader samples EAR timing
         * very precisely. When tape is idle, zx_frame() is simpler. */
        if (tzx_is_playing(&tape)) {
            do {
                zx_set_ear(zx, tzx_update(&tape, zx->cpu.clocks));
            } while (!zx_tick(zx, 0));
            /* Detect end-of-tape. */
            if (!tzx_is_playing(&tape))
                printf("Tape: end of tape\n");
        } else {
            zx_frame(zx);
        }

        /* Update texture from framebuffer and present. */
        SDL_UpdateTexture(texture, NULL, fb, ZX_FB_WIDTH * 3);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        /* Queue audio. */
        if (audio_dev > 0) {
            SDL_QueueAudio(audio_dev, zx->audio_buffer, audio_frame_bytes);
        }
    }

    /* Cleanup. */
    if (audio_dev > 0) SDL_CloseAudioDevice(audio_dev);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    free(fb);
    if (tape_data) free(tape_data);
    free(zx);
    return 0;
}

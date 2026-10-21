/*
 * S5LWinBox — Main Entry Point and SDL2 Frontend.
 *
 * Provides the interactive desktop window, touchscreen mouse emulation,
 * hardware button keys, stereo audio playback, IPSW/IPA drag-and-drop,
 * and live inspector web logger.
 *
 * MIT licensed.
 */

#include "engine.h"
#include "ui.h"
#include "platform_compat.h"
#include "web_log_server.h"
#include "VMButtonQueue.h"

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#define DEFAULT_WINDOW_WIDTH  640
#define DEFAULT_WINDOW_HEIGHT (960 + UI_TOOLBAR_HEIGHT)
#define GUEST_SCREEN_WIDTH    320
#define GUEST_SCREEN_HEIGHT   480
#define AUDIO_SAMPLE_RATE     48000
#define AUDIO_BUFFER_SAMPLES  1024

static engine_t *g_engine = NULL;
static SDL_AudioDeviceID g_audio_dev = 0;
static bool g_mouse_down = false;

/* Audio callback: feed guest stereo PCM samples to SDL2 */
static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    engine_t *engine = (engine_t *)userdata;
    int16_t *dst = (int16_t *)stream;
    size_t max_samples = (size_t)len / sizeof(int16_t);

    if (!engine) {
        memset(stream, 0, (size_t)len);
        return;
    }

    size_t read_samples = engine_read_audio_pcm(engine, dst, max_samples);
    if (read_samples < max_samples) {
        memset(dst + read_samples, 0, (max_samples - read_samples) * sizeof(int16_t));
    }
}

static void compute_viewport(int win_w, int win_h, SDL_Rect *out_rect, float *out_scale)
{
    float scale_x = (float)win_w / (float)GUEST_SCREEN_WIDTH;
    float scale_y = (float)win_h / (float)GUEST_SCREEN_HEIGHT;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    if (scale < 0.1f) scale = 0.1f;

    int vp_w = (int)((float)GUEST_SCREEN_WIDTH * scale);
    int vp_h = (int)((float)GUEST_SCREEN_HEIGHT * scale);
    int vp_x = (win_w - vp_w) / 2;
    int vp_y = (win_h - vp_h) / 2;

    if (out_rect) {
        out_rect->x = vp_x;
        out_rect->y = vp_y;
        out_rect->w = vp_w;
        out_rect->h = vp_h;
    }
    if (out_scale) {
        *out_scale = scale;
    }
}

static void get_default_data_dir(char *out, size_t cap)
{
#if defined(_WIN32)
    const char *appdata = getenv("LOCALAPPDATA");
    if (appdata && appdata[0]) {
        snprintf(out, cap, "%s\\S5LWinBox", appdata);
        return;
    }
#endif
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(out, cap, "%s/.s5lwinbox", home);
        return;
    }
    snprintf(out, cap, "./s5lwinbox_data");
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    char root_dir[512];
    get_default_data_dir(root_dir, sizeof root_dir);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) {
            snprintf(root_dir, sizeof root_dir, "%s", argv[++i]);
        }
    }

    printf("====================================================\n");
    printf(" S5LWinBox — iPhone OS 3 Emulation Environment\n");
    printf(" Storage Root: %s\n", root_dir);
    printf("====================================================\n");

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "Failed to initialize SDL2: %s\n", SDL_GetError());
        return 1;
    }

    /* Create engine */
    g_engine = engine_create(root_dir);
    if (!g_engine) {
        fprintf(stderr, "Failed to create emulation engine\n");
        SDL_Quit();
        return 1;
    }
    web_log_server_set_engine(g_engine);

    /* Start Web Inspector log server */
    uint16_t log_port = engine_get_web_log_port(g_engine);
    if (log_port == 0) {
        log_port = web_log_server_start(8080);
    }
    printf("Web Log Inspector: http://localhost:%u\n", log_port);

    /* Open SDL Audio */
    SDL_AudioSpec wanted, obtained;
    SDL_zero(wanted);
    wanted.freq = AUDIO_SAMPLE_RATE;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2;
    wanted.samples = AUDIO_BUFFER_SAMPLES;
    wanted.callback = audio_callback;
    wanted.userdata = g_engine;

    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, 0);
    if (g_audio_dev != 0) {
        SDL_PauseAudioDevice(g_audio_dev, 0);
    } else {
        printf("Audio output warning: %s\n", SDL_GetError());
    }

    /* Create Window & Renderer */
    SDL_Window *window = SDL_CreateWindow(
        "S5LWinBox — iPhone OS 3",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        DEFAULT_WINDOW_WIDTH, DEFAULT_WINDOW_HEIGHT,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );

    if (!window) {
        fprintf(stderr, "Failed to create SDL window: %s\n", SDL_GetError());
        engine_destroy(g_engine);
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(
        window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }

    SDL_Texture *texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING,
        GUEST_SCREEN_WIDTH, GUEST_SCREEN_HEIGHT
    );

    /* Resolve or create default machine */
    char current_machine[64] = {0};
    if (engine_get_or_create_default_machine(g_engine, current_machine, sizeof current_machine) == 0) {
        printf("Selected machine ID: %s\n", current_machine);
        char start_err[256] = {0};
        int start_rc = engine_start(g_engine, current_machine, start_err, sizeof start_err);
        if (start_rc == 0) {
            printf("Engine started for machine: %s\n", current_machine);
        } else {
            printf("Engine start note: %s\n", start_err[0] ? start_err : "waiting for firmware");
        }
    } else {
        fprintf(stderr, "Failed to resolve or create machine\n");
    }

    uint32_t frame_buffer[GUEST_SCREEN_WIDTH * GUEST_SCREEN_HEIGHT];
    memset(frame_buffer, 0, sizeof frame_buffer);

    char fw_json[512] = {0};
    engine_firmware_json(g_engine, fw_json, sizeof fw_json);
    bool fw_ready = (strstr(fw_json, "\"complete\":true") != NULL);
    if (fw_ready) {
        printf("\n=== iPhone OS 3.1.3 is running! ===\n");
        printf("Controls: Mouse/Click = Touchscreen, Esc = Home, F1 = Power, F2/F3 = Vol -/+, F5 = Mute, F11 = Fullscreen, Space = Pause\n");
        printf("Note: If the screen is black or asleep, press Esc (Home) or F1 (Power) to wake up the screen.\n\n");
    } else {
        printf("\n>>> Drag and drop an iPhone OS 3.1.3 IPSW file into this window to install firmware! <<<\n");
        printf("Hotkeys: Esc=Home, F1=Power, F2/F3=Vol-/+, F5=Mute, F11=Fullscreen, Space=Pause\n\n");
    }

    ui_state_t g_ui;
    ui_init(&g_ui);

    bool running = true;
    bool fullscreen = false;
    uint32_t last_title_update = SDL_GetTicks();

    while (running) {
        SDL_Event ev;
        int win_w, win_h;
        SDL_GetWindowSize(window, &win_w, &win_h);

        ui_layout(&g_ui, win_w, win_h);

        while (SDL_PollEvent(&ev)) {
            if (ui_handle_event(&g_ui, g_engine, &ev, window, &running, &fullscreen)) {
                continue;
            }

            switch (ev.type) {
            case SDL_QUIT:
                running = false;
                break;

            case SDL_DROPFILE: {
                char *dropped_file = ev.drop.file;
                if (dropped_file) {
                    size_t flen = strlen(dropped_file);
                    char detail[512] = {0};
                    if (flen > 5 && !_stricmp(dropped_file + flen - 5, ".ipsw")) {
                        printf("Importing IPSW: %s\n", dropped_file);
                        engine_stop(g_engine, ENGINE_STOP_PLAIN);
                        int rc = engine_import_ipsw(g_engine, dropped_file, false, detail, sizeof detail);
                        printf("IPSW import %s: %s\n", rc == 0 ? "succeeded" : "failed", detail);
                        if (rc == 0) {
                            if (!current_machine[0]) {
                                engine_get_or_create_default_machine(g_engine, current_machine, sizeof current_machine);
                            }
                            char start_err[256] = {0};
                            int s_rc = engine_start(g_engine, current_machine, start_err, sizeof start_err);
                            printf("Engine start after IPSW import (%s): rc=%d %s\n", current_machine, s_rc, start_err);
                        }
                    } else if (flen > 4 && !_stricmp(dropped_file + flen - 4, ".ipa")) {
                        if (!current_machine[0]) {
                            engine_get_or_create_default_machine(g_engine, current_machine, sizeof current_machine);
                        }
                        printf("Installing IPA: %s into %s\n", dropped_file, current_machine);
                        char cache_dir[512];
                        snprintf(cache_dir, sizeof cache_dir, "%s/cache", root_dir);
                        engine_stop(g_engine, ENGINE_STOP_PLAIN);
                        int rc = engine_machine_install_ipa(g_engine, current_machine, cache_dir, dropped_file, detail, sizeof detail);
                        printf("IPA install %s: %s\n", rc == 0 ? "succeeded" : "failed", detail);
                        if (rc == 0) {
                            char start_err[256] = {0};
                            engine_start(g_engine, current_machine, start_err, sizeof start_err);
                        }
                    }
                    SDL_free(dropped_file);
                }
                break;
            }

            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    g_mouse_down = true;
                    double gx = (double)(ev.button.x - g_ui.screen_rect.x) / (double)g_ui.screen_scale;
                    double gy = (double)(ev.button.y - g_ui.screen_rect.y) / (double)g_ui.screen_scale;
                    if (gx >= 0.0 && gx < (double)GUEST_SCREEN_WIDTH &&
                        gy >= 0.0 && gy < (double)GUEST_SCREEN_HEIGHT) {
                        engine_touch(g_engine, gx, gy, (double)GUEST_SCREEN_WIDTH, (double)GUEST_SCREEN_HEIGHT, 0 /* BEGAN */);
                    }
                }
                break;

            case SDL_MOUSEMOTION:
                if (g_mouse_down) {
                    double gx = (double)(ev.motion.x - g_ui.screen_rect.x) / (double)g_ui.screen_scale;
                    double gy = (double)(ev.motion.y - g_ui.screen_rect.y) / (double)g_ui.screen_scale;
                    if (gx < 0.0) gx = 0.0;
                    if (gx >= (double)GUEST_SCREEN_WIDTH) gx = (double)(GUEST_SCREEN_WIDTH - 1);
                    if (gy < 0.0) gy = 0.0;
                    if (gy >= (double)GUEST_SCREEN_HEIGHT) gy = (double)(GUEST_SCREEN_HEIGHT - 1);
                    engine_touch(g_engine, gx, gy, (double)GUEST_SCREEN_WIDTH, (double)GUEST_SCREEN_HEIGHT, 1 /* MOVED */);
                }
                break;

            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT && g_mouse_down) {
                    g_mouse_down = false;
                    double gx = (double)(ev.button.x - g_ui.screen_rect.x) / (double)g_ui.screen_scale;
                    double gy = (double)(ev.button.y - g_ui.screen_rect.y) / (double)g_ui.screen_scale;
                    if (gx < 0.0) gx = 0.0;
                    if (gx >= (double)GUEST_SCREEN_WIDTH) gx = (double)(GUEST_SCREEN_WIDTH - 1);
                    if (gy < 0.0) gy = 0.0;
                    if (gy >= (double)GUEST_SCREEN_HEIGHT) gy = (double)(GUEST_SCREEN_HEIGHT - 1);
                    engine_touch(g_engine, gx, gy, (double)GUEST_SCREEN_WIDTH, (double)GUEST_SCREEN_HEIGHT, 2 /* ENDED */);
                }
                break;

            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                bool pressed = (ev.type == SDL_KEYDOWN);
                switch (ev.key.keysym.scancode) {
                case SDL_SCANCODE_ESCAPE:
                case SDL_SCANCODE_AC_HOME:
                    engine_button(g_engine, VM_BUTTON_HOME, pressed);
                    break;
                case SDL_SCANCODE_F1:
                    engine_button(g_engine, VM_BUTTON_POWER, pressed);
                    break;
                case SDL_SCANCODE_F2:
                    engine_button(g_engine, VM_BUTTON_VOLUME_DOWN, pressed);
                    break;
                case SDL_SCANCODE_F3:
                    engine_button(g_engine, VM_BUTTON_VOLUME_UP, pressed);
                    break;
                case SDL_SCANCODE_F5:
                    if (pressed) {
                        static bool s_ringer_silent = false;
                        s_ringer_silent = !s_ringer_silent;
                        g_ui.ringer_silent = s_ringer_silent;
                        engine_button(g_engine, VM_BUTTON_RINGER_SILENT, s_ringer_silent);
                    }
                    break;
                case SDL_SCANCODE_F11:
                    if (pressed) {
                        fullscreen = !fullscreen;
                        SDL_SetWindowFullscreen(window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    }
                    break;
                case SDL_SCANCODE_SPACE:
                    if (pressed) {
                        static bool s_paused = false;
                        s_paused = !s_paused;
                        g_ui.paused = s_paused;
                        engine_set_paused(g_engine, s_paused);
                    }
                    break;
                default:
                    break;
                }
                break;
            }

            default:
                break;
            }
        }

        /* Drain engine console output to terminal stdout */
        char console_buf[1024];
        size_t c_drained = engine_console_drain(g_engine, console_buf, sizeof console_buf - 1);
        if (c_drained > 0) {
            console_buf[c_drained] = '\0';
            fputs(console_buf, stdout);
            fflush(stdout);
        }

        /* Check for frame update */
        uint32_t fw = 0, fh = 0;
        if (engine_copy_frame(g_engine, frame_buffer, sizeof frame_buffer, &fw, &fh)) {
            if (fw > 0 && fh > 0) {
                SDL_UpdateTexture(texture, NULL, frame_buffer, (int)(fw * sizeof(uint32_t)));
            }
        }

        /* Render */
        SDL_SetRenderDrawColor(renderer, 10, 12, 16, 255);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, &g_ui.screen_rect);
        ui_render(&g_ui, g_engine, renderer, win_w, win_h);
        SDL_RenderPresent(renderer);

        /* Status update twice per second */
        uint32_t now = SDL_GetTicks();
        if (now - last_title_update >= 500) {
            last_title_update = now;
            char status_json[512] = {0};
            engine_status_json(g_engine, status_json, sizeof status_json);

            char title[384];
            const char *fps_ptr = strstr(status_json, "\"fps\":");
            const char *rate_ptr = strstr(status_json, "\"rate\":");
            const char *ready_ptr = strstr(status_json, "\"machineReady\":true");
            const char *prep_ptr = strstr(status_json, "\"preparing\":true");
            const char *pdone_ptr = strstr(status_json, "\"prepareDone\":");
            const char *ptot_ptr = strstr(status_json, "\"prepareTotal\":");
            const char *ret_ptr = strstr(status_json, "\"retired\":");

            uint64_t retired = ret_ptr ? strtoull(ret_ptr + 10, NULL, 10) : 0;
            char ret_str[32];
            if (retired >= 1000000000ULL) {
                snprintf(ret_str, sizeof ret_str, "%.2fG", (double)retired / 1e9);
            } else if (retired >= 1000000ULL) {
                snprintf(ret_str, sizeof ret_str, "%.1fM", (double)retired / 1e6);
            } else {
                snprintf(ret_str, sizeof ret_str, "%llu", (unsigned long long)retired);
            }

            if (prep_ptr) {
                uint64_t done = pdone_ptr ? strtoull(pdone_ptr + 14, NULL, 10) : 0;
                uint64_t total = ptot_ptr ? strtoull(ptot_ptr + 15, NULL, 10) : 0;
                unsigned pct = (total > 0) ? (unsigned)(done * 100ULL / total) : 0;
                snprintf(title, sizeof title, "S5LWinBox — [Подготовка файловой системы: %u%%] | Inspector: http://localhost:%u", pct, log_port);
            } else if (ready_ptr) {
                double cur_fps = fps_ptr ? atof(fps_ptr + 6) : 0.0;
                double cur_mips = rate_ptr ? atof(rate_ptr + 7) : 0.0;
                if (cur_mips < 0.05) {
                    snprintf(title, sizeof title, "S5LWinBox — [Сон / Standby: нажмите Esc или F1] | Инструкций: %s | Inspector: http://localhost:%u", ret_str, log_port);
                } else {
                    snprintf(title, sizeof title, "S5LWinBox — [FPS: %.1f | MIPS: %.1f | Инструкций: %s] | Inspector: http://localhost:%u", cur_fps, cur_mips, ret_str, log_port);
                }
            } else {
                snprintf(title, sizeof title, "S5LWinBox — [Ожидание IPSW: перетащите .ipsw в окно] | Inspector: http://localhost:%u", log_port);
            }
            SDL_SetWindowTitle(window, title);
        }

        SDL_Delay(8); /* ~120 Hz event poll rate */
    }

    printf("Shutting down emulation...\n");
    if (g_audio_dev != 0) {
        SDL_CloseAudioDevice(g_audio_dev);
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    engine_stop(g_engine, ENGINE_STOP_CHECKPOINT);
    engine_destroy(g_engine);
    SDL_Quit();
    return 0;
}

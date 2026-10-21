/*
 * S5LWinBox / S5LPCBox — UI Frontend, Buttons Toolbar, and Machine Manager.
 *
 * Provides on-screen physical button emulation (Home, Power, Vol-, Vol+, Mute),
 * quick action controls (Pause, Reboot, Fullscreen), and an in-app
 * Machine Manager dialog.
 *
 * MIT licensed.
 */

#ifndef S5L_UI_H
#define S5L_UI_H

#include "engine.h"
#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_TOOLBAR_HEIGHT 54

typedef enum {
    UI_BTN_NONE = 0,
    UI_BTN_HOME,
    UI_BTN_POWER,
    UI_BTN_VOL_DOWN,
    UI_BTN_VOL_UP,
    UI_BTN_MUTE,
    UI_BTN_PAUSE,
    UI_BTN_RESET,
    UI_BTN_MACHINES,
    UI_BTN_FULLSCREEN
} ui_btn_id_t;

typedef struct {
    char id[64];
    char name[64];
    unsigned disk_gib;
    uint64_t retired;
    bool ready;
    bool running;
} ui_machine_item_t;

typedef struct {
    /* Machine Manager dialog state */
    bool mm_open;
    ui_machine_item_t machines[32];
    unsigned machine_count;
    int selected_machine;
    char running_id[64];

    /* Sub-dialogs */
    bool create_dialog_open;
    char new_machine_name[64];
    unsigned new_machine_disk_gib;

    bool rename_dialog_open;
    char rename_machine_name[64];

    /* Transient toast message */
    char toast[128];
    uint32_t toast_until;

    /* Toolbar button interaction states */
    ui_btn_id_t hovered_btn;
    ui_btn_id_t pressed_btn;
    bool ringer_silent;
    bool paused;

    /* Layout rects */
    SDL_Rect screen_rect;
    SDL_Rect toolbar_rect;
    float screen_scale;
} ui_state_t;

void ui_init(ui_state_t *ui);
void ui_refresh_machines(ui_state_t *ui, engine_t *engine);

/* Calculate layout rects based on current window size */
void ui_layout(ui_state_t *ui, int win_w, int win_h);

/* Handle SDL event. Returns true if consumed by UI */
bool ui_handle_event(ui_state_t *ui, engine_t *engine, const SDL_Event *ev,
                     SDL_Window *win, bool *out_running, bool *out_fullscreen);

/* Render Toolbar & Machine Manager overlay */
void ui_render(ui_state_t *ui, engine_t *engine, SDL_Renderer *renderer, int win_w, int win_h);

/* Display temporary toast banner */
void ui_show_toast(ui_state_t *ui, const char *msg, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif /* S5L_UI_H */

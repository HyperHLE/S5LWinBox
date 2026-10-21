/*
 * S5LWinBox / S5LPCBox — UI Frontend, Buttons Toolbar, and Machine Manager.
 *
 * MIT licensed.
 */

#include "ui.h"
#include "ui_font.h"
#include "VMButtonQueue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define GUEST_SCREEN_WIDTH  320
#define GUEST_SCREEN_HEIGHT 480

/* Color palette */
static const SDL_Color COLOR_BG            = { 13, 17, 23, 255 };
static const SDL_Color COLOR_TOOLBAR_BG    = { 22, 27, 34, 255 };
static const SDL_Color COLOR_BORDER        = { 48, 54, 61, 255 };
static const SDL_Color COLOR_BTN_NORMAL    = { 33, 38, 45, 255 };
static const SDL_Color COLOR_BTN_HOVER     = { 48, 54, 61, 255 };
static const SDL_Color COLOR_BTN_PRESSED   = { 31, 111, 235, 255 };
static const SDL_Color COLOR_ACCENT        = { 31, 111, 235, 255 };
static const SDL_Color COLOR_TEXT_WHITE    = { 240, 246, 252, 255 };
static const SDL_Color COLOR_TEXT_DIM      = { 139, 148, 158, 255 };
static const SDL_Color COLOR_TEXT_MUTED    = { 100, 108, 118, 255 };
static const SDL_Color COLOR_GREEN         = { 46, 160, 67, 255 };
static const SDL_Color COLOR_RED           = { 248, 81, 73, 255 };
static const SDL_Color COLOR_YELLOW        = { 210, 153, 34, 255 };

typedef struct {
    ui_btn_id_t id;
    const char *label;
    const char *sub;
    int width;
    SDL_Rect rect;
} tb_button_t;

static tb_button_t s_tb_buttons[] = {
    { UI_BTN_HOME,       "HOME",  "Esc",   58, {0,0,0,0} },
    { UI_BTN_POWER,      "POWER", "F1",    62, {0,0,0,0} },
    { UI_BTN_VOL_DOWN,   "VOL -", "F2",    56, {0,0,0,0} },
    { UI_BTN_VOL_UP,     "VOL +", "F3",    56, {0,0,0,0} },
    { UI_BTN_MUTE,       "MUTE",  "F5",    56, {0,0,0,0} },
    { UI_BTN_PAUSE,      "PAUSE", "Space", 62, {0,0,0,0} },
    { UI_BTN_RESET,      "RESET", "F6",    58, {0,0,0,0} },
    { UI_BTN_MACHINES,   "MACHINES", "F4", 90, {0,0,0,0} },
    { UI_BTN_FULLSCREEN, "FULL",  "F11",   54, {0,0,0,0} },
};
static const size_t s_tb_btn_count = sizeof(s_tb_buttons) / sizeof(s_tb_buttons[0]);

/* ------------------------------------------------------------- Font & Drawing */

static void ui_draw_char(SDL_Renderer *r, int x, int y, char c, int scale, SDL_Color color)
{
    uint8_t uc = (uint8_t)c;
    if (uc > 127) uc = '?';
    const uint8_t *bitmap = font8x8_basic[uc];

    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
    for (int row = 0; row < 8; row++) {
        uint8_t line = bitmap[row];
        for (int col = 0; col < 8; col++) {
            if (line & (1 << col)) {
                if (scale == 1) {
                    SDL_RenderDrawPoint(r, x + col, y + row);
                } else {
                    SDL_Rect pixel = { x + col * scale, y + row * scale, scale, scale };
                    SDL_RenderFillRect(r, &pixel);
                }
            }
        }
    }
}

static void ui_draw_text(SDL_Renderer *r, int x, int y, const char *str, int scale, SDL_Color color)
{
    if (!str) return;
    int cx = x;
    while (*str) {
        if (*str == '\n') {
            y += 9 * scale;
            cx = x;
        } else {
            ui_draw_char(r, cx, y, *str, scale, color);
            cx += 8 * scale;
        }
        str++;
    }
}

static void ui_draw_text_centered(SDL_Renderer *r, const SDL_Rect *bounds, const char *str, int scale, SDL_Color color)
{
    if (!str || !bounds) return;
    size_t len = strlen(str);
    int tw = (int)len * 8 * scale;
    int th = 8 * scale;
    int x = bounds->x + (bounds->w - tw) / 2;
    int y = bounds->y + (bounds->h - th) / 2;
    ui_draw_text(r, x, y, str, scale, color);
}

static void ui_draw_rect_filled(SDL_Renderer *r, const SDL_Rect *rect, SDL_Color color)
{
    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(r, rect);
}

static void ui_draw_rect_outline(SDL_Renderer *r, const SDL_Rect *rect, SDL_Color color)
{
    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
    SDL_RenderDrawRect(r, rect);
}

static void ui_draw_button(SDL_Renderer *r, const SDL_Rect *rect, const char *label, const char *sub,
                           bool hover, bool pressed, bool active, SDL_Color accent_color)
{
    SDL_Color fill = COLOR_BTN_NORMAL;
    SDL_Color border = COLOR_BORDER;
    SDL_Color text_col = COLOR_TEXT_WHITE;

    if (pressed) {
        fill = COLOR_BTN_PRESSED;
        border = COLOR_TEXT_WHITE;
    } else if (active) {
        fill = accent_color;
        border = COLOR_TEXT_WHITE;
    } else if (hover) {
        fill = COLOR_BTN_HOVER;
        border = COLOR_TEXT_DIM;
    }

    ui_draw_rect_filled(r, rect, fill);
    ui_draw_rect_outline(r, rect, border);

    if (sub && sub[0]) {
        /* Button with primary label and small hotkey subtext */
        SDL_Rect top_rect = { rect->x, rect->y + 5, rect->w, 14 };
        SDL_Rect sub_rect = { rect->x, rect->y + 22, rect->w, 12 };
        ui_draw_text_centered(r, &top_rect, label, 1, text_col);
        ui_draw_text_centered(r, &sub_rect, sub, 1, COLOR_TEXT_DIM);
    } else {
        ui_draw_text_centered(r, rect, label, 1, text_col);
    }
}

/* ------------------------------------------------------------- Initialization */

void ui_init(ui_state_t *ui)
{
    if (!ui) return;
    memset(ui, 0, sizeof(*ui));
    ui->selected_machine = 0;
    ui->new_machine_disk_gib = 8;
    snprintf(ui->new_machine_name, sizeof(ui->new_machine_name), "iPhone 3G");
}

void ui_show_toast(ui_state_t *ui, const char *msg, uint32_t duration_ms)
{
    if (!ui || !msg) return;
    snprintf(ui->toast, sizeof(ui->toast), "%s", msg);
    ui->toast_until = SDL_GetTicks() + duration_ms;
}

/* ------------------------------------------------------------- Machine Refresh */

void ui_refresh_machines(ui_state_t *ui, engine_t *engine)
{
    if (!ui || !engine) return;
    char json_buf[4096] = {0};
    int len = engine_machines_json(engine, json_buf, sizeof(json_buf));
    if (len <= 0) return;

    /* Extract runningId */
    ui->running_id[0] = '\0';
    const char *r_ptr = strstr(json_buf, "\"runningId\":");
    if (r_ptr) {
        r_ptr += 12;
        while (*r_ptr == ' ' || *r_ptr == '"') r_ptr++;
        char *end_q = strchr(r_ptr, '"');
        if (end_q) {
            size_t id_len = (size_t)(end_q - r_ptr);
            if (id_len < sizeof(ui->running_id)) {
                memcpy(ui->running_id, r_ptr, id_len);
                ui->running_id[id_len] = '\0';
            }
        }
    }

    /* Parse machines array */
    ui->machine_count = 0;
    const char *m_ptr = strstr(json_buf, "\"machines\":[");
    if (!m_ptr) return;
    m_ptr += 12;

    while (*m_ptr && *m_ptr != ']' && ui->machine_count < 32) {
        const char *obj = strchr(m_ptr, '{');
        if (!obj) break;
        const char *obj_end = strchr(obj, '}');
        if (!obj_end) break;

        ui_machine_item_t *item = &ui->machines[ui->machine_count];
        memset(item, 0, sizeof(*item));

        /* ID */
        const char *id_key = strstr(obj, "\"id\":");
        if (id_key && id_key < obj_end) {
            id_key += 5;
            while (*id_key == ' ' || *id_key == '"') id_key++;
            char *q = strchr(id_key, '"');
            if (q && q < obj_end) {
                size_t l = (size_t)(q - id_key);
                if (l < sizeof(item->id)) {
                    memcpy(item->id, id_key, l);
                    item->id[l] = '\0';
                }
            }
        }

        /* Name */
        const char *n_key = strstr(obj, "\"name\":");
        if (n_key && n_key < obj_end) {
            n_key += 7;
            while (*n_key == ' ' || *n_key == '"') n_key++;
            char *q = strchr(n_key, '"');
            if (q && q < obj_end) {
                size_t l = (size_t)(q - n_key);
                if (l < sizeof(item->name)) {
                    memcpy(item->name, n_key, l);
                    item->name[l] = '\0';
                }
            }
        }

        /* DiskGiB */
        const char *d_key = strstr(obj, "\"diskGiB\":");
        if (d_key && d_key < obj_end) {
            item->disk_gib = (unsigned)strtoul(d_key + 10, NULL, 10);
        } else {
            item->disk_gib = 8;
        }

        /* Retired total */
        const char *ret_key = strstr(obj, "\"retiredTotal\":");
        if (ret_key && ret_key < obj_end) {
            item->retired = strtoull(ret_key + 15, NULL, 10);
        }

        /* Ready */
        const char *rdy_key = strstr(obj, "\"ready\":true");
        item->ready = (rdy_key && rdy_key < obj_end);

        /* Running state */
        item->running = (ui->running_id[0] && strcmp(item->id, ui->running_id) == 0);

        ui->machine_count++;
        m_ptr = obj_end + 1;
    }

    if (ui->selected_machine >= (int)ui->machine_count) {
        ui->selected_machine = ui->machine_count > 0 ? 0 : -1;
    }
}

/* ------------------------------------------------------------- Layout */

void ui_layout(ui_state_t *ui, int win_w, int win_h)
{
    if (!ui) return;

    /* Toolbar sits at the bottom */
    ui->toolbar_rect.x = 0;
    ui->toolbar_rect.y = win_h - UI_TOOLBAR_HEIGHT;
    ui->toolbar_rect.w = win_w;
    ui->toolbar_rect.h = UI_TOOLBAR_HEIGHT;

    /* Guest screen centered above toolbar */
    int avail_w = win_w;
    int avail_h = win_h - UI_TOOLBAR_HEIGHT;
    if (avail_h < 100) avail_h = 100;

    float scale_x = (float)avail_w / (float)GUEST_SCREEN_WIDTH;
    float scale_y = (float)avail_h / (float)GUEST_SCREEN_HEIGHT;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    if (scale < 0.1f) scale = 0.1f;

    int vp_w = (int)((float)GUEST_SCREEN_WIDTH * scale);
    int vp_h = (int)((float)GUEST_SCREEN_HEIGHT * scale);
    int vp_x = (avail_w - vp_w) / 2;
    int vp_y = (avail_h - vp_h) / 2;

    ui->screen_rect.x = vp_x;
    ui->screen_rect.y = vp_y;
    ui->screen_rect.w = vp_w;
    ui->screen_rect.h = vp_h;
    ui->screen_scale = scale;

    /* Layout toolbar buttons centered */
    int total_btns_w = 0;
    int gap = 6;
    for (size_t i = 0; i < s_tb_btn_count; i++) {
        total_btns_w += s_tb_buttons[i].width;
        if (i + 1 < s_tb_btn_count) total_btns_w += gap;
    }

    int cur_x = (win_w - total_btns_w) / 2;
    if (cur_x < 4) {
        cur_x = 4;
        gap = 4;
    }

    int btn_y = ui->toolbar_rect.y + 8;
    int btn_h = UI_TOOLBAR_HEIGHT - 16;

    for (size_t i = 0; i < s_tb_btn_count; i++) {
        s_tb_buttons[i].rect.x = cur_x;
        s_tb_buttons[i].rect.y = btn_y;
        s_tb_buttons[i].rect.w = s_tb_buttons[i].width;
        s_tb_buttons[i].rect.h = btn_h;
        cur_x += s_tb_buttons[i].width + gap;
    }
}

/* ------------------------------------------------------------- Event Handling */

static ui_btn_id_t get_toolbar_btn_at(int x, int y)
{
    for (size_t i = 0; i < s_tb_btn_count; i++) {
        const SDL_Rect *r = &s_tb_buttons[i].rect;
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) {
            return s_tb_buttons[i].id;
        }
    }
    return UI_BTN_NONE;
}

bool ui_handle_event(ui_state_t *ui, engine_t *engine, const SDL_Event *ev,
                     SDL_Window *win, bool *out_running, bool *out_fullscreen)
{
    if (!ui || !engine || !ev) return false;

    /* Handle Machine Manager overlay events first */
    if (ui->mm_open) {
        if (ev->type == SDL_KEYDOWN) {
            if (ev->key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                if (ui->create_dialog_open) {
                    ui->create_dialog_open = false;
                } else if (ui->rename_dialog_open) {
                    ui->rename_dialog_open = false;
                } else {
                    ui->mm_open = false;
                }
                return true;
            }
            if (ev->key.keysym.scancode == SDL_SCANCODE_F4) {
                ui->mm_open = false;
                return true;
            }
            if (ui->create_dialog_open) {
                if (ev->key.keysym.scancode == SDL_SCANCODE_RETURN) {
                    /* Confirm create */
                    char new_id[64] = {0};
                    char detail[256] = {0};
                    bool opts[VM_INSTANCE_OPTION_MAX] = {false};
                    int rc = engine_machine_create(engine, ui->new_machine_name, opts, 0,
                                                   ui->new_machine_disk_gib, new_id,
                                                   sizeof(new_id), detail, sizeof(detail));
                    if (rc == 0) {
                        ui_refresh_machines(ui, engine);
                        ui_show_toast(ui, "Machine created!", 2000);
                        ui->create_dialog_open = false;
                    } else {
                        ui_show_toast(ui, detail[0] ? detail : "Failed to create", 3000);
                    }
                    return true;
                } else if (ev->key.keysym.scancode == SDL_SCANCODE_BACKSPACE) {
                    size_t l = strlen(ui->new_machine_name);
                    if (l > 0) ui->new_machine_name[l - 1] = '\0';
                    return true;
                }
            } else if (ui->rename_dialog_open) {
                if (ev->key.keysym.scancode == SDL_SCANCODE_RETURN) {
                    if (ui->selected_machine >= 0 && ui->selected_machine < (int)ui->machine_count) {
                        char detail[256] = {0};
                        int rc = engine_machine_rename(engine, ui->machines[ui->selected_machine].id,
                                                       ui->rename_machine_name, detail, sizeof(detail));
                        if (rc == 0) {
                            ui_refresh_machines(ui, engine);
                            ui_show_toast(ui, "Renamed successfully", 2000);
                            ui->rename_dialog_open = false;
                        } else {
                            ui_show_toast(ui, detail[0] ? detail : "Rename failed", 3000);
                        }
                    }
                    return true;
                } else if (ev->key.keysym.scancode == SDL_SCANCODE_BACKSPACE) {
                    size_t l = strlen(ui->rename_machine_name);
                    if (l > 0) ui->rename_machine_name[l - 1] = '\0';
                    return true;
                }
            }
            return true;
        }

        if (ev->type == SDL_TEXTINPUT) {
            if (ui->create_dialog_open) {
                size_t l = strlen(ui->new_machine_name);
                if (l + strlen(ev->text.text) < sizeof(ui->new_machine_name) - 1) {
                    strcat(ui->new_machine_name, ev->text.text);
                }
                return true;
            } else if (ui->rename_dialog_open) {
                size_t l = strlen(ui->rename_machine_name);
                if (l + strlen(ev->text.text) < sizeof(ui->rename_machine_name) - 1) {
                    strcat(ui->rename_machine_name, ev->text.text);
                }
                return true;
            }
        }

        if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_LEFT) {
            int mx = ev->button.x;
            int my = ev->button.y;
            int win_w, win_h;
            SDL_GetWindowSize(win, &win_w, &win_h);

            int dw = 520;
            int dh = 420;
            if (dw > win_w - 20) dw = win_w - 20;
            if (dh > win_h - UI_TOOLBAR_HEIGHT - 20) dh = win_h - UI_TOOLBAR_HEIGHT - 20;
            int dx = (win_w - dw) / 2;
            int dy = (win_h - UI_TOOLBAR_HEIGHT - dh) / 2;

            /* Check close button [X] at top-right of dialog */
            SDL_Rect close_btn = { dx + dw - 32, dy + 8, 24, 24 };
            if (mx >= close_btn.x && mx < close_btn.x + close_btn.w &&
                my >= close_btn.y && my < close_btn.y + close_btn.h) {
                ui->mm_open = false;
                ui->create_dialog_open = false;
                ui->rename_dialog_open = false;
                return true;
            }

            if (ui->create_dialog_open) {
                /* Create subdialog buttons */
                SDL_Rect btn_2gb = { dx + 60,  dy + 180, 70, 28 };
                SDL_Rect btn_4gb = { dx + 140, dy + 180, 70, 28 };
                SDL_Rect btn_8gb = { dx + 220, dy + 180, 90, 28 };
                SDL_Rect btn_ok  = { dx + dw - 220, dy + dh - 46, 95, 32 };
                SDL_Rect btn_can = { dx + dw - 115, dy + dh - 46, 95, 32 };

                if (mx >= btn_2gb.x && mx < btn_2gb.x + btn_2gb.w && my >= btn_2gb.y && my < btn_2gb.y + btn_2gb.h) {
                    ui->new_machine_disk_gib = 2;
                } else if (mx >= btn_4gb.x && mx < btn_4gb.x + btn_4gb.w && my >= btn_4gb.y && my < btn_4gb.y + btn_4gb.h) {
                    ui->new_machine_disk_gib = 4;
                } else if (mx >= btn_8gb.x && mx < btn_8gb.x + btn_8gb.w && my >= btn_8gb.y && my < btn_8gb.y + btn_8gb.h) {
                    ui->new_machine_disk_gib = 8;
                } else if (mx >= btn_ok.x && mx < btn_ok.x + btn_ok.w && my >= btn_ok.y && my < btn_ok.y + btn_ok.h) {
                    char new_id[64] = {0};
                    char detail[256] = {0};
                    bool opts[VM_INSTANCE_OPTION_MAX] = {false};
                    int rc = engine_machine_create(engine, ui->new_machine_name, opts, 0,
                                                   ui->new_machine_disk_gib, new_id,
                                                   sizeof(new_id), detail, sizeof(detail));
                    if (rc == 0) {
                        ui_refresh_machines(ui, engine);
                        ui_show_toast(ui, "Machine created!", 2000);
                        ui->create_dialog_open = false;
                    } else {
                        ui_show_toast(ui, detail[0] ? detail : "Failed to create", 3000);
                    }
                } else if (mx >= btn_can.x && mx < btn_can.x + btn_can.w && my >= btn_can.y && my < btn_can.y + btn_can.h) {
                    ui->create_dialog_open = false;
                }
                return true;
            }

            if (ui->rename_dialog_open) {
                SDL_Rect btn_ok  = { dx + dw - 220, dy + dh - 46, 95, 32 };
                SDL_Rect btn_can = { dx + dw - 115, dy + dh - 46, 95, 32 };
                if (mx >= btn_ok.x && mx < btn_ok.x + btn_ok.w && my >= btn_ok.y && my < btn_ok.y + btn_ok.h) {
                    if (ui->selected_machine >= 0 && ui->selected_machine < (int)ui->machine_count) {
                        char detail[256] = {0};
                        int rc = engine_machine_rename(engine, ui->machines[ui->selected_machine].id,
                                                       ui->rename_machine_name, detail, sizeof(detail));
                        if (rc == 0) {
                            ui_refresh_machines(ui, engine);
                            ui_show_toast(ui, "Renamed successfully", 2000);
                            ui->rename_dialog_open = false;
                        } else {
                            ui_show_toast(ui, detail[0] ? detail : "Rename failed", 3000);
                        }
                    }
                } else if (mx >= btn_can.x && mx < btn_can.x + btn_can.w && my >= btn_can.y && my < btn_can.y + btn_can.h) {
                    ui->rename_dialog_open = false;
                }
                return true;
            }

            /* Machine list rows */
            int list_y = dy + 45;
            for (unsigned i = 0; i < ui->machine_count && i < 6; i++) {
                SDL_Rect row_rect = { dx + 12, list_y + (int)i * 44, dw - 24, 40 };
                if (mx >= row_rect.x && mx < row_rect.x + row_rect.w &&
                    my >= row_rect.y && my < row_rect.y + row_rect.h) {
                    ui->selected_machine = (int)i;
                    return true;
                }
            }

            /* Action buttons for selected machine */
            SDL_Rect btn_boot   = { dx + 12,  dy + dh - 96, 115, 32 };
            SDL_Rect btn_stop   = { dx + 135, dy + dh - 96, 90, 32 };
            SDL_Rect btn_reset  = { dx + 232, dy + dh - 96, 95, 32 };
            SDL_Rect btn_rename = { dx + 334, dy + dh - 96, 85, 32 };
            SDL_Rect btn_del    = { dx + 426, dy + dh - 96, 82, 32 };

            /* Bottom action buttons */
            SDL_Rect btn_new   = { dx + 12,      dy + dh - 48, 160, 34 };
            SDL_Rect btn_close = { dx + dw - 120, dy + dh - 48, 108, 34 };

            if (mx >= btn_boot.x && mx < btn_boot.x + btn_boot.w &&
                my >= btn_boot.y && my < btn_boot.y + btn_boot.h) {
                if (ui->selected_machine >= 0 && ui->selected_machine < (int)ui->machine_count) {
                    ui_machine_item_t *sel = &ui->machines[ui->selected_machine];
                    char detail[256] = {0};
                    engine_stop(engine, ENGINE_STOP_PLAIN);
                    int rc = engine_start(engine, sel->id, detail, sizeof(detail));
                    if (rc == 0) {
                        ui_refresh_machines(ui, engine);
                        ui_show_toast(ui, "Machine started!", 2500);
                        ui->mm_open = false;
                    } else {
                        ui_show_toast(ui, detail[0] ? detail : "Failed to boot", 3000);
                    }
                }
            } else if (mx >= btn_stop.x && mx < btn_stop.x + btn_stop.w &&
                       my >= btn_stop.y && my < btn_stop.y + btn_stop.h) {
                engine_stop(engine, ENGINE_STOP_PLAIN);
                ui_refresh_machines(ui, engine);
                ui_show_toast(ui, "Machine stopped.", 2000);
            } else if (mx >= btn_reset.x && mx < btn_reset.x + btn_reset.w &&
                       my >= btn_reset.y && my < btn_reset.y + btn_reset.h) {
                if (ui->selected_machine >= 0 && ui->selected_machine < (int)ui->machine_count) {
                    ui_machine_item_t *sel = &ui->machines[ui->selected_machine];
                    char detail[256] = {0};
                    engine_stop(engine, ENGINE_STOP_PLAIN);
                    engine_start(engine, sel->id, detail, sizeof(detail));
                    ui_refresh_machines(ui, engine);
                    ui_show_toast(ui, "Machine rebooted!", 2000);
                    ui->mm_open = false;
                }
            } else if (mx >= btn_rename.x && mx < btn_rename.x + btn_rename.w &&
                       my >= btn_rename.y && my < btn_rename.y + btn_rename.h) {
                if (ui->selected_machine >= 0 && ui->selected_machine < (int)ui->machine_count) {
                    snprintf(ui->rename_machine_name, sizeof(ui->rename_machine_name),
                             "%s", ui->machines[ui->selected_machine].name);
                    ui->rename_dialog_open = true;
                }
            } else if (mx >= btn_del.x && mx < btn_del.x + btn_del.w &&
                       my >= btn_del.y && my < btn_del.y + btn_del.h) {
                if (ui->selected_machine >= 0 && ui->selected_machine < (int)ui->machine_count) {
                    ui_machine_item_t *sel = &ui->machines[ui->selected_machine];
                    if (sel->running) {
                        ui_show_toast(ui, "Cannot delete running machine! Stop it first.", 3000);
                    } else {
                        char detail[256] = {0};
                        int rc = engine_machine_delete(engine, sel->id, detail, sizeof(detail));
                        if (rc == 0) {
                            ui_refresh_machines(ui, engine);
                            ui_show_toast(ui, "Machine deleted.", 2000);
                        } else {
                            ui_show_toast(ui, detail[0] ? detail : "Failed to delete", 3000);
                        }
                    }
                }
            } else if (mx >= btn_new.x && mx < btn_new.x + btn_new.w &&
                       my >= btn_new.y && my < btn_new.y + btn_new.h) {
                snprintf(ui->new_machine_name, sizeof(ui->new_machine_name),
                         "iPhone 3G (%u)", ui->machine_count + 1);
                ui->new_machine_disk_gib = 8;
                ui->create_dialog_open = true;
            } else if (mx >= btn_close.x && mx < btn_close.x + btn_close.w &&
                       my >= btn_close.y && my < btn_close.y + btn_close.h) {
                ui->mm_open = false;
            }

            return true;
        }

        return true; /* Absorb all events while modal is open */
    }

    /* Machine Manager is closed -> check toolbar interactions */
    switch (ev->type) {
    case SDL_MOUSEMOTION:
        ui->hovered_btn = get_toolbar_btn_at(ev->motion.x, ev->motion.y);
        break;

    case SDL_MOUSEBUTTONDOWN:
        if (ev->button.button == SDL_BUTTON_LEFT) {
            ui_btn_id_t btn = get_toolbar_btn_at(ev->button.x, ev->button.y);
            if (btn != UI_BTN_NONE) {
                ui->pressed_btn = btn;
                if (btn == UI_BTN_HOME) {
                    engine_button(engine, VM_BUTTON_HOME, true);
                } else if (btn == UI_BTN_POWER) {
                    engine_button(engine, VM_BUTTON_POWER, true);
                } else if (btn == UI_BTN_VOL_DOWN) {
                    engine_button(engine, VM_BUTTON_VOLUME_DOWN, true);
                } else if (btn == UI_BTN_VOL_UP) {
                    engine_button(engine, VM_BUTTON_VOLUME_UP, true);
                }
                return true;
            }
        }
        break;

    case SDL_MOUSEBUTTONUP:
        if (ev->button.button == SDL_BUTTON_LEFT && ui->pressed_btn != UI_BTN_NONE) {
            ui_btn_id_t btn = ui->pressed_btn;
            ui->pressed_btn = UI_BTN_NONE;

            if (btn == UI_BTN_HOME) {
                engine_button(engine, VM_BUTTON_HOME, false);
            } else if (btn == UI_BTN_POWER) {
                engine_button(engine, VM_BUTTON_POWER, false);
            } else if (btn == UI_BTN_VOL_DOWN) {
                engine_button(engine, VM_BUTTON_VOLUME_DOWN, false);
            } else if (btn == UI_BTN_VOL_UP) {
                engine_button(engine, VM_BUTTON_VOLUME_UP, false);
            } else if (btn == UI_BTN_MUTE) {
                ui->ringer_silent = !ui->ringer_silent;
                engine_button(engine, VM_BUTTON_RINGER_SILENT, ui->ringer_silent);
                ui_show_toast(ui, ui->ringer_silent ? "Ringer: SILENT" : "Ringer: SOUND ON", 1500);
            } else if (btn == UI_BTN_PAUSE) {
                ui->paused = !ui->paused;
                engine_set_paused(engine, ui->paused);
                ui_show_toast(ui, ui->paused ? "PAUSED" : "RESUMED", 1500);
            } else if (btn == UI_BTN_RESET) {
                char start_err[256] = {0};
                engine_stop(engine, ENGINE_STOP_PLAIN);
                engine_start(engine, ui->running_id, start_err, sizeof(start_err));
                ui_show_toast(ui, "REBOOTING...", 2000);
            } else if (btn == UI_BTN_MACHINES) {
                ui_refresh_machines(ui, engine);
                ui->mm_open = true;
            } else if (btn == UI_BTN_FULLSCREEN) {
                if (out_fullscreen) {
                    *out_fullscreen = !*out_fullscreen;
                    SDL_SetWindowFullscreen(win, *out_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                }
            }
            return true;
        }
        break;

    case SDL_KEYDOWN:
        if (ev->key.keysym.scancode == SDL_SCANCODE_F4 ||
            (ev->key.keysym.scancode == SDL_SCANCODE_M && (SDL_GetModState() & KMOD_CTRL))) {
            ui_refresh_machines(ui, engine);
            ui->mm_open = true;
            return true;
        }
        if (ev->key.keysym.scancode == SDL_SCANCODE_F6) {
            char start_err[256] = {0};
            engine_stop(engine, ENGINE_STOP_PLAIN);
            engine_start(engine, ui->running_id, start_err, sizeof(start_err));
            ui_show_toast(ui, "REBOOTING...", 2000);
            return true;
        }
        break;

    default:
        break;
    }

    return false;
}

/* ------------------------------------------------------------- Rendering */

void ui_render(ui_state_t *ui, engine_t *engine, SDL_Renderer *renderer, int win_w, int win_h)
{
    if (!ui || !renderer) return;

    /* 1. Render Toolbar Bar */
    ui_draw_rect_filled(renderer, &ui->toolbar_rect, COLOR_TOOLBAR_BG);

    /* Toolbar top border line */
    SDL_Rect tb_line = { 0, ui->toolbar_rect.y, win_w, 1 };
    ui_draw_rect_filled(renderer, &tb_line, COLOR_BORDER);

    /* Render Toolbar Buttons */
    for (size_t i = 0; i < s_tb_btn_count; i++) {
        tb_button_t *b = &s_tb_buttons[i];
        bool is_hover = (ui->hovered_btn == b->id);
        bool is_pressed = (ui->pressed_btn == b->id);
        bool is_active = false;
        SDL_Color accent = COLOR_ACCENT;

        if (b->id == UI_BTN_MUTE && ui->ringer_silent) {
            is_active = true;
            accent = COLOR_YELLOW;
        } else if (b->id == UI_BTN_PAUSE && ui->paused) {
            is_active = true;
            accent = COLOR_YELLOW;
        } else if (b->id == UI_BTN_MACHINES) {
            accent = COLOR_ACCENT;
        }

        ui_draw_button(renderer, &b->rect, b->label, b->sub, is_hover, is_pressed, is_active, accent);
    }

    /* 2. Render Machine Manager Modal if open */
    if (ui->mm_open) {
        /* Semi-transparent dark overlay over guest viewport */
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 5, 8, 12, 220);
        SDL_Rect backdrop = { 0, 0, win_w, win_h - UI_TOOLBAR_HEIGHT };
        SDL_RenderFillRect(renderer, &backdrop);

        /* Dialog Box */
        int dw = 520;
        int dh = 420;
        if (dw > win_w - 20) dw = win_w - 20;
        if (dh > win_h - UI_TOOLBAR_HEIGHT - 20) dh = win_h - UI_TOOLBAR_HEIGHT - 20;
        int dx = (win_w - dw) / 2;
        int dy = (win_h - UI_TOOLBAR_HEIGHT - dh) / 2;

        SDL_Rect dlg = { dx, dy, dw, dh };
        ui_draw_rect_filled(renderer, &dlg, COLOR_TOOLBAR_BG);
        ui_draw_rect_outline(renderer, &dlg, COLOR_ACCENT);

        /* Dialog Header */
        SDL_Rect header_rect = { dx + 16, dy + 12, dw - 60, 20 };
        ui_draw_text(renderer, header_rect.x, header_rect.y, "MACHINE MANAGER  (S5L8900)", 1, COLOR_TEXT_WHITE);

        /* Close [X] Button */
        SDL_Rect close_btn = { dx + dw - 32, dy + 8, 24, 24 };
        ui_draw_rect_filled(renderer, &close_btn, COLOR_BTN_NORMAL);
        ui_draw_rect_outline(renderer, &close_btn, COLOR_BORDER);
        ui_draw_text_centered(renderer, &close_btn, "X", 1, COLOR_TEXT_WHITE);

        /* Machine List */
        int list_y = dy + 45;
        if (ui->machine_count == 0) {
            SDL_Rect empty_rect = { dx + 20, list_y + 40, dw - 40, 30 };
            ui_draw_text_centered(renderer, &empty_rect, "No machines found. Click [+ NEW MACHINE] below.", 1, COLOR_TEXT_DIM);
        } else {
            for (unsigned i = 0; i < ui->machine_count && i < 6; i++) {
                ui_machine_item_t *m = &ui->machines[i];
                SDL_Rect row_rect = { dx + 12, list_y + (int)i * 44, dw - 24, 40 };
                bool is_sel = (ui->selected_machine == (int)i);

                SDL_Color row_bg = is_sel ? COLOR_BTN_HOVER : COLOR_BTN_NORMAL;
                SDL_Color row_border = is_sel ? COLOR_ACCENT : COLOR_BORDER;
                ui_draw_rect_filled(renderer, &row_rect, row_bg);
                ui_draw_rect_outline(renderer, &row_rect, row_border);

                /* Machine name & info */
                char line1[64];
                snprintf(line1, sizeof(line1), "%s", m->name[0] ? m->name : "Unnamed Machine");
                ui_draw_text(renderer, row_rect.x + 10, row_rect.y + 7, line1, 1, COLOR_TEXT_WHITE);

                char line2[128];
                snprintf(line2, sizeof(line2), "ID: %s  |  Disk: %uGB", m->id, m->disk_gib);
                ui_draw_text(renderer, row_rect.x + 10, row_rect.y + 23, line2, 1, COLOR_TEXT_DIM);

                /* Status badge on right */
                SDL_Rect badge = { row_rect.x + row_rect.w - 95, row_rect.y + 10, 85, 20 };
                if (m->running) {
                    ui_draw_rect_filled(renderer, &badge, COLOR_GREEN);
                    ui_draw_text_centered(renderer, &badge, "RUNNING", 1, COLOR_TEXT_WHITE);
                } else {
                    ui_draw_rect_filled(renderer, &badge, COLOR_BORDER);
                    ui_draw_text_centered(renderer, &badge, "IDLE", 1, COLOR_TEXT_DIM);
                }
            }
        }

        /* Action buttons for selected machine */
        SDL_Rect btn_boot   = { dx + 12,  dy + dh - 96, 115, 32 };
        SDL_Rect btn_stop   = { dx + 135, dy + dh - 96, 90, 32 };
        SDL_Rect btn_reset  = { dx + 232, dy + dh - 96, 95, 32 };
        SDL_Rect btn_rename = { dx + 334, dy + dh - 96, 85, 32 };
        SDL_Rect btn_del    = { dx + 426, dy + dh - 96, 82, 32 };

        ui_draw_button(renderer, &btn_boot,   "> BOOT",   NULL, false, false, false, COLOR_GREEN);
        ui_draw_button(renderer, &btn_stop,   "# STOP",   NULL, false, false, false, COLOR_YELLOW);
        ui_draw_button(renderer, &btn_reset,  "@ REBOOT", NULL, false, false, false, COLOR_ACCENT);
        ui_draw_button(renderer, &btn_rename, "RENAME",   NULL, false, false, false, COLOR_BTN_NORMAL);
        ui_draw_button(renderer, &btn_del,    "DELETE",   NULL, false, false, false, COLOR_RED);

        /* Dialog Footer buttons */
        SDL_Rect btn_new   = { dx + 12,      dy + dh - 48, 160, 34 };
        SDL_Rect btn_close = { dx + dw - 120, dy + dh - 48, 108, 34 };

        ui_draw_button(renderer, &btn_new,   "+ NEW MACHINE", NULL, false, false, true, COLOR_ACCENT);
        ui_draw_button(renderer, &btn_close, "CLOSE",         NULL, false, false, false, COLOR_BTN_NORMAL);

        /* Create Machine Sub-Dialog */
        if (ui->create_dialog_open) {
            SDL_Rect c_overlay = { dx + 30, dy + 50, dw - 60, dh - 100 };
            ui_draw_rect_filled(renderer, &c_overlay, COLOR_BG);
            ui_draw_rect_outline(renderer, &c_overlay, COLOR_ACCENT);

            ui_draw_text(renderer, c_overlay.x + 20, c_overlay.y + 20, "CREATE NEW MACHINE", 1, COLOR_TEXT_WHITE);

            ui_draw_text(renderer, c_overlay.x + 20, c_overlay.y + 55, "Machine Name (Type to edit):", 1, COLOR_TEXT_DIM);
            SDL_Rect name_box = { c_overlay.x + 20, c_overlay.y + 75, c_overlay.w - 40, 30 };
            ui_draw_rect_filled(renderer, &name_box, COLOR_BTN_NORMAL);
            ui_draw_rect_outline(renderer, &name_box, COLOR_ACCENT);
            ui_draw_text(renderer, name_box.x + 10, name_box.y + 10, ui->new_machine_name, 1, COLOR_TEXT_WHITE);

            ui_draw_text(renderer, c_overlay.x + 20, c_overlay.y + 125, "Virtual Disk Size:", 1, COLOR_TEXT_DIM);
            SDL_Rect btn_2gb = { c_overlay.x + 20,  c_overlay.y + 145, 70, 28 };
            SDL_Rect btn_4gb = { c_overlay.x + 100, c_overlay.y + 145, 70, 28 };
            SDL_Rect btn_8gb = { c_overlay.x + 180, c_overlay.y + 145, 95, 28 };

            ui_draw_button(renderer, &btn_2gb, "2 GB", NULL, false, false, ui->new_machine_disk_gib == 2, COLOR_ACCENT);
            ui_draw_button(renderer, &btn_4gb, "4 GB", NULL, false, false, ui->new_machine_disk_gib == 4, COLOR_ACCENT);
            ui_draw_button(renderer, &btn_8gb, "8 GB (Rec)", NULL, false, false, ui->new_machine_disk_gib == 8, COLOR_ACCENT);

            SDL_Rect btn_ok  = { c_overlay.x + c_overlay.w - 210, c_overlay.y + c_overlay.h - 45, 95, 30 };
            SDL_Rect btn_can = { c_overlay.x + c_overlay.w - 105, c_overlay.y + c_overlay.h - 45, 95, 30 };
            ui_draw_button(renderer, &btn_ok,  "CREATE", NULL, false, false, true, COLOR_GREEN);
            ui_draw_button(renderer, &btn_can, "CANCEL", NULL, false, false, false, COLOR_BTN_NORMAL);
        }

        /* Rename Sub-Dialog */
        if (ui->rename_dialog_open) {
            SDL_Rect r_overlay = { dx + 40, dy + 100, dw - 80, 180 };
            ui_draw_rect_filled(renderer, &r_overlay, COLOR_BG);
            ui_draw_rect_outline(renderer, &r_overlay, COLOR_ACCENT);

            ui_draw_text(renderer, r_overlay.x + 20, r_overlay.y + 20, "RENAME MACHINE", 1, COLOR_TEXT_WHITE);
            ui_draw_text(renderer, r_overlay.x + 20, r_overlay.y + 50, "Enter New Name:", 1, COLOR_TEXT_DIM);

            SDL_Rect name_box = { r_overlay.x + 20, r_overlay.y + 70, r_overlay.w - 40, 30 };
            ui_draw_rect_filled(renderer, &name_box, COLOR_BTN_NORMAL);
            ui_draw_rect_outline(renderer, &name_box, COLOR_ACCENT);
            ui_draw_text(renderer, name_box.x + 10, name_box.y + 10, ui->rename_machine_name, 1, COLOR_TEXT_WHITE);

            SDL_Rect btn_ok  = { r_overlay.x + r_overlay.w - 210, r_overlay.y + r_overlay.h - 45, 95, 30 };
            SDL_Rect btn_can = { r_overlay.x + r_overlay.w - 105, r_overlay.y + r_overlay.h - 45, 95, 30 };
            ui_draw_button(renderer, &btn_ok,  "SAVE",   NULL, false, false, true, COLOR_GREEN);
            ui_draw_button(renderer, &btn_can, "CANCEL", NULL, false, false, false, COLOR_BTN_NORMAL);
        }
    }

    /* 3. Render Toast Notification Banner */
    uint32_t now = SDL_GetTicks();
    if (ui->toast[0] && now < ui->toast_until) {
        int tw = (int)strlen(ui->toast) * 8 + 30;
        int th = 28;
        SDL_Rect t_rect = { (win_w - tw) / 2, 16, tw, th };
        ui_draw_rect_filled(renderer, &t_rect, COLOR_BG);
        ui_draw_rect_outline(renderer, &t_rect, COLOR_ACCENT);
        ui_draw_text_centered(renderer, &t_rect, ui->toast, 1, COLOR_TEXT_WHITE);
    }
}

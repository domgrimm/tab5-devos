/* devos_powerdlg: see devos_powerdlg.h. */
#include "devos_powerdlg.h"
#include "devos_cmdpal.h"
#include "devos_hud.h"
#include "devos_shortcuts.h"
#include "devos_widgets.h"
#include "devos_focus.h"
#include "devos_theme.h"
#include "devos_core.h"
#include "devos_config.h"

#include <stdio.h>

#define DLG_W 520
#define DLG_H 200

typedef enum { MODE_NONE = 0, MODE_RESTART, MODE_SHUTDOWN } dlg_mode_t;

static lv_obj_t *s_overlay, *s_title, *s_msg, *s_ok, *s_ok_lbl;
static devos_focus_t s_focus;
static dlg_mode_t s_mode = MODE_NONE;

bool devos_powerdlg_is_open(void)
{
    return s_overlay && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void cancel_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_powerdlg_close();
}

static void confirm_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_mode == MODE_RESTART) {
        devos_w_set_text(s_msg, "Restarting...");
        lv_refr_now(NULL);
        devos_core_restart();       /* the current app finishes up first (hide) */
    } else {
        devos_w_set_text(s_msg, "Shutting down...");
        lv_refr_now(NULL);
        devos_core_shutdown();
    }
}

static void build(void)
{
    if (s_overlay) return;
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, DEVOS_SCREEN_WIDTH, DEVOS_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_50, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(s_overlay);
    lv_obj_set_size(box, DLG_W, DLG_H);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, DEVOS_TOP_BAR_HEIGHT / 2);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_pad_all(box, 20, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    devos_w_track(box, DEVOS_W_MODAL);

    s_title = devos_w_label(box, &lv_font_montserrat_16, DEVOS_W_TEXT_ACCENT, "");
    s_msg = devos_w_label(box, &lv_font_montserrat_14, DEVOS_W_TEXT_DIM, "");
    lv_obj_set_width(s_msg, DLG_W - 44);
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_msg, 0, 40);

    lv_obj_t *cancel = devos_w_btn(box, "Cancel  (Esc)", 130, cancel_cb, NULL, NULL);
    lv_obj_set_height(cancel, 36);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_RIGHT, -150, 0);
    s_ok = devos_w_btn_kind(box, DEVOS_W_BTN_PRIMARY, "", 136, confirm_cb, NULL, &s_ok_lbl);
    lv_obj_set_height(s_ok, 36);
    lv_obj_align(s_ok, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    devos_focus_init(&s_focus);
    devos_focus_add(&s_focus, cancel);
    devos_focus_add(&s_focus, s_ok);
    lv_obj_update_layout(s_overlay);    /* real coordinates before the first show redraws them */
}

static void open(dlg_mode_t mode)
{
    devos_cmdpal_close();
    devos_hud_close();
    devos_shortcuts_close();
    build();
    s_mode = mode;

    bool restart = mode == MODE_RESTART;
    const char *why = devos_core_restart_check();
    char m[220];
    if (why) {
        snprintf(m, sizeof(m), "%s - %s now stops it.", why, restart ? "restarting" : "shutting down");
    } else if (restart) {
        snprintf(m, sizeof(m), "The Tab5 starts again in a few seconds. Apps save their work first; "
                               "SSH sessions and the VPNs reconnect afterwards.");
    } else {
        snprintf(m, sizeof(m), "The Tab5 will power off. Apps save their work first.");
    }
    devos_w_set_text(s_title, restart ? "Restart devOS?" : "Shut down?");
    devos_w_set_text(s_msg, m);
    devos_w_track(s_msg, why ? DEVOS_W_TEXT_WARN : DEVOS_W_TEXT_DIM);
    devos_w_set_text(s_ok_lbl, restart ? LV_SYMBOL_REFRESH "  Restart" : LV_SYMBOL_POWER "  Shutdown");

    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    devos_focus_set(&s_focus, s_ok);         /* Enter goes ahead, Esc cancels */
}

void devos_powerdlg_restart(void)  { open(MODE_RESTART); }
void devos_powerdlg_shutdown(void) { open(MODE_SHUTDOWN); }

void devos_powerdlg_close(void)
{
    if (!s_overlay) return;
    devos_focus_clear(&s_focus);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_mode = MODE_NONE;
}

/* The dialog owns the keyboard while it is open: Enter presses the focused
 * button, Esc cancels, the arrows / Tab move between them; everything else is
 * swallowed so nothing reaches the app unseen. */
static bool powerdlg_key(uint32_t key, uint8_t mods)
{
    if (!devos_powerdlg_is_open()) return false;
    if (key == LV_KEY_ESC) {
        devos_powerdlg_close();
        return true;
    }
    devos_focus_key(&s_focus, key, mods);
    return true;
}

void devos_powerdlg_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    devos_core_add_key_hook(powerdlg_key);
}

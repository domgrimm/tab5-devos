/* devos_toast: see devos_toast.h. */
#include "devos_toast.h"
#include "devos_widgets.h"
#include "devos_theme.h"
#include "devos_core.h"
#include "devos_config.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()   portENTER_CRITICAL(&s_mux)
#define UNLOCK() portEXIT_CRITICAL(&s_mux)
#else
#include <pthread.h>
static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&s_mx)
#define UNLOCK() pthread_mutex_unlock(&s_mx)
#endif

#define TOAST_MAX_W 760

/* The latest request, handed from any task to the UI task. */
static struct {
    bool pending;
    char msg[128];
    devos_toast_type_t type;
    uint32_t ms;
} s_req;

static lv_obj_t *s_box, *s_icon, *s_text;
static uint32_t s_shown_at, s_show_ms;

void devos_toast_show(const char *msg, devos_toast_type_t type, uint32_t duration_ms)
{
    if (!msg || !*msg) return;
    char m[sizeof(s_req.msg)];
    snprintf(m, sizeof(m), "%s", msg);
    LOCK();
    memcpy(s_req.msg, m, sizeof(m));
    s_req.type = type;
    s_req.ms = duration_ms ? duration_ms : DEVOS_TOAST_DEFAULT_MS;
    s_req.pending = true;
    UNLOCK();
}

static void style_for(devos_toast_type_t type)
{
    static const char *const icons[] = { LV_SYMBOL_BELL, LV_SYMBOL_OK, LV_SYMBOL_WARNING, LV_SYMBOL_CLOSE };
    static const devos_w_kind_t kinds[] = { DEVOS_W_TEXT_ACCENT, DEVOS_W_TEXT_OK, DEVOS_W_TEXT_WARN, DEVOS_W_TEXT_ERR };
    int t = type >= DEVOS_TOAST_INFO && type <= DEVOS_TOAST_ERROR ? (int)type : 0;
    lv_label_set_text(s_icon, icons[t]);
    devos_w_track(s_icon, kinds[t]);
    const devos_palette_t *p = devos_theme_get();
    lv_color_t c = t == 1 ? p->accent_secondary : t == 2 ? p->accent_warning : t == 3 ? p->accent_danger
                                                                                      : p->accent_primary;
    lv_obj_set_style_border_color(s_box, c, 0);
}

static void tick_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    bool show = false;
    char msg[128];
    devos_toast_type_t type = DEVOS_TOAST_INFO;
    uint32_t ms = 0;
    LOCK();
    if (s_req.pending) {
        s_req.pending = false;
        memcpy(msg, s_req.msg, sizeof(msg));
        type = s_req.type;
        ms = s_req.ms;
        show = true;
    }
    UNLOCK();
    if (show) {
        lv_label_set_text(s_text, msg);
        style_for(type);
        lv_obj_remove_flag(s_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_box);          /* over the overlays too */
        s_shown_at = lv_tick_get();
        s_show_ms = ms;
    } else if (!lv_obj_has_flag(s_box, LV_OBJ_FLAG_HIDDEN) && lv_tick_elaps(s_shown_at) >= s_show_ms) {
        lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    }
}

void devos_toast_init(void)
{
    if (s_box) return;
    s_box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_box, TOAST_MAX_W, 0);
    lv_obj_align(s_box, LV_ALIGN_TOP_MID, 0, DEVOS_TOP_BAR_HEIGHT + 8);
    lv_obj_set_style_radius(s_box, 18, 0);
    lv_obj_set_style_border_width(s_box, 2, 0);
    lv_obj_set_style_pad_hor(s_box, 16, 0);
    lv_obj_set_style_pad_ver(s_box, 8, 0);
    lv_obj_set_style_pad_column(s_box, 10, 0);
    lv_obj_set_style_shadow_width(s_box, 16, 0);
    lv_obj_set_style_shadow_opa(s_box, LV_OPA_40, 0);
    lv_obj_set_flex_flow(s_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);   /* taps go through */
    devos_w_track(s_box, DEVOS_W_PANEL);
    s_icon = devos_w_label(s_box, &lv_font_montserrat_16, DEVOS_W_TEXT_ACCENT, LV_SYMBOL_BELL);
    s_text = devos_w_label(s_box, &lv_font_montserrat_16, DEVOS_W_TEXT, "");
    lv_obj_set_style_max_width(s_text, TOAST_MAX_W - 70, 0);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(tick_cb, 50, NULL);
}

/* ------------------------------------------------------------------ system toasts */
void devos_toast_watch(void)
{
    static bool primed;
    static bool wifi, ts, wg, low15, low5;
    static char ssid[33];
    const devos_telemetry_t *t = devos_telemetry_get();
    bool bat_low = t->battery_valid && t->battery_present && !t->battery_charging;
    if (!primed) {                              /* the state at boot is no news */
        primed = true;
        wifi = t->wifi_connected;
        ts = t->tailscale_online;
        wg = t->wireguard_online;
        snprintf(ssid, sizeof(ssid), "%s", t->wifi_ssid);
        return;
    }
    char m[96];
    if (t->wifi_connected != wifi) {
        wifi = t->wifi_connected;
        if (wifi) snprintf(m, sizeof(m), "Wi-Fi connected to %.32s", t->wifi_ssid);
        else snprintf(m, sizeof(m), "Wi-Fi disconnected from %.32s", ssid);
        devos_toast_show(m, wifi ? DEVOS_TOAST_OK : DEVOS_TOAST_WARN, 0);
        snprintf(ssid, sizeof(ssid), "%s", t->wifi_ssid);
    }
    if (t->tailscale_online != ts) {
        ts = t->tailscale_online;
        devos_toast_show(ts ? "Tailscale connected" : "Tailscale disconnected", ts ? DEVOS_TOAST_OK : DEVOS_TOAST_INFO,
                         0);
    }
    if (t->wireguard_online != wg) {
        wg = t->wireguard_online;
        if (wg) snprintf(m, sizeof(m), "WireGuard tunnel up (%.19s)", t->wireguard_ip);
        else snprintf(m, sizeof(m), "WireGuard tunnel down");
        devos_toast_show(m, wg ? DEVOS_TOAST_OK : DEVOS_TOAST_INFO, 0);
    }
    /* low battery: once at 15 % and once at 5 %, again after a charge */
    if (!bat_low || t->battery_percent > 20) low15 = low5 = false;
    if (bat_low && t->battery_percent <= 5 && !low5) {
        low5 = low15 = true;
        snprintf(m, sizeof(m), "Battery at %u%% - plug in soon", t->battery_percent);
        devos_toast_show(m, DEVOS_TOAST_ERROR, 5000);
    } else if (bat_low && t->battery_percent <= 15 && !low15) {
        low15 = true;
        snprintf(m, sizeof(m), "Battery low (%u%%)", t->battery_percent);
        devos_toast_show(m, DEVOS_TOAST_WARN, 4000);
    }
}

/* devos_hud: see devos_hud.h. Everything comes from devos_telemetry_get(),
 * which devos_sysmon refreshes once a second. */
#include "devos_hud.h"
#include "devos_cmdpal.h"
#include "devos_shortcuts.h"
#include "devos_widgets.h"
#include "devos_theme.h"
#include "devos_core.h"
#include "devos_config.h"

#include <stdio.h>
#include <string.h>

#define HUD_W       880
#define HUD_H       500
#define HUD_PAD     16
#define HUD_INNER_W (HUD_W - 2 * HUD_PAD - 4)
#define HUD_INNER_H (HUD_H - 2 * HUD_PAD - 4)
#define CARD_GAP    12
#define CARD_Y      34
#define CARD_W      ((HUD_INNER_W - CARD_GAP) / 2)
#define CARD_H      ((HUD_INNER_H - CARD_Y - 26 - CARD_GAP) / 2)
#define ROW_Y0      24
#define ROW_PITCH   24
#define VAL_X       160

enum {
    R_BAT, R_VOLT, R_CUR, R_PWR, R_LEFT,
    R_SRAM, R_SRAM_MIN, R_SRAM_BIG, R_PSRAM, R_SD,
    R_WIFI, R_BSSID, R_IP, R_TS, R_DERP, R_WG,
    R_UP, R_CLOCK, R_VER,
    R_COUNT
};

static lv_obj_t *s_overlay, *s_box;
static lv_obj_t *s_val[R_COUNT];
static int8_t s_kind[R_COUNT];
static lv_obj_t *s_cpu_bar[2], *s_cpu_pct[2];
static lv_timer_t *s_timer;

bool devos_hud_is_open(void)
{
    return s_overlay && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------ layout */
static lv_obj_t *card(int col, int row, const char *title)
{
    lv_obj_t *c = devos_w_panel(s_box, col * (CARD_W + CARD_GAP), CARD_Y + row * (CARD_H + CARD_GAP), CARD_W, CARD_H,
                                DEVOS_W_PANEL_ALT);
    lv_obj_set_style_radius(c, 6, 0);
    lv_obj_set_style_pad_all(c, 12, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);       /* a tap anywhere closes */
    devos_w_label(c, &lv_font_montserrat_12, DEVOS_W_TEXT_ACCENT, title);
    return c;
}

static lv_obj_t *key_label(lv_obj_t *c, int line, const char *key)
{
    lv_obj_t *k = devos_w_label(c, &lv_font_montserrat_14, DEVOS_W_TEXT_DIM, key);
    lv_obj_set_pos(k, 0, ROW_Y0 + line * ROW_PITCH);
    return k;
}

static void row(lv_obj_t *c, int line, const char *key, int r)
{
    key_label(c, line, key);
    lv_obj_t *v = devos_w_label(c, &lv_font_montserrat_14, DEVOS_W_TEXT, "--");
    lv_obj_set_pos(v, VAL_X, ROW_Y0 + line * ROW_PITCH);
    lv_obj_set_width(v, CARD_W - 26 - VAL_X);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    s_val[r] = v;
    s_kind[r] = DEVOS_W_TEXT;
}

static void cpu_row(lv_obj_t *c, int line, const char *key, int core)
{
    key_label(c, line, key);
    lv_obj_t *b = lv_bar_create(c);
    lv_obj_set_size(b, 150, 10);
    lv_obj_set_pos(b, VAL_X, ROW_Y0 + line * ROW_PITCH + 5);
    lv_bar_set_range(b, 0, 100);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_set_style_radius(b, 3, LV_PART_INDICATOR);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    devos_w_track(b, DEVOS_W_PROGRESS);
    s_cpu_bar[core] = b;
    s_cpu_pct[core] = devos_w_label(c, &lv_font_montserrat_14, DEVOS_W_TEXT, "--");
    lv_obj_set_pos(s_cpu_pct[core], VAL_X + 162, ROW_Y0 + line * ROW_PITCH);
}

static void overlay_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_hud_close();
}

static void refresh(void);

static void tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (devos_hud_is_open()) refresh();
}

static void build(void)
{
    if (s_overlay) return;
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, DEVOS_SCREEN_WIDTH, DEVOS_SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_40, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_overlay, overlay_click_cb, LV_EVENT_CLICKED, NULL);

    s_box = lv_obj_create(s_overlay);
    lv_obj_set_size(s_box, HUD_W, HUD_H);
    lv_obj_align(s_box, LV_ALIGN_CENTER, 0, DEVOS_TOP_BAR_HEIGHT / 2);
    lv_obj_set_style_border_width(s_box, 2, 0);
    lv_obj_set_style_radius(s_box, 8, 0);
    lv_obj_set_style_pad_all(s_box, HUD_PAD, 0);
    lv_obj_remove_flag(s_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    devos_w_track(s_box, DEVOS_W_MODAL);

    devos_w_label(s_box, &lv_font_montserrat_16, DEVOS_W_TEXT_ACCENT, "System info");
    lv_obj_t *ver = devos_w_label(s_box, &lv_font_montserrat_12, DEVOS_W_TEXT_MUTED, "updated every second");
    lv_obj_align(ver, LV_ALIGN_TOP_RIGHT, 0, 3);

    lv_obj_t *c = card(0, 0, "POWER");
    row(c, 0, "Battery", R_BAT);
    row(c, 1, "Voltage", R_VOLT);
    row(c, 2, "Current", R_CUR);
    row(c, 3, "Power", R_PWR);
    row(c, 4, "Time left", R_LEFT);

    c = card(1, 0, "MEMORY");
    row(c, 0, "Internal RAM free", R_SRAM);
    row(c, 1, "Lowest since boot", R_SRAM_MIN);
    row(c, 2, "Largest free block", R_SRAM_BIG);
    row(c, 3, "PSRAM free", R_PSRAM);
    row(c, 4, "SD card", R_SD);

    c = card(0, 1, "NETWORK");
    row(c, 0, "Wi-Fi", R_WIFI);
    row(c, 1, "Access point", R_BSSID);
    row(c, 2, "Local IP", R_IP);
    row(c, 3, "Tailscale", R_TS);
    row(c, 4, "DERP relay", R_DERP);
    row(c, 5, "WireGuard", R_WG);

    c = card(1, 1, "CPU & SYSTEM");
    cpu_row(c, 0, "Core 0 (network)", 0);
    cpu_row(c, 1, "Core 1 (screen)", 1);
    row(c, 2, "Uptime", R_UP);
    row(c, 3, "Clock", R_CLOCK);
    row(c, 4, "Firmware", R_VER);

    lv_obj_t *keys = devos_w_label(s_box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM,
                                   "Esc / Sym+I  close   Sym+Space  commands");
    lv_obj_align(keys, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    s_timer = lv_timer_create(tick_cb, 500, NULL);
    lv_timer_pause(s_timer);
    lv_obj_update_layout(s_overlay);    /* real coordinates before the first show redraws them */
}

/* ------------------------------------------------------------------ values */
static void set_val(int r, const char *text, devos_w_kind_t kind)
{
    devos_w_set_text(s_val[r], text);
    if (s_kind[r] != (int8_t)kind) {
        s_kind[r] = (int8_t)kind;
        devos_w_track(s_val[r], kind);
    }
}

static void fmt_kb(char *out, size_t cap, uint32_t kb)
{
    if (kb >= 10 * 1024) snprintf(out, cap, "%lu.%lu MB", (unsigned long)(kb / 1024), (unsigned long)(kb % 1024 * 10 / 1024));
    else snprintf(out, cap, "%lu KB", (unsigned long)kb);
}

static void fmt_duration(char *out, size_t cap, uint32_t s)
{
    if (s >= 86400) snprintf(out, cap, "%lu d %lu h", (unsigned long)(s / 86400), (unsigned long)(s % 86400 / 3600));
    else if (s >= 3600) snprintf(out, cap, "%lu h %02lu min", (unsigned long)(s / 3600), (unsigned long)(s % 3600 / 60));
    else snprintf(out, cap, "%lu min %02lu s", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static void refresh_power(const devos_telemetry_t *t)
{
    char b[64];
    if (!t->battery_valid) {
        for (int r = R_BAT; r <= R_LEFT; r++) set_val(r, "--", DEVOS_W_TEXT_MUTED);
        set_val(R_BAT, "No reading from the power monitor", DEVOS_W_TEXT_WARN);
        return;
    }
    if (!t->battery_present) {
        set_val(R_BAT, "None fitted (USB power)", DEVOS_W_TEXT);
    } else {
        snprintf(b, sizeof(b), "%u%%%s", t->battery_percent, t->battery_charging ? ", charging" : "");
        set_val(R_BAT, b, t->battery_percent < 15 && !t->battery_charging ? DEVOS_W_TEXT_ERR : DEVOS_W_TEXT);
    }
    snprintf(b, sizeof(b), "%u.%02u V", t->battery_voltage_mv / 1000, t->battery_voltage_mv % 1000 / 10);
    set_val(R_VOLT, b, DEVOS_W_TEXT);
    int ma = t->battery_current_ma;
    snprintf(b, sizeof(b), "%d mA%s", ma < 0 ? -ma : ma, ma < -5 ? " out of the battery" : ma > 5 ? " charging" : "");
    set_val(R_CUR, b, DEVOS_W_TEXT);
    snprintf(b, sizeof(b), "%u.%02u W", t->battery_power_mw / 1000, t->battery_power_mw % 1000 / 10);
    set_val(R_PWR, b, DEVOS_W_TEXT);
    if (!t->battery_present) set_val(R_LEFT, "--", DEVOS_W_TEXT_MUTED);
    else if (t->battery_charging) set_val(R_LEFT, "Charging", DEVOS_W_TEXT);
    else if (t->runtime_minutes_left) {
        snprintf(b, sizeof(b), "about %d h %02d min", t->runtime_minutes_left / 60, t->runtime_minutes_left % 60);
        set_val(R_LEFT, b, t->runtime_minutes_left < 30 ? DEVOS_W_TEXT_WARN : DEVOS_W_TEXT);
    } else {
        set_val(R_LEFT, "--", DEVOS_W_TEXT_MUTED);
    }
}

static void refresh_memory(const devos_telemetry_t *t)
{
    char b[64], x[24], y[24];
    /* mbedTLS / SSH handshakes want ~120 KB of internal RAM free, ~40 KB of
     * it in one piece (AGENTS.md rule 2). */
    fmt_kb(b, sizeof(b), t->free_sram_kb);
    set_val(R_SRAM, b, t->free_sram_kb < 64 ? DEVOS_W_TEXT_ERR : t->free_sram_kb < 120 ? DEVOS_W_TEXT_WARN : DEVOS_W_TEXT);
    fmt_kb(b, sizeof(b), t->sram_min_free_kb);
    set_val(R_SRAM_MIN, b, t->sram_min_free_kb < 64 ? DEVOS_W_TEXT_WARN : DEVOS_W_TEXT);
    fmt_kb(b, sizeof(b), t->sram_largest_kb);
    set_val(R_SRAM_BIG, b, t->sram_largest_kb < 40 ? DEVOS_W_TEXT_ERR : t->sram_largest_kb < 64 ? DEVOS_W_TEXT_WARN : DEVOS_W_TEXT);
    fmt_kb(x, sizeof(x), t->free_psram_kb);
    if (t->psram_total_kb) {
        fmt_kb(y, sizeof(y), t->psram_total_kb);
        snprintf(b, sizeof(b), "%s of %s", x, y);
    } else {
        snprintf(b, sizeof(b), "%s", x);
    }
    set_val(R_PSRAM, b, DEVOS_W_TEXT);
    if (t->sd_mounted && t->sd_total_mb) {
        snprintf(b, sizeof(b), "%lu.%lu GB free of %lu.%lu GB", (unsigned long)(t->sd_free_mb / 1024),
                 (unsigned long)(t->sd_free_mb % 1024 * 10 / 1024), (unsigned long)(t->sd_total_mb / 1024),
                 (unsigned long)(t->sd_total_mb % 1024 * 10 / 1024));
        set_val(R_SD, b, DEVOS_W_TEXT);
    } else {
        set_val(R_SD, t->sd_mounted ? "Mounted" : "No card", t->sd_mounted ? DEVOS_W_TEXT : DEVOS_W_TEXT_MUTED);
    }
}

static void refresh_network(const devos_telemetry_t *t)
{
    char b[80];
    /* wifi_state mirrors devos_wifi_state_t: 0 off, 1 idle, 2 connecting, 3 connected, 4 failed */
    if (t->wifi_state == 3) {
        if (t->wifi_channel) snprintf(b, sizeof(b), "%s   %d dBm   ch %u", t->wifi_ssid, t->wifi_rssi, t->wifi_channel);
        else snprintf(b, sizeof(b), "%s   %d dBm", t->wifi_ssid, t->wifi_rssi);
        set_val(R_WIFI, b, t->wifi_rssi < -75 ? DEVOS_W_TEXT_WARN : DEVOS_W_TEXT);
    } else {
        const char *s = t->wifi_state == 2 ? "Connecting..." : t->wifi_state == 4 ? "Connection failed"
                      : t->wifi_state == 0 ? "Unavailable" : "Not connected";
        set_val(R_WIFI, s, t->wifi_state == 4 ? DEVOS_W_TEXT_ERR : DEVOS_W_TEXT_MUTED);
    }
    set_val(R_BSSID, t->wifi_bssid[0] ? t->wifi_bssid : "--", t->wifi_bssid[0] ? DEVOS_W_TEXT : DEVOS_W_TEXT_MUTED);
    set_val(R_IP, t->local_ip[0] ? t->local_ip : "--", t->local_ip[0] ? DEVOS_W_TEXT : DEVOS_W_TEXT_MUTED);
    if (t->tailscale_online) {
        snprintf(b, sizeof(b), "%s   %u peer%s online", t->tailscale_ip, t->tailscale_peers_online,
                 t->tailscale_peers_online == 1 ? "" : "s");
        set_val(R_TS, b, DEVOS_W_TEXT_OK);
        if (t->tailscale_derp_ms > 0) snprintf(b, sizeof(b), "%s, %d ms", t->tailscale_derp, t->tailscale_derp_ms);
        else snprintf(b, sizeof(b), "%s", t->tailscale_derp[0] ? t->tailscale_derp : "--");
        set_val(R_DERP, b, DEVOS_W_TEXT);
    } else {
        set_val(R_TS, "Off", DEVOS_W_TEXT_MUTED);
        set_val(R_DERP, "--", DEVOS_W_TEXT_MUTED);
    }
    if (t->wireguard_online) {
        snprintf(b, sizeof(b), "Up, %s", t->wireguard_ip);
        set_val(R_WG, b, DEVOS_W_TEXT_OK);
    } else {
        set_val(R_WG, "Down", DEVOS_W_TEXT_MUTED);
    }
}

static void refresh_system(const devos_telemetry_t *t)
{
    char b[64];
    uint8_t load[2] = { t->cpu_load_core0, t->cpu_load_core1 };
    for (int c = 0; c < 2; c++) {
        lv_bar_set_value(s_cpu_bar[c], load[c], LV_ANIM_OFF);
        snprintf(b, sizeof(b), "%u%%", load[c]);
        devos_w_set_text(s_cpu_pct[c], b);
    }
    fmt_duration(b, sizeof(b), t->uptime_s);
    set_val(R_UP, b, DEVOS_W_TEXT);
    if (t->time_valid) {
        snprintf(b, sizeof(b), "%02u:%02u:%02u   %s", t->rtc_hour, t->rtc_min, t->rtc_sec, t->rtc_date_str);
        set_val(R_CLOCK, b, DEVOS_W_TEXT);
    } else {
        set_val(R_CLOCK, "Not set yet (waits for Wi-Fi)", DEVOS_W_TEXT_MUTED);
    }
    set_val(R_VER, DEVOS_VERSION_STR, DEVOS_W_TEXT);
}

static void refresh(void)
{
    const devos_telemetry_t *t = devos_telemetry_get();
    refresh_power(t);
    refresh_memory(t);
    refresh_network(t);
    refresh_system(t);
}

/* ------------------------------------------------------------------ open / close */
void devos_hud_open(void)
{
    if (devos_hud_is_open()) return;
    devos_cmdpal_close();
    devos_shortcuts_close();
    build();
    refresh();
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    lv_timer_resume(s_timer);
}

void devos_hud_close(void)
{
    if (!devos_hud_is_open()) return;
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(s_timer);
}

static bool hud_key(uint32_t key, uint8_t mods)
{
    bool toggle = (mods & DEVOS_MOD_FN) && (key == 'i' || key == 'I');
    if (!devos_hud_is_open()) {
        if (!toggle) return false;
        devos_hud_open();
        return true;
    }
    if (toggle || key == LV_KEY_ESC || key == '\r' || key == '\n') {
        devos_hud_close();
        return true;
    }
    if (mods & DEVOS_MOD_FN) return false;     /* Sym shortcuts still work (theme, brightness ...) */
    return true;                                /* nothing gets typed into the app unseen */
}

void devos_hud_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    devos_core_add_key_hook(hud_key);
}

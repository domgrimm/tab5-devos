#include "devos_core.h"
#include "devos_theme.h"
#include <stdio.h>
#include <string.h>

static devos_app_descriptor_t *registered_apps[DEVOS_APP_COUNT] = {NULL};
static devos_app_id_t current_app_id = DEVOS_APP_LAUNCHER;
static devos_app_id_t previous_app_id = DEVOS_APP_LAUNCHER;

static devos_telemetry_t telemetry_data = {
    .battery_voltage_mv     = 7820,     /* 7.82 V (NP-F550 nominal 7.4V) */
    .battery_current_ma     = -410,     /* 410 mA discharge */
    .battery_power_mw       = 3206,     /* 3.2 W */
    .battery_percent        = 94,       /* 94% */
    .battery_charging       = false,
    .runtime_minutes_left   = 288,      /* ~4.8 hours */

    .wifi_connected         = true,
    .wifi_ssid              = "DevNet",
    .wifi_rssi              = -58,
    .tailscale_online       = true,
    .tailscale_ip           = "100.77.11.92",
    .tailscale_peers_online = 6,
    .tailscale_derp         = "Sydney (18ms)",

    .sd_mounted             = true,
    .sd_total_mb            = 31200,
    .sd_free_mb             = 29412,
    .free_psram_kb          = 28416,    /* 28.4 MB Free PSRAM */
    .free_sram_kb           = 428,      /* 428 KB Internal SRAM */

    .cpu_load_core0         = 4,        /* Network & Crypto core */
    .cpu_load_core1         = 18,       /* GUI & Input core */

    .rtc_hour               = 14,
    .rtc_min                = 28,
    .rtc_sec                = 0,
    .rtc_date_str           = "Wednesday, Sep 20",

    .opendev_status         = "Idle",
    .opendev_model          = "Sonnet 3.7",
    .terminal_sessions      = 1,
    .terminal_host          = "workstation (bash)",
    .editor_file            = "welcome.md",
    .editor_file_kb         = 14,
    .agy_bridge_online      = true,
    .agy_subagents_count    = 2
};

void devos_core_init(void)
{
    current_app_id = DEVOS_APP_COUNT;
    previous_app_id = DEVOS_APP_LAUNCHER;
}

void devos_core_register_app(devos_app_descriptor_t *app)
{
    if (app && app->id < DEVOS_APP_COUNT) {
        registered_apps[app->id] = app;
        if (app->init) {
            app->init();
        }
    }
}

void devos_core_switch_app(devos_app_id_t app_id)
{
    if (app_id >= DEVOS_APP_COUNT || !registered_apps[app_id]) {
        return;
    }

    if (current_app_id == app_id) {
        return;
    }

    /* Hide currently active app */
    if (current_app_id < DEVOS_APP_COUNT && registered_apps[current_app_id]) {
        if (registered_apps[current_app_id]->hide) {
            registered_apps[current_app_id]->hide();
        }
        if (registered_apps[current_app_id]->screen) {
            lv_obj_add_flag(registered_apps[current_app_id]->screen, LV_OBJ_FLAG_HIDDEN);
        }
    }

    previous_app_id = (current_app_id < DEVOS_APP_COUNT) ? current_app_id : DEVOS_APP_LAUNCHER;
    current_app_id = app_id;

    /* Show new app */
    if (registered_apps[current_app_id]->screen) {
        lv_obj_remove_flag(registered_apps[current_app_id]->screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(registered_apps[current_app_id]->screen);
    }
    if (registered_apps[current_app_id]->show) {
        registered_apps[current_app_id]->show();
    }
}

devos_app_id_t devos_core_get_current_app(void)
{
    return current_app_id;
}

devos_app_id_t devos_core_get_previous_app(void)
{
    return previous_app_id;
}

devos_app_descriptor_t *devos_core_get_app(devos_app_id_t app_id)
{
    if (app_id < DEVOS_APP_COUNT) {
        return registered_apps[app_id];
    }
    return NULL;
}

const devos_telemetry_t *devos_telemetry_get(void)
{
    return &telemetry_data;
}

void devos_telemetry_update(const devos_telemetry_t *new_telemetry)
{
    if (new_telemetry) {
        memcpy(&telemetry_data, new_telemetry, sizeof(devos_telemetry_t));
    }
}

void devos_telemetry_tick_sim(void)
{
    telemetry_data.rtc_sec++;
    if (telemetry_data.rtc_sec >= 60) {
        telemetry_data.rtc_sec = 0;
        telemetry_data.rtc_min++;
        if (telemetry_data.rtc_min >= 60) {
            telemetry_data.rtc_min = 0;
            telemetry_data.rtc_hour = (telemetry_data.rtc_hour + 1) % 24;
        }
    }
}

bool devos_core_dispatch_key(uint32_t key, uint8_t modifiers)
{
    /* 1. Global Hotkey: Home Screen Return (Fn + H) */
    if ((modifiers & DEVOS_MOD_FN) && (key == 'h' || key == 'H')) {
        devos_core_switch_app(DEVOS_APP_LAUNCHER);
        return true;
    }

    /* 2. Global Hotkey: Theme Toggle (Fn + T) */
    if ((modifiers & DEVOS_MOD_FN) && (key == 't' || key == 'T')) {
        devos_theme_toggle();
        return true;
    }

    /* 3. Global Hotkey: Switch Apps (Fn + 1 .. Fn + 6) */
    if (modifiers & DEVOS_MOD_FN) {
        if (key >= '1' && key <= '6') {
            devos_app_id_t target = (devos_app_id_t)(key - '0');
            devos_core_switch_app(target);
            return true;
        }
    }

    /* 4. Alt + Tab: Cycle Previous App */
    if ((modifiers & DEVOS_MOD_ALT) && (key == LV_KEY_NEXT || key == '\t')) {
        devos_core_switch_app(previous_app_id);
        return true;
    }

    /* 5. Forward to active app handler */
    if (registered_apps[current_app_id] && registered_apps[current_app_id]->handle_key) {
        if (registered_apps[current_app_id]->handle_key(key, modifiers)) {
            return true;
        }
    }

    /* 6. Fallback: Escape returns to Home Screen from any app if unhandled */
    if (key == LV_KEY_ESC && current_app_id != DEVOS_APP_LAUNCHER) {
        devos_core_switch_app(DEVOS_APP_LAUNCHER);
        return true;
    }

    return false;
}

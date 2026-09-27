#include "devos_core.h"
#include "devos_power.h"
#include <stdio.h>
#include <string.h>

static devos_theme_toggle_fn s_theme_toggle_cb = NULL;
static devos_brightness_step_fn s_brightness_step_cb = NULL;

static devos_app_descriptor_t *registered_apps[DEVOS_MAX_APPS] = {NULL};
static int registered_count = 0;
static devos_app_id_t current_app_id = DEVOS_APP_LAUNCHER;
static devos_app_id_t previous_app_id = DEVOS_APP_LAUNCHER;

#ifndef ESP_PLATFORM
/* Simulator: demo values so every launcher tile and the telemetry strip have
 * something to show on a desktop. */
static devos_telemetry_t telemetry_data = {
    .battery_voltage_mv     = 7820,
    .battery_current_ma     = -410,
    .battery_power_mw       = 3206,
    .battery_percent        = 94,
    .battery_charging       = false,
    .runtime_minutes_left   = 288,
    .battery_valid          = true,
    .battery_present        = true,

    .wifi_connected         = true,
    .wifi_state             = 3,
    .wifi_ssid              = "DevNet",
    .wifi_rssi              = -58,
    .local_ip               = "10.2.132.54",
    .tailscale_online       = true,
    .tailscale_ip           = "100.77.11.92",
    .tailscale_peers_online = 6,
    .tailscale_derp         = "Sydney (18ms)",

    .sd_mounted             = true,
    .sd_total_mb            = 31200,
    .sd_free_mb             = 29412,
    .free_psram_kb          = 28416,
    .free_sram_kb           = 428,

    .cpu_load_core0         = 4,
    .cpu_load_core1         = 18,

    .rtc_hour               = 14,
    .rtc_min                = 28,
    .rtc_sec                = 0,
    .rtc_date_str           = "Wednesday, Sep 20",
    .time_valid             = true,

    .terminal_sessions      = 2,
    .terminal_host          = "workstation (bash)",
    .terminal_requested_host = "",
    .editor_file            = "welcome.md",
    .editor_file_kb         = 14,
};
#else
/* Target: start empty. devos_sysmon fills in hardware/network data within the
 * first second and apps update their own fields; nothing here is invented. */
static devos_telemetry_t telemetry_data;
#endif

#ifndef ESP_PLATFORM
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static void detect_local_ip(char *out_ip, size_t max_len)
{
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) return;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(ifa->ifa_name, "lo") == 0) continue;
        if (strncmp(ifa->ifa_name, "tailscale", 9) == 0) continue;
        if (strncmp(ifa->ifa_name, "docker", 6) == 0) continue;
        if (strncmp(ifa->ifa_name, "br-", 3) == 0) continue;

        struct sockaddr_in *pAddr = (struct sockaddr_in *)ifa->ifa_addr;
        inet_ntop(AF_INET, &pAddr->sin_addr, out_ip, max_len);
        break;
    }
    freeifaddrs(ifaddr);
}
#endif

void devos_core_init(void)
{
    current_app_id = DEVOS_APP_NONE;
    previous_app_id = DEVOS_APP_LAUNCHER;

#ifndef ESP_PLATFORM
    detect_local_ip(telemetry_data.local_ip, sizeof(telemetry_data.local_ip));
#endif
}

void devos_core_register_app(devos_app_descriptor_t *app)
{
    if (!app || registered_count >= DEVOS_MAX_APPS) return;
    /* ponytail: id/uid collision replaces (re-register on reboot is harmless) */
    for (int i = 0; i < registered_count; i++) {
        bool match = false;
        if (app->uid && registered_apps[i]->uid && strcmp(registered_apps[i]->uid, app->uid) == 0) {
            match = true;
        } else if (app->id != DEVOS_APP_LAUNCHER && registered_apps[i]->id == app->id) {
            match = true;
        }
        if (match) {
            registered_apps[i] = app;
            if (app->init) app->init();
            return;
        }
    }
    /* Auto-assign non-zero ID if not assigned and not launcher */
    if (app->id == DEVOS_APP_LAUNCHER && (!app->uid || strcmp(app->uid, "launcher") != 0)) {
        app->id = (devos_app_id_t)(100 + registered_count);
    }
    registered_apps[registered_count++] = app;
    if (app->init) {
        app->init();
    }
}

static devos_app_descriptor_t *find_by_id(devos_app_id_t app_id)
{
    for (int i = 0; i < registered_count; i++) {
        if (registered_apps[i] && registered_apps[i]->id == app_id) {
            return registered_apps[i];
        }
    }
    return NULL;
}

int devos_core_app_count(void) { return registered_count; }

devos_app_descriptor_t *devos_core_get_app_at(int index)
{
    if (index < 0 || index >= registered_count) return NULL;
    return registered_apps[index];
}

devos_app_descriptor_t *devos_core_find_app(const char *uid)
{
    if (!uid) return NULL;
    for (int i = 0; i < registered_count; i++) {
        if (registered_apps[i] && registered_apps[i]->uid &&
            strcmp(registered_apps[i]->uid, uid) == 0) {
            return registered_apps[i];
        }
    }
    return NULL;
}

void devos_core_switch_app_by_uid(const char *uid)
{
    devos_app_descriptor_t *app = devos_core_find_app(uid);
    if (app) {
        devos_core_switch_app(app->id);
    }
}

static struct {
    char uid[24];
    char action[24];
    char arg[512];
} s_intent;

bool devos_core_open_with(const char *uid, const char *action, const char *arg)
{
    devos_app_descriptor_t *app = devos_core_find_app(uid);
    if (!app) return false;
    if (!strcmp(uid, "terminal") && action && !strcmp(action, "ssh")) {
        /* the terminal's older mechanism */
        snprintf(telemetry_data.terminal_requested_host, sizeof(telemetry_data.terminal_requested_host), "%s",
                 arg ? arg : "");
    } else {
        snprintf(s_intent.uid, sizeof(s_intent.uid), "%s", uid);
        snprintf(s_intent.action, sizeof(s_intent.action), "%s", action ? action : "");
        snprintf(s_intent.arg, sizeof(s_intent.arg), "%s", arg ? arg : "");
    }
    if (current_app_id == app->id && app->show) app->show();   /* already there: re-check */
    else devos_core_switch_app(app->id);
    return true;
}

bool devos_core_take_intent(const char *uid, char *action, size_t action_cap, char *arg, size_t arg_cap)
{
    if (!uid || !s_intent.uid[0] || strcmp(uid, s_intent.uid) != 0) return false;
    if (action && action_cap) snprintf(action, action_cap, "%s", s_intent.action);
    if (arg && arg_cap) snprintf(arg, arg_cap, "%s", s_intent.arg);
    s_intent.uid[0] = '\0';
    return true;
}

void devos_core_switch_app(devos_app_id_t app_id)
{
    devos_app_descriptor_t *target = find_by_id(app_id);
    if (!target) {
        return;
    }

    if (current_app_id == app_id) {
        return;
    }

    /* Hide currently active app */
    devos_app_descriptor_t *cur = find_by_id(current_app_id);
    if (cur) {
        if (cur->hide) cur->hide();
        if (cur->screen) {
            lv_obj_add_flag(cur->screen, LV_OBJ_FLAG_HIDDEN);
        }
    }

    previous_app_id = current_app_id;
    current_app_id = app_id;

    /* Show new app */
    if (target->screen) {
        lv_obj_remove_flag(target->screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(target->screen);
    }
    if (target->show) {
        target->show();
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
    return find_by_id(app_id);
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

void devos_core_set_theme_toggle_cb(devos_theme_toggle_fn cb)
{
    s_theme_toggle_cb = cb;
}

void devos_core_set_brightness_step_cb(devos_brightness_step_fn cb)
{
    s_brightness_step_cb = cb;
}

bool devos_core_dispatch_key(uint32_t key, uint8_t modifiers)
{
    /* Any keypress is activity: wakes from dim/sleep, resets idle. */
    devos_power_activity();

    /* 1. Global Hotkey: Home Screen Return (Sym + H) */
    if ((modifiers & DEVOS_MOD_FN) && (key == 'h' || key == 'H')) {
        devos_core_switch_app(DEVOS_APP_LAUNCHER);
        return true;
    }

    /* 2. Global Hotkey: Theme Toggle (Sym + T) */
    if ((modifiers & DEVOS_MOD_FN) && (key == 't' || key == 'T')) {
        if (s_theme_toggle_cb) {
            s_theme_toggle_cb();
        }
        return true;
    }

    /* 2b. Global Hotkey: Brightness (Sym + - / Sym + +) */
    if ((modifiers & DEVOS_MOD_FN) && (key == '-' || key == '+' || key == '=')) {
        if (s_brightness_step_cb) {
            s_brightness_step_cb(key == '-' ? -10 : 10);
        }
        return true;
    }

    /* 3. Global Hotkey: Switch Apps (Sym + 1 .. Sym + 8) */
    if (modifiers & DEVOS_MOD_FN) {
        if (key >= '1' && key <= '8') {
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
    {
        devos_app_descriptor_t *cur = find_by_id(current_app_id);
        if (cur && cur->handle_key && cur->handle_key(key, modifiers)) {
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

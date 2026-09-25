#pragma once

#include "lvgl.h"
#include "devos_config.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Modifier bitmasks */
#define DEVOS_MOD_NONE      0x00
#define DEVOS_MOD_CTRL      0x01
#define DEVOS_MOD_SHIFT     0x02
#define DEVOS_MOD_ALT       0x04
/* "System" modifier. The Tab5 keyboard has no Fn key: tab5_keyboard reports
 * this while Sym is held with a key whose Sym layer is otherwise unused
 * (letters, digits, arrows, Space, Esc, Tab, Enter, Backspace, - and +).
 * User-facing text calls it "Sym". */
#define DEVOS_MOD_FN        0x08

/* Registry capacity (Phase 7: up to 32 self-registering apps) */
#define DEVOS_MAX_APPS 32
#define DEVOS_MAX_UID 24

/* Synthetic page keys (no ASCII equivalent; see sim key watcher) */
#define DEVOS_KEY_PGUP ((uint32_t)0x10001u)
#define DEVOS_KEY_PGDN ((uint32_t)0x10002u)

typedef struct {
    devos_app_id_t id;          /* small int, back-compat switch key */
    const char *uid;            /* stable string id ("terminal") for layout */
    const char *name;           /* short tile name */
    const char *title;          /* tile title */
    const char *subtitle;
    const char *icon;           /* LVGL symbol string, may be NULL */
    const char *category;       /* may be NULL */
    lv_obj_t *screen;
    void (*init)(void);
    void (*show)(void);
    void (*hide)(void);
    bool (*handle_key)(uint32_t key, uint8_t modifiers);
    /* Live tile lines; NULL = static subtitle fallback. Return count. */
    int (*get_telemetry_lines)(char lines[3][64]);
} devos_app_descriptor_t;

typedef struct {
    /* Power Telemetry (INA226) */
    uint16_t battery_voltage_mv;
    int16_t  battery_current_ma;
    uint16_t battery_power_mw;
    uint8_t  battery_percent;
    bool     battery_charging;
    uint16_t runtime_minutes_left;
    bool     battery_valid;         /* power monitor answered */
    bool     battery_present;       /* a pack is fitted (false = USB power only) */

    /* Network Telemetry */
    bool     wifi_connected;
    uint8_t  wifi_state;            /* devos_wifi_state_t */
    char     wifi_ssid[33];
    int8_t   wifi_rssi;
    char     local_ip[20];
    bool     tailscale_online;
    char     tailscale_ip[20];
    uint8_t  tailscale_peers_online;
    char     tailscale_derp[20];

    /* Storage & Memory */
    bool     sd_mounted;
    uint32_t sd_total_mb;
    uint32_t sd_free_mb;
    uint32_t free_psram_kb;
    uint32_t free_sram_kb;
    uint32_t psram_total_kb;
    uint32_t sram_min_free_kb;      /* low-water mark since boot */
    uint32_t uptime_s;

    /* CPU Telemetry */
    uint8_t  cpu_load_core0;
    uint8_t  cpu_load_core1;

    /* Real-Time Clock */
    uint8_t  rtc_hour;
    uint8_t  rtc_min;
    uint8_t  rtc_sec;
    char     rtc_date_str[32];
    bool     time_valid;            /* wall clock set (RTC or NTP) */

    /* App Subsystem Statuses */
    char     opendev_status[24];
    char     opendev_model[24];
    uint8_t  terminal_sessions;
    char     terminal_host[32];
    char     terminal_requested_host[64];
    char     editor_file[32];
    uint32_t editor_file_kb;
    bool     agy_bridge_online;
    uint8_t  agy_subagents_count;
} devos_telemetry_t;

/* Core Engine API */
void devos_core_init(void);
void devos_core_register_app(devos_app_descriptor_t *app);
void devos_core_switch_app(devos_app_id_t app_id);
devos_app_id_t devos_core_get_current_app(void);
devos_app_id_t devos_core_get_previous_app(void);
devos_app_descriptor_t *devos_core_get_app(devos_app_id_t app_id);
/* Registry traversal (registration order) */
int devos_core_app_count(void);
devos_app_descriptor_t *devos_core_get_app_at(int index);
devos_app_descriptor_t *devos_core_find_app(const char *uid);
void devos_core_switch_app_by_uid(const char *uid);

/* Telemetry API */
const devos_telemetry_t *devos_telemetry_get(void);
void devos_telemetry_update(const devos_telemetry_t *new_telemetry);
void devos_telemetry_tick_sim(void);
/* Brightness step for the global Sym+-/Sym++ hotkeys (set by main). */
typedef void (*devos_brightness_step_fn)(int delta);
void devos_core_set_brightness_step_cb(devos_brightness_step_fn cb);

/* Input & Hotkey Dispatcher */
typedef void (*devos_theme_toggle_fn)(void);
void devos_core_set_theme_toggle_cb(devos_theme_toggle_fn cb);
bool devos_core_dispatch_key(uint32_t key, uint8_t modifiers);
void devos_core_toggle_focus_mode(void);
void devos_core_toggle_left_panel(void);
void devos_core_toggle_right_panel(void);

#ifdef __cplusplus
}
#endif

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
#define DEVOS_MOD_FN        0x08

typedef struct {
    devos_app_id_t id;
    const char *name;
    const char *title;
    const char *subtitle;
    lv_obj_t *screen;
    void (*init)(void);
    void (*show)(void);
    void (*hide)(void);
    bool (*handle_key)(uint32_t key, uint8_t modifiers);
} devos_app_descriptor_t;

typedef struct {
    /* Power Telemetry (INA226) */
    uint16_t battery_voltage_mv;
    int16_t  battery_current_ma;
    uint16_t battery_power_mw;
    uint8_t  battery_percent;
    bool     battery_charging;
    uint16_t runtime_minutes_left;

    /* Network Telemetry */
    bool     wifi_connected;
    char     wifi_ssid[32];
    int8_t   wifi_rssi;
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

    /* CPU Telemetry */
    uint8_t  cpu_load_core0;
    uint8_t  cpu_load_core1;

    /* Real-Time Clock */
    uint8_t  rtc_hour;
    uint8_t  rtc_min;
    uint8_t  rtc_sec;
    char     rtc_date_str[32];

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

/* Telemetry API */
const devos_telemetry_t *devos_telemetry_get(void);
void devos_telemetry_update(const devos_telemetry_t *new_telemetry);
void devos_telemetry_tick_sim(void);

/* Input & Hotkey Dispatcher */
bool devos_core_dispatch_key(uint32_t key, uint8_t modifiers);
void devos_core_toggle_focus_mode(void);
void devos_core_toggle_left_panel(void);
void devos_core_toggle_right_panel(void);

#ifdef __cplusplus
}
#endif

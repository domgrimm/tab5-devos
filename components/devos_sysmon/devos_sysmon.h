#pragma once

/* devos_sysmon: the source of truth for the system telemetry shown on the top
 * bar, home screen and Settings.
 *
 * A Core 0 task samples the hardware and network once a second (battery via
 * INA226, Wi-Fi, Tailscale, SD card, heap, per-core CPU load) into a locked
 * snapshot; the GUI task calls devos_sysmon_apply() at 1 Hz to copy it into
 * devos_telemetry (only the fields sysmon owns, so app-owned fields survive).
 * Nothing here touches the radio or I2C from the GUI task.
 *
 * It also owns wall-clock time: restored from the RX8130 RTC at boot, synced
 * over SNTP once Wi-Fi is up (and written back to the RTC), displayed in the
 * user's time zone (persisted).
 */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEVOS_TIME_UNSET = 0,   /* no RTC time and no network sync yet */
    DEVOS_TIME_RTC,         /* restored from the hardware clock */
    DEVOS_TIME_NTP,         /* synced from the internet */
} devos_time_source_t;

typedef struct {
    const char *name;       /* shown in Settings */
    const char *posix;      /* POSIX TZ string */
} devos_timezone_t;

/* Call once during boot, after devos_net_init() and the BSP. */
void devos_sysmon_init(void);
/* GUI task, 1 Hz: publish the latest snapshot into devos_telemetry. */
void devos_sysmon_apply(void);

devos_time_source_t devos_sysmon_time_source(void);
/* Local time of the last successful NTP sync (0 if never). */
time_t devos_sysmon_last_sync(void);

/* Compact, locked snapshot for engines that are not the GUI task (the Jobs
 * scheduler). Never read GUI-owned telemetry from a worker. Plain fields, no
 * LVGL. On the simulator the hardware samples are zeroed. */
typedef struct {
    bool     battery_valid, battery_present, charging;
    uint8_t  battery_percent;
    bool     wifi_connected;
    char     wifi_ssid[33];
    char     local_ip[20];
    int8_t   wifi_rssi;
    bool     time_valid;
    uint32_t uptime_s;
    uint8_t  cpu_core0, cpu_core1;
    uint32_t psram_free_kb, sram_free_kb, sram_largest_kb;
} devos_sysmon_snapshot_t;
void devos_sysmon_get_snapshot(devos_sysmon_snapshot_t *out);

/* Time zones offered in Settings; index into this list is persisted. */
const devos_timezone_t *devos_sysmon_timezones(int *count);
int  devos_sysmon_timezone_index(void);
void devos_sysmon_set_timezone_index(int index);

#ifdef __cplusplus
}
#endif

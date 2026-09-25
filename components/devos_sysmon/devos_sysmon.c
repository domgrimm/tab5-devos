/* devos_sysmon: see devos_sysmon.h. */
#include "devos_sysmon.h"
#include "devos_core.h"
#include "devos_config.h"
#include "devos_net.h"
#include "devos_storage.h"
#include "devos_tailnet.h"
#include "bsp_tab5.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "nvs.h"
static const char *TAG = "sysmon";
#define SYSMON_NVS_NS "devos"
#endif

/* Alphabetic zone abbreviations only (no <+07> quoting) for newlib tzset. */
static const devos_timezone_t s_zones[] = {
    { "UTC",                              "UTC0" },
    { "Honolulu (HST)",                   "HST10" },
    { "Anchorage (AKST/AKDT)",            "AKST9AKDT,M3.2.0,M11.1.0" },
    { "Los Angeles (PST/PDT)",            "PST8PDT,M3.2.0,M11.1.0" },
    { "Denver (MST/MDT)",                 "MST7MDT,M3.2.0,M11.1.0" },
    { "Phoenix (MST)",                    "MST7" },
    { "Chicago (CST/CDT)",                "CST6CDT,M3.2.0,M11.1.0" },
    { "New York (EST/EDT)",               "EST5EDT,M3.2.0,M11.1.0" },
    { "Halifax (AST/ADT)",                "AST4ADT,M3.2.0,M11.1.0" },
    { "Sao Paulo (BRT)",                  "BRT3" },
    { "London (GMT/BST)",                 "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Lagos (WAT)",                      "WAT-1" },
    { "Berlin / Paris (CET/CEST)",        "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Johannesburg (SAST)",              "SAST-2" },
    { "Athens / Helsinki (EET/EEST)",     "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Moscow (MSK)",                     "MSK-3" },
    { "Dubai (GST)",                      "GST-4" },
    { "India (IST)",                      "IST-5:30" },
    { "Bangkok / Jakarta (ICT)",          "ICT-7" },
    { "Singapore / Beijing / Perth",      "SGT-8" },
    { "Tokyo / Seoul (JST)",              "JST-9" },
    { "Darwin (ACST)",                    "ACST-9:30" },
    { "Adelaide (ACST/ACDT)",             "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
    { "Brisbane (AEST)",                  "AEST-10" },
    { "Sydney / Melbourne (AEST/AEDT)",   "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Auckland (NZST/NZDT)",             "NZST-12NZDT,M9.5.0,M4.1.0/3" },
};
#define ZONE_COUNT ((int)(sizeof(s_zones) / sizeof(s_zones[0])))

static int s_tz_index = 0;
static volatile devos_time_source_t s_time_src = DEVOS_TIME_UNSET;
static volatile time_t s_last_sync = 0;

/* 2024-01-01T00:00:00Z: anything earlier means the clock was never set. */
#define TIME_VALID_AFTER 1704067200

static void apply_timezone(void)
{
    setenv("TZ", s_zones[s_tz_index].posix, 1);
    tzset();
}

const devos_timezone_t *devos_sysmon_timezones(int *count)
{
    if (count) *count = ZONE_COUNT;
    return s_zones;
}

int devos_sysmon_timezone_index(void) { return s_tz_index; }

void devos_sysmon_set_timezone_index(int index)
{
    if (index < 0 || index >= ZONE_COUNT) return;
    s_tz_index = index;
    apply_timezone();
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open(SYSMON_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "tz", s_zones[index].posix);
        nvs_commit(h);
        nvs_close(h);
    }
#endif
}

devos_time_source_t devos_sysmon_time_source(void) { return s_time_src; }
time_t devos_sysmon_last_sync(void) { return s_last_sync; }

/* Wall-clock fields shared by both platforms. */
static void fill_time(devos_telemetry_t *t)
{
    time_t now = time(NULL);
    t->time_valid = now >= TIME_VALID_AFTER;
    if (!t->time_valid) {
        t->rtc_hour = t->rtc_min = t->rtc_sec = 0;
        snprintf(t->rtc_date_str, sizeof(t->rtc_date_str), "Clock not set");
        return;
    }
    struct tm lt;
    localtime_r(&now, &lt);
    t->rtc_hour = (uint8_t)lt.tm_hour;
    t->rtc_min = (uint8_t)lt.tm_min;
    t->rtc_sec = (uint8_t)lt.tm_sec;
    char wd[12], mon[6];
    strftime(wd, sizeof(wd), "%A", &lt);
    strftime(mon, sizeof(mon), "%b", &lt);
    snprintf(t->rtc_date_str, sizeof(t->rtc_date_str), "%s, %s %d", wd, mon, lt.tm_mday);
}

#ifdef ESP_PLATFORM
/* ========================================================================= */
typedef struct {
    bool     bat_valid;
    bool     bat_present;
    bool     charging;
    bool     chg_stat;
    uint16_t mv;
    int32_t  ma;
    uint32_t mw;
    uint8_t  pct;
    uint16_t runtime_min;

    devos_wifi_status_t wifi;

    bool     ts_online;
    char     ts_ip[20];
    uint8_t  ts_peers;
    char     ts_derp[20];

    bool     sd_mounted;
    uint32_t sd_total_mb;
    uint32_t sd_free_mb;

    uint32_t psram_free_kb;
    uint32_t psram_total_kb;
    uint32_t sram_free_kb;
    uint32_t sram_min_kb;
    uint8_t  cpu[2];
    uint32_t uptime_s;
} snapshot_t;

static snapshot_t s_snap;
static SemaphoreHandle_t s_lock = NULL;
static volatile bool s_rtc_writeback = false;
static bool s_sntp_started = false;

/* timegm() replacement (newlib lacks it): days-from-civil, H. Hinnant. */
static time_t utc_mktime(const struct tm *t)
{
    int y = t->tm_year + 1900;
    int m = t->tm_mon + 1;
    int d = t->tm_mday;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = (long long)era * 146097 + doe - 719468;
    return (time_t)(days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec);
}

static void on_sntp_sync(struct timeval *tv)
{
    s_time_src = DEVOS_TIME_NTP;
    s_last_sync = tv ? tv->tv_sec : time(NULL);
    s_rtc_writeback = true;   /* I2C write happens on the sysmon task */
}

static void start_sntp(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = on_sntp_sync;
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        s_sntp_started = true;
        ESP_LOGI(TAG, "SNTP started (pool.ntp.org)");
    }
}

static void sample_power(snapshot_t *n)
{
    static uint32_t ema_mw = 0;
    bsp_tab5_power_t p;
    n->bat_valid = bsp_tab5_read_power(&p);
    if (!n->bat_valid) {
        n->bat_present = false;
        return;
    }
    n->mv = p.voltage_mv;
    n->ma = p.current_ma;
    n->mw = p.power_mw;
    n->charging = p.charging;
    n->chg_stat = p.chg_stat;
    /* Below ~5.5 V there is no 2S pack on the terminals: running on USB. */
    n->bat_present = p.voltage_mv >= 5500 && p.voltage_mv <= 9000;

    /* Per-cell level, same mapping as M5Unified for the Tab5. */
    int cell = p.voltage_mv / 2;
    int level = (cell - 3300) * 100 / 800;
    n->pct = (uint8_t)(level < 0 ? 0 : level > 100 ? 100 : level);

    ema_mw = ema_mw ? (ema_mw * 4 + p.power_mw) / 5 : p.power_mw;
    if (n->bat_present && p.current_ma < -10 && ema_mw > 100) {
        uint32_t remaining_mwh = (uint32_t)n->pct * DEVOS_BATTERY_CAPACITY_MWH / 100;
        uint32_t mins = remaining_mwh * 60 / ema_mw;
        n->runtime_min = (uint16_t)(mins > 5999 ? 5999 : mins);
    } else {
        n->runtime_min = 0;
    }
}

static void sample_cpu(snapshot_t *n)
{
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    static uint64_t last_idle[2], last_wall;
    uint64_t wall = (uint64_t)esp_timer_get_time();
    uint64_t dt = wall - last_wall;
    for (int c = 0; c < 2; c++) {
        uint64_t idle = (uint64_t)ulTaskGetIdleRunTimeCounterForCore(c);
        if (last_wall && dt > 0) {
            uint64_t di = idle - last_idle[c];
            int load = 100 - (int)(di * 100 / dt);
            n->cpu[c] = (uint8_t)(load < 0 ? 0 : load > 100 ? 100 : load);
        }
        last_idle[c] = idle;
    }
    last_wall = wall;
#else
    n->cpu[0] = n->cpu[1] = 0;
#endif
}

static void sample(snapshot_t *n, uint32_t tick)
{
    sample_power(n);

    if (tick % 5 == 0) devos_net_wifi_refresh_rssi();
    devos_net_wifi_get_status(&n->wifi);

    static devos_ts_info_t ts;             /* sysmon task only */
    devos_tailnet_get_info(&ts);
    n->ts_online = ts.state == DEVOS_TS_CONNECTED && ts.ip[0];
    if (n->ts_online) {
        snprintf(n->ts_ip, sizeof(n->ts_ip), "%.19s", ts.ip);
        snprintf(n->ts_derp, sizeof(n->ts_derp), "%.19s", ts.derp);
        n->ts_peers = (uint8_t)devos_tailnet_peer_count();
    } else {
        n->ts_ip[0] = n->ts_derp[0] = '\0';
        n->ts_peers = 0;
    }

    n->sd_mounted = devos_storage_is_mounted();
    if (n->sd_mounted && (tick % 30 == 0 || n->sd_total_mb == 0)) {
        devos_storage_refresh_stats();
    }
    n->sd_total_mb = n->sd_mounted ? devos_storage_get_total_mb() : 0;
    n->sd_free_mb = n->sd_mounted ? devos_storage_get_free_mb() : 0;

    n->psram_free_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    n->psram_total_kb = (uint32_t)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024);
    n->sram_free_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    n->sram_min_kb = (uint32_t)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
    sample_cpu(n);
    n->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static void sysmon_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t tick = 0;
    snapshot_t n;
    memset(&n, 0, sizeof(n));
    for (;;) {
        sample(&n, tick);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_snap = n;
        xSemaphoreGive(s_lock);

        if (n.wifi.state == DEVOS_WIFI_STATE_CONNECTED && !s_sntp_started) {
            start_sntp();
        }
        if (s_rtc_writeback) {
            s_rtc_writeback = false;
            time_t now = time(NULL);
            struct tm utc;
            gmtime_r(&now, &utc);
            bool ok = bsp_tab5_rtc_set(&utc);
            ESP_LOGI(TAG, "Clock synced from NTP%s", ok ? "; RTC updated" : " (RTC write failed)");
        }
        tick++;
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000));
    }
}

void devos_sysmon_init(void)
{
    /* Time zone first, so the restored clock displays correctly at once. */
    nvs_handle_t h;
    if (nvs_open(SYSMON_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        char tz[48];
        size_t len = sizeof(tz);
        if (nvs_get_str(h, "tz", tz, &len) == ESP_OK) {
            for (int i = 0; i < ZONE_COUNT; i++) {
                if (strcmp(s_zones[i].posix, tz) == 0) { s_tz_index = i; break; }
            }
        }
        nvs_close(h);
    }
    apply_timezone();

    struct tm utc;
    if (bsp_tab5_rtc_get(&utc) && utc.tm_year + 1900 >= 2024) {
        struct timeval tv = { .tv_sec = utc_mktime(&utc), .tv_usec = 0 };
        settimeofday(&tv, NULL);
        s_time_src = DEVOS_TIME_RTC;
        ESP_LOGI(TAG, "Clock restored from RTC: %04d-%02d-%02d %02d:%02d:%02d UTC",
                 utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    } else {
        ESP_LOGW(TAG, "RTC has no valid time; clock unset until NTP sync");
    }

    s_lock = xSemaphoreCreateMutex();
    memset(&s_snap, 0, sizeof(s_snap));
    /* Headroom for the ESP-Hosted RPC (RSSI) and FATFS free-space scan. */
    xTaskCreatePinnedToCore(sysmon_task, "sysmon", 6144, NULL, 3, NULL, DEVOS_CORE_NET_CRYPTO);
}

void devos_sysmon_apply(void)
{
    if (!s_lock) return;
    snapshot_t n;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    n = s_snap;
    xSemaphoreGive(s_lock);

    devos_telemetry_t t;
    memcpy(&t, devos_telemetry_get(), sizeof(t));

    t.battery_valid = n.bat_valid;
    t.battery_present = n.bat_present;
    t.battery_voltage_mv = n.mv;
    t.battery_current_ma = (int16_t)(n.ma < -32768 ? -32768 : n.ma > 32767 ? 32767 : n.ma);
    t.battery_power_mw = (uint16_t)(n.mw > 65535 ? 65535 : n.mw);
    t.battery_percent = n.pct;
    t.battery_charging = n.charging;
    t.charger_signal = n.chg_stat;
    t.runtime_minutes_left = n.runtime_min;

    t.wifi_state = (uint8_t)n.wifi.state;
    t.wifi_connected = n.wifi.state == DEVOS_WIFI_STATE_CONNECTED;
    snprintf(t.wifi_ssid, sizeof(t.wifi_ssid), "%s", n.wifi.ssid);
    t.wifi_rssi = n.wifi.rssi;
    snprintf(t.local_ip, sizeof(t.local_ip), "%s", t.wifi_connected ? n.wifi.ip : "");

    t.tailscale_online = n.ts_online;
    snprintf(t.tailscale_ip, sizeof(t.tailscale_ip), "%s", n.ts_ip);
    snprintf(t.tailscale_derp, sizeof(t.tailscale_derp), "%s", n.ts_derp);
    t.tailscale_peers_online = n.ts_peers;

    t.sd_mounted = n.sd_mounted;
    t.sd_total_mb = n.sd_total_mb;
    t.sd_free_mb = n.sd_free_mb;
    t.free_psram_kb = n.psram_free_kb;
    t.psram_total_kb = n.psram_total_kb;
    t.free_sram_kb = n.sram_free_kb;
    t.sram_min_free_kb = n.sram_min_kb;
    t.cpu_load_core0 = n.cpu[0];
    t.cpu_load_core1 = n.cpu[1];
    t.uptime_s = n.uptime_s;

    fill_time(&t);
    devos_telemetry_update(&t);
}

#else  /* !ESP_PLATFORM — simulator: host clock + simulated Wi-Fi, demo power */

static time_t s_boot = 0;

void devos_sysmon_init(void)
{
    s_boot = time(NULL);
    s_time_src = DEVOS_TIME_NTP;
    s_last_sync = s_boot;
    /* Keep the host's own zone unless the user picks one. */
}

void devos_sysmon_apply(void)
{
    devos_telemetry_t t;
    memcpy(&t, devos_telemetry_get(), sizeof(t));
    devos_wifi_status_t w;
    devos_net_wifi_get_status(&w);
    t.wifi_state = (uint8_t)w.state;
    t.wifi_connected = w.state == DEVOS_WIFI_STATE_CONNECTED;
    snprintf(t.wifi_ssid, sizeof(t.wifi_ssid), "%s", w.ssid);
    t.wifi_rssi = w.rssi;
    devos_ts_info_t ts;
    devos_tailnet_get_info(&ts);
    t.tailscale_online = ts.state == DEVOS_TS_CONNECTED && ts.ip[0];
    snprintf(t.tailscale_ip, sizeof(t.tailscale_ip), "%.19s", t.tailscale_online ? ts.ip : "");
    snprintf(t.tailscale_derp, sizeof(t.tailscale_derp), "%.19s", t.tailscale_online ? ts.derp : "");
    t.tailscale_peers_online = t.tailscale_online ? (uint8_t)ts.peer_count : 0;
    t.uptime_s = (uint32_t)(time(NULL) - s_boot);
    fill_time(&t);
    devos_telemetry_update(&t);
}

#endif /* ESP_PLATFORM */

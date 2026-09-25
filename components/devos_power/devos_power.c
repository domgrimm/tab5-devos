/* devos_power: see devos_power.h. */
#include "devos_power.h"
#include <stddef.h>

#ifdef ESP_PLATFORM
#include "nvs.h"
#define POWER_NVS_NS "devos"
#endif

static uint32_t s_last_activity = 0;
static uint32_t s_now = 0;
static devos_power_mode_t s_mode = DEVOS_POWER_ACTIVE;
static bool s_init = false;

static devos_power_backlight_fn s_backlight_cb = NULL;
static int s_user_brightness = 100;
static int s_applied_backlight = -1;
static uint32_t s_dim_after_s = DEVOS_POWER_DIM_AFTER_S;
static uint32_t s_sleep_after_s = DEVOS_POWER_SLEEP_AFTER_S;

static void apply_backlight(void)
{
    if (!s_backlight_cb) return;
    int pct = s_user_brightness;
    if (s_mode == DEVOS_POWER_DIMMED) {
        pct = pct * 40 / 100;
        if (pct < 5) pct = 5;
    } else if (s_mode == DEVOS_POWER_SLEEP) {
        pct = 0;
    }
    if (pct != s_applied_backlight) {
        s_applied_backlight = pct;
        s_backlight_cb(pct);
    }
}

#ifdef ESP_PLATFORM
static void save_prefs(void)
{
    nvs_handle_t h;
    if (nvs_open(POWER_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "bright", (uint8_t)s_user_brightness);
    nvs_set_u32(h, "dim_s", s_dim_after_s);
    nvs_set_u32(h, "sleep_s", s_sleep_after_s);
    nvs_commit(h);
    nvs_close(h);
}
#else
static void save_prefs(void) {}
#endif

void devos_power_init(void)
{
    s_last_activity = 0;
    s_now = 0;
    s_mode = DEVOS_POWER_ACTIVE;
    s_init = true;
}

void devos_power_load_prefs(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open(POWER_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t b;
        uint32_t v;
        if (nvs_get_u8(h, "bright", &b) == ESP_OK && b >= 5 && b <= 100) s_user_brightness = b;
        if (nvs_get_u32(h, "dim_s", &v) == ESP_OK) s_dim_after_s = v;
        if (nvs_get_u32(h, "sleep_s", &v) == ESP_OK) s_sleep_after_s = v;
        nvs_close(h);
    }
#endif
    apply_backlight();
}

void devos_power_activity(void)
{
    if (!s_init) return;
    s_last_activity = s_now;
    if (s_mode != DEVOS_POWER_ACTIVE) {
        s_mode = DEVOS_POWER_ACTIVE;
        apply_backlight();
    }
}

void devos_power_poll(uint32_t now_s)
{
    if (!s_init) return;
    s_now = now_s;
    uint32_t idle = now_s - s_last_activity;
    devos_power_mode_t want = DEVOS_POWER_ACTIVE;
    if (s_sleep_after_s && idle >= s_sleep_after_s) {
        want = DEVOS_POWER_SLEEP;
    } else if (s_dim_after_s && idle >= s_dim_after_s) {
        want = DEVOS_POWER_DIMMED;
    }
    /* A manual "sleep now" sticks until the next input. */
    if (s_mode == DEVOS_POWER_SLEEP && want != DEVOS_POWER_SLEEP) return;
    if (want != s_mode) {
        s_mode = want;
        apply_backlight();
    }
}

devos_power_mode_t devos_power_mode(void) { return s_mode; }

uint32_t devos_power_idle_s(void)
{
    return s_init ? s_now - s_last_activity : 0;
}

int devos_power_brightness(void)
{
    if (s_mode == DEVOS_POWER_SLEEP) return 0;
    if (s_mode == DEVOS_POWER_DIMMED) return 40;
    return 100;
}

void devos_power_sleep_now(void)
{
    if (!s_init) return;
    s_mode = DEVOS_POWER_SLEEP;
    apply_backlight();
}

const char *devos_power_mode_text(void)
{
    if (s_mode == DEVOS_POWER_SLEEP) return "Sleep";
    if (s_mode == DEVOS_POWER_DIMMED) return "Dimmed";
    return "Active";
}

void devos_power_set_backlight_cb(devos_power_backlight_fn cb)
{
    s_backlight_cb = cb;
    s_applied_backlight = -1;
    apply_backlight();
}

void devos_power_set_user_brightness(int percent)
{
    if (percent < 5) percent = 5;
    if (percent > 100) percent = 100;
    if (percent == s_user_brightness) return;
    s_user_brightness = percent;
    apply_backlight();
    save_prefs();
}

int devos_power_user_brightness(void) { return s_user_brightness; }

void devos_power_step_brightness(int delta)
{
    devos_power_set_user_brightness(s_user_brightness + delta);
}

void devos_power_set_timeouts(uint32_t dim_after_s, uint32_t sleep_after_s)
{
    s_dim_after_s = dim_after_s;
    s_sleep_after_s = sleep_after_s;
    save_prefs();
}

uint32_t devos_power_dim_after_s(void) { return s_dim_after_s; }
uint32_t devos_power_sleep_after_s(void) { return s_sleep_after_s; }

/* devos_power: see devos_power.h. */
#include "devos_power.h"

static uint32_t s_last_activity = 0;
static uint32_t s_now = 0;
static devos_power_mode_t s_mode = DEVOS_POWER_ACTIVE;
static bool s_init = false;

void devos_power_init(void)
{
    s_last_activity = 0;
    s_now = 0;
    s_mode = DEVOS_POWER_ACTIVE;
    s_init = true;
}

void devos_power_activity(void)
{
    if (!s_init) return;
    s_last_activity = s_now;
    if (s_mode != DEVOS_POWER_ACTIVE) {
        s_mode = DEVOS_POWER_ACTIVE;
#ifdef ESP_PLATFORM
        /* BSP: restore backlight; abort light sleep on next wake source. */
#endif
    }
}

void devos_power_poll(uint32_t now_s)
{
    if (!s_init) return;
    s_now = now_s;
    uint32_t idle = now_s - s_last_activity;
    devos_power_mode_t want = DEVOS_POWER_ACTIVE;
    if (idle >= DEVOS_POWER_SLEEP_AFTER_S) {
        want = DEVOS_POWER_SLEEP;
    } else if (idle >= DEVOS_POWER_DIM_AFTER_S) {
        want = DEVOS_POWER_DIMMED;
    }
    if (want != s_mode) {
        s_mode = want;
#ifdef ESP_PLATFORM
        /* BSP: apply backlight level; SLEEP arms light-sleep entry. */
#endif
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
#ifdef ESP_PLATFORM
    /* BSP: blank display now; light sleep until GPIO/BMI/key wake. */
#endif
}

const char *devos_power_mode_text(void)
{
    if (s_mode == DEVOS_POWER_SLEEP) return "Sleep";
    if (s_mode == DEVOS_POWER_DIMMED) return "Dimmed";
    return "Active";
}

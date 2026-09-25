#pragma once

/* devos_power: power-mode state machine (INA226 telemetry is consumed
 * elsewhere; this owns idle tracking, dimming, sleep, and wake).
 *
 * Modes follow PLAN §4.3: ACTIVE (< dim timeout), DIMMED (brightness 40% of
 * the user level), SLEEP (> sleep timeout or on demand, backlight off).
 * Any input (devos_power_activity) returns to ACTIVE and wakes.
 *
 * The backlight is driven through a callback (main wires it to the BSP), so
 * this module stays platform-neutral and host-testable (tools/ota_test.c).
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEVOS_POWER_ACTIVE = 0,
    DEVOS_POWER_DIMMED,
    DEVOS_POWER_SLEEP
} devos_power_mode_t;

/* Defaults; both are user-configurable at runtime (0 = never). */
#define DEVOS_POWER_DIM_AFTER_S 120
#define DEVOS_POWER_SLEEP_AFTER_S 600

void devos_power_init(void);
/* Call on any user input (key, touch). Call from the GUI task. */
void devos_power_activity(void);
/* Call at 1 Hz with monotonic seconds (sim tick or target task). */
void devos_power_poll(uint32_t now_s);
devos_power_mode_t devos_power_mode(void);
uint32_t devos_power_idle_s(void);
/* Mode level: 100 active, 40 dimmed, 0 sleeping (fraction of user brightness). */
int devos_power_brightness(void);
void devos_power_sleep_now(void);
const char *devos_power_mode_text(void);

/* Backlight output: called with the effective 0-100% level on every change. */
typedef void (*devos_power_backlight_fn)(int percent);
void devos_power_set_backlight_cb(devos_power_backlight_fn cb);

/* User brightness (5-100%). Applied immediately; persisted on the target. */
void devos_power_set_user_brightness(int percent);
int  devos_power_user_brightness(void);
void devos_power_step_brightness(int delta);

/* Idle timeouts in seconds (0 = never). Persisted on the target. */
void devos_power_set_timeouts(uint32_t dim_after_s, uint32_t sleep_after_s);
uint32_t devos_power_dim_after_s(void);
uint32_t devos_power_sleep_after_s(void);

/* Target: restore brightness/timeouts from NVS (no-op on the host). */
void devos_power_load_prefs(void);

#ifdef __cplusplus
}
#endif

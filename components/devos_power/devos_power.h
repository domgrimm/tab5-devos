#pragma once

/* devos_power: power-mode state machine (IN A226 telemetry is consumed
 * elsewhere; this owns idle tracking, dimming, sleep, and wake).
 *
 * Modes follow PLAN §4.3: ACTIVE (<2 min idle), DIMMED (2–10 min,
 * brightness 40%), SLEEP (>10 min or on demand, brightness 0).
 * Any input (devos_power_activity) returns to ACTIVE and wakes.
 *
 * Target behavior beyond bookkeeping (display dimming via BSP, light
 * sleep via esp_sleep) is marked ESP_PLATFORM; the simulator tracks
 * state and brightness only.
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

#define DEVOS_POWER_DIM_AFTER_S 120
#define DEVOS_POWER_SLEEP_AFTER_S 600

void devos_power_init(void);
/* Call on any user input (key, touch). Safe from any task. */
void devos_power_activity(void);
/* Call at 1 Hz with monotonic seconds (sim tick or target task). */
void devos_power_poll(uint32_t now_s);
devos_power_mode_t devos_power_mode(void);
uint32_t devos_power_idle_s(void);
/* 100 active, 40 dimmed, 0 sleeping. Target BSP consumes this. */
int devos_power_brightness(void);
void devos_power_sleep_now(void);
const char *devos_power_mode_text(void);

#ifdef __cplusplus
}
#endif

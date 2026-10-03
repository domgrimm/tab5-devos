#pragma once

/* jobs_platform: the small platform seam for the Jobs engine - a monotonic
 * clock (injectable so scheduler tests are deterministic), a lock, and the
 * scheduler task. No LVGL. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t (*jobs_clock_fn)(void *user);

void jobs_platform_init(void);
/* Override the clock (host tests); NULL restores the real monotonic clock. */
void jobs_platform_set_clock(jobs_clock_fn fn, void *user);
int64_t jobs_now_ms(void);
/* True when a test has injected a clock (so the scheduler must not auto-run). */
bool jobs_platform_clock_overridden(void);

void jobs_lock(void);
void jobs_unlock(void);

/* Start the scheduler loop on Core 0 (target) / a thread (host). */
bool jobs_platform_start_scheduler(void (*fn)(void *), void *arg);
void jobs_platform_sleep_ms(int ms);

#ifdef __cplusplus
}
#endif

/* jobs_platform: monotonic clock, lock and scheduler task (see jobs_platform.h). */
#include "jobs_platform.h"

#include <stddef.h>

#ifdef ESP_PLATFORM
#include "devos_config.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static SemaphoreHandle_t s_mx;
static jobs_clock_fn s_clock;
static void *s_clock_user;

void jobs_platform_init(void)
{
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
}
void jobs_lock(void) { if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY); }
void jobs_unlock(void) { if (s_mx) xSemaphoreGive(s_mx); }
int64_t jobs_now_ms(void) { return s_clock ? s_clock(s_clock_user) : esp_timer_get_time() / 1000; }
void jobs_platform_sleep_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static void sched_task(void *arg)
{
    void (*fn)(void *) = arg;
    fn(NULL);
    vTaskDelete(NULL);
}

bool jobs_platform_start_scheduler(void (*fn)(void *), void *arg)
{
    (void)arg;
    /* 8 KiB: the tick drains the bounded event queue (devos_events_drain copies
     * a batch to its stack before running callbacks outside the lock). */
    return xTaskCreatePinnedToCore(sched_task, "jobs", 8192, (void *)fn, 3, NULL,
                                   DEVOS_CORE_NET_CRYPTO) == pdPASS;
}
#else
#include <pthread.h>
#include <time.h>

static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static jobs_clock_fn s_clock;
static void *s_clock_user;

void jobs_platform_init(void) {}
void jobs_lock(void) { pthread_mutex_lock(&s_mx); }
void jobs_unlock(void) { pthread_mutex_unlock(&s_mx); }

int64_t jobs_now_ms(void)
{
    if (s_clock) return s_clock(s_clock_user);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
void jobs_platform_sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void *sched_thread(void *arg)
{
    void (*fn)(void *) = arg;
    fn(NULL);
    return NULL;
}

bool jobs_platform_start_scheduler(void (*fn)(void *), void *arg)
{
    pthread_t t;
    (void)arg;
    if (pthread_create(&t, NULL, sched_thread, (void *)fn) != 0) return false;
    pthread_detach(t);
    return true;
}
#endif

void jobs_platform_set_clock(jobs_clock_fn fn, void *user)
{
    s_clock = fn;
    s_clock_user = user;
}

bool jobs_platform_clock_overridden(void) { return s_clock != NULL; }

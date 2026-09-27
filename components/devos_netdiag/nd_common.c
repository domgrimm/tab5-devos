/* devos_netdiag: platform glue. */
#include "nd_int.h"
#include "devos_config.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"

void nd_mutex_create(nd_mutex_t *m)
{
    if (!*m) *m = xSemaphoreCreateMutex();
}
void nd_lock(nd_mutex_t *m) { if (*m) xSemaphoreTake(*m, portMAX_DELAY); }
void nd_unlock(nd_mutex_t *m) { if (*m) xSemaphoreGive(*m); }
int64_t nd_ms(void) { return esp_timer_get_time() / 1000; }
void nd_sleep_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms > 0 ? ms : 1)); }
void *nd_alloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
    return p ? p : calloc(1, n);
}

typedef struct {
    void (*fn)(void *);
    void *arg;
} spawn_t;

static void spawn_task(void *p)
{
    spawn_t s = *(spawn_t *)p;
    free(p);
    s.fn(s.arg);
    vTaskDelete(NULL);
}

int nd_spawn(void (*fn)(void *), void *arg, const char *name, int stack)
{
    spawn_t *s = malloc(sizeof(*s));
    if (!s) return -1;
    s->fn = fn;
    s->arg = arg;
    if (xTaskCreatePinnedToCore(spawn_task, name, stack, s, 3, NULL, DEVOS_CORE_NET_CRYPTO) != pdPASS) {
        free(s);
        return -1;
    }
    return 0;
}
#else
void nd_mutex_create(nd_mutex_t *m) { (void)m; }
void nd_lock(nd_mutex_t *m) { pthread_mutex_lock(m); }
void nd_unlock(nd_mutex_t *m) { pthread_mutex_unlock(m); }
int64_t nd_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
void nd_sleep_ms(int ms) { usleep((useconds_t)(ms > 0 ? ms : 1) * 1000); }
void *nd_alloc(size_t n) { return calloc(1, n); }

typedef struct {
    void (*fn)(void *);
    void *arg;
} spawn_t;

static void *spawn_thread(void *p)
{
    spawn_t s = *(spawn_t *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

int nd_spawn(void (*fn)(void *), void *arg, const char *name, int stack)
{
    (void)name;
    (void)stack;
    spawn_t *s = malloc(sizeof(*s));
    if (!s) return -1;
    s->fn = fn;
    s->arg = arg;
    pthread_t t;
    if (pthread_create(&t, NULL, spawn_thread, s) != 0) {
        free(s);
        return -1;
    }
    pthread_detach(t);
    return 0;
}
#endif

int nd_wait_readable(int fd, int timeout_ms)
{
    fd_set r;
    FD_ZERO(&r);
    FD_SET(fd, &r);
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    int n = select(fd + 1, &r, NULL, NULL, &tv);
    if (n < 0) return errno == EINTR ? 0 : -1;
    return n > 0 ? 1 : 0;
}

/* devos_maptiles: see devos_maptiles.h. */
#include "devos_maptiles.h"
#include "devos_config.h"
#include "devos_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
static SemaphoreHandle_t s_mx;
#define LOCK()   do { if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_mx) xSemaphoreGive(s_mx); } while (0)
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void sleep_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}
#else
#include <pthread.h>
#include <unistd.h>
static pthread_mutex_t s_mx_sim = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&s_mx_sim)
#define UNLOCK() pthread_mutex_unlock(&s_mx_sim)
static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
static void *big_alloc(size_t n) { return malloc(n); }
#endif

#define CACHE_DIR   TAB5_SD_MOUNT_POINT "/.devos/maps/osm"
#define TILE_URL    "https://tile.openstreetmap.org/%u/%u/%u.png"
#define TILE_MAX    (256 * 1024)
#define RAM_SLOTS   48
#define FAIL_SLOTS  64
#define RETRY_MS    30000
#define MAX_AGE_S   (30 * 24 * 3600)
#define STR2(x) #x
#define STR(x)  STR2(x)
#define USER_AGENT  "devOS-Tab5/" STR(DEVOS_VERSION_MAJOR) "." STR(DEVOS_VERSION_MINOR) "." STR(DEVOS_VERSION_PATCH) \
                    " (+https://github.com/domgrimm/tab5-devos)"

typedef struct {
    devos_tile_t t;
    uint8_t *png;
    size_t len;
    uint32_t used;
} ram_slot_t;

typedef struct {
    devos_tile_t t;
    int64_t at;
} fail_t;

static EXT_RAM_BSS_ATTR devos_tile_t s_want[DEVOS_MAPTILES_MAX_WANT];
static int s_nwant;
static uint32_t s_want_gen;                 /* bumps with every want() */
/* the tables above live in PSRAM: internal RAM is kept for TLS / Wi-Fi */
static EXT_RAM_BSS_ATTR ram_slot_t s_ram[RAM_SLOTS];
static uint32_t s_clock;
static EXT_RAM_BSS_ATTR fail_t s_fail[FAIL_SLOTS];
static int s_nfail;
static uint32_t s_gen;
static char s_err[96];
static bool s_inited, s_worker;

static bool same(devos_tile_t a, devos_tile_t b) { return a.z == b.z && a.x == b.x && a.y == b.y; }

static void tile_path(devos_tile_t t, char *out, size_t cap)
{
    snprintf(out, cap, CACHE_DIR "/%u/%u/%u.png", (unsigned)t.z, (unsigned)t.x, (unsigned)t.y);
}

/* ------------------------------------------------------------------ caches (call locked) */
static ram_slot_t *ram_find(devos_tile_t t)
{
    for (int i = 0; i < RAM_SLOTS; i++)
        if (s_ram[i].png && same(s_ram[i].t, t)) return &s_ram[i];
    return NULL;
}

static void ram_put(devos_tile_t t, const uint8_t *png, size_t len)
{
    ram_slot_t *s = ram_find(t);
    if (!s) {
        s = &s_ram[0];
        for (int i = 0; i < RAM_SLOTS; i++) {
            if (!s_ram[i].png) { s = &s_ram[i]; break; }
            if (s_ram[i].used < s->used) s = &s_ram[i];
        }
    }
    uint8_t *copy = big_alloc(len);
    if (!copy) return;
    memcpy(copy, png, len);
    free(s->png);
    s->t = t;
    s->png = copy;
    s->len = len;
    s->used = ++s_clock;
}

static fail_t *fail_find(devos_tile_t t)
{
    for (int i = 0; i < s_nfail; i++)
        if (same(s_fail[i].t, t)) return &s_fail[i];
    return NULL;
}

static void fail_set(devos_tile_t t, bool failed)
{
    fail_t *f = fail_find(t);
    if (!failed) {
        if (f) *f = s_fail[--s_nfail];
        return;
    }
    if (!f) f = s_nfail < FAIL_SLOTS ? &s_fail[s_nfail++] : &s_fail[0];
    f->t = t;
    f->at = now_ms();
}

/* ------------------------------------------------------------------ SD */
static void mkdirs(const char *path)
{
    char p[128];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = '\0';
        mkdir(p, 0755);
        *s = '/';
    }
}

/* Reads a cached tile; *stale = older than MAX_AGE_S (when the clock is set). */
static uint8_t *sd_read(devos_tile_t t, size_t *len, bool *stale)
{
    char path[128];
    tile_path(t, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > TILE_MAX) return NULL;
    if (stale) {
        time_t now = time(NULL);
        *stale = now > 1700000000 && now - st.st_mtime > MAX_AGE_S;
    }
    if (!len) return (uint8_t *)1;                  /* existence check only */
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = big_alloc((size_t)st.st_size);
    size_t n = buf ? fread(buf, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    if (n != (size_t)st.st_size) {
        free(buf);
        return NULL;
    }
    *len = n;
    return buf;
}

static void sd_write(devos_tile_t t, const void *png, size_t len)
{
    char path[128], tmp[136];
    tile_path(t, path, sizeof(path));
    mkdirs(path);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;                                 /* no card: the RAM cache still has it */
    bool ok = fwrite(png, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
}

/* ------------------------------------------------------------------ worker */
static bool pick(devos_tile_t *out)
{
    bool found = false;
    LOCK();
    int64_t t = now_ms();
    for (int i = 0; i < s_nwant && !found; i++) {
        devos_tile_t w = s_want[i];
        if (ram_find(w)) continue;
        fail_t *f = fail_find(w);
        if (f && t - f->at < RETRY_MS) continue;
        bool stale = false;
        UNLOCK();
        bool on_sd = sd_read(w, NULL, &stale) != NULL;
        LOCK();
        if (on_sd && !stale) continue;
        *out = w;
        found = true;
    }
    UNLOCK();
    return found;
}

static void fetch(devos_tile_t t)
{
    char url[96];
    snprintf(url, sizeof(url), TILE_URL, (unsigned)t.z, (unsigned)t.x, (unsigned)t.y);
    devos_http_req_t q = {
        .url = url,
        .headers = "User-Agent: " USER_AGENT "\nAccept: image/png",
        .timeout_ms = 10000,
        .max_redirects = 2,
        .max_body = TILE_MAX,
    };
    devos_http_resp_t r;
    devos_http_request(&q, &r);
    bool ok = r.status == 200 && r.body && r.body_len > 8 && !r.truncated && !memcmp(r.body, "\x89PNG", 4);
    if (ok) sd_write(t, r.body, r.body_len);
    LOCK();
    if (ok) {
        ram_put(t, (const uint8_t *)r.body, r.body_len);
        fail_set(t, false);
        s_err[0] = '\0';
    } else {
        fail_set(t, true);
        if (!r.status) snprintf(s_err, sizeof(s_err), "%.95s", r.error);
        else snprintf(s_err, sizeof(s_err), "Tile server: HTTP %d", r.status);
    }
    s_gen++;
    UNLOCK();
    devos_http_resp_free(&r);
}

/* Once everything wanted is here, rescan only when the list changes (or
 * every few seconds, for failed tiles to be retried). */
static void worker_loop(void)
{
    uint32_t idle_gen = 0;
    int64_t idle_at = 0;
    bool idle = false;
    for (;;) {
        LOCK();
        uint32_t g = s_want_gen;
        UNLOCK();
        if (idle && g == idle_gen && now_ms() - idle_at < 5000) {
            sleep_ms(200);
            continue;
        }
        devos_tile_t t;
        if (pick(&t)) {
            idle = false;
            fetch(t);
        } else {
            idle = true;
            idle_gen = g;
            idle_at = now_ms();
        }
    }
}

#ifdef ESP_PLATFORM
static void worker_task(void *arg)
{
    (void)arg;
    worker_loop();
}
#else
static void *worker_thread(void *arg)
{
    (void)arg;
    worker_loop();
    return NULL;
}
#endif

/* ------------------------------------------------------------------ api */
void devos_maptiles_init(void)
{
    if (s_inited) return;
    s_inited = true;
#ifdef ESP_PLATFORM
    s_mx = xSemaphoreCreateMutex();
#endif
}

void devos_maptiles_want(const devos_tile_t *tiles, int n)
{
    if (!s_inited) return;
    if (n > DEVOS_MAPTILES_MAX_WANT) n = DEVOS_MAPTILES_MAX_WANT;
    LOCK();
    if (n > 0) memcpy(s_want, tiles, sizeof(*tiles) * (size_t)n);
    s_nwant = n < 0 ? 0 : n;
    s_want_gen++;
    bool start = s_nwant && !s_worker;
    s_worker = s_worker || start;
    UNLOCK();
    if (!start) return;
#ifdef ESP_PLATFORM
    xTaskCreatePinnedToCore(worker_task, "maptiles", 8192, NULL, 2, NULL, DEVOS_CORE_NET_CRYPTO);
#else
    pthread_t th;
    pthread_create(&th, NULL, worker_thread, NULL);
    pthread_detach(th);
#endif
}

int devos_maptiles_get(devos_tile_t t, uint8_t **png, size_t *len)
{
    if (!s_inited) return -1;
    LOCK();
    ram_slot_t *s = ram_find(t);
    if (s) {
        uint8_t *copy = big_alloc(s->len);
        if (copy) {
            memcpy(copy, s->png, s->len);
            *png = copy;
            *len = s->len;
            s->used = ++s_clock;
        }
        UNLOCK();
        return copy ? 1 : 0;
    }
    bool failed = fail_find(t) != NULL;
    UNLOCK();
    uint8_t *buf = sd_read(t, len, NULL);
    if (buf) {
        *png = buf;
        return 1;
    }
    return failed ? -1 : 0;
}

uint32_t devos_maptiles_generation(void)
{
    LOCK();
    uint32_t g = s_gen;
    UNLOCK();
    return g;
}

void devos_maptiles_error(char *out, size_t cap)
{
    LOCK();
    snprintf(out, cap, "%s", s_err);
    UNLOCK();
}

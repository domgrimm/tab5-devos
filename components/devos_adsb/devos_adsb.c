/* devos_adsb: see devos_adsb.h. */
#include "devos_adsb.h"
#include "devos_config.h"
#include "devos_http.h"
#include "devos_json.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
    return p ? p : calloc(1, n);
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
static void *big_alloc(size_t n) { return calloc(1, n); }
#endif

#define CFG_FILE  TAB5_SD_MOUNT_POINT "/.devos/adsb.json"
#define POLL_MS   1000
#define STALE_S   60.0f
#define BODY_MAX  (1024 * 1024)

static devos_adsb_config_t s_cfg;
static devos_adsb_status_t s_st;
static devos_adsb_ac_t *s_ac;               /* DEVOS_ADSB_MAX */
static int s_n;
static uint32_t s_gen;
static volatile bool s_active;
static bool s_worker, s_inited, s_rx_asked;
static double s_prev_msgs = -1;
static int64_t s_prev_msgs_ms;

/* ------------------------------------------------------------------ helpers */
void devos_adsb_range_bearing(double lat0, double lon0, double lat, double lon, float *nm, float *brg)
{
    double p1 = lat0 * M_PI / 180, p2 = lat * M_PI / 180, dl = (lon - lon0) * M_PI / 180;
    double a = sin((p2 - p1) / 2) * sin((p2 - p1) / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    if (nm) *nm = (float)(2 * atan2(sqrt(a), sqrt(1 - a)) * 3440.065);
    if (brg) {
        double y = sin(dl) * cos(p2), x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
        double b = atan2(y, x) * 180 / M_PI;
        *brg = (float)(b < 0 ? b + 360 : b);
    }
}

const char *devos_adsb_category_name(const char *c)
{
    if (!c || strlen(c) != 2) return NULL;
    static const char *A[] = { NULL, "Light aircraft", "Small aircraft", "Large aircraft", "High-vortex large",
                               "Heavy jet", "High performance", "Helicopter" };
    static const char *B[] = { NULL, "Glider", "Lighter than air", "Parachutist", "Ultralight", NULL, "Drone",
                               "Space vehicle" };
    static const char *C[] = { NULL, "Emergency vehicle", "Service vehicle", "Obstacle", "Obstacle", "Obstacle",
                               NULL, NULL };
    int i = c[1] - '0';
    if (i < 0 || i > 7) return NULL;
    if (c[0] == 'A') return A[i];
    if (c[0] == 'B') return B[i];
    if (c[0] == 'C') return C[i];
    return NULL;
}

static bool get_num(const char *js, size_t len, const char *key, double *out)
{
    const char *v = devos_json_find_key(js, js + len, key);
    if (!v) return false;
    while (v < js + len && isspace((unsigned char)*v)) v++;
    if (v >= js + len || !(isdigit((unsigned char)*v) || *v == '-' || *v == '.')) return false;
    *out = strtod(v, NULL);
    return true;
}

/* ------------------------------------------------------------------ config */
static void load_config(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.range_nm = 100;
    FILE *f = fopen(CFG_FILE, "rb");
    if (!f) return;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    devos_json_get_str(buf, n, "url", s_cfg.url, sizeof(s_cfg.url));
    double d;
    if (get_num(buf, n, "lat", &d)) s_cfg.lat = d;
    if (get_num(buf, n, "lon", &d)) s_cfg.lon = d;
    if (get_num(buf, n, "range_nm", &d) && d >= 5 && d <= 500) s_cfg.range_nm = (int)d;
}

static void save_config(void)
{
    mkdir(TAB5_SD_MOUNT_POINT "/.devos", 0755);
    FILE *f = fopen(CFG_FILE, "wb");
    if (!f) return;
    char url[500];
    devos_json_escape(s_cfg.url, url, sizeof(url));
    fprintf(f, "{\"url\": \"%s\", \"lat\": %.6f, \"lon\": %.6f, \"range_nm\": %d}\n", url, s_cfg.lat, s_cfg.lon, s_cfg.range_nm);
    fclose(f);
}

void devos_adsb_get_config(devos_adsb_config_t *out)
{
    LOCK();
    *out = s_cfg;
    UNLOCK();
}

bool devos_adsb_configured(void) { return s_cfg.url[0] != '\0'; }

void devos_adsb_set_config(const devos_adsb_config_t *c)
{
    LOCK();
    s_cfg = *c;
    s_n = 0;
    memset(&s_st, 0, sizeof(s_st));
    s_rx_asked = false;
    s_prev_msgs = -1;
    if (s_cfg.lat || s_cfg.lon) {
        s_st.lat = s_cfg.lat;
        s_st.lon = s_cfg.lon;
        s_st.have_pos = true;
    }
    s_gen++;
    UNLOCK();
    save_config();
}

/* ------------------------------------------------------------------ fetch */
static void fetch_receiver(void)
{
    char url[220];
    LOCK();
    snprintf(url, sizeof(url), "%s", s_cfg.url);
    UNLOCK();
    char *slash = strrchr(url, '/');
    if (!slash) return;
    snprintf(slash + 1, sizeof(url) - (size_t)(slash + 1 - url), "receiver.json");
    devos_http_req_t q = { .url = url, .timeout_ms = 5000, .insecure = true, .max_body = 16 * 1024 };
    devos_http_resp_t r;
    devos_http_request(&q, &r);
    double lat, lon;
    if (r.status == 200 && r.body && get_num(r.body, r.body_len, "lat", &lat) && get_num(r.body, r.body_len, "lon", &lon) &&
        (lat || lon)) {
        LOCK();
        s_st.lat = lat;
        s_st.lon = lon;
        s_st.have_pos = true;
        s_gen++;
        UNLOCK();
    }
    devos_http_resp_free(&r);
}

typedef struct {
    devos_adsb_ac_t *tmp;
    int n;
} parse_ud_t;

static void ac_one(const char *e, size_t len, void *ud)
{
    parse_ud_t *u = ud;
    if (u->n >= DEVOS_ADSB_MAX || *e != '{') return;
    devos_adsb_ac_t *a = &u->tmp[u->n];
    memset(a, 0, sizeof(*a));
    char hex[12] = "";
    devos_json_get_str(e, len, "hex", hex, sizeof(hex));
    const char *h = hex[0] == '~' ? hex + 1 : hex;
    if (!*h) return;
    snprintf(a->hex, sizeof(a->hex), "%s", h);
    char fl[16] = "";
    devos_json_get_str(e, len, "flight", fl, sizeof(fl));
    size_t fn = strlen(fl);
    while (fn && fl[fn - 1] == ' ') fl[--fn] = '\0';
    snprintf(a->flight, sizeof(a->flight), "%s", fl);
    devos_json_get_str(e, len, "squawk", a->squawk, sizeof(a->squawk));
    devos_json_get_str(e, len, "category", a->category, sizeof(a->category));
    double d;
    const char *alt = devos_json_find_key(e, e + len, "alt_baro");
    if (!alt) alt = devos_json_find_key(e, e + len, "altitude");          /* older dump1090 */
    if (alt) {
        while (*alt == ' ') alt++;
        if (*alt == '"') a->ground = !strncmp(alt, "\"ground\"", 8);
        else if (isdigit((unsigned char)*alt) || *alt == '-') {
            a->alt_ft = atoi(alt);
            a->has_alt = true;
        }
    }
    if (get_num(e, len, "alt_geom", &d)) {
        a->alt_geom_ft = (int)d;
        if (!a->has_alt && !a->ground) { a->alt_ft = (int)d; a->has_alt = true; }
    }
    if (get_num(e, len, "baro_rate", &d) || get_num(e, len, "geom_rate", &d) || get_num(e, len, "vert_rate", &d))
        a->vrate_fpm = (int)d;
    if (get_num(e, len, "gs", &d) || get_num(e, len, "speed", &d)) { a->gs_kt = (float)d; a->has_gs = true; }
    if (get_num(e, len, "track", &d)) { a->track_deg = (float)d; a->has_track = true; }
    double lat, lon;
    if (get_num(e, len, "lat", &lat) && get_num(e, len, "lon", &lon)) {
        a->lat = lat;
        a->lon = lon;
        a->has_pos = true;
    }
    if (get_num(e, len, "seen", &d)) a->seen_s = (float)d;
    a->seen_pos_s = get_num(e, len, "seen_pos", &d) ? (float)d : a->seen_s;
    if (get_num(e, len, "rssi", &d)) a->rssi = (float)d;
    if (get_num(e, len, "messages", &d)) a->messages = (uint32_t)d;
    char em[16] = "";
    devos_json_get_str(e, len, "emergency", em, sizeof(em));
    a->emergency = (em[0] && strcmp(em, "none")) || !strcmp(a->squawk, "7500") || !strcmp(a->squawk, "7600") ||
                   !strcmp(a->squawk, "7700");
    const char *ml = devos_json_find_key(e, e + len, "mlat");
    if (ml) {
        while (*ml == ' ') ml++;
        a->mlat = ml[0] == '[' && ml[1] != ']' && strstr(ml, "\"lat\"") && strstr(ml, "\"lat\"") < strchr(ml, ']');
    }
    if (a->has_pos && a->seen_pos_s > STALE_S) a->has_pos = false;
    if (a->seen_s > STALE_S) return;
    u->n++;
}

static void fetch_aircraft(void)
{
    char url[220];
    LOCK();
    snprintf(url, sizeof(url), "%s", s_cfg.url);
    UNLOCK();
    devos_http_req_t q = { .url = url, .timeout_ms = 5000, .insecure = true, .max_body = BODY_MAX, .max_redirects = 3 };
    devos_http_resp_t r;
    devos_http_request(&q, &r);
    if (r.status != 200 || !r.body) {
        LOCK();
        if (!r.status) snprintf(s_st.error, sizeof(s_st.error), "%s", r.error);
        else snprintf(s_st.error, sizeof(s_st.error), "HTTP %d %s from the receiver%s", r.status, r.reason,
                      r.status == 404 ? " - check the aircraft.json URL" : "");
        s_gen++;
        UNLOCK();
        devos_http_resp_free(&r);
        return;
    }
    static devos_adsb_ac_t *tmp;
    if (!tmp) tmp = big_alloc(sizeof(devos_adsb_ac_t) * DEVOS_ADSB_MAX);
    if (!tmp) {
        devos_http_resp_free(&r);
        return;
    }
    parse_ud_t u = { tmp, 0 };
    size_t al;
    const char *arr = devos_json_find_key(r.body, r.body + r.body_len, "aircraft");
    if (arr) {
        while (*arr == ' ') arr++;
        const char *end = *arr == '[' ? devos_json_span(arr, r.body + r.body_len) : NULL;
        al = end ? (size_t)(end - arr) : 0;
        if (al) devos_json_array_each(arr, al, ac_one, &u);
    }
    double msgs = -1;
    get_num(r.body, r.body_len, "messages", &msgs);
    devos_http_resp_free(&r);
    if (!arr) {
        LOCK();
        snprintf(s_st.error, sizeof(s_st.error), "No \"aircraft\" in the reply - is that an aircraft.json URL?");
        s_gen++;
        UNLOCK();
        return;
    }
    LOCK();
    /* carry trails over by hex */
    for (int i = 0; i < u.n; i++) {
        devos_adsb_ac_t *a = &tmp[i];
        for (int k = 0; k < s_n; k++) {
            if (strcmp(s_ac[k].hex, a->hex)) continue;
            a->trail_n = s_ac[k].trail_n;
            memcpy(a->trail_lat, s_ac[k].trail_lat, sizeof(a->trail_lat));
            memcpy(a->trail_lon, s_ac[k].trail_lon, sizeof(a->trail_lon));
            break;
        }
        if (a->has_pos) {
            bool moved = !a->trail_n || fabsf(a->trail_lat[a->trail_n - 1] - (float)a->lat) > 0.002f ||
                         fabsf(a->trail_lon[a->trail_n - 1] - (float)a->lon) > 0.002f;
            if (moved) {
                if (a->trail_n == DEVOS_ADSB_TRAIL) {
                    memmove(a->trail_lat, a->trail_lat + 1, sizeof(float) * (DEVOS_ADSB_TRAIL - 1));
                    memmove(a->trail_lon, a->trail_lon + 1, sizeof(float) * (DEVOS_ADSB_TRAIL - 1));
                    a->trail_n--;
                }
                a->trail_lat[a->trail_n] = (float)a->lat;
                a->trail_lon[a->trail_n] = (float)a->lon;
                a->trail_n++;
            }
        }
    }
    memcpy(s_ac, tmp, sizeof(devos_adsb_ac_t) * (size_t)u.n);
    s_n = u.n;
    s_st.total = u.n;
    s_st.with_pos = 0;
    for (int i = 0; i < u.n; i++) {
        if (!tmp[i].has_pos) continue;
        s_st.with_pos++;
        if (s_st.have_pos) {
            float nm;
            devos_adsb_range_bearing(s_st.lat, s_st.lon, tmp[i].lat, tmp[i].lon, &nm, NULL);
            if (nm > s_st.max_range_nm && nm < 600) s_st.max_range_nm = nm;
        }
    }
    int64_t t = now_ms();
    if (msgs >= 0) {
        if (s_prev_msgs >= 0 && t > s_prev_msgs_ms && msgs >= s_prev_msgs)
            s_st.msg_rate = (float)((msgs - s_prev_msgs) * 1000.0 / (double)(t - s_prev_msgs_ms));
        s_prev_msgs = msgs;
        s_prev_msgs_ms = t;
    }
    s_st.updated = time(NULL);
    s_st.error[0] = '\0';
    s_gen++;
    UNLOCK();
}

static void worker_loop(void)
{
    for (;;) {
        int64_t t0 = now_ms();
        if (s_active && devos_adsb_configured()) {
            bool need_rx;
            LOCK();
            need_rx = !s_st.have_pos && !s_rx_asked;
            s_rx_asked = s_rx_asked || need_rx;
            UNLOCK();
            if (need_rx) fetch_receiver();
            fetch_aircraft();
        }
        int left = POLL_MS - (int)(now_ms() - t0);
        sleep_ms(left > 50 ? left : 50);
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
void devos_adsb_init(void)
{
    if (s_inited) return;
    s_inited = true;
#ifdef ESP_PLATFORM
    s_mx = xSemaphoreCreateMutex();
#endif
    s_ac = big_alloc(sizeof(devos_adsb_ac_t) * DEVOS_ADSB_MAX);
    load_config();
    if (s_cfg.lat || s_cfg.lon) {
        s_st.lat = s_cfg.lat;
        s_st.lon = s_cfg.lon;
        s_st.have_pos = true;
    }
}

void devos_adsb_set_active(bool active)
{
    s_active = active;
    LOCK();
    s_st.active = active;
    UNLOCK();
    if (active && !s_worker && s_ac) {
        s_worker = true;
#ifdef ESP_PLATFORM
        xTaskCreatePinnedToCore(worker_task, "adsb", 12288, NULL, 3, NULL, DEVOS_CORE_NET_CRYPTO);
#else
        pthread_t t;
        pthread_create(&t, NULL, worker_thread, NULL);
        pthread_detach(t);
#endif
    }
}

void devos_adsb_status(devos_adsb_status_t *out)
{
    LOCK();
    *out = s_st;
    out->configured = s_cfg.url[0] != '\0';
    UNLOCK();
}

int devos_adsb_list(devos_adsb_ac_t *out, int max)
{
    LOCK();
    int n = s_n < max ? s_n : max;
    if (n > 0) memcpy(out, s_ac, sizeof(*out) * (size_t)n);
    UNLOCK();
    return n;
}

uint32_t devos_adsb_generation(void)
{
    LOCK();
    uint32_t g = s_gen;
    UNLOCK();
    return g;
}

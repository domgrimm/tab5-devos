/* devos_docker: see devos_docker.h. */
#include "devos_docker.h"
#include "devos_config.h"
#include "devos_http.h"
#include "devos_json.h"

#include <ctype.h>
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
#include "nvs.h"
static SemaphoreHandle_t s_mx, s_wake;
#define LOCK()   do { if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_mx) xSemaphoreGive(s_mx); } while (0)
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}
#else
#include <pthread.h>
#include <unistd.h>
static pthread_mutex_t s_mx_sim = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_wmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_wcv = PTHREAD_COND_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&s_mx_sim)
#define UNLOCK() pthread_mutex_unlock(&s_mx_sim)
static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void *big_alloc(size_t n) { return malloc(n); }
#endif

#define CFG_FILE   TAB5_SD_MOUNT_POINT "/.devos/docker.json"
#define LOG_CAP    (128 * 1024)
#define BODY_MAX   (768 * 1024)

static devos_docker_config_t s_cfg;
static devos_docker_status_t s_st;
static devos_docker_ct_t *s_ct;             /* DEVOS_DOCKER_MAX */
static int s_nct;
static uint32_t s_gen;
static devos_docker_stats_t s_stats;
static char *s_log;                         /* LOG_CAP */
static size_t s_log_len;
static uint32_t s_log_gen;
static char s_sel[16];
static bool s_sel_logs;
static int64_t s_log_last_s = -1;           /* newest log line: seconds + nanoseconds */
static int32_t s_log_last_ns;
static bool s_worker, s_inited;
static volatile bool s_active, s_kick;
static char s_act_id[16], s_act_what[12];
static int s_ep_resolved;                   /* Portainer endpoint in use */

static void wake(void)
{
    s_kick = true;
#ifdef ESP_PLATFORM
    if (s_wake) xSemaphoreGive(s_wake);
#else
    pthread_mutex_lock(&s_wmx);
    pthread_cond_signal(&s_wcv);
    pthread_mutex_unlock(&s_wmx);
#endif
}

/* ------------------------------------------------------------------ config */
static void load_config(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.interval_s = 5;
    FILE *f = fopen(CFG_FILE, "rb");
    if (f) {
        char buf[1024];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        int v;
        char mode[16] = "";
        devos_json_get_str(buf, n, "mode", mode, sizeof(mode));
        s_cfg.mode = !strcasecmp(mode, "portainer") ? DEVOS_DOCKER_PORTAINER : DEVOS_DOCKER_DIRECT;
        devos_json_get_str(buf, n, "url", s_cfg.url, sizeof(s_cfg.url));
        if (devos_json_get_int(buf, n, "endpoint", &v) == 0) s_cfg.endpoint = v;
        if (devos_json_get_int(buf, n, "insecure", &v) == 0) s_cfg.insecure = v != 0;
        if (devos_json_get_int(buf, n, "interval", &v) == 0 && v >= 2 && v <= 300) s_cfg.interval_s = v;
#ifndef ESP_PLATFORM
        devos_json_get_str(buf, n, "api_key", s_cfg.api_key, sizeof(s_cfg.api_key));
#endif
    }
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("docker", NVS_READONLY, &h) == ESP_OK) {
        size_t l = sizeof(s_cfg.api_key);
        if (nvs_get_str(h, "key", s_cfg.api_key, &l) != ESP_OK) s_cfg.api_key[0] = '\0';
        nvs_close(h);
    }
#endif
}

static void save_config(void)
{
    mkdir(TAB5_SD_MOUNT_POINT "/.devos", 0755);
    FILE *f = fopen(CFG_FILE, "wb");
    if (f) {
        char url[400];
        devos_json_escape(s_cfg.url, url, sizeof(url));
        fprintf(f, "{\"mode\": \"%s\", \"url\": \"%s\", \"endpoint\": %d, \"insecure\": %d, \"interval\": %d",
                s_cfg.mode == DEVOS_DOCKER_PORTAINER ? "portainer" : "docker", url, s_cfg.endpoint,
                s_cfg.insecure ? 1 : 0, s_cfg.interval_s);
#ifndef ESP_PLATFORM
        char key[300];
        devos_json_escape(s_cfg.api_key, key, sizeof(key));
        fprintf(f, ", \"api_key\": \"%s\"", key);           /* simulator only: no NVS */
#endif
        fprintf(f, "}\n");
        fclose(f);
    }
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("docker", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "key", s_cfg.api_key);
        nvs_commit(h);
        nvs_close(h);
    }
#endif
}

void devos_docker_get_config(devos_docker_config_t *out)
{
    LOCK();
    *out = s_cfg;
    UNLOCK();
}

bool devos_docker_configured(void) { return s_cfg.url[0] != '\0'; }

void devos_docker_set_config(const devos_docker_config_t *c)
{
    LOCK();
    s_cfg = *c;
    if (s_cfg.interval_s < 2) s_cfg.interval_s = 5;
    size_t l = strlen(s_cfg.url);
    while (l && (s_cfg.url[l - 1] == '/' || s_cfg.url[l - 1] == ' ')) s_cfg.url[--l] = '\0';
    s_ep_resolved = 0;
    s_nct = 0;
    memset(&s_st, 0, sizeof(s_st));
    s_st.configured = s_cfg.url[0] != '\0';
    s_gen++;
    UNLOCK();
    save_config();
    wake();
}

/* ------------------------------------------------------------------ http */
static int api(const char *method, const char *path, devos_http_resp_t *r, size_t max_body)
{
    char url[512], hdr[200] = "";
    devos_docker_config_t c;
    LOCK();
    c = s_cfg;
    int ep = s_ep_resolved;
    UNLOCK();
    if (c.mode == DEVOS_DOCKER_PORTAINER) {
        if (!strncmp(path, "/api/", 5)) snprintf(url, sizeof(url), "%s%s", c.url, path);
        else snprintf(url, sizeof(url), "%s/api/endpoints/%d/docker%s", c.url, ep, path);
        if (c.api_key[0]) snprintf(hdr, sizeof(hdr), "X-API-Key: %s", c.api_key);
    } else {
        snprintf(url, sizeof(url), "%s%s", c.url, path);
    }
    devos_http_req_t q = { .method = method, .url = url, .headers = hdr[0] ? hdr : NULL, .insecure = c.insecure,
                           .timeout_ms = 12000, .max_body = max_body };
    devos_http_request(&q, r);
    return r->status;
}

static void set_error_from(const devos_http_resp_t *r, const char *what)
{
    LOCK();
    if (!r->status) snprintf(s_st.error, sizeof(s_st.error), "%s: %s", what, r->error);
    else if (r->status == 401 || r->status == 403)
        snprintf(s_st.error, sizeof(s_st.error), "%s: access denied (%d) - check the API key", what, r->status);
    else if (r->status == 404) snprintf(s_st.error, sizeof(s_st.error), "%s: not found (404) - wrong URL or environment?", what);
    else snprintf(s_st.error, sizeof(s_st.error), "%s: HTTP %d %.40s", what, r->status, r->reason);
    s_gen++;
    UNLOCK();
}

/* ------------------------------------------------------------------ json helpers */
static const char *span_of(const char *js, size_t len, const char *key, size_t *slen)
{
    const char *v = devos_json_find_key(js, js + len, key);
    if (!v) return NULL;
    while (v < js + len && isspace((unsigned char)*v)) v++;
    if (v >= js + len || (*v != '{' && *v != '[')) return NULL;
    const char *e = devos_json_span(v, js + len);
    if (!e) return NULL;
    *slen = (size_t)(e - v);
    return v;
}

static uint64_t get_u64(const char *js, size_t len, const char *key)
{
    const char *v = devos_json_find_key(js, js + len, key);
    if (!v) return 0;
    while (v < js + len && isspace((unsigned char)*v)) v++;
    return strtoull(v, NULL, 10);
}

static uint64_t get_nested_u64(const char *js, size_t len, const char *outer, const char *inner, const char *key)
{
    size_t l1, l2;
    const char *a = span_of(js, len, outer, &l1);
    if (!a) return 0;
    if (inner) {
        const char *b = span_of(a, l1, inner, &l2);
        return b ? get_u64(b, l2, key) : 0;
    }
    return get_u64(a, l1, key);
}

/* ------------------------------------------------------------------ list */
typedef struct {
    devos_docker_ct_t *out;
    int n, max;
} list_ud_t;

typedef struct {
    char *buf;
    size_t cap, len;
    int seen[24];
    int nseen;
} ports_ud_t;

static void port_one(const char *e, size_t len, void *ud)
{
    ports_ud_t *p = ud;
    int priv = 0, pub = 0;
    char type[8] = "tcp";
    devos_json_get_int(e, len, "PrivatePort", &priv);
    devos_json_get_int(e, len, "PublicPort", &pub);
    devos_json_get_str(e, len, "Type", type, sizeof(type));
    int key = pub * 65536 + priv;
    for (int i = 0; i < p->nseen; i++) if (p->seen[i] == key) return;      /* IPv4 + IPv6 twins */
    if (p->nseen < 24) p->seen[p->nseen++] = key;
    if (p->len + 24 >= p->cap) return;
    if (pub) p->len += (size_t)snprintf(p->buf + p->len, p->cap - p->len, "%s%d->%d/%s", p->len ? ", " : "", pub, priv, type);
    else p->len += (size_t)snprintf(p->buf + p->len, p->cap - p->len, "%s%d/%s", p->len ? ", " : "", priv, type);
}

static void ct_one(const char *e, size_t len, void *ud)
{
    list_ud_t *u = ud;
    if (u->n >= u->max || *e != '{') return;
    devos_docker_ct_t *c = &u->out[u->n];
    memset(c, 0, sizeof(*c));
    char id[80] = "";
    devos_json_get_str(e, len, "Id", id, sizeof(id));
    snprintf(c->id, sizeof(c->id), "%.12s", id);
    const char *names = devos_json_find_key(e, e + len, "Names");
    if (names) {
        while (*names == ' ' || *names == '[') names++;
        char nm[96] = "";
        if (*names == '"' && devos_json_parse_str(names, e + len, nm, sizeof(nm)))
            snprintf(c->name, sizeof(c->name), "%s", nm[0] == '/' ? nm + 1 : nm);
    }
    devos_json_get_str(e, len, "Image", c->image, sizeof(c->image));
    devos_json_get_str(e, len, "State", c->state, sizeof(c->state));
    devos_json_get_str(e, len, "Status", c->status, sizeof(c->status));
    int created = 0;
    if (devos_json_get_int(e, len, "Created", &created) == 0) c->created = created;
    size_t pl;
    const char *ports = span_of(e, len, "Ports", &pl);
    if (ports) {
        ports_ud_t pu = { c->ports, sizeof(c->ports), 0, {0}, 0 };
        devos_json_array_each(ports, pl, port_one, &pu);
    }
    size_t ll;
    const char *labels = span_of(e, len, "Labels", &ll);
    if (labels) devos_json_get_str(labels, ll, "com.docker.compose.project", c->compose, sizeof(c->compose));
    if (strstr(c->status, "(unhealthy)")) c->health = 2;
    else if (strstr(c->status, "(healthy)")) c->health = 1;
    else if (strstr(c->status, "health: starting")) c->health = 3;
    if (!c->name[0]) snprintf(c->name, sizeof(c->name), "%s", c->id);
    u->n++;
}

static int ct_rank(const devos_docker_ct_t *c)
{
    if (!strcmp(c->state, "running")) return 0;
    if (!strcmp(c->state, "restarting") || !strcmp(c->state, "paused")) return 1;
    return 2;
}

static int ct_cmp(const void *a, const void *b)
{
    const devos_docker_ct_t *x = a, *y = b;
    int d = ct_rank(x) - ct_rank(y);
    return d ? d : strcasecmp(x->name, y->name);
}

static void fetch_endpoint(void)
{
    devos_http_resp_t r;
    if (api("GET", "/api/endpoints", &r, 256 * 1024) == 200 && r.body) {
        int want;
        LOCK();
        want = s_cfg.endpoint;
        UNLOCK();
        const char *a = strchr(r.body, '[');
        int found = 0;
        char name[48] = "";
        /* first environment, or the configured id */
        for (const char *p = a; p && *p && !found;) {
            p = strchr(p, '{');
            if (!p) break;
            const char *end = devos_json_span(p, r.body + r.body_len);
            if (!end) break;
            int id = 0;
            devos_json_get_int(p, (size_t)(end - p), "Id", &id);
            if (!want || id == want) {
                found = id;
                devos_json_get_str(p, (size_t)(end - p), "Name", name, sizeof(name));
            }
            p = end;
        }
        LOCK();
        if (found) {
            s_ep_resolved = found;
            snprintf(s_st.endpoint_name, sizeof(s_st.endpoint_name), "%s", name);
        } else {
            snprintf(s_st.error, sizeof(s_st.error), want ? "Portainer has no environment %d" : "Portainer has no environments", want);
        }
        s_gen++;
        UNLOCK();
    } else {
        set_error_from(&r, "Portainer");
    }
    devos_http_resp_free(&r);
}

static void fetch_info(void)
{
    devos_http_resp_t r;
    if (api("GET", "/info", &r, 128 * 1024) == 200 && r.body) {
        LOCK();
        devos_json_get_str(r.body, r.body_len, "Name", s_st.host, sizeof(s_st.host));
        devos_json_get_str(r.body, r.body_len, "ServerVersion", s_st.version, sizeof(s_st.version));
        UNLOCK();
    }
    devos_http_resp_free(&r);
}

static bool fetch_list(void)
{
    devos_http_resp_t r;
    bool ok = false;
    LOCK();
    s_st.busy = true;
    UNLOCK();
    if (api("GET", "/containers/json?all=1", &r, BODY_MAX) == 200 && r.body && !r.truncated) {
        static devos_docker_ct_t *tmp;
        if (!tmp) tmp = big_alloc(sizeof(devos_docker_ct_t) * DEVOS_DOCKER_MAX);
        if (tmp) {
            list_ud_t u = { tmp, 0, DEVOS_DOCKER_MAX };
            const char *a = strchr(r.body, '[');
            if (a) devos_json_array_each(a, r.body_len - (size_t)(a - r.body), ct_one, &u);
            qsort(tmp, (size_t)u.n, sizeof(*tmp), ct_cmp);
            LOCK();
            memcpy(s_ct, tmp, sizeof(*tmp) * (size_t)u.n);
            s_nct = u.n;
            s_st.total = u.n;
            s_st.running = 0;
            for (int i = 0; i < u.n; i++) s_st.running += !strcmp(tmp[i].state, "running");
            s_st.updated = time(NULL);
            s_st.error[0] = '\0';
            s_gen++;
            UNLOCK();
            ok = true;
        }
    } else if (r.truncated) {
        LOCK();
        snprintf(s_st.error, sizeof(s_st.error), "The container list is too big (over 768 KB)");
        s_gen++;
        UNLOCK();
    } else {
        set_error_from(&r, "Containers");
    }
    devos_http_resp_free(&r);
    LOCK();
    s_st.busy = false;
    UNLOCK();
    return ok;
}

/* ------------------------------------------------------------------ stats */
static void fetch_stats(const char *id)
{
    char path[96];
    snprintf(path, sizeof(path), "/containers/%s/stats?stream=false", id);
    devos_http_resp_t r;
    devos_docker_stats_t st;
    memset(&st, 0, sizeof(st));
    snprintf(st.id, sizeof(st.id), "%s", id);
    if (api("GET", path, &r, 64 * 1024) == 200 && r.body) {
        const char *b = r.body;
        size_t n = r.body_len;
        uint64_t cpu = get_nested_u64(b, n, "cpu_stats", "cpu_usage", "total_usage");
        uint64_t pcpu = get_nested_u64(b, n, "precpu_stats", "cpu_usage", "total_usage");
        uint64_t sys = get_nested_u64(b, n, "cpu_stats", NULL, "system_cpu_usage");
        uint64_t psys = get_nested_u64(b, n, "precpu_stats", NULL, "system_cpu_usage");
        uint64_t cpus = get_nested_u64(b, n, "cpu_stats", NULL, "online_cpus");
        if (!cpus) cpus = 1;
        if (sys > psys && cpu >= pcpu) st.cpu_pct = (float)((double)(cpu - pcpu) / (double)(sys - psys) * (double)cpus * 100.0);
        size_t ml;
        const char *mem = span_of(b, n, "memory_stats", &ml);
        if (mem) {
            st.mem_used = get_u64(mem, ml, "usage");
            st.mem_limit = get_u64(mem, ml, "limit");
            size_t sl;
            const char *ms = span_of(mem, ml, "stats", &sl);
            if (ms) {
                uint64_t cache = get_u64(ms, sl, "inactive_file");
                if (!cache) cache = get_u64(ms, sl, "total_inactive_file");
                if (cache < st.mem_used) st.mem_used -= cache;
            }
        }
        size_t nl;
        const char *nets = span_of(b, n, "networks", &nl);
        for (const char *p = nets; p && p < nets + nl;) {
            const char *rx = devos_json_find_key(p, nets + nl, "rx_bytes");
            if (!rx) break;
            st.net_rx += strtoull(rx, NULL, 10);
            const char *tx = devos_json_find_key(rx, nets + nl, "tx_bytes");
            if (!tx) break;
            st.net_tx += strtoull(tx, NULL, 10);
            p = tx;
        }
        st.pids = (int)get_nested_u64(b, n, "pids_stats", NULL, "current");
        st.valid = true;
    }
    devos_http_resp_free(&r);
    LOCK();
    if (!strcmp(s_sel, id)) {
        s_stats = st;
        s_gen++;
    }
    UNLOCK();
}

/* ------------------------------------------------------------------ logs */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* "2024-01-02T03:04:05.123456789Z" -> seconds + nanoseconds */
static bool parse_ts(const char *s, int64_t *sec, int32_t *ns)
{
    int y, mo, d, h, mi, se;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &se) != 6) return false;
    *sec = days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + se;
    *ns = 0;
    const char *f = s + 19;
    if (*f == '.') {
        int digits = 0;
        for (f++; isdigit((unsigned char)*f) && digits < 9; f++, digits++) *ns = *ns * 10 + (*f - '0');
        for (; digits < 9; digits++) *ns *= 10;
    }
    return true;
}

static void log_append(const char *line, size_t n, bool err)
{
    /* local time for the prefix */
    int64_t sec;
    int32_t ns;
    char pre[16] = "";
    const char *msg = line;
    size_t ml = n;
    if (n > 20 && parse_ts(line, &sec, &ns)) {
        if (sec < s_log_last_s || (sec == s_log_last_s && ns <= s_log_last_ns)) return;   /* seen */
        s_log_last_s = sec;
        s_log_last_ns = ns;
        time_t t = (time_t)sec;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(pre, sizeof(pre), "%H:%M:%S ", &tm);
        const char *sp = memchr(line, ' ', n);
        if (sp) {
            msg = sp + 1;
            ml = n - (size_t)(msg - line);
        }
    }
    while (ml && (msg[ml - 1] == '\r' || msg[ml - 1] == '\n')) ml--;
    size_t need = strlen(pre) + (err ? 2 : 0) + ml + 1;
    if (need > LOG_CAP / 2) ml = LOG_CAP / 2 - 64;
    if (s_log_len + need >= LOG_CAP) {                  /* drop the oldest quarter */
        size_t cut = s_log_len / 4;
        while (cut < s_log_len && s_log[cut] != '\n') cut++;
        if (cut < s_log_len) cut++;
        memmove(s_log, s_log + cut, s_log_len - cut);
        s_log_len -= cut;
    }
    s_log_len += (size_t)snprintf(s_log + s_log_len, LOG_CAP - s_log_len, "%s%s%.*s\n", pre, err ? "! " : "", (int)ml, msg);
}

/* Docker's multiplexed stream (8-byte frame headers) or plain text (TTY) */
static void log_ingest(const char *b, size_t n)
{
    bool mux = n >= 8 && (b[0] == 0 || b[0] == 1 || b[0] == 2) && !b[1] && !b[2] && !b[3];
    static EXT_RAM_BSS_ATTR char part[2][2048];
    static size_t plen[2];
    plen[0] = plen[1] = 0;
    size_t i = 0;
    while (i < n) {
        const char *p;
        size_t len;
        int stream = 0;
        if (mux) {
            if (i + 8 > n) break;
            stream = b[i] == 2 ? 1 : 0;
            len = (size_t)((uint8_t)b[i + 4] << 24 | (uint8_t)b[i + 5] << 16 | (uint8_t)b[i + 6] << 8 | (uint8_t)b[i + 7]);
            p = b + i + 8;
            if (i + 8 + len > n) len = n - i - 8;
            i += 8 + len;
        } else {
            p = b + i;
            len = n - i;
            i = n;
        }
        /* split into lines, carrying partial lines per stream */
        for (size_t k = 0; k < len; k++) {
            char ch = p[k];
            if (ch == '\n') {
                log_append(part[stream], plen[stream], stream == 1);
                plen[stream] = 0;
            } else if (plen[stream] < sizeof(part[0]) - 1) {
                part[stream][plen[stream]++] = ch;
            }
        }
    }
    for (int s = 0; s < 2; s++) if (plen[s]) log_append(part[s], plen[s], s == 1);
}

static void fetch_logs(const char *id, bool first)
{
    char path[160];
    if (first || s_log_last_s < 0)
        snprintf(path, sizeof(path), "/containers/%s/logs?stdout=1&stderr=1&timestamps=1&tail=300", id);
    else
        snprintf(path, sizeof(path), "/containers/%s/logs?stdout=1&stderr=1&timestamps=1&since=%lld", id,
                 (long long)s_log_last_s);
    devos_http_resp_t r;
    if (api("GET", path, &r, 512 * 1024) == 200 && r.body) {
        LOCK();
        bool still = !strcmp(s_sel, id);
        UNLOCK();
        if (still) {
            LOCK();
            size_t before = s_log_len;
            log_ingest(r.body, r.body_len);
            if (s_log_len != before || first) s_log_gen++;
            UNLOCK();
        }
    } else if (first) {
        LOCK();
        s_log_len = (size_t)snprintf(s_log, LOG_CAP, "Couldn't get the logs: %s\n",
                                     r.status ? (r.status == 404 ? "container not found" : r.reason) : r.error);
        s_log_gen++;
        UNLOCK();
    }
    devos_http_resp_free(&r);
}

/* ------------------------------------------------------------------ action */
static void do_action(const char *id, const char *what)
{
    char path[96], name[64] = "";
    snprintf(path, sizeof(path), "/containers/%s/%s%s", id, what, strcmp(what, "start") ? "?t=10" : "");
    LOCK();
    for (int i = 0; i < s_nct; i++) if (!strcmp(s_ct[i].id, id)) snprintf(name, sizeof(name), "%s", s_ct[i].name);
    snprintf(s_st.note, sizeof(s_st.note), "%s %s...", !strcmp(what, "restart") ? "Restarting" : !strcmp(what, "stop") ? "Stopping" : "Starting",
             name[0] ? name : id);
    s_gen++;
    UNLOCK();
    devos_http_resp_t r;
    int st = api("POST", path, &r, 16 * 1024);
    LOCK();
    if (st == 204 || st == 304) {
        snprintf(s_st.note, sizeof(s_st.note), "%s %s", !strcmp(what, "restart") ? "Restarted" : !strcmp(what, "stop") ? "Stopped" : "Started",
                 name[0] ? name : id);
    } else {
        char msg[80] = "";
        if (r.body) devos_json_get_str(r.body, r.body_len, "message", msg, sizeof(msg));
        snprintf(s_st.note, sizeof(s_st.note), "Couldn't %s %s: %s", what, name[0] ? name : id,
                 msg[0] ? msg : st ? r.reason : r.error);
    }
    s_gen++;
    UNLOCK();
    devos_http_resp_free(&r);
}

/* ------------------------------------------------------------------ worker */
static void worker_loop(void)
{
    int64_t next_list = 0, next_logs = 0, next_stats = 0;
    char logs_for[16] = "", stats_for[16] = "";
    bool need_info = true;
    for (;;) {
#ifdef ESP_PLATFORM
        xSemaphoreTake(s_wake, pdMS_TO_TICKS(400));
#else
        pthread_mutex_lock(&s_wmx);
        if (!s_kick) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 400000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&s_wcv, &s_wmx, &ts);
        }
        pthread_mutex_unlock(&s_wmx);
#endif
        bool kick = s_kick;
        s_kick = false;
        if (!s_active || !devos_docker_configured()) {
            need_info = true;
            continue;
        }
        char act_id[16] = "", act_what[12] = "";
        LOCK();
        if (s_act_id[0]) {
            snprintf(act_id, sizeof(act_id), "%s", s_act_id);
            snprintf(act_what, sizeof(act_what), "%s", s_act_what);
            s_act_id[0] = '\0';
        }
        bool portainer = s_cfg.mode == DEVOS_DOCKER_PORTAINER;
        int ep = s_ep_resolved;
        int interval = s_cfg.interval_s;
        char sel[16];
        snprintf(sel, sizeof(sel), "%s", s_sel);
        bool sel_logs = s_sel_logs;
        UNLOCK();
        if (portainer && !ep) {
            fetch_endpoint();
            LOCK();
            ep = s_ep_resolved;
            UNLOCK();
            if (!ep) {
                next_list = now_ms() + 10000;
                continue;
            }
        }
        if (act_id[0]) {
            do_action(act_id, act_what);
            kick = true;
            next_stats = 0;
        }
        int64_t now = now_ms();
        if (kick || now >= next_list) {
            if (need_info) fetch_info();
            need_info = !fetch_list();
            next_list = now_ms() + interval * 1000;
        }
        if (sel[0] && sel_logs) {
            bool first = strcmp(logs_for, sel) != 0;
            if (first || now_ms() >= next_logs) {
                snprintf(logs_for, sizeof(logs_for), "%s", sel);
                fetch_logs(sel, first);
                next_logs = now_ms() + 2000;
            }
        } else {
            logs_for[0] = '\0';
        }
        if (sel[0] && (strcmp(stats_for, sel) || now_ms() >= next_stats)) {
            snprintf(stats_for, sizeof(stats_for), "%s", sel);
            fetch_stats(sel);
            next_stats = now_ms() + 5000;
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
void devos_docker_init(void)
{
    if (s_inited) return;
    s_inited = true;
#ifdef ESP_PLATFORM
    s_mx = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
#endif
    s_ct = big_alloc(sizeof(devos_docker_ct_t) * DEVOS_DOCKER_MAX);
    s_log = big_alloc(LOG_CAP);
    if (s_log) s_log[0] = '\0';
    load_config();
    s_st.configured = s_cfg.url[0] != '\0';
}

void devos_docker_set_active(bool active)
{
    s_active = active;
    LOCK();
    s_st.active = active;
    UNLOCK();
    if (active && !s_worker && s_ct && s_log) {
        s_worker = true;                            /* started on first use */
#ifdef ESP_PLATFORM
        xTaskCreatePinnedToCore(worker_task, "docker", 12288, NULL, 3, NULL, DEVOS_CORE_NET_CRYPTO);
#else
        pthread_t t;
        pthread_create(&t, NULL, worker_thread, NULL);
        pthread_detach(t);
#endif
    }
    if (active) wake();
}

void devos_docker_refresh(void) { wake(); }

void devos_docker_status(devos_docker_status_t *out)
{
    LOCK();
    *out = s_st;
    out->configured = s_cfg.url[0] != '\0';
    UNLOCK();
}

int devos_docker_list(devos_docker_ct_t *out, int max)
{
    LOCK();
    int n = s_nct < max ? s_nct : max;
    if (n > 0) memcpy(out, s_ct, sizeof(*out) * (size_t)n);
    UNLOCK();
    return n;
}

uint32_t devos_docker_generation(void)
{
    LOCK();
    uint32_t g = s_gen + s_log_gen;
    UNLOCK();
    return g;
}

int devos_docker_action(const char *id, const char *action)
{
    if (!id || !id[0] || !action) return -1;
    LOCK();
    snprintf(s_act_id, sizeof(s_act_id), "%s", id);
    snprintf(s_act_what, sizeof(s_act_what), "%s", action);
    UNLOCK();
    wake();
    return 0;
}

void devos_docker_select(const char *id, bool logs)
{
    LOCK();
    bool changed = strcmp(s_sel, id ? id : "") != 0;
    snprintf(s_sel, sizeof(s_sel), "%s", id ? id : "");
    bool logs_changed = logs != s_sel_logs;
    s_sel_logs = logs;
    if (changed) {
        memset(&s_stats, 0, sizeof(s_stats));
    }
    if (changed || (logs && logs_changed)) {
        s_log_len = 0;
        if (s_log) s_log[0] = '\0';
        s_log_last_s = -1;
        s_log_last_ns = 0;
        s_log_gen++;
    }
    UNLOCK();
    if (changed || logs_changed) wake();
}

void devos_docker_stats(devos_docker_stats_t *out)
{
    LOCK();
    *out = s_stats;
    UNLOCK();
}

size_t devos_docker_logs(char *out, size_t cap, uint32_t *gen)
{
    if (!out || !cap) {
        LOCK();
        if (gen) *gen = s_log_gen;
        UNLOCK();
        return 0;
    }
    LOCK();
    size_t n = s_log_len < cap - 1 ? s_log_len : cap - 1;
    /* keep the newest if it doesn't fit */
    const char *src = s_log ? s_log + (s_log_len - n) : "";
    memcpy(out, src, n);
    out[n] = '\0';
    if (gen) *gen = s_log_gen;
    UNLOCK();
    return n;
}

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
static volatile int s_init_lock;            /* once-only init guard across cores */
static volatile bool s_active, s_kick;
static int s_ep_resolved;                   /* Portainer endpoint in use */

/* Correlated container commands (PLAN.md 7.3): submitted from any task, run by
 * the single worker whether or not the UI is being polled. Each carries an
 * immutable config / endpoint snapshot so a later settings change cannot send
 * it to the wrong host, and the worker never overwrites another command. */
typedef struct {
    bool used;
    bool busy;                  /* the worker copied it and is running it */
    bool cancelled;
    bool ui_note;               /* UI-originated: update s_st.note, auto-free */
    bool inspect;
    uint32_t id;                /* ticket (> 0) */
    char what[12];
    char arg[80];               /* container id or name */
    char name[64];              /* UI display name, when known */
    devos_docker_config_t cfg;  /* snapshot at submit */
    int ep;                     /* Portainer endpoint snapshot (0 = resolve) */
    int timeout_ms;
    devos_docker_req_result_t res;
} docker_req_t;
static EXT_RAM_BSS_ATTR docker_req_t s_req[DEVOS_DOCKER_REQS];
static uint32_t s_req_seq;      /* monotonic ticket ids */

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
static int api_cfg(const devos_docker_config_t *c, int ep, const char *method, const char *path,
                   devos_http_resp_t *r, size_t max_body, int timeout_ms)
{
    char url[512], hdr[200] = "";
    if (c->mode == DEVOS_DOCKER_PORTAINER) {
        if (!strncmp(path, "/api/", 5)) snprintf(url, sizeof(url), "%s%s", c->url, path);
        else snprintf(url, sizeof(url), "%s/api/endpoints/%d/docker%s", c->url, ep, path);
        if (c->api_key[0]) snprintf(hdr, sizeof(hdr), "X-API-Key: %s", c->api_key);
    } else {
        snprintf(url, sizeof(url), "%s%s", c->url, path);
    }
    devos_http_req_t q = { .method = method, .url = url, .headers = hdr[0] ? hdr : NULL, .insecure = c->insecure,
                           .timeout_ms = timeout_ms > 0 ? timeout_ms : 12000, .max_body = max_body };
    devos_http_request(&q, r);
    return r->status;
}

static int api(const char *method, const char *path, devos_http_resp_t *r, size_t max_body)
{
    devos_docker_config_t c;
    int ep;
    LOCK();
    c = s_cfg;
    ep = s_ep_resolved;
    UNLOCK();
    return api_cfg(&c, ep, method, path, r, max_body, 12000);
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

/* Resolve a Portainer environment (0 = the first one) from an explicit config
 * snapshot. Returns the id, or 0 with *err set. */
static int resolve_endpoint_cfg(const devos_docker_config_t *cfg, int want, char *name, size_t namecap,
                                char *err, size_t errcap)
{
    devos_http_resp_t r;
    int found = 0;
    if (name && namecap) name[0] = '\0';
    if (err && errcap) err[0] = '\0';
    if (api_cfg(cfg, 0, "GET", "/api/endpoints", &r, 256 * 1024, 12000) == 200 && r.body) {
        const char *a = strchr(r.body, '[');
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
                if (name && namecap) devos_json_get_str(p, (size_t)(end - p), "Name", name, namecap);
            }
            p = end;
        }
        if (!found && err && errcap)
            snprintf(err, errcap, want ? "Portainer has no environment %d" : "Portainer has no environments", want);
    } else if (err && errcap) {
        if (!r.status) snprintf(err, errcap, "Portainer: %s", r.error);
        else snprintf(err, errcap, "Portainer: HTTP %d %.40s", r.status, r.reason);
    }
    devos_http_resp_free(&r);
    return found;
}

static void fetch_endpoint(void)
{
    devos_docker_config_t cfg;
    int want;
    LOCK();
    cfg = s_cfg;
    want = s_cfg.endpoint;
    UNLOCK();
    char name[48] = "", err[112] = "";
    int id = resolve_endpoint_cfg(&cfg, want, name, sizeof(name), err, sizeof(err));
    LOCK();
    if (id) {
        s_ep_resolved = id;
        if (name[0]) snprintf(s_st.endpoint_name, sizeof(s_st.endpoint_name), "%s", name);
    } else if (err[0]) {
        snprintf(s_st.error, sizeof(s_st.error), "%s", err);
    }
    s_gen++;
    UNLOCK();
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

/* Parse a /containers/<id>/json inspect response. Returns false when the body
 * isn't a JSON object; absent fields stay empty/0. Never consults the UI's
 * cached list, so it reflects the daemon's current state. */
static bool parse_inspect(const char *b, size_t n, devos_docker_inspect_t *out)
{
    if (!b || !n || *b != '{') return false;
    memset(out, 0, sizeof(*out));
    char id[80] = "", name[96] = "";
    devos_json_get_str(b, n, "Id", id, sizeof(id));
    snprintf(out->id, sizeof(out->id), "%.12s", id);
    devos_json_get_str(b, n, "Name", name, sizeof(name));
    snprintf(out->name, sizeof(out->name), "%s", name[0] == '/' ? name + 1 : name);
    size_t cl;
    const char *cfg = span_of(b, n, "Config", &cl);
    if (cfg) devos_json_get_str(cfg, cl, "Image", out->image, sizeof(out->image));
    size_t sl;
    const char *st = span_of(b, n, "State", &sl);
    if (st) {
        devos_json_get_str(st, sl, "Status", out->state, sizeof(out->state));
        char started[40] = "";
        if (devos_json_get_str(st, sl, "StartedAt", started, sizeof(started)) == 0 && started[0]) {
            int64_t sec;
            int32_t ns;
            if (parse_ts(started, &sec, &ns)) out->started = sec;
        }
        size_t hl;
        const char *h = span_of(st, sl, "Health", &hl);
        if (h) devos_json_get_str(h, hl, "Status", out->health, sizeof(out->health));
    }
    if (!out->name[0]) snprintf(out->name, sizeof(out->name), "%s", out->id);
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

/* ------------------------------------------------------------------ requests */
static int run_action_cfg(const devos_docker_config_t *cfg, int ep, const char *id, const char *what,
                          int timeout_ms, char *err, size_t errcap)
{
    char path[96];
    snprintf(path, sizeof(path), "/containers/%s/%s%s", id, what, strcmp(what, "start") ? "?t=10" : "");
    if (err && errcap) err[0] = '\0';
    devos_http_resp_t r;
    int st = api_cfg(cfg, ep, "POST", path, &r, 16 * 1024, timeout_ms);
    if (st != 204 && st != 304 && err && errcap) {
        char msg[80] = "";
        if (r.body) devos_json_get_str(r.body, r.body_len, "message", msg, sizeof(msg));
        if (!st) snprintf(err, errcap, "%s", r.error[0] ? r.error : "request failed");
        else snprintf(err, errcap, "%s", msg[0] ? msg : (r.reason[0] ? r.reason : "HTTP error"));
    }
    devos_http_resp_free(&r);
    return st;
}

static int run_inspect_cfg(const devos_docker_config_t *cfg, int ep, const char *id, int timeout_ms,
                           devos_docker_inspect_t *out, char *err, size_t errcap)
{
    char path[128];
    snprintf(path, sizeof(path), "/containers/%s/json", id);
    if (err && errcap) err[0] = '\0';
    devos_http_resp_t r;
    int st = api_cfg(cfg, ep, "GET", path, &r, 256 * 1024, timeout_ms);
    if (st == 200 && r.body) {
        parse_inspect(r.body, r.body_len, out);
        out->observed = (int64_t)time(NULL);
    } else if (err && errcap) {
        char msg[80] = "";
        if (r.body) devos_json_get_str(r.body, r.body_len, "message", msg, sizeof(msg));
        if (!st) snprintf(err, errcap, "%s", r.error[0] ? r.error : "request failed");
        else if (st == 404) snprintf(err, errcap, "container not found");
        else snprintf(err, errcap, "%s", msg[0] ? msg : (r.reason[0] ? r.reason : "HTTP error"));
    }
    devos_http_resp_free(&r);
    return st;
}

/* Run every pending request once. Serialised by the single worker: a command is
 * copied out under the lock, run without it, then its own slot - matched by
 * ticket, never a shared "last command" - is filled. A released or cancelled
 * ticket is discarded rather than reported. */
static void process_requests(void)
{
    for (;;) {
        docker_req_t local;
        int idx = -1;
        LOCK();
        for (int i = 0; i < DEVOS_DOCKER_REQS; i++) {
            if (s_req[i].used && !s_req[i].busy && s_req[i].res.state == DEVOS_DOCKER_REQ_PENDING) { idx = i; break; }
        }
        if (idx >= 0) {
            local = s_req[idx];
            s_req[idx].busy = true;
        }
        UNLOCK();
        if (idx < 0) return;

        int st = 0;
        char err[sizeof(local.res.error)] = "";
        devos_docker_inspect_t ins;
        memset(&ins, 0, sizeof(ins));
        if (!local.cfg.url[0]) {
            snprintf(err, sizeof(err), "Docker is not configured");
        } else {
            int ep = local.ep;
            if (local.cfg.mode == DEVOS_DOCKER_PORTAINER && !ep)
                ep = resolve_endpoint_cfg(&local.cfg, local.cfg.endpoint, NULL, 0, err, sizeof(err));
            if (ep || local.cfg.mode != DEVOS_DOCKER_PORTAINER) {
                if (local.inspect) st = run_inspect_cfg(&local.cfg, ep, local.arg, local.timeout_ms, &ins, err, sizeof(err));
                else st = run_action_cfg(&local.cfg, ep, local.arg, local.what, local.timeout_ms, err, sizeof(err));
            }
        }

        LOCK();
        docker_req_t *r = &s_req[idx];
        if (r->used && r->id == local.id && !r->cancelled) {
            r->res.status = st;
            r->res.inspect = ins;
            snprintf(r->res.error, sizeof(r->res.error), "%s", err);
            r->res.state = local.cfg.url[0] ? DEVOS_DOCKER_REQ_DONE : DEVOS_DOCKER_REQ_FAILED;
            if (local.ui_note) {
                const char *nm = local.name[0] ? local.name : (ins.name[0] ? ins.name : local.arg);
                bool is_restart = !strcmp(local.what, "restart");
                bool is_stop = !strcmp(local.what, "stop");
                const char *verb = is_restart ? "restart" : is_stop ? "stop" : "start";
                if (st == 204 || st == 304)
                    snprintf(s_st.note, sizeof(s_st.note), "%s %s",
                             is_restart ? "Restarted" : is_stop ? "Stopped" : "Started", nm);
                else
                    snprintf(s_st.note, sizeof(s_st.note), "Couldn't %s %s: %s", verb, nm, err[0] ? err : "no response");
                s_gen++;
            }
            r->busy = false;
            if (!local.inspect) s_kick = true;  /* refresh the UI list, if shown */
            if (local.ui_note) {
                r->used = false;                /* UI never polls: free now */
                memset(r, 0, sizeof(*r));
            }
        } else if (r->used && r->id == local.id) {
            r->busy = false;                    /* cancelled: discard the result */
        }
        UNLOCK();
    }
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

        /* Container commands run whether or not the UI is being polled; only
         * the list/stats/logs refresh below is gated by s_active. */
        process_requests();
        kick = kick || s_kick;
        s_kick = false;

        if (!s_active || !devos_docker_configured()) {
            need_info = true;
            continue;
        }
        LOCK();
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

/* Start the worker on first use (UI or Jobs). Container commands run even
 * when the UI is not polling, so a Jobs-only request must start it too. */
static void ensure_worker(void)
{
    if (s_worker || !s_ct || !s_log) return;
    s_worker = true;
#ifdef ESP_PLATFORM
    xTaskCreatePinnedToCore(worker_task, "docker", 12288, NULL, 3, NULL, DEVOS_CORE_NET_CRYPTO);
#else
    pthread_t t;
    pthread_create(&t, NULL, worker_thread, NULL);
    pthread_detach(t);
#endif
}

/* ------------------------------------------------------------------ requests */
static uint32_t req_submit(const char *id, const char *action, int timeout_ms, bool ui_note)
{
    if (!id || !id[0] || !action || !action[0]) return 0;
    devos_docker_init();
    if (!devos_docker_configured()) return 0;
    uint32_t ticket = 0;
    LOCK();
    int slot = -1;
    for (int i = 0; i < DEVOS_DOCKER_REQS; i++) if (!s_req[i].used) { slot = i; break; }
    if (slot >= 0) {
        docker_req_t *r = &s_req[slot];
        memset(r, 0, sizeof(*r));
        r->used = true;
        r->id = ++s_req_seq;
        if (!r->id) r->id = ++s_req_seq;            /* never 0 */
        r->ui_note = ui_note;
        r->inspect = strcmp(action, "inspect") == 0;
        snprintf(r->what, sizeof(r->what), "%s", action);
        snprintf(r->arg, sizeof(r->arg), "%s", id);
        if (ui_note) {                              /* display name for the note */
            for (int i = 0; i < s_nct; i++)
                if (!strcmp(s_ct[i].id, id)) { snprintf(r->name, sizeof(r->name), "%s", s_ct[i].name); break; }
        }
        r->cfg = s_cfg;                             /* immutable snapshot */
        r->ep = s_ep_resolved;
        r->timeout_ms = timeout_ms > 0 ? timeout_ms : 12000;
        r->res.state = DEVOS_DOCKER_REQ_PENDING;
        snprintf(r->res.action, sizeof(r->res.action), "%s", action);
        ticket = r->id;
    }
    UNLOCK();
    if (ticket) {
        ensure_worker();
        wake();
    }
    return ticket;
}

uint32_t devos_docker_request(const char *id, const char *action, int timeout_ms)
{
    return req_submit(id, action, timeout_ms, false);
}

uint32_t devos_docker_inspect(const char *id, int timeout_ms)
{
    return req_submit(id, "inspect", timeout_ms, false);
}

bool devos_docker_request_poll(uint32_t ticket, devos_docker_req_result_t *out)
{
    if (!ticket) return false;
    bool ok = false;
    LOCK();
    for (int i = 0; i < DEVOS_DOCKER_REQS; i++)
        if (s_req[i].used && s_req[i].id == ticket) {
            if (out) *out = s_req[i].res;
            ok = true;
            break;
        }
    UNLOCK();
    return ok;
}

void devos_docker_request_cancel(uint32_t ticket)
{
    if (!ticket) return;
    LOCK();
    for (int i = 0; i < DEVOS_DOCKER_REQS; i++)
        if (s_req[i].used && s_req[i].id == ticket) {
            s_req[i].cancelled = true;
            s_req[i].res.state = DEVOS_DOCKER_REQ_FAILED;
            snprintf(s_req[i].res.error, sizeof(s_req[i].res.error), "cancelled");
            break;
        }
    UNLOCK();
}

void devos_docker_request_release(uint32_t ticket)
{
    if (!ticket) return;
    LOCK();
    for (int i = 0; i < DEVOS_DOCKER_REQS; i++)
        if (s_req[i].used && s_req[i].id == ticket) {
            s_req[i].used = false;
            s_req[i].busy = false;
            break;
        }
    UNLOCK();
}

/* ------------------------------------------------------------------ api */
void devos_docker_init(void)
{
    if (s_inited) return;
    /* The Docker app (Core 1) and a Jobs provider (Core 0) may both call this
     * first; the winner allocates once, the loser spins then returns. */
    while (__sync_lock_test_and_set(&s_init_lock, 1)) { /* spin briefly */ }
    if (s_inited) { __sync_lock_release(&s_init_lock); return; }
#ifdef ESP_PLATFORM
    s_mx = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
#endif
    s_ct = big_alloc(sizeof(devos_docker_ct_t) * DEVOS_DOCKER_MAX);
    s_log = big_alloc(LOG_CAP);
    if (s_log) s_log[0] = '\0';
    load_config();
    s_st.configured = s_cfg.url[0] != '\0';
    s_inited = true;
    __sync_lock_release(&s_init_lock);
}

void devos_docker_set_active(bool active)
{
    s_active = active;
    LOCK();
    s_st.active = active;
    UNLOCK();
    if (active) ensure_worker();
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
    /* Immediate UI feedback; the worker corrects the note when it completes. */
    char nm[64] = "";
    LOCK();
    for (int i = 0; i < s_nct; i++)
        if (!strcmp(s_ct[i].id, id)) { snprintf(nm, sizeof(nm), "%s", s_ct[i].name); break; }
    if (!nm[0]) snprintf(nm, sizeof(nm), "%s", id);
    snprintf(s_st.note, sizeof(s_st.note), "%s %s...",
             !strcmp(action, "restart") ? "Restarting" : !strcmp(action, "stop") ? "Stopping" : "Starting", nm);
    s_gen++;
    UNLOCK();
    return req_submit(id, action, 0, true) ? 0 : -1;
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

/* ------------------------------------------------------------------ deep links */
int devos_docker_published(const devos_docker_ct_t *c, int priv)
{
    /* c->ports: "8080->80/tcp, 443/tcp" (host->container, unpublished alone) */
    for (const char *p = c ? c->ports : ""; *p;) {
        while (*p == ' ' || *p == ',') p++;
        int a = 0, b = 0;
        char type[8] = "";
        if (sscanf(p, "%d->%d/%7[a-z]", &a, &b, type) == 3 && b == priv && !strcmp(type, "tcp")) return a;
        while (*p && *p != ',') p++;
    }
    return 0;
}

/* The Docker host: the server URL's host name. */
static bool docker_host(char *out, size_t cap)
{
    devos_docker_config_t cfg;
    devos_docker_get_config(&cfg);
    bool https;
    int port;
    char path[8];
    return devos_http_parse_url(cfg.url, &https, out, cap, &port, path, sizeof(path)) == 0 && out[0];
}

bool devos_docker_ssh_target(const devos_docker_ct_t *c, char *out, size_t cap)
{
    int pub = devos_docker_published(c, 22);
    if (!pub) pub = devos_docker_published(c, 2222);
    char host[96];
    if (!pub || !docker_host(host, sizeof(host))) return false;
    snprintf(out, cap, "%s:%d", host, pub);
    return true;
}

bool devos_docker_web_url(const devos_docker_ct_t *c, char *out, size_t cap)
{
    static const struct { int port; bool tls; } web[] = {
        { 80, false }, { 443, true }, { 8080, false }, { 8443, true }, { 8000, false }, { 3000, false },
        { 5000, false }, { 5001, true }, { 8081, false }, { 8008, false }, { 8888, false }, { 9000, false },
        { 9090, false }, { 9443, true },
    };
    char host[96];
    for (size_t i = 0; i < sizeof(web) / sizeof(web[0]); i++) {
        int pub = devos_docker_published(c, web[i].port);
        if (!pub) continue;
        if (!docker_host(host, sizeof(host))) return false;
        snprintf(out, cap, "%s://%s:%d/", web[i].tls ? "https" : "http", host, pub);
        return true;
    }
    return false;
}

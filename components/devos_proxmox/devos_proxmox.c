/* devos_proxmox: see devos_proxmox.h. */
#include "devos_proxmox.h"
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

#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

#define CFG_FILE   TAB5_SD_MOUNT_POINT "/.devos/proxmox.json"
#define BODY_MAX   (256 * 1024)         /* /cluster/resources for the whole cluster */
#define API_TIMEOUT 12000

static devos_proxmox_config_t s_cfg;
static devos_proxmox_status_t s_st;
static devos_proxmox_guest_t *s_guest;      /* DEVOS_PROXMOX_MAX */
static int s_nguest;
static devos_proxmox_node_t *s_node;        /* DEVOS_PROXMOX_MAX_NODES */
static int s_nnode;
static uint32_t s_gen;
static bool s_worker, s_inited;
static volatile bool s_active, s_kick;

typedef struct {
    uint32_t id;                    /* ticket id, 0 = free */
    volatile int state;             /* 0 free, 1 pending, 2 running, 3 done */
    bool used, started, cancelled;
    bool ui;                        /* fire-and-forget (the UI wrapper): the
                                     * worker frees the slot when it finishes */
    int vmid;
    char action[12];
    int timeout_ms;
    int status;
    int out_state;                  /* DEVOS_PROXMOX_REQ_DONE / _FAILED */
    char error[96];
    char task[80];
    devos_proxmox_inspect_t inspect;
    devos_proxmox_config_t cfg;     /* snapshot at submit: a later settings
                                     * change cannot redirect this request */
} prox_req_t;

static EXT_RAM_BSS_ATTR prox_req_t s_req[DEVOS_PROXMOX_REQS];
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
    s_cfg.interval_s = 10;
    FILE *f = fopen(CFG_FILE, "rb");
    if (f) {
        char buf[1024];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        int v;
        devos_json_get_str(buf, n, "url", s_cfg.url, sizeof(s_cfg.url));
        devos_json_get_str(buf, n, "token_id", s_cfg.token_id, sizeof(s_cfg.token_id));
        if (devos_json_get_int(buf, n, "insecure", &v) == 0) s_cfg.insecure = v != 0;
        if (devos_json_get_int(buf, n, "interval", &v) == 0 && v >= 2 && v <= 300) s_cfg.interval_s = v;
#ifndef ESP_PLATFORM
        devos_json_get_str(buf, n, "secret", s_cfg.secret, sizeof(s_cfg.secret));
#endif
    }
#ifdef ESP_PLATFORM
    /* The token secret is a credential, so it lives in NVS rather than the
     * card. It is not encrypted at rest until NVS encryption is on (the same
     * statement devos_secrets makes); the URL and token id are not secret. */
    nvs_handle_t h;
    if (nvs_open("proxmox", NVS_READONLY, &h) == ESP_OK) {
        size_t l = sizeof(s_cfg.secret);
        if (nvs_get_str(h, "secret", s_cfg.secret, &l) != ESP_OK) s_cfg.secret[0] = '\0';
        nvs_close(h);
    }
#endif
}

static void save_config(void)
{
    mkdir(TAB5_SD_MOUNT_POINT "/.devos", 0755);
    FILE *f = fopen(CFG_FILE, "wb");
    if (f) {
        char url[400], tid[300];
        devos_json_escape(s_cfg.url, url, sizeof(url));
        devos_json_escape(s_cfg.token_id, tid, sizeof(tid));
        fprintf(f, "{\"url\": \"%s\", \"token_id\": \"%s\", \"insecure\": %d, \"interval\": %d",
                url, tid, s_cfg.insecure ? 1 : 0, s_cfg.interval_s);
#ifndef ESP_PLATFORM
        char sec[300];
        devos_json_escape(s_cfg.secret, sec, sizeof(sec));
        fprintf(f, ", \"secret\": \"%s\"", sec);           /* simulator only: no NVS */
#endif
        fprintf(f, "}\n");
        fclose(f);
    }
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("proxmox", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "secret", s_cfg.secret);
        nvs_commit(h);
        nvs_close(h);
    }
#endif
}

void devos_proxmox_get_config(devos_proxmox_config_t *out)
{
    LOCK();
    *out = s_cfg;
    UNLOCK();
}

bool devos_proxmox_configured(void) { return s_cfg.url[0] != '\0' && s_cfg.token_id[0] != '\0'; }

void devos_proxmox_set_config(const devos_proxmox_config_t *c)
{
    LOCK();
    s_cfg = *c;
    if (s_cfg.interval_s < 2) s_cfg.interval_s = 10;
    size_t l = strlen(s_cfg.url);
    while (l && (s_cfg.url[l - 1] == '/' || s_cfg.url[l - 1] == ' ')) s_cfg.url[--l] = '\0';
    UNLOCK();
    save_config();
    wake();
}

/* ------------------------------------------------------------------ http */
static int api_cfg(const devos_proxmox_config_t *c, const char *method, const char *path,
                   devos_http_resp_t *r, size_t max_body, int timeout_ms)
{
    char url[400], hdr[300];
    snprintf(url, sizeof(url), "%s/api2/json%s", c->url, path);
    /* API token: one header, no session to renew. */
    snprintf(hdr, sizeof(hdr), "Authorization: PVEAPIToken=%s=%s", c->token_id, c->secret);
    devos_http_req_t q = { .method = method, .url = url, .headers = hdr, .insecure = c->insecure,
                           .timeout_ms = timeout_ms > 0 ? timeout_ms : API_TIMEOUT, .max_body = max_body };
    devos_http_request(&q, r);
    memset(hdr, 0, sizeof(hdr));                 /* the secret copy is devos_http's now */
    return r->status;
}

static int api(const char *method, const char *path, devos_http_resp_t *r, size_t max_body)
{
    devos_proxmox_config_t c;
    LOCK();
    c = s_cfg;
    UNLOCK();
    return api_cfg(&c, method, path, r, max_body, API_TIMEOUT);
}

static void set_error_from(const devos_http_resp_t *r, const char *what)
{
    LOCK();
    if (!r->status) snprintf(s_st.error, sizeof(s_st.error), "%s: %s", what, r->error);
    else if (r->status == 401 || r->status == 403)
        snprintf(s_st.error, sizeof(s_st.error), "%s: access denied (%d) - check the API token and its permissions",
                 what, r->status);
    else if (r->status == 404) snprintf(s_st.error, sizeof(s_st.error), "%s: not found (404) - wrong URL or vmid?", what);
    else if (r->status == 595 || (r->status == 0 && strstr(r->error, "certificate")))
        snprintf(s_st.error, sizeof(s_st.error), "%s: TLS rejected - turn on \"Accept any certificate\"", what);
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

/* The top-level "data" as a string (Proxmox returns a task UPID here for an
 * action, and "null" when it accepted without a task). */
static void data_str(const char *js, size_t len, char *out, size_t cap)
{
    out[0] = '\0';
    const char *d = devos_json_find_key(js, js + len, "data");
    if (!d) return;
    while (d < js + len && isspace((unsigned char)*d)) d++;
    if (d >= js + len || *d != '"') return;
    devos_json_parse_str(d, js + len, out, cap);
}

/* ------------------------------------------------------------------ guests */
static int guest_cmp(const void *a, const void *b)
{
    const devos_proxmox_guest_t *x = a, *y = b;
    bool xr = !strcmp(x->status, "running"), yr = !strcmp(y->status, "running");
    if (xr != yr) return xr ? -1 : 1;
    int c = strcasecmp(x->name, y->name);
    if (c) return c;
    return x->vmid - y->vmid;
}

typedef struct {
    devos_proxmox_guest_t *out;
    int n, max;
} guest_ud_t;

static void guest_one(const char *e, size_t len, void *ud)
{
    guest_ud_t *u = ud;
    if (u->n >= u->max || *e != '{') return;
    devos_proxmox_guest_t *g = &u->out[u->n];
    memset(g, 0, sizeof(*g));
    int vmid = 0;
    char type[12] = "";
    if (devos_json_get_int(e, len, "vmid", &vmid) != 0 || vmid <= 0) return;
    devos_json_get_str(e, len, "type", type, sizeof(type));
    /* /cluster/resources lists every guest; only VMs and containers are ours. */
    if (strcmp(type, "qemu") && strcmp(type, "lxc")) return;
    g->vmid = vmid;
    g->kind = strcmp(type, "lxc") == 0 ? DEVOS_PROXMOX_LXC : DEVOS_PROXMOX_QEMU;
    devos_json_get_str(e, len, "name", g->name, sizeof(g->name));
    if (!g->name[0]) snprintf(g->name, sizeof(g->name), "%s/%d", type, vmid);
    devos_json_get_str(e, len, "node", g->node, sizeof(g->node));
    devos_json_get_str(e, len, "status", g->status, sizeof(g->status));
    g->mem = get_u64(e, len, "mem");
    g->maxmem = get_u64(e, len, "maxmem");
    g->disk = get_u64(e, len, "disk");
    g->maxdisk = get_u64(e, len, "maxdisk");
    g->uptime_s = (int64_t)get_u64(e, len, "uptime");
    const char *cp = devos_json_find_key(e, e + len, "cpu");
    g->cpu = cp ? (float)strtod(cp, NULL) : 0.0f;
    const char *tpl = devos_json_find_key(e, e + len, "template");
    g->template_guest = tpl && *tpl == '1';
    u->n++;
}

static bool fetch_guests(void)
{
    devos_http_resp_t r;
    bool ok = false;
    LOCK();
    s_st.busy = true;
    UNLOCK();
    if (api("GET", "/cluster/resources?type=vm", &r, BODY_MAX) == 200 && r.body && !r.truncated) {
        static devos_proxmox_guest_t *tmp;
        if (!tmp) tmp = big_alloc(sizeof(devos_proxmox_guest_t) * DEVOS_PROXMOX_MAX);
        if (tmp) {
            guest_ud_t u = { tmp, 0, DEVOS_PROXMOX_MAX };
            const char *a = strchr(r.body, '[');
            if (a) devos_json_array_each(a, r.body_len - (size_t)(a - r.body), guest_one, &u);
            qsort(tmp, (size_t)u.n, sizeof(*tmp), guest_cmp);
            LOCK();
            memcpy(s_guest, tmp, sizeof(*tmp) * (size_t)u.n);
            s_nguest = u.n;
            s_st.total = u.n;
            s_st.running = 0;
            for (int i = 0; i < u.n; i++) s_st.running += !strcmp(tmp[i].status, "running");
            s_st.updated = time(NULL);
            s_st.error[0] = '\0';
            s_gen++;
            UNLOCK();
            ok = true;
        }
    } else if (r.truncated) {
        LOCK();
        snprintf(s_st.error, sizeof(s_st.error), "The cluster resource list is too big (over 256 KB)");
        s_gen++;
        UNLOCK();
    } else {
        set_error_from(&r, "Guests");
    }
    devos_http_resp_free(&r);
    LOCK();
    s_st.busy = false;
    UNLOCK();
    return ok;
}

/* ------------------------------------------------------------------ nodes */
typedef struct {
    devos_proxmox_node_t *out;
    int n, max;
} node_ud_t;

static void node_one(const char *e, size_t len, void *ud)
{
    node_ud_t *u = ud;
    if (u->n >= u->max || *e != '{') return;
    devos_proxmox_node_t *n = &u->out[u->n];
    memset(n, 0, sizeof(*n));
    devos_json_get_str(e, len, "node", n->node, sizeof(n->node));
    if (!n->node[0]) return;
    devos_json_get_str(e, len, "status", n->status, sizeof(n->status));
    n->mem = get_u64(e, len, "mem");
    n->maxmem = get_u64(e, len, "maxmem");
    n->uptime_s = (int64_t)get_u64(e, len, "uptime");
    const char *c = devos_json_find_key(e, e + len, "cpu");
    n->cpu = c ? (float)strtod(c, NULL) : 0.0f;
    u->n++;
}

static void fetch_nodes(void)
{
    devos_http_resp_t r;
    if (api("GET", "/nodes", &r, BODY_MAX) == 200 && r.body && !r.truncated) {
        static devos_proxmox_node_t *tmp;
        if (!tmp) tmp = big_alloc(sizeof(devos_proxmox_node_t) * DEVOS_PROXMOX_MAX_NODES);
        if (tmp) {
            node_ud_t u = { tmp, 0, DEVOS_PROXMOX_MAX_NODES };
            const char *a = strchr(r.body, '[');
            if (a) devos_json_array_each(a, r.body_len - (size_t)(a - r.body), node_one, &u);
            LOCK();
            memcpy(s_node, tmp, sizeof(*tmp) * (size_t)u.n);
            s_nnode = u.n;
            s_st.nodes = u.n;
            s_gen++;
            UNLOCK();
        }
    } else if (!r.truncated) {
        set_error_from(&r, "Nodes");
    }
    devos_http_resp_free(&r);
}

static void fetch_version(void)
{
    devos_http_resp_t r;
    if (api("GET", "/version", &r, 4096) == 200 && r.body) {
        char v[24] = "";
        size_t sl;
        const char *d = span_of(r.body, r.body_len, "data", &sl);
        if (d) devos_json_get_str(d, sl, "version", v, sizeof(v));
        LOCK();
        snprintf(s_st.version, sizeof(s_st.version), "%s", v);
        snprintf(s_st.host, sizeof(s_st.host), "%s", s_cfg.url);
        s_gen++;
        UNLOCK();
    }
    devos_http_resp_free(&r);
}

/* ------------------------------------------------------------------ resolve */
/* vmid -> (node, "qemu"/"lxc"). Jobs and the UI only need the vmid, so a job
 * never has to know which node a guest lives on. */
static bool resolve_guest(int vmid, char *node, size_t nodecap, char *kind, size_t kindcap)
{
    for (int pass = 0; pass < 2; pass++) {
        LOCK();
        for (int i = 0; i < s_nguest; i++) {
            if (s_guest[i].vmid != vmid) continue;
            snprintf(node, nodecap, "%s", s_guest[i].node);
            snprintf(kind, kindcap, "%s", s_guest[i].kind == DEVOS_PROXMOX_LXC ? "lxc" : "qemu");
            UNLOCK();
            return true;
        }
        UNLOCK();
        if (pass == 0) fetch_guests();          /* not cached: look it up once */
    }
    return false;
}

/* ------------------------------------------------------------------ requests */
static void run_status_cfg(const devos_proxmox_config_t *c, int vmid, int timeout_ms,
                           devos_proxmox_req_result_t *out)
{
    char node[32], kind[8];
    if (!resolve_guest(vmid, node, sizeof(node), kind, sizeof(kind))) {
        out->state = DEVOS_PROXMOX_REQ_FAILED;
        snprintf(out->error, sizeof(out->error), "no guest with vmid %d in the cluster", vmid);
        return;
    }
    char path[128];
    snprintf(path, sizeof(path), "/nodes/%s/%s/%d/status/current", node, kind, vmid);
    devos_http_resp_t r;
    int st = api_cfg(c, "GET", path, &r, 64 * 1024, timeout_ms);
    out->state = DEVOS_PROXMOX_REQ_DONE;
    out->status = st;
    if (st == 200 && r.body && !r.truncated) {
        size_t sl;
        const char *d = span_of(r.body, r.body_len, "data", &sl);
        if (d) {
            devos_proxmox_inspect_t *g = &out->inspect;
            g->vmid = vmid;
            g->kind = strcmp(kind, "lxc") == 0 ? DEVOS_PROXMOX_LXC : DEVOS_PROXMOX_QEMU;
            devos_json_get_str(d, sl, "name", g->name, sizeof(g->name));
            snprintf(g->node, sizeof(g->node), "%s", node);
            devos_json_get_str(d, sl, "status", g->status, sizeof(g->status));
            g->mem = get_u64(d, sl, "mem");
            g->maxmem = get_u64(d, sl, "maxmem");
            g->uptime_s = (int64_t)get_u64(d, sl, "uptime");
            const char *cp = devos_json_find_key(d, d + sl, "cpu");
            g->cpu = cp ? (float)strtod(cp, NULL) : 0.0f;
            g->observed = time(NULL);
        }
    } else if (!r.error[0] && r.body) {
        snprintf(out->error, sizeof(out->error), "HTTP %d %.60s", st, r.reason);
    } else {
        snprintf(out->error, sizeof(out->error), "%s", r.error);
    }
    devos_http_resp_free(&r);
}

static void run_action_cfg(const devos_proxmox_config_t *c, int vmid, const char *action,
                           int timeout_ms, devos_proxmox_req_result_t *out)
{
    char node[32], kind[8];
    if (!resolve_guest(vmid, node, sizeof(node), kind, sizeof(kind))) {
        out->state = DEVOS_PROXMOX_REQ_FAILED;
        snprintf(out->error, sizeof(out->error), "no guest with vmid %d in the cluster", vmid);
        return;
    }
    char path[128];
    snprintf(path, sizeof(path), "/nodes/%s/%s/%d/status/%s", node, kind, vmid, action);
    devos_http_resp_t r;
    int st = api_cfg(c, "POST", path, &r, 8192, timeout_ms);
    out->state = DEVOS_PROXMOX_REQ_DONE;
    out->status = st;
    if (st == 200 && r.body) {
        data_str(r.body, r.body_len, out->task, sizeof(out->task));
    } else if (r.body && !r.error[0]) {
        /* Proxmox puts a reason in the body for a 4xx/5xx. */
        char msg[64] = "";
        devos_json_get_str(r.body, r.body_len, "message", msg, sizeof(msg));
        snprintf(out->error, sizeof(out->error), "HTTP %d %.60s%s%s", st, r.reason,
                 msg[0] ? ": " : "", msg);
    } else {
        snprintf(out->error, sizeof(out->error), "%s", r.error);
    }
    devos_http_resp_free(&r);
}

static void process_requests(void)
{
    for (int i = 0; i < DEVOS_PROXMOX_REQS; i++) {
        prox_req_t *q = &s_req[i];
        LOCK();
        bool take = q->used && q->state == 1 && !q->cancelled;
        devos_proxmox_config_t cfg = q->cfg;    /* the snapshot taken at submit */
        int vmid = q->vmid, timeout = q->timeout_ms;
        char action[12];
        snprintf(action, sizeof(action), "%s", q->action);
        if (take) { q->state = 2; q->started = true; }
        UNLOCK();
        if (!take) continue;

        devos_proxmox_req_result_t res;
        memset(&res, 0, sizeof(res));
        snprintf(res.action, sizeof(res.action), "%s", action);
        if (!devos_proxmox_configured()) {
            res.state = DEVOS_PROXMOX_REQ_FAILED;
            snprintf(res.error, sizeof(res.error), "Proxmox is not configured");
        } else if (!strcmp(action, "status")) {
            run_status_cfg(&cfg, vmid, timeout, &res);
        } else {
            run_action_cfg(&cfg, vmid, action, timeout, &res);
        }

        LOCK();
        bool keep = !q->cancelled;
        q->status = res.status;
        q->out_state = res.state;
        snprintf(q->error, sizeof(q->error), "%s", res.error);
        snprintf(q->task, sizeof(q->task), "%s", res.task);
        q->inspect = res.inspect;
        q->state = 3;
        if (keep && res.state == DEVOS_PROXMOX_REQ_FAILED)
            snprintf(s_st.error, sizeof(s_st.error), "%s", res.error);
        else if (keep && res.status == 200)
            snprintf(s_st.note, sizeof(s_st.note), "%s vmid %d accepted", action, vmid);
        s_gen++;
        /* A fire-and-forget UI request has no poller, so the worker frees its
         * slot; Jobs tickets stay readable until their release(). */
        if (q->ui) memset(q, 0, sizeof(*q));
        UNLOCK();
        if (keep) wake();                        /* the UI refreshes after an action */
    }
}

/* ------------------------------------------------------------------ worker */
static void worker_loop(void)
{
    int64_t next_list = 0;
    bool need_meta = true;
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
        s_kick = false;

        /* Commands run whether or not the UI is being polled; only the list
         * refresh below is gated by s_active. */
        process_requests();

        if (!s_active || !devos_proxmox_configured()) {
            need_meta = true;
            continue;
        }
        if (need_meta) {
            fetch_version();
            need_meta = false;
        }
        if (now_ms() >= next_list) {
            LOCK();
            int interval = s_cfg.interval_s;
            UNLOCK();
            fetch_nodes();
            fetch_guests();
            next_list = now_ms() + (int64_t)interval * 1000;
        }
    }
}

#ifdef ESP_PLATFORM
static void worker_task(void *arg)
{
    (void)arg;
    worker_loop();
    vTaskDelete(NULL);
}
#else
static void *worker_thread(void *arg)
{
    (void)arg;
    worker_loop();
    return NULL;
}
#endif

static void ensure_worker(void)
{
    if (s_worker) return;
    s_worker = true;
#ifdef ESP_PLATFORM
    xTaskCreatePinnedToCore(worker_task, "proxmox", 6144, NULL, 4, NULL, DEVOS_CORE_NET_CRYPTO);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, worker_thread, NULL) == 0) pthread_detach(t);
#endif
}

/* ------------------------------------------------------------------ tickets */
static uint32_t req_submit(int vmid, const char *action, int timeout_ms, bool ui)
{
    if (vmid <= 0 || !action || !action[0]) return 0;
    if (!devos_proxmox_configured()) return 0;
    if (!strcmp(action, "start") || !strcmp(action, "stop") || !strcmp(action, "shutdown") ||
        !strcmp(action, "reboot") || !strcmp(action, "status")) {
        /* known action */
    } else return 0;
    LOCK();
    int slot = -1;
    for (int i = 0; i < DEVOS_PROXMOX_REQS; i++) if (!s_req[i].used) { slot = i; break; }
    if (slot < 0) { UNLOCK(); return 0; }
    prox_req_t *q = &s_req[slot];
    memset(q, 0, sizeof(*q));
    q->used = true;
    q->state = 1;
    q->ui = ui;
    q->id = ++s_req_seq;
    if (!q->id) q->id = ++s_req_seq;
    q->vmid = vmid;
    q->cfg = s_cfg;                          /* snapshot now, not when it runs */
    snprintf(q->action, sizeof(q->action), "%s", action);
    q->timeout_ms = timeout_ms > 0 ? timeout_ms : API_TIMEOUT;
    uint32_t id = q->id;
    if (ui) snprintf(s_st.note, sizeof(s_st.note), "%s vmid %d...", action, vmid);
    UNLOCK();
    ensure_worker();
    wake();
    return id;
}

uint32_t devos_proxmox_request(int vmid, const char *action, int timeout_ms)
{
    return req_submit(vmid, action, timeout_ms, false);
}

bool devos_proxmox_request_poll(uint32_t ticket, devos_proxmox_req_result_t *out)
{
    if (!ticket || !out) return false;
    bool ok = false;
    LOCK();
    for (int i = 0; i < DEVOS_PROXMOX_REQS; i++) {
        prox_req_t *q = &s_req[i];
        if (!q->used || q->id != ticket) continue;
        memset(out, 0, sizeof(*out));
        snprintf(out->action, sizeof(out->action), "%s", q->action);
        if (q->state == 1 || q->state == 2) {
            out->state = DEVOS_PROXMOX_REQ_PENDING;
        } else {
            out->state = q->out_state ? (devos_proxmox_req_state_t)q->out_state
                                      : DEVOS_PROXMOX_REQ_DONE;
            out->status = q->status;
            snprintf(out->error, sizeof(out->error), "%s", q->error);
            snprintf(out->task, sizeof(out->task), "%s", q->task);
            out->inspect = q->inspect;
        }
        ok = true;
        break;
    }
    UNLOCK();
    return ok;
}

bool devos_proxmox_request_started(uint32_t ticket)
{
    if (!ticket) return false;
    bool started = false;
    LOCK();
    for (int i = 0; i < DEVOS_PROXMOX_REQS; i++)
        if (s_req[i].used && s_req[i].id == ticket) { started = s_req[i].started; break; }
    UNLOCK();
    return started;
}

void devos_proxmox_request_cancel(uint32_t ticket)
{
    if (!ticket) return;
    LOCK();
    for (int i = 0; i < DEVOS_PROXMOX_REQS; i++)
        if (s_req[i].used && s_req[i].id == ticket) { s_req[i].cancelled = true; break; }
    UNLOCK();
}

void devos_proxmox_request_release(uint32_t ticket)
{
    if (!ticket) return;
    LOCK();
    for (int i = 0; i < DEVOS_PROXMOX_REQS; i++)
        if (s_req[i].used && s_req[i].id == ticket) { memset(&s_req[i], 0, sizeof(s_req[i])); break; }
    UNLOCK();
}

/* ------------------------------------------------------------------ public */
bool devos_proxmox_ready(void) { return s_inited; }

void devos_proxmox_init(void)
{
    if (s_inited) return;
#ifdef ESP_PLATFORM
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
    if (!s_wake) s_wake = xSemaphoreCreateBinary();
#endif
    s_guest = big_alloc(sizeof(devos_proxmox_guest_t) * DEVOS_PROXMOX_MAX);
    s_node = big_alloc(sizeof(devos_proxmox_node_t) * DEVOS_PROXMOX_MAX_NODES);
    if (!s_guest || !s_node) return;
    load_config();
    s_inited = true;
    ensure_worker();
}

void devos_proxmox_set_active(bool active)
{
    s_active = active;
    if (active) wake();
}

void devos_proxmox_refresh(void) { wake(); }

void devos_proxmox_status(devos_proxmox_status_t *out)
{
    LOCK();
    *out = s_st;
    out->configured = devos_proxmox_configured();
    out->active = s_active;
    UNLOCK();
}

int devos_proxmox_list(devos_proxmox_guest_t *out, int max)
{
    if (!out || max <= 0) return 0;
    LOCK();
    int n = s_nguest < max ? s_nguest : max;
    if (n) memcpy(out, s_guest, sizeof(*out) * (size_t)n);
    UNLOCK();
    return n;
}

int devos_proxmox_nodes(devos_proxmox_node_t *out, int max)
{
    if (!out || max <= 0) return 0;
    LOCK();
    int n = s_nnode < max ? s_nnode : max;
    if (n) memcpy(out, s_node, sizeof(*out) * (size_t)n);
    UNLOCK();
    return n;
}

uint32_t devos_proxmox_generation(void)
{
    LOCK();
    uint32_t g = s_gen;
    UNLOCK();
    return g;
}

int devos_proxmox_action(int vmid, const char *action)
{
    uint32_t t = req_submit(vmid, action, API_TIMEOUT, true);
    return t ? 0 : -1;
}

/* ------------------------------------------------------------------ deep links */
bool devos_proxmox_console_url(const devos_proxmox_guest_t *g, char *out, size_t cap)
{
    if (!g || !out || !cap) return false;
    devos_proxmox_config_t c;
    LOCK();
    c = s_cfg;
    UNLOCK();
    if (!c.url[0]) return false;
    snprintf(out, cap, "%s/?console=%s&novnc=1&vmid=%d&node=%s",
             c.url, g->kind == DEVOS_PROXMOX_LXC ? "lxc" : "kvm", g->vmid, g->node);
    return true;
}

bool devos_proxmox_ssh_target(char *out, size_t cap)
{
    if (!out || !cap) return false;
    devos_proxmox_config_t c;
    LOCK();
    c = s_cfg;
    UNLOCK();
    const char *host = strstr(c.url, "://");
    if (!host) return false;
    host += 3;
    /* Drop any port: SSH goes to 22, not the web UI's 8006. */
    char h[80];
    snprintf(h, sizeof(h), "%s", host);
    char *colon = strchr(h, ':');
    if (colon) *colon = '\0';
    char *slash = strchr(h, '/');
    if (slash) *slash = '\0';
    if (!h[0]) return false;
    /* The token id is "user@realm!token"; SSH wants the user part. */
    char user[40] = "";
    const char *at = strchr(c.token_id, '@');
    if (at) {
        size_t n = (size_t)(at - c.token_id);
        if (n >= sizeof(user)) n = sizeof(user) - 1;
        memcpy(user, c.token_id, n);
        user[n] = '\0';
    }
    snprintf(out, cap, "%s%s%s", user[0] ? user : "root", user[0] ? "@" : "", h);
    return true;
}

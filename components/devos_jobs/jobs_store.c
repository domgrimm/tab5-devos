/* jobs_store: transactional definition storage (see jobs_store.h). */
#include "jobs_store.h"
#include "devos_json.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "devos_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
static SemaphoreHandle_t s_wmx;
static TaskHandle_t s_io_task;
#define W_LOCK()   do { if (s_wmx) xSemaphoreTake(s_wmx, portMAX_DELAY); } while (0)
#define W_UNLOCK() do { if (s_wmx) xSemaphoreGive(s_wmx); } while (0)
#else
#include <pthread.h>
static pthread_mutex_t s_wmx = PTHREAD_MUTEX_INITIALIZER;
#define W_LOCK()   pthread_mutex_lock(&s_wmx)
#define W_UNLOCK() pthread_mutex_unlock(&s_wmx)
#endif

#ifndef TAB5_SD_MOUNT_POINT
#define TAB5_SD_MOUNT_POINT "./sim_sdcard"
#endif

#define JOBS_STORE_ROOT_MAX 128
#define JOBS_STORE_PATH_MAX 400
#define JOBS_HISTORY_CAP    (64 * 1024)

typedef struct {
    char id[JOBS_STORE_ID_MAX];
    uint32_t rev;
    bool enabled;
} entry_t;

static char s_root[JOBS_STORE_ROOT_MAX];
static entry_t s_entries[32];
static int s_entries_n;
static jobs_store_status_t s_st;
static int s_fail_at, s_step;

/* ---------------------------------------------------------------- helpers */
static bool fault(void) { s_step++; return s_fail_at > 0 && s_step == s_fail_at; }

static void path_join(char *out, size_t cap, const char *fmt, ...)
{
    char tail[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tail, sizeof(tail), fmt, ap);
    va_end(ap);
    snprintf(out, cap, "%s/%s", s_root, tail);
}

static void make_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) mkdir(path, 0755);
}

static bool read_file(const char *path, char **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return false; }
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return false; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = '\0';
    *out = b;
    *out_len = got;
    return true;
}

/* Write data to path via a .tmp sibling, then rename over it. */
static bool write_atomic(const char *path, const char *data, size_t len)
{
    char tmp[JOBS_STORE_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(tmp); return false; }
    remove(path);                            /* FAT can't rename over a file */
    if (rename(tmp, path) != 0) { remove(tmp); return false; }
    return true;
}

static devos_err_t degrade(const char *why)
{
    s_st.degraded = true;
    snprintf(s_st.error, sizeof(s_st.error), "%s", why);
    return DEVOS_ERR_FAIL;
}

/* ---------------------------------------------------------------- catalog */
static entry_t *entry_find(const char *id)
{
    for (int i = 0; i < s_entries_n; i++)
        if (strcmp(s_entries[i].id, id) == 0) return &s_entries[i];
    return NULL;
}

static entry_t *entry_add(const char *id)
{
    if (s_entries_n >= (int)(sizeof(s_entries) / sizeof(s_entries[0]))) return NULL;
    entry_t *e = &s_entries[s_entries_n++];
    snprintf(e->id, sizeof(e->id), "%s", id);
    e->rev = 0;
    e->enabled = false;
    return e;
}

/* Parse the catalog; true when a jobs array was found (even if empty). A torn
 * or malformed file returns false so the caller can use .prev. */
static bool catalog_parse(const char *buf, size_t len)
{
    s_entries_n = 0;
    if (!buf || len == 0) return false;
    const char *jobs = devos_json_member(buf, buf + len, "jobs");
    if (!jobs || *jobs != '[') return false;
    const char *end = devos_json_span(jobs, buf + len);
    if (!end) return false;
    /* iterate the array elements (objects) */
    const char *p = jobs + 1;
    while (p < end && s_entries_n < 32) {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) p++;
        if (p >= end || *p == ']') break;
        const char *el = devos_json_span(p, end);
        if (!el || *p != '{') break;
        char id[JOBS_STORE_ID_MAX] = "";
        double rev = 0, en = 0;
        if (devos_json_member_str(p, el, "id", id, sizeof(id)) == 0 && id[0]) {
            devos_json_member_num(p, el, "rev", &rev);
            devos_json_member_num(p, el, "enabled", &en);
            entry_t *e = entry_add(id);
            if (e) { e->rev = (uint32_t)rev; e->enabled = en != 0; }
        }
        p = el;
    }
    return true;
}

static bool catalog_load(void)
{
    char path[JOBS_STORE_PATH_MAX], prev[JOBS_STORE_PATH_MAX];
    path_join(path, sizeof(path), ".devos/jobs/catalog.json");
    path_join(prev, sizeof(prev), ".devos/jobs/catalog.json.prev");
    char *buf = NULL;
    size_t len = 0;
    if (read_file(path, &buf, &len) && catalog_parse(buf, len)) { free(buf); return true; }
    free(buf);
    buf = NULL;
    len = 0;
    if (read_file(prev, &buf, &len) && catalog_parse(buf, len)) { free(buf); return true; }
    free(buf);
    s_entries_n = 0;
    return false;
}

static devos_err_t catalog_save(void)
{
    char path[JOBS_STORE_PATH_MAX], prev[JOBS_STORE_PATH_MAX], tmp[JOBS_STORE_PATH_MAX];
    path_join(path, sizeof(path), ".devos/jobs/catalog.json");
    path_join(prev, sizeof(prev), ".devos/jobs/catalog.json.prev");
    path_join(tmp, sizeof(tmp), ".devos/jobs/catalog.json.tmp");

    char *buf = malloc(4096);
    if (!buf) return DEVOS_ERR_NO_MEM;
    size_t o = 0;
    o += (size_t)snprintf(buf + o, 4096 - o, "{\"schema\":1,\"jobs\":[");
    for (int i = 0; i < s_entries_n && o < 4096 - 96; i++) {
        char esc[JOBS_STORE_ID_MAX * 6 + 8];
        devos_json_escape(s_entries[i].id, esc, sizeof(esc));
        o += (size_t)snprintf(buf + o, 4096 - o, "%s{\"id\":\"%s\",\"rev\":%u,\"enabled\":%d}",
                              i ? "," : "", esc, (unsigned)s_entries[i].rev, s_entries[i].enabled ? 1 : 0);
    }
    o += (size_t)snprintf(buf + o, 4096 - o, "]}\n");

    if (fault()) { free(buf); return degrade("catalog write failed (injected)"); }
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(buf); return degrade("can't write the catalog"); }
    bool ok = fwrite(buf, 1, o, f) == o;
    if (fclose(f) != 0) ok = false;
    free(buf);
    if (!ok) { remove(tmp); return degrade("catalog write incomplete"); }

    if (fault()) return degrade("catalog rotate failed (injected)");
    remove(prev);
    rename(path, prev);                       /* keep the last good catalog */
    remove(path);
    if (fault() || rename(tmp, path) != 0) return degrade("catalog commit failed");
    s_st.degraded = false;
    s_st.error[0] = '\0';
    s_st.jobs = s_entries_n;
    return DEVOS_OK;
}

/* ---------------------------------------------------------------- init */
bool jobs_store_init(const char *root)
{
    memset(&s_st, 0, sizeof(s_st));
    memset(s_entries, 0, sizeof(s_entries));
    s_entries_n = 0;
    snprintf(s_root, sizeof(s_root), "%s", root ? root : TAB5_SD_MOUNT_POINT);
    s_fail_at = s_step = 0;

    make_dir(s_root);
    char p[JOBS_STORE_PATH_MAX];
    path_join(p, sizeof(p), "jobs");                 make_dir(p);
    path_join(p, sizeof(p), "jobs/examples");        make_dir(p);
    path_join(p, sizeof(p), ".devos");               make_dir(p);
    path_join(p, sizeof(p), ".devos/jobs");          make_dir(p);
    path_join(p, sizeof(p), ".devos/jobs/revisions");make_dir(p);
    path_join(p, sizeof(p), ".devos/jobs/history");  make_dir(p);
    path_join(p, sizeof(p), ".devos/jobs/drafts");   make_dir(p);

    path_join(p, sizeof(p), ".devos/jobs");
    struct stat st;
    if (stat(p, &st) != 0) {
        snprintf(s_st.error, sizeof(s_st.error), "no SD card / can't create the Jobs folders");
        return false;
    }
    s_st.available = true;
    catalog_load();
    s_st.jobs = s_entries_n;
    return true;
}

void jobs_store_status(jobs_store_status_t *out) { if (out) *out = s_st; }
bool jobs_store_available(void) { return s_st.available; }

/* ---------------------------------------------------------------- load */
static int collect_revs(const char *id, uint32_t *out, int max)
{
    char dir[JOBS_STORE_PATH_MAX];
    path_join(dir, sizeof(dir), ".devos/jobs/revisions/%s", id);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".job") != 0) continue;
        out[n++] = (uint32_t)strtoul(e->d_name, NULL, 10);
    }
    closedir(d);
    /* sort descending */
    for (int i = 0; i < n; i++)
        for (int k = i + 1; k < n; k++)
            if (out[k] > out[i]) { uint32_t t = out[i]; out[i] = out[k]; out[k] = t; }
    return n;
}

int jobs_store_load(jobs_store_load_cb cb, void *user)
{
    if (!s_st.available || !cb) return 0;
    int accepted = 0;
    for (int i = 0; i < s_entries_n; i++) {
        entry_t *e = &s_entries[i];
        uint32_t revs[32];
        int n = collect_revs(e->id, revs, 32);
        /* try the catalog's revision first, then the rest newest-first */
        uint32_t order[33];
        int on = 0;
        order[on++] = e->rev;
        for (int k = 0; k < n; k++) if (revs[k] != e->rev) order[on++] = revs[k];
        for (int k = 0; k < on; k++) {
            char path[JOBS_STORE_PATH_MAX];
            path_join(path, sizeof(path), ".devos/jobs/revisions/%s/%u.job", e->id, (unsigned)order[k]);
            char *buf = NULL;
            size_t len = 0;
            if (!read_file(path, &buf, &len)) continue;
            bool ok = cb(e->id, buf, len, e->enabled, order[k], user);
            free(buf);
            if (ok) { accepted++; break; }
        }
    }
    return accepted;
}

/* ---------------------------------------------------------------- commit */
devos_err_t jobs_store_commit(const char *id, const char *source, size_t len,
                              uint32_t revision, bool enabled)
{
    if (!s_st.available) return DEVOS_ERR_INVALID_STATE;
    if (!id || !id[0] || !source) return DEVOS_ERR_INVALID_ARG;
    s_step = 0;

    char dir[JOBS_STORE_PATH_MAX], tmp[JOBS_STORE_PATH_MAX], fin[JOBS_STORE_PATH_MAX];
    path_join(dir, sizeof(dir), ".devos/jobs/revisions/%s", id);
    make_dir(dir);
    path_join(tmp, sizeof(tmp), ".devos/jobs/revisions/%s/%u.job.tmp", id, (unsigned)revision);
    path_join(fin, sizeof(fin), ".devos/jobs/revisions/%s/%u.job", id, (unsigned)revision);

    if (fault()) return degrade("revision write failed (injected)");
    if (!write_atomic(fin, source, len)) return degrade("couldn't write the revision");
    (void)tmp;

    entry_t *e = entry_find(id);
    bool had = e != NULL;
    entry_t old;
    if (had) old = *e;
    if (!e) e = entry_add(id);
    if (!e) return DEVOS_ERR_NO_MEM;
    e->rev = revision;
    e->enabled = enabled;
    devos_err_t rc = catalog_save();
    if (rc != DEVOS_OK) {
        /* the in-memory catalog must not claim a revision that isn't durable */
        if (had) *e = old;
        else if (s_entries_n > 0) s_entries_n--;
        return rc;                             /* old catalog intact */
    }

    /* public projection: best effort, never authority */
    char pub[JOBS_STORE_PATH_MAX];
    path_join(pub, sizeof(pub), "jobs/%s.job", id);
    write_atomic(pub, source, len);
    return DEVOS_OK;
}

devos_err_t jobs_store_set_enabled(const char *id, bool enabled)
{
    if (!s_st.available) return DEVOS_ERR_INVALID_STATE;
    entry_t *e = entry_find(id);
    if (!e) return DEVOS_ERR_NOT_FOUND;
    bool old = e->enabled;
    e->enabled = enabled;
    devos_err_t rc = catalog_save();
    if (rc != DEVOS_OK) e->enabled = old;
    return rc;
}

devos_err_t jobs_store_remove(const char *id)
{
    if (!s_st.available) return DEVOS_ERR_INVALID_STATE;
    char path[JOBS_STORE_PATH_MAX];
    uint32_t revs[32];
    int n = collect_revs(id, revs, 32);
    for (int i = 0; i < n; i++) {
        path_join(path, sizeof(path), ".devos/jobs/revisions/%s/%u.job", id, (unsigned)revs[i]);
        remove(path);
    }
    path_join(path, sizeof(path), ".devos/jobs/revisions/%s", id);
    rmdir(path);
    path_join(path, sizeof(path), "jobs/%s.job", id);
    remove(path);
    for (int i = 0; i < s_entries_n; i++) {
        if (strcmp(s_entries[i].id, id) == 0) {
            s_entries[i] = s_entries[s_entries_n - 1];
            s_entries_n--;
            break;
        }
    }
    return catalog_save();
}

devos_err_t jobs_store_history_append(const char *id, const char *line)
{
    if (!s_st.available) return DEVOS_ERR_INVALID_STATE;
    char path[JOBS_STORE_PATH_MAX], prev[JOBS_STORE_PATH_MAX];
    path_join(path, sizeof(path), ".devos/jobs/history/%s.jsonl", id);
    path_join(prev, sizeof(prev), ".devos/jobs/history/%s.prev.jsonl", id);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size >= JOBS_HISTORY_CAP) {
        remove(prev);
        rename(path, prev);
    }
    FILE *f = fopen(path, "a");
    if (!f) return degrade("can't append history");
    fprintf(f, "%s\n", line);
    fclose(f);
    return DEVOS_OK;
}

void jobs_store_fail_after(int step) { s_fail_at = step; s_step = 0; }
void jobs_store_reset_fail(void) { s_fail_at = 0; s_step = 0; }

/* ---------------------------------------------------------------- drafts */
devos_err_t jobs_store_draft_save(const char *id, const char *source, size_t len)
{
    if (!s_st.available) return DEVOS_ERR_INVALID_STATE;
    if (!id || !id[0] || !source) return DEVOS_ERR_INVALID_ARG;
    char path[JOBS_STORE_PATH_MAX];
    path_join(path, sizeof(path), ".devos/jobs/drafts/%s.job", id);
    if (!write_atomic(path, source, len)) return degrade("couldn't save the draft");
    return DEVOS_OK;
}

devos_err_t jobs_store_draft_load(const char *id, char *out, size_t cap, size_t *out_len)
{
    if (!s_st.available) return DEVOS_ERR_INVALID_STATE;
    char path[JOBS_STORE_PATH_MAX];
    path_join(path, sizeof(path), ".devos/jobs/drafts/%s.job", id);
    char *buf = NULL;
    size_t len = 0;
    if (!read_file(path, &buf, &len)) return DEVOS_ERR_NOT_FOUND;
    size_t n = len < cap - 1 ? len : cap - 1;
    memcpy(out, buf, n);
    out[n] = '\0';
    if (out_len) *out_len = n;
    free(buf);
    return DEVOS_OK;
}

/* ---------------------------------------------------------------- worker */
typedef struct { char id[JOBS_STORE_ID_MAX]; char line[128]; } hreq_t;
#define HQUEUE 16
static hreq_t s_hq[HQUEUE];
static int s_hqh, s_hqn;
static volatile bool s_worker_run;
static uint32_t s_hdrop;

void jobs_store_history_append_async(const char *id, const char *line)
{
    if (!s_worker_run) { jobs_store_history_append(id, line); return; }
    W_LOCK();
    if (s_hqn >= HQUEUE) { s_hqh = (s_hqh + 1) % HQUEUE; s_hqn--; s_hdrop++; }
    int idx = (s_hqh + s_hqn) % HQUEUE;
    snprintf(s_hq[idx].id, sizeof(s_hq[idx].id), "%s", id ? id : "");
    snprintf(s_hq[idx].line, sizeof(s_hq[idx].line), "%s", line ? line : "");
    s_hqn++;
    W_UNLOCK();
}

uint32_t jobs_store_history_dropped(void) { return s_hdrop; }

int jobs_store_history_read(const char *id, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!s_st.available || !id) return 0;
    char path[JOBS_STORE_PATH_MAX];
    path_join(path, sizeof(path), ".devos/jobs/history/%s.jsonl", id);
    char *buf = NULL;
    size_t len = 0;
    if (!read_file(path, &buf, &len)) return 0;
    size_t n = len < cap - 1 ? len : cap - 1;
    /* keep the newest bytes when truncating */
    memcpy(out, buf + (len - n), n);
    out[n] = '\0';
    free(buf);
    return (int)n;
}

#ifdef ESP_PLATFORM
static void store_worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_worker_run) { vTaskDelete(NULL); return; }
        hreq_t req;
        bool have = false;
        W_LOCK();
        if (s_hqn > 0) { req = s_hq[s_hqh]; s_hqh = (s_hqh + 1) % HQUEUE; s_hqn--; have = true; }
        W_UNLOCK();
        if (have) jobs_store_history_append(req.id, req.line);
        else vTaskDelay(pdMS_TO_TICKS(100));
    }
}
bool jobs_store_worker_start(void)
{
    if (!s_wmx) s_wmx = xSemaphoreCreateMutex();
    if (s_worker_run) return true;
    s_worker_run = true;
    if (xTaskCreatePinnedToCore(store_worker_task, "jobs_io", 4096, NULL, 2, &s_io_task,
                                DEVOS_CORE_UI_INPUT) != pdPASS) {
        s_worker_run = false;
        return false;
    }
    return true;
}
void jobs_store_worker_stop(void) { s_worker_run = false; }
int jobs_store_stack_free(void)
{
    return s_io_task ? (int)uxTaskGetStackHighWaterMark(s_io_task) : 0;
}
#else
static void *store_worker_thread(void *arg)
{
    (void)arg;
    while (s_worker_run) {
        hreq_t req;
        bool have = false;
        W_LOCK();
        if (s_hqn > 0) { req = s_hq[s_hqh]; s_hqh = (s_hqh + 1) % HQUEUE; s_hqn--; have = true; }
        W_UNLOCK();
        if (have) jobs_store_history_append(req.id, req.line);
        else usleep(20 * 1000);
    }
    return NULL;
}
bool jobs_store_worker_start(void)
{
    if (s_worker_run) return true;
    s_worker_run = true;
    pthread_t t;
    if (pthread_create(&t, NULL, store_worker_thread, NULL) != 0) { s_worker_run = false; return false; }
    pthread_detach(t);
    return true;
}
void jobs_store_worker_stop(void) { s_worker_run = false; }
int jobs_store_stack_free(void) { return 0; }
#endif

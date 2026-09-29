/* App switches: the boot mask behind Settings > Apps (see devos_core.h).
 *
 * Stored in NVS namespace "apps" (simulator: <sd>/.devos/apps.cfg), strings:
 *   off    comma-separated uids switched off - what the next boot uses
 *   good   the last "off" that kept the Home Screen up for 15 s
 *   tries  boots started with an "off" that isn't "good" yet
 *   cost   uid=sram/psram,... bytes each app took at start (last measured)
 * A changed mask gets MAX_TRIES boots to reach devos_core_apps_boot_ok();
 * after that the next boot puts "good" back. */
#include "devos_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "nvs.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_sleep.h"
#else
#include <malloc.h>
#endif

#define MASK_CAP   512
#define COST_CAP   1536
#define MAX_TRIES  2

static char s_off_now[MASK_CAP];        /* this boot */
static char s_off_next[MASK_CAP];       /* after the next restart */
static bool s_loaded;
static devos_apps_boot_t s_boot_kind = DEVOS_APPS_BOOT_NORMAL;

typedef struct {
    char uid[DEVOS_MAX_UID];
    int32_t sram, psram;                /* this boot's figure (or the stored one) */
    int32_t saved_sram, saved_psram;
    bool saved, now;
} cost_t;
static cost_t s_cost[DEVOS_MAX_APPS];
static int s_cost_n;

/* ------------------------------------------------------------------ storage */
#ifdef ESP_PLATFORM
#define NS "apps"

static bool kv_get(const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = cap;
    esp_err_t e = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (e != ESP_OK) out[0] = '\0';
    return e == ESP_OK;
}

static void kv_set(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, key, val) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}
#else
#define APPS_FILE TAB5_SD_MOUNT_POINT "/.devos/apps.cfg"

/* key=value lines */
static bool kv_get(const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    FILE *f = fopen(APPS_FILE, "r");
    if (!f) return false;
    char line[COST_CAP + 32];
    size_t kl = strlen(key);
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, kl) != 0 || line[kl] != '=') continue;
        char *v = line + kl + 1;
        v[strcspn(v, "\r\n")] = '\0';
        snprintf(out, cap, "%s", v);
        found = true;
    }
    fclose(f);
    return found;
}

static void kv_set(const char *key, const char *val)
{
    static const char *const keys[] = { "off", "good", "tries", "cost" };
    static char vals[4][COST_CAP];
    for (int i = 0; i < 4; i++) {
        if (strcmp(keys[i], key) == 0) snprintf(vals[i], sizeof(vals[i]), "%s", val);
        else kv_get(keys[i], vals[i], sizeof(vals[i]));
    }
    FILE *f = fopen(APPS_FILE, "w");
    if (!f) return;
    for (int i = 0; i < 4; i++) fprintf(f, "%s=%s\n", keys[i], vals[i]);
    fclose(f);
}
#endif

/* ------------------------------------------------------------------ masks */
static bool mask_has(const char *mask, const char *uid)
{
    if (!uid || !*uid) return false;
    size_t n = strlen(uid);
    for (const char *p = mask; *p;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && strncmp(p, uid, n) == 0) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

/* Same set of uids (order doesn't matter). */
static bool mask_subset(const char *a, const char *b)
{
    char tok[DEVOS_MAX_UID];
    for (const char *p = a; *p;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len && len < sizeof(tok)) {
            memcpy(tok, p, len);
            tok[len] = '\0';
            if (!mask_has(b, tok)) return false;
        }
        if (!e) break;
        p = e + 1;
    }
    return true;
}

static bool mask_equal(const char *a, const char *b)
{
    return mask_subset(a, b) && mask_subset(b, a);
}

static void mask_set(char *mask, size_t cap, const char *uid, bool off)
{
    char out[MASK_CAP] = "";
    size_t n = strlen(uid), o = 0;
    for (const char *p = mask; *p;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len && !(len == n && strncmp(p, uid, n) == 0) && o + len + 2 < sizeof(out)) {
            if (o) out[o++] = ',';
            memcpy(out + o, p, len);
            o += len;
            out[o] = '\0';
        }
        if (!e) break;
        p = e + 1;
    }
    if (off && o + n + 2 < sizeof(out)) {
        if (o) out[o++] = ',';
        memcpy(out + o, uid, n + 1);
    }
    snprintf(mask, cap, "%s", out);
}

/* ------------------------------------------------------------------ costs */
static cost_t *cost_find(const char *uid, bool add)
{
    for (int i = 0; i < s_cost_n; i++) {
        if (strcmp(s_cost[i].uid, uid) == 0) return &s_cost[i];
    }
    if (!add || s_cost_n >= DEVOS_MAX_APPS) return NULL;
    cost_t *c = &s_cost[s_cost_n++];
    memset(c, 0, sizeof(*c));
    snprintf(c->uid, sizeof(c->uid), "%s", uid);
    return c;
}

static void costs_load(void)
{
    char buf[COST_CAP];
    kv_get("cost", buf, sizeof(buf));
    for (char *p = buf; *p;) {
        char *e = strchr(p, ',');
        if (e) *e = '\0';
        char *eq = strchr(p, '=');
        long sram = 0, psram = 0;
        if (eq && sscanf(eq + 1, "%ld/%ld", &sram, &psram) == 2) {
            *eq = '\0';
            cost_t *c = *p ? cost_find(p, true) : NULL;
            if (c) {
                c->sram = c->saved_sram = (int32_t)sram;
                c->psram = c->saved_psram = (int32_t)psram;
                c->saved = true;
            }
        }
        if (!e) break;
        p = e + 1;
    }
}

static void costs_save_if_changed(void)
{
    bool changed = false;
    for (int i = 0; i < s_cost_n; i++) {
        const cost_t *c = &s_cost[i];
        if (!c->now) continue;
        if (!c->saved || labs((long)(c->sram - c->saved_sram)) > 2048 ||
            labs((long)(c->psram - c->saved_psram)) > 2048) changed = true;
    }
    if (!changed) return;                     /* spare the flash */
    char buf[COST_CAP];
    size_t o = 0;
    buf[0] = '\0';
    for (int i = 0; i < s_cost_n; i++) {
        cost_t *c = &s_cost[i];
        int n = snprintf(buf + o, sizeof(buf) - o, "%s%s=%ld/%ld", o ? "," : "", c->uid,
                         (long)c->sram, (long)c->psram);
        if (n < 0 || (size_t)n >= sizeof(buf) - o) break;
        o += (size_t)n;
        c->saved_sram = c->sram;
        c->saved_psram = c->psram;
        c->saved = true;
    }
    kv_set("cost", buf);
}

void devos_core_mem_mark(devos_mem_mark_t *m)
{
#ifdef ESP_PLATFORM
    m->sram = (int64_t)heap_caps_get_total_size(MALLOC_CAP_INTERNAL) -
              (int64_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    m->psram = (int64_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM) -
               (int64_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    lv_mem_monitor_t mon;                      /* LVGL objects live in its own PSRAM pool */
    lv_mem_monitor(&mon);
    m->psram += (int64_t)mon.total_size - (int64_t)mon.free_size;
#else
    struct mallinfo2 mi = mallinfo2();
    m->sram = 0;
    m->psram = (int64_t)(mi.uordblks + mi.hblkhd);
#endif
}

void devos_core_app_add_cost(const char *uid, const devos_mem_mark_t *since)
{
    if (!uid || !since) return;
    devos_mem_mark_t now;
    devos_core_mem_mark(&now);
    cost_t *c = cost_find(uid, true);
    if (!c) return;
    if (!c->now) {
        c->sram = c->psram = 0;
        c->now = true;
    }
    int64_t ds = now.sram - since->sram, dp = now.psram - since->psram;
    c->sram += (int32_t)(ds > 0 ? ds : 0);
    c->psram += (int32_t)(dp > 0 ? dp : 0);
}

bool devos_core_app_cost(const char *uid, int32_t *sram, int32_t *psram, bool *this_boot)
{
    const cost_t *c = uid ? cost_find(uid, false) : NULL;
    if (!c || (!c->now && !c->saved)) return false;
    if (sram) *sram = c->sram;
    if (psram) *psram = c->psram;
    if (this_boot) *this_boot = c->now;
    return true;
}

/* ------------------------------------------------------------------ mask API */
void devos_core_apps_load(bool safe_start)
{
    if (s_loaded) return;
    s_loaded = true;
    char good[MASK_CAP], tries_s[12];
    kv_get("off", s_off_now, sizeof(s_off_now));
    kv_get("good", good, sizeof(good));        /* none yet: all on */
    kv_get("tries", tries_s, sizeof(tries_s));

    if (safe_start) {
        printf("[devOS] Safe start: every app switched back on\n");
        s_off_now[0] = '\0';
        kv_set("off", "");
        kv_set("good", "");
        kv_set("tries", "0");
        s_boot_kind = DEVOS_APPS_BOOT_SAFE;
    } else if (!mask_equal(s_off_now, good)) {
        int tries = atoi(tries_s) + 1;
        if (tries > MAX_TRIES) {
            printf("[devOS] Apps: the new switches (off: %s) never reached the Home Screen; "
                   "putting back the last ones that did (off: %s)\n",
                   s_off_now[0] ? s_off_now : "none", good[0] ? good : "none");
            snprintf(s_off_now, sizeof(s_off_now), "%s", good);
            kv_set("off", good);
            kv_set("tries", "0");
            s_boot_kind = DEVOS_APPS_BOOT_REVERTED;
        } else {
            snprintf(tries_s, sizeof(tries_s), "%d", tries);
            kv_set("tries", tries_s);
        }
    }
    snprintf(s_off_next, sizeof(s_off_next), "%s", s_off_now);
    if (s_off_now[0]) printf("[devOS] Apps switched off: %s\n", s_off_now);
    costs_load();
}

devos_apps_boot_t devos_core_apps_boot_kind(void) { return s_boot_kind; }

bool devos_core_app_required(const char *uid)
{
    return !uid || !*uid || strcmp(uid, "launcher") == 0 || strcmp(uid, "settings") == 0;
}

bool devos_core_app_enabled(const char *uid)
{
    return devos_core_app_required(uid) || !mask_has(s_off_now, uid);
}

bool devos_core_app_enabled_next(const char *uid)
{
    return devos_core_app_required(uid) || !mask_has(s_off_next, uid);
}

void devos_core_set_app_enabled_next(const char *uid, bool on)
{
    if (devos_core_app_required(uid) || devos_core_app_enabled_next(uid) == on) return;
    mask_set(s_off_next, sizeof(s_off_next), uid, !on);
    kv_set("off", s_off_next);
    kv_set("tries", "0");                      /* a fresh mask gets its own tries */
}

bool devos_core_apps_restart_pending(void)
{
    return !mask_equal(s_off_now, s_off_next);
}

void devos_core_apps_boot_ok(void)
{
    char good[MASK_CAP];
    kv_get("good", good, sizeof(good));
    if (!mask_equal(good, s_off_now)) {
        kv_set("good", s_off_now);
        printf("[devOS] Apps: these switches boot fine; kept as the fallback\n");
    }
    /* the running mask proved itself; a pending one keeps its own count */
    char tries_s[12];
    kv_get("tries", tries_s, sizeof(tries_s));
    if (mask_equal(s_off_now, s_off_next) && tries_s[0] && strcmp(tries_s, "0") != 0) kv_set("tries", "0");
    costs_save_if_changed();
}

static devos_restart_fn s_restart_cb;

void devos_core_set_restart_cb(devos_restart_fn cb) { s_restart_cb = cb; }

void devos_core_restart(void)
{
    /* Leave the current app first: apps finish up in hide() (the editor
     * saves its file, recordings stop). */
    devos_app_descriptor_t *cur = devos_core_get_app(devos_core_get_current_app());
    if (cur && cur->hide) cur->hide();
    printf("[devOS] Restarting\n");
    fflush(stdout);
    if (s_restart_cb) s_restart_cb();
#ifdef ESP_PLATFORM
    esp_restart();
#endif
}

void devos_core_shutdown(void)
{
    /* Leave the current app first. */
    devos_app_descriptor_t *cur = devos_core_get_app(devos_core_get_current_app());
    if (cur && cur->hide) cur->hide();
    printf("[devOS] Shutting down\n");
    fflush(stdout);
#ifdef ESP_PLATFORM
#include "esp_sleep.h"
    esp_deep_sleep_start();
#else
    exit(0);
#endif
}

static devos_restart_check_fn s_restart_checks[8];
static int s_restart_check_count;

void devos_core_add_restart_check(devos_restart_check_fn fn)
{
    if (fn && s_restart_check_count < (int)(sizeof(s_restart_checks) / sizeof(s_restart_checks[0]))) {
        s_restart_checks[s_restart_check_count++] = fn;
    }
}

const char *devos_core_restart_check(void)
{
    for (int i = 0; i < s_restart_check_count; i++) {
        const char *why = s_restart_checks[i]();
        if (why && *why) return why;
    }
    return NULL;
}

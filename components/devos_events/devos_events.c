/* devos_events: bounded typed event delivery (see devos_events.h). Platform
 * synchronization only - no UI, Jobs or app dependency. */
#include "devos_events.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t s_mx;
#define EV_LOCK()   do { if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY); } while (0)
#define EV_UNLOCK() do { if (s_mx) xSemaphoreGive(s_mx); } while (0)
static void *ev_alloc(size_t n) { void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM); return p ? p : malloc(n); }
#else
#include <pthread.h>
static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
#define EV_LOCK()   pthread_mutex_lock(&s_mx)
#define EV_UNLOCK() pthread_mutex_unlock(&s_mx)
static void *ev_alloc(size_t n) { return malloc(n); }
#endif

typedef struct {
    devos_event_schema_t schema;
    char topic[DEVOS_EVENTS_TOPIC_MAX];
    char fields[192];
    char desc[96];
} topic_t;

typedef struct {
    devos_event_t ev;           /* payload pointer owned here */
    uint8_t *payload;
} queued_t;

typedef struct {
    bool used;
    char pattern[DEVOS_EVENTS_TOPIC_MAX];
    devos_event_cb_t cb;
    void *user;
} sub_t;

static topic_t s_topics[DEVOS_EVENTS_MAX_TOPICS];
static int s_topic_n;
static queued_t s_q[DEVOS_EVENTS_QUEUE];
static int s_q_head, s_q_n;
static sub_t s_subs[DEVOS_EVENTS_SUBS_MAX];
static uint32_t s_seq;
static devos_events_stats_t s_stats;
static bool s_init;

devos_err_t devos_events_init(void)
{
#ifdef ESP_PLATFORM
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
    if (!s_mx) return DEVOS_ERR_NO_MEM;
#endif
    EV_LOCK();
    s_topic_n = 0;
    s_q_head = s_q_n = 0;
    memset(s_subs, 0, sizeof(s_subs));
    s_seq = 0;
    memset(&s_stats, 0, sizeof(s_stats));
    s_init = true;
    EV_UNLOCK();
    return DEVOS_OK;
}

devos_err_t devos_events_register_topic(const devos_event_schema_t *schema)
{
    if (!schema || !schema->topic || !schema->topic[0]) return DEVOS_ERR_INVALID_ARG;
    if (strlen(schema->topic) >= DEVOS_EVENTS_TOPIC_MAX) return DEVOS_ERR_INVALID_SIZE;
    EV_LOCK();
    for (int i = 0; i < s_topic_n; i++) {
        if (strcmp(s_topics[i].topic, schema->topic) == 0) {
            EV_UNLOCK();
            return DEVOS_OK;                 /* idempotent */
        }
    }
    if (s_topic_n >= DEVOS_EVENTS_MAX_TOPICS) { EV_UNLOCK(); return DEVOS_ERR_NO_MEM; }
    topic_t *t = &s_topics[s_topic_n++];
    snprintf(t->topic, sizeof(t->topic), "%s", schema->topic);
    snprintf(t->fields, sizeof(t->fields), "%s", schema->fields ? schema->fields : "");
    snprintf(t->desc, sizeof(t->desc), "%s", schema->description ? schema->description : "");
    t->schema.topic = t->topic;
    t->schema.fields = t->fields;
    t->schema.description = t->desc;
    EV_UNLOCK();
    return DEVOS_OK;
}

const devos_event_schema_t *devos_events_topic_schema(const char *topic)
{
    if (!topic) return NULL;
    const devos_event_schema_t *r = NULL;
    EV_LOCK();
    for (int i = 0; i < s_topic_n; i++) {
        if (strcmp(s_topics[i].topic, topic) == 0) { r = &s_topics[i].schema; break; }
    }
    EV_UNLOCK();
    return r;
}

int devos_events_topic_count(void)
{
    EV_LOCK();
    int n = s_topic_n;
    EV_UNLOCK();
    return n;
}

const devos_event_schema_t *devos_events_topic_at(int index)
{
    const devos_event_schema_t *r = NULL;
    EV_LOCK();
    if (index >= 0 && index < s_topic_n) r = &s_topics[index].schema;
    EV_UNLOCK();
    return r;
}

/* MQTT-style topic match: exact, '+' (one non-empty level), terminal '#'.
 * A leading wildcard does not match '$'-prefixed system topics. */
static bool match_levels(const char *p, const char *t)
{
    if (!*p) return !*t;
    const char *ps = p;
    while (*p && *p != '/') p++;
    size_t pl = (size_t)(p - ps);
    const char *ts = t;
    while (*t && *t != '/') t++;
    size_t tl = (size_t)(t - ts);
    bool pat_more = (*p == '/');
    bool top_more = (*t == '/');
    if (pl == 1 && ps[0] == '#') return true;
    bool ok = (pl == 1 && ps[0] == '+') ? (tl > 0) : (pl == tl && strncmp(ps, ts, pl) == 0);
    if (!ok) return false;
    if (pat_more) p++;
    if (top_more) t++;
    if (!pat_more && !top_more) return !*p && !*t;
    if (!pat_more && top_more) return false;
    if (pat_more && !top_more) return (p[0] == '#' && p[1] == '\0');
    return match_levels(p, t);
}

static bool topic_match(const char *pat, const char *topic)
{
    if (!pat || !topic) return false;
    if (topic[0] == '$' && (pat[0] == '#' || pat[0] == '+')) return false;
    return match_levels(pat, topic);
}

bool devos_events_topic_match(const char *pattern, const char *topic)
{
    return topic_match(pattern, topic);
}

int devos_events_subscribe(const char *pattern, devos_event_cb_t cb, void *user)
{
    if (!pattern || !pattern[0] || !cb) return -1;
    if (strlen(pattern) >= DEVOS_EVENTS_TOPIC_MAX) return -1;
    int id = -1;
    EV_LOCK();
    for (int i = 0; i < DEVOS_EVENTS_SUBS_MAX; i++) {
        if (!s_subs[i].used) {
            s_subs[i].used = true;
            snprintf(s_subs[i].pattern, sizeof(s_subs[i].pattern), "%s", pattern);
            s_subs[i].cb = cb;
            s_subs[i].user = user;
            id = i + 1;
            break;
        }
    }
    EV_UNLOCK();
    return id;
}

void devos_events_unsubscribe(int sub_id)
{
    EV_LOCK();
    if (sub_id >= 1 && sub_id <= DEVOS_EVENTS_SUBS_MAX) s_subs[sub_id - 1].used = false;
    EV_UNLOCK();
}

devos_err_t devos_events_publish(const devos_event_t *ev, const void *payload, uint32_t len)
{
    if (!ev || !ev->topic[0]) return DEVOS_ERR_INVALID_ARG;
    EV_LOCK();
    if (s_q_n >= DEVOS_EVENTS_QUEUE) {
        s_stats.published++;
        s_stats.dropped++;
        EV_UNLOCK();
        return DEVOS_ERR_NO_MEM;
    }
    int idx = (s_q_head + s_q_n) % DEVOS_EVENTS_QUEUE;
    queued_t *q = &s_q[idx];
    q->ev = *ev;
    q->ev.seq = ++s_seq;
    q->payload = NULL;
    q->ev.payload = NULL;
    q->ev.payload_len = 0;
    q->ev.truncated = false;
    if (payload && len) {
        uint32_t cap = len > DEVOS_EVENTS_PAYLOAD_MAX ? DEVOS_EVENTS_PAYLOAD_MAX : len;
        uint8_t *copy = ev_alloc(cap ? cap : 1);
        if (copy) {
            memcpy(copy, payload, cap);
            q->payload = copy;
            q->ev.payload = copy;
            q->ev.payload_len = cap;
            q->ev.truncated = len > DEVOS_EVENTS_PAYLOAD_MAX;
        } else {
            q->ev.truncated = true;
        }
    }
    s_q_n++;
    s_stats.published++;
    s_stats.queued = s_q_n;
    EV_UNLOCK();
    return DEVOS_OK;
}

/* Deliver up to max queued events to the subscribers present at the start of
 * the call. Callbacks run outside the lock. */
int devos_events_drain(int max)
{
    if (max <= 0) max = DEVOS_EVENTS_QUEUE;
    queued_t batch[DEVOS_EVENTS_QUEUE];
    sub_t subs[DEVOS_EVENTS_SUBS_MAX];
    int n = 0;
    EV_LOCK();
    int take = s_q_n < max ? s_q_n : max;
    for (int i = 0; i < take; i++) {
        int idx = (s_q_head + i) % DEVOS_EVENTS_QUEUE;
        batch[i] = s_q[idx];
        s_q[idx].payload = NULL;
    }
    s_q_head = (s_q_head + take) % DEVOS_EVENTS_QUEUE;
    s_q_n -= take;
    s_stats.queued = s_q_n;
    memcpy(subs, s_subs, sizeof(subs));
    n = take;
    EV_UNLOCK();

    for (int i = 0; i < n; i++) {
        for (int s = 0; s < DEVOS_EVENTS_SUBS_MAX; s++) {
            if (!subs[s].used) continue;
            if (!topic_match(subs[s].pattern, batch[i].ev.topic)) continue;
            subs[s].cb(&batch[i].ev, subs[s].user);
            EV_LOCK();
            s_stats.delivered++;
            EV_UNLOCK();
        }
        free(batch[i].payload);
    }
    return n;
}

void devos_events_stats(devos_events_stats_t *out)
{
    if (!out) return;
    EV_LOCK();
    *out = s_stats;
    out->queued = s_q_n;
    EV_UNLOCK();
}

/* devos_mqtt: see devos_mqtt.h. */
#include "devos_mqtt.h"
#include "devos_config.h"
#include "devos_json.h"
#include "devos_net.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
static SemaphoreHandle_t s_mx;
/* Safe before *_init(): other tasks (sysmon, launcher tiles) may ask for
 * status first, and a NULL semaphore asserts. */
#define LOCK()   do { if (s_mx) xSemaphoreTake(s_mx, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_mx) xSemaphoreGive(s_mx); } while (0)
static void *big_alloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
    return p ? p : calloc(1, n);
}
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void sleep_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static uint32_t rand32(void) { return esp_random(); }
#else
#include <pthread.h>
#include <unistd.h>
static pthread_mutex_t s_mx_sim = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&s_mx_sim)
#define UNLOCK() pthread_mutex_unlock(&s_mx_sim)
static void *big_alloc(size_t n) { return calloc(1, n); }
static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
static uint32_t rand32(void) { return (uint32_t)rand() ^ ((uint32_t)rand() << 16); }
#endif

#define MQTT_CFG_FILE TAB5_SD_MOUNT_POINT "/.devos/mqtt.json"
#define RX_CAP        (DEVOS_MQTT_TOPIC_MAX + DEVOS_MQTT_PAYLOAD_MAX + 64)
#define PUB_QUEUE     8

/* ---- state (guarded by LOCK) ---- */
typedef struct {
    devos_mqtt_msg_t meta;
    char payload[DEVOS_MQTT_PAYLOAD_MAX + 1];
} slot_t;

typedef struct {
    char topic[DEVOS_MQTT_TOPIC_MAX];
    char *payload;
    size_t len;
    uint8_t qos;
    bool retain;
} pub_t;

static devos_mqtt_config_t s_cfg;
static slot_t *s_ring;
static devos_mqtt_topic_t *s_topics;
static int s_topic_n;
static uint32_t s_seq_first = 1, s_seq_next = 1;
static uint32_t s_gen;
static uint32_t s_total_msgs, s_total_bytes;
static uint32_t s_bucket[5], s_bucket_sec;     /* messages per second, last 5 s */
static pub_t s_pubq[PUB_QUEUE];
static int s_pub_n;

static volatile devos_mqtt_state_t s_state;
static volatile bool s_want, s_restart;
static bool s_worker_started;
static char s_status[160] = "Not connected";

/* ---- config ---- */
static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_status(const char *fmt, ...)
{
    char buf[sizeof(s_status)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LOCK();
    memcpy(s_status, buf, sizeof(s_status));
    UNLOCK();
}

static void cfg_defaults(devos_mqtt_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->port = 1883;
    c->keepalive_s = 30;
    snprintf(c->subs[0], sizeof(c->subs[0]), "#");
}

static void cfg_load(void)
{
    cfg_defaults(&s_cfg);
    FILE *f = fopen(MQTT_CFG_FILE, "rb");
    if (f) {
        char buf[2048];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        int v;
        devos_json_get_str(buf, n, "host", s_cfg.host, sizeof(s_cfg.host));
        if (devos_json_get_int(buf, n, "port", &v) == 0 && v > 0 && v < 65536) s_cfg.port = v;
        devos_json_get_str(buf, n, "client_id", s_cfg.client_id, sizeof(s_cfg.client_id));
        devos_json_get_str(buf, n, "username", s_cfg.username, sizeof(s_cfg.username));
        if (devos_json_get_int(buf, n, "keepalive", &v) == 0 && v >= 5 && v <= 600) s_cfg.keepalive_s = v;
        char key[8];
        bool any = false;
        for (int i = 0; i < DEVOS_MQTT_MAX_SUBS; i++) {
            snprintf(key, sizeof(key), "sub%d", i + 1);
            s_cfg.subs[i][0] = '\0';
            if (devos_json_get_str(buf, n, key, s_cfg.subs[i], sizeof(s_cfg.subs[i])) == 0 && s_cfg.subs[i][0]) any = true;
        }
        if (!any) snprintf(s_cfg.subs[0], sizeof(s_cfg.subs[0]), "#");
#ifndef ESP_PLATFORM
        devos_json_get_str(buf, n, "password", s_cfg.password, sizeof(s_cfg.password));
#endif
    }
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("mqtt", NVS_READONLY, &h) == ESP_OK) {
        size_t l = sizeof(s_cfg.password);
        if (nvs_get_str(h, "password", s_cfg.password, &l) != ESP_OK) s_cfg.password[0] = '\0';
        nvs_close(h);
    }
#endif
}

static void json_field(FILE *f, const char *key, const char *val, bool comma)
{
    char esc[400];
    devos_json_escape(val, esc, sizeof(esc));
    fprintf(f, "  \"%s\": \"%s\"%s\n", key, esc, comma ? "," : "");
}

static void cfg_save(const devos_mqtt_config_t *c)
{
    FILE *f = fopen(MQTT_CFG_FILE, "wb");
    if (f) {
        fprintf(f, "{\n");
        json_field(f, "host", c->host, true);
        fprintf(f, "  \"port\": %d,\n  \"keepalive\": %d,\n", c->port, c->keepalive_s);
        json_field(f, "client_id", c->client_id, true);
        json_field(f, "username", c->username, true);
#ifndef ESP_PLATFORM
        json_field(f, "password", c->password, true);
#endif
        for (int i = 0; i < DEVOS_MQTT_MAX_SUBS; i++) {
            char key[8];
            snprintf(key, sizeof(key), "sub%d", i + 1);
            json_field(f, key, c->subs[i], i + 1 < DEVOS_MQTT_MAX_SUBS);
        }
        fprintf(f, "}\n");
        fclose(f);
    }
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("mqtt", NVS_READWRITE, &h) == ESP_OK) {
        char old[sizeof(c->password)];
        size_t l = sizeof(old);
        if (nvs_get_str(h, "password", old, &l) != ESP_OK || strcmp(old, c->password) != 0) {
            if (nvs_set_str(h, "password", c->password) == ESP_OK) nvs_commit(h);
        }
        nvs_close(h);
    }
#endif
}

void devos_mqtt_get_config(devos_mqtt_config_t *out)
{
    LOCK();
    *out = s_cfg;
    UNLOCK();
}

void devos_mqtt_set_config(const devos_mqtt_config_t *cfg)
{
    devos_mqtt_config_t c = *cfg;
    if (c.port <= 0 || c.port > 65535) c.port = 1883;
    if (c.keepalive_s < 5 || c.keepalive_s > 600) c.keepalive_s = 30;
    bool any = false;
    for (int i = 0; i < DEVOS_MQTT_MAX_SUBS; i++) if (c.subs[i][0]) any = true;
    if (!any) snprintf(c.subs[0], sizeof(c.subs[0]), "#");
    LOCK();
    s_cfg = c;
    UNLOCK();
    cfg_save(&c);
    s_restart = true;
}

bool devos_mqtt_configured(void)
{
    LOCK();
    bool ok = s_cfg.host[0] != '\0';
    UNLOCK();
    return ok;
}

/* ---- message store ---- */
static void rate_tick(uint32_t sec)
{
    if (sec != s_bucket_sec) {
        uint32_t gap = sec - s_bucket_sec;
        for (uint32_t i = 1; i <= gap && i <= 5; i++) s_bucket[(s_bucket_sec + i) % 5] = 0;
        s_bucket_sec = sec;
    }
}

static void topic_note(const char *topic, uint32_t seq, uint32_t t)
{
    int lo = 0, hi = s_topic_n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(s_topics[mid].topic, topic);
        if (c == 0) {
            s_topics[mid].count++;
            s_topics[mid].last_seq = seq;
            s_topics[mid].last_time_s = t;
            return;
        }
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    if (s_topic_n >= DEVOS_MQTT_MAX_TOPICS) return;
    memmove(&s_topics[lo + 1], &s_topics[lo], (size_t)(s_topic_n - lo) * sizeof(s_topics[0]));
    devos_mqtt_topic_t *e = &s_topics[lo];
    memset(e, 0, sizeof(*e));
    snprintf(e->topic, sizeof(e->topic), "%s", topic);
    e->count = 1;
    e->last_seq = seq;
    e->last_time_s = t;
    s_topic_n++;
}

static void store_message(const char *topic, size_t tlen, const uint8_t *payload, size_t stored,
                          uint32_t full_len, uint8_t qos, bool retain)
{
    LOCK();
    if (!s_ring) { UNLOCK(); return; }
    uint32_t seq = s_seq_next++;
    if (s_seq_next - s_seq_first > DEVOS_MQTT_RING) s_seq_first = s_seq_next - DEVOS_MQTT_RING;
    slot_t *sl = &s_ring[seq % DEVOS_MQTT_RING];
    if (tlen > DEVOS_MQTT_TOPIC_MAX - 1) tlen = DEVOS_MQTT_TOPIC_MAX - 1;
    if (stored > DEVOS_MQTT_PAYLOAD_MAX) stored = DEVOS_MQTT_PAYLOAD_MAX;
    memcpy(sl->meta.topic, topic, tlen);
    sl->meta.topic[tlen] = '\0';
    memcpy(sl->payload, payload, stored);
    sl->payload[stored] = '\0';
    sl->meta.seq = seq;
    sl->meta.time_s = (uint32_t)time(NULL);
    sl->meta.len = full_len;
    sl->meta.stored = (uint16_t)stored;
    sl->meta.qos = qos;
    sl->meta.retain = retain;
    topic_note(sl->meta.topic, seq, sl->meta.time_s);
    s_total_msgs++;
    s_total_bytes += full_len;
    uint32_t sec = now_ms() / 1000;
    rate_tick(sec);
    s_bucket[sec % 5]++;
    s_gen++;
    UNLOCK();
}

uint32_t devos_mqtt_generation(void) { return s_gen; }

uint32_t devos_mqtt_seq_first(void)
{
    LOCK();
    uint32_t v = s_seq_first;
    UNLOCK();
    return v;
}

uint32_t devos_mqtt_seq_next(void)
{
    LOCK();
    uint32_t v = s_seq_next;
    UNLOCK();
    return v;
}

bool devos_mqtt_get(uint32_t seq, devos_mqtt_msg_t *meta, char *payload, size_t cap)
{
    bool ok = false;
    LOCK();
    if (s_ring && seq >= s_seq_first && seq < s_seq_next) {
        slot_t *sl = &s_ring[seq % DEVOS_MQTT_RING];
        if (meta) *meta = sl->meta;
        if (payload && cap) {
            size_t n = sl->meta.stored < cap - 1 ? sl->meta.stored : cap - 1;
            memcpy(payload, sl->payload, n);
            payload[n] = '\0';
        }
        ok = true;
    }
    UNLOCK();
    return ok;
}

void devos_mqtt_clear(void)
{
    LOCK();
    s_seq_first = s_seq_next;
    s_topic_n = 0;
    s_total_msgs = s_total_bytes = 0;
    memset(s_bucket, 0, sizeof(s_bucket));
    s_gen++;
    UNLOCK();
}

int devos_mqtt_topic_count(void)
{
    LOCK();
    int n = s_topic_n;
    UNLOCK();
    return n;
}

bool devos_mqtt_topic(int idx, devos_mqtt_topic_t *out)
{
    bool ok = false;
    LOCK();
    if (s_topics && idx >= 0 && idx < s_topic_n) {
        *out = s_topics[idx];
        ok = true;
    }
    UNLOCK();
    return ok;
}

void devos_mqtt_stats(uint32_t *messages, uint32_t *bytes, float *per_s)
{
    LOCK();
    rate_tick(now_ms() / 1000);
    uint32_t sum = 0;
    for (int i = 0; i < 5; i++) sum += s_bucket[i];
    if (messages) *messages = s_total_msgs;
    if (bytes) *bytes = s_total_bytes;
    if (per_s) *per_s = sum / 5.0f;
    UNLOCK();
}

/* ---- publish queue ---- */
int devos_mqtt_publish(const char *topic, const char *payload, size_t len, int qos, bool retain)
{
    if (!topic || !topic[0] || strchr(topic, '#') || strchr(topic, '+')) return -1;
    if (s_state != DEVOS_MQTT_UP) return -1;
    char *copy = malloc(len + 1);
    if (!copy) return -1;
    if (len) memcpy(copy, payload, len);
    copy[len] = '\0';
    int rc = -1;
    LOCK();
    if (s_pub_n < PUB_QUEUE) {
        pub_t *p = &s_pubq[s_pub_n++];
        snprintf(p->topic, sizeof(p->topic), "%s", topic);
        p->payload = copy;
        p->len = len;
        p->qos = (uint8_t)(qos < 0 ? 0 : qos > 1 ? 1 : qos);   /* QoS 2 is sent as 1 */
        p->retain = retain;
        copy = NULL;
        rc = 0;
    }
    UNLOCK();
    free(copy);
    return rc;
}

/* ---- wire format ---- */
static size_t put_u16(uint8_t *b, unsigned v)
{
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
    return 2;
}

static size_t put_str(uint8_t *b, const char *s, size_t n)
{
    put_u16(b, (unsigned)n);
    memcpy(b + 2, s, n);
    return n + 2;
}

static size_t put_remlen(uint8_t *b, size_t n)
{
    size_t i = 0;
    do {
        uint8_t d = n % 128;
        n /= 128;
        if (n) d |= 0x80;
        b[i++] = d;
    } while (n && i < 4);
    return i;
}

/* Send a packet: header byte + remaining length + body. */
static int send_packet(int sock, uint8_t hdr, const uint8_t *body, size_t n)
{
    uint8_t h[5];
    h[0] = hdr;
    size_t hl = 1 + put_remlen(h + 1, n);
    if (devos_net_socket_send_all(sock, h, hl) < 0) return -1;
    if (n && devos_net_socket_send_all(sock, body, n) < 0) return -1;
    return 0;
}

static int send_connect(int sock, const devos_mqtt_config_t *c, const char *client_id)
{
    uint8_t b[512];
    size_t n = 0;
    n += put_str(b + n, "MQTT", 4);
    b[n++] = 4;                                 /* protocol level 3.1.1 */
    uint8_t flags = 0x02;                       /* clean session */
    if (c->username[0]) flags |= 0x80;
    if (c->username[0] && c->password[0]) flags |= 0x40;
    b[n++] = flags;
    n += put_u16(b + n, (unsigned)c->keepalive_s);
    n += put_str(b + n, client_id, strlen(client_id));
    if (flags & 0x80) n += put_str(b + n, c->username, strlen(c->username));
    if (flags & 0x40) n += put_str(b + n, c->password, strlen(c->password));
    return send_packet(sock, 0x10, b, n);
}

static int send_subscribe(int sock, const devos_mqtt_config_t *c, unsigned pid)
{
    uint8_t b[DEVOS_MQTT_MAX_SUBS * (DEVOS_MQTT_TOPIC_MAX + 3) + 2];
    size_t n = put_u16(b, pid);
    int count = 0;
    for (int i = 0; i < DEVOS_MQTT_MAX_SUBS; i++) {
        if (!c->subs[i][0]) continue;
        n += put_str(b + n, c->subs[i], strlen(c->subs[i]));
        b[n++] = 2;                             /* QoS 2: messages keep their own QoS */
        count++;
    }
    return count ? send_packet(sock, 0x82, b, n) : 0;
}

static int send_publish(int sock, const pub_t *p, unsigned pid)
{
    size_t tl = strlen(p->topic);
    size_t n = 2 + tl + (p->qos ? 2 : 0) + p->len;
    uint8_t *b = malloc(n);
    if (!b) return -1;
    size_t o = put_str(b, p->topic, tl);
    if (p->qos) o += put_u16(b + o, pid);
    memcpy(b + o, p->payload, p->len);
    int rc = send_packet(sock, (uint8_t)(0x30 | (p->qos << 1) | (p->retain ? 1 : 0)), b, n);
    free(b);
    return rc;
}

static int send_ack(int sock, uint8_t hdr, unsigned pid)
{
    uint8_t b[2];
    put_u16(b, pid);
    return send_packet(sock, hdr, b, 2);
}

/* ---- streaming parser ---- */
typedef struct {
    enum { P_HDR, P_LEN, P_BODY, P_SKIP } st;
    uint8_t hdr;
    uint32_t rem, got, skip;
    int shift;
    uint8_t *body;                              /* RX_CAP bytes */
} parser_t;

typedef struct {
    int sock;
    bool connack;
    int connack_rc;
    uint32_t last_rx;
    bool fail;
} link_t;

static const char *connack_text(int rc)
{
    switch (rc) {
    case 1: return "broker doesn't speak MQTT 3.1.1";
    case 2: return "client ID rejected";
    case 3: return "broker unavailable";
    case 4: return "bad username or password";
    case 5: return "not authorised";
    default: return "refused";
    }
}

static void handle_packet(link_t *ln, uint8_t hdr, const uint8_t *b, uint32_t stored, uint32_t full)
{
    uint8_t type = hdr >> 4;
    ln->last_rx = now_ms();
    switch (type) {
    case 2:                                     /* CONNACK */
        if (stored >= 2) {
            ln->connack = true;
            ln->connack_rc = b[1];
        }
        break;
    case 3: {                                   /* PUBLISH */
        uint8_t qos = (hdr >> 1) & 3;
        bool retain = hdr & 1;
        if (stored < 2) break;
        uint32_t tl = ((uint32_t)b[0] << 8) | b[1];
        uint32_t off = 2 + tl + (qos ? 2 : 0);
        if (off > stored) break;                /* topic cut off: not worth showing */
        unsigned pid = qos ? (((unsigned)b[2 + tl] << 8) | b[3 + tl]) : 0;
        store_message((const char *)b + 2, tl, b + off, stored - off, full - off, qos, retain);
        if (qos == 1) send_ack(ln->sock, 0x40, pid);         /* PUBACK */
        else if (qos == 2) send_ack(ln->sock, 0x50, pid);    /* PUBREC */
        break;
    }
    case 6:                                     /* PUBREL -> PUBCOMP */
        if (stored >= 2) send_ack(ln->sock, 0x70, ((unsigned)b[0] << 8) | b[1]);
        break;
    case 9:                                     /* SUBACK */
        for (uint32_t i = 2; i < stored; i++) {
            if (b[i] == 0x80) set_status("Connected, but the broker refused a subscription");
        }
        break;
    default:                                    /* PUBACK, PINGRESP, ... */
        break;
    }
}

static void parser_feed(parser_t *ps, link_t *ln, const uint8_t *d, size_t n)
{
    size_t i = 0;
    while (i < n) {
        switch (ps->st) {
        case P_HDR:
            ps->hdr = d[i++];
            ps->rem = 0;
            ps->shift = 0;
            ps->st = P_LEN;
            break;
        case P_LEN: {
            uint8_t c = d[i++];
            ps->rem |= (uint32_t)(c & 0x7F) << ps->shift;
            ps->shift += 7;
            if (c & 0x80) {
                if (ps->shift > 21) { ln->fail = true; return; }
                break;
            }
            ps->got = 0;
            if (ps->rem == 0) {
                handle_packet(ln, ps->hdr, ps->body, 0, 0);
                ps->st = P_HDR;
            } else {
                ps->st = P_BODY;
            }
            break;
        }
        case P_BODY: {
            uint32_t want = (ps->rem < RX_CAP ? ps->rem : RX_CAP) - ps->got;
            size_t take = n - i < want ? n - i : want;
            memcpy(ps->body + ps->got, d + i, take);
            ps->got += (uint32_t)take;
            i += take;
            if (ps->got == (ps->rem < RX_CAP ? ps->rem : RX_CAP)) {
                handle_packet(ln, ps->hdr, ps->body, ps->got, ps->rem);
                if (ps->rem > ps->got) {
                    ps->skip = ps->rem - ps->got;   /* too big to keep: drop the rest */
                    ps->st = P_SKIP;
                } else {
                    ps->st = P_HDR;
                }
            }
            break;
        }
        case P_SKIP: {
            size_t take = n - i < ps->skip ? n - i : ps->skip;
            ps->skip -= (uint32_t)take;
            i += take;
            if (!ps->skip) ps->st = P_HDR;
            break;
        }
        }
    }
}

/* ---- worker ---- */
static bool recv_some(link_t *ln, parser_t *ps, uint8_t *buf, size_t cap, int timeout_ms)
{
    int r = devos_net_socket_recv(ln->sock, buf, cap, timeout_ms);
    if (r > 0) {
        parser_feed(ps, ln, buf, (size_t)r);
        return !ln->fail;
    }
    if (r == 0) return false;                   /* closed by the broker */
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static void session(devos_mqtt_config_t *c, parser_t *ps, uint8_t *rbuf, int *backoff_s)
{
    char client_id[48];
    if (c->client_id[0]) snprintf(client_id, sizeof(client_id), "%s", c->client_id);
    else snprintf(client_id, sizeof(client_id), "devos-tab5-%04x", (unsigned)(rand32() & 0xFFFF));

    s_state = DEVOS_MQTT_CONNECTING;
    set_status("Connecting to %s:%d...", c->host, c->port);
    link_t ln = { .sock = devos_net_socket_connect(c->host, c->port, 5000) };
    if (ln.sock < 0) {
        set_status("Can't reach %s:%d", c->host, c->port);
        return;
    }
    uint8_t *body = ps->body;
    memset(ps, 0, sizeof(*ps));
    ps->body = body;
    ps->st = P_HDR;
    if (send_connect(ln.sock, c, client_id) < 0) {
        set_status("Connection to %s dropped", c->host);
        devos_net_socket_close(ln.sock);
        return;
    }
    uint32_t t0 = now_ms();
    while (!ln.connack && now_ms() - t0 < 5000 && s_want && !s_restart) {
        if (!recv_some(&ln, ps, rbuf, 1024, 200)) break;
    }
    if (!ln.connack) {
        set_status("%s:%d didn't answer as an MQTT broker", c->host, c->port);
        devos_net_socket_close(ln.sock);
        return;
    }
    if (ln.connack_rc != 0) {
        set_status("Broker refused: %s", connack_text(ln.connack_rc));
        devos_net_socket_close(ln.sock);
        if (ln.connack_rc == 4 || ln.connack_rc == 5 || ln.connack_rc == 2) s_want = false;  /* retrying won't help */
        return;
    }
    unsigned pid = 1;
    send_subscribe(ln.sock, c, pid++);
    char subs[160] = "";
    for (int i = 0; i < DEVOS_MQTT_MAX_SUBS; i++) {
        if (!c->subs[i][0]) continue;
        size_t l = strlen(subs);
        snprintf(subs + l, sizeof(subs) - l, "%s%s", l ? ", " : "", c->subs[i]);
    }
    set_status("Connected to %s:%d  -  %s", c->host, c->port, subs);
    s_state = DEVOS_MQTT_UP;
    *backoff_s = 2;

    uint32_t last_tx = now_ms();
    bool ping_out = false;
    uint32_t ka = (uint32_t)c->keepalive_s * 1000;
    while (s_want && !s_restart) {
        if (!recv_some(&ln, ps, rbuf, 2048, 100)) {
            set_status("Lost the connection to %s", c->host);
            break;
        }
        if (ping_out && ln.last_rx - last_tx < 0x80000000u) ping_out = false;
        /* queued publishes */
        for (;;) {
            pub_t p;
            bool have = false;
            LOCK();
            if (s_pub_n) {
                p = s_pubq[0];
                memmove(&s_pubq[0], &s_pubq[1], (size_t)(s_pub_n - 1) * sizeof(s_pubq[0]));
                s_pub_n--;
                have = true;
            }
            UNLOCK();
            if (!have) break;
            int rc = send_publish(ln.sock, &p, pid++);
            free(p.payload);
            if (pid > 0xFFFF) pid = 1;
            if (rc < 0) { ln.fail = true; break; }
            last_tx = now_ms();
        }
        if (ln.fail) {
            set_status("Lost the connection to %s", c->host);
            break;
        }
        uint32_t now = now_ms();
        if (!ping_out && now - last_tx >= ka / 2) {
            if (send_packet(ln.sock, 0xC0, NULL, 0) < 0) {     /* PINGREQ */
                set_status("Lost the connection to %s", c->host);
                break;
            }
            last_tx = now;
            ping_out = true;
        }
        if (now - ln.last_rx > ka + ka / 2) {
            set_status("%s stopped answering", c->host);
            break;
        }
    }
    if (!s_want) send_packet(ln.sock, 0xE0, NULL, 0);          /* DISCONNECT */
    devos_net_socket_close(ln.sock);
}

static void worker(void)
{
    parser_t *ps = big_alloc(sizeof(parser_t));
    uint8_t *body = big_alloc(RX_CAP);
    uint8_t *rbuf = big_alloc(2048);
    if (!ps || !body || !rbuf) {
        set_status("Out of memory");
        s_want = false;
        return;
    }
    ps->body = body;
    int backoff = 2;
    for (;;) {
        if (!s_want) {
            if (s_state != DEVOS_MQTT_IDLE) s_state = DEVOS_MQTT_IDLE;
            sleep_ms(200);
            continue;
        }
        s_restart = false;
        devos_mqtt_config_t c;
        devos_mqtt_get_config(&c);
        if (!c.host[0]) {
            set_status("Set a broker first");
            s_want = false;
            continue;
        }
        session(&c, ps, rbuf, &backoff);
        /* drop queued publishes from the lost session */
        LOCK();
        for (int i = 0; i < s_pub_n; i++) free(s_pubq[i].payload);
        s_pub_n = 0;
        UNLOCK();
        if (!s_want) {
            s_state = DEVOS_MQTT_IDLE;
            char st[sizeof(s_status)];
            LOCK();
            memcpy(st, s_status, sizeof(st));
            UNLOCK();
            if (strncmp(st, "Broker refused", 14) != 0) set_status("Disconnected");
            continue;
        }
        if (s_restart) continue;                /* new settings: reconnect now */
        s_state = DEVOS_MQTT_RETRYING;
        char st[sizeof(s_status)];
        LOCK();
        memcpy(st, s_status, sizeof(st));
        UNLOCK();
        set_status("%.110s  -  retrying in %d s", st, backoff);
        for (int i = 0; i < backoff * 5 && s_want && !s_restart; i++) sleep_ms(200);
        backoff = backoff * 2 > 30 ? 30 : backoff * 2;
    }
}

#ifdef ESP_PLATFORM
static void worker_task(void *arg)
{
    (void)arg;
    worker();
    vTaskDelete(NULL);
}
#else
static void *worker_thread(void *arg)
{
    (void)arg;
    worker();
    return NULL;
}
#endif

void devos_mqtt_init(void)
{
#ifdef ESP_PLATFORM
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
#endif
    cfg_load();
}

int devos_mqtt_start(void)
{
#ifdef ESP_PLATFORM
    if (!s_mx) devos_mqtt_init();               /* the worker needs the lock */
#endif
    LOCK();
    if (!s_ring) {
        s_ring = big_alloc(sizeof(slot_t) * DEVOS_MQTT_RING);
        s_topics = big_alloc(sizeof(devos_mqtt_topic_t) * DEVOS_MQTT_MAX_TOPICS);
    }
    bool ok = s_ring && s_topics;
    UNLOCK();
    if (!ok) {
        set_status("Out of memory");
        return -1;
    }
    s_want = true;
    s_restart = true;
    if (!s_worker_started) {
        s_worker_started = true;
#ifdef ESP_PLATFORM
        xTaskCreatePinnedToCore(worker_task, "mqtt", 6144, NULL, 4, NULL, DEVOS_CORE_NET_CRYPTO);
#else
        pthread_t t;
        pthread_create(&t, NULL, worker_thread, NULL);
        pthread_detach(t);
#endif
    }
    return 0;
}

void devos_mqtt_stop(void)
{
    s_want = false;
    set_status("Disconnected");
}

devos_mqtt_state_t devos_mqtt_state(void) { return s_state; }

void devos_mqtt_status_text(char *out, size_t cap)
{
    LOCK();
    snprintf(out, cap, "%s", s_status);
    UNLOCK();
}

/* Host end-to-end test for the mqtt.publish Jobs provider and the devos_mqtt
 * ticket / subscription-ownership / ingress-event contract, driven against a
 * minimal in-process MQTT broker (CONNACK, SUBACK, PUBACK, PINGRESP, inbound
 * PUBLISH). Covers: QoS 0 sent (terminal), QoS 1 acknowledged, a withheld
 * PUBACK timing out, a dropped session resolving tickets as LOST (never a
 * false delivery), QoS 2 rejected, owned subscriptions that do not clobber the
 * user's, duplicate/budget rejection, and the mqtt.message ingress event.
 *
 * Run from an ISOLATED CWD (the store writes ./sim_sdcard):
 *
 *   mkdir -p /tmp/jobs_mqtt_test && cd /tmp/jobs_mqtt_test
 *   gcc -O2 -I$REPO/main/jobs_providers -I$REPO/components/devos_actions \
 *       -I$REPO/components/devos_err -I$REPO/components/devos_mqtt \
 *       -I$REPO/components/devos_net -I$REPO/components/devos_events \
 *       -I$REPO/components/devos_json -I$REPO/components/devos_config/include \
 *       -I$REPO/components/devos_jobs -I$REPO/components/devos_tailnet \
 *       $REPO/tools/jobs_mqtt_test.c $REPO/main/jobs_providers/jobs_mqtt.c \
 *       $REPO/components/devos_mqtt/devos_mqtt.c $REPO/components/devos_events/devos_events.c \
 *       $REPO/components/devos_actions/devos_actions.c $REPO/components/devos_net/devos_net.c \
 *       $REPO/components/devos_json/devos_json.c -lpthread -o /tmp/jobs_mqtt_test && /tmp/jobs_mqtt_test
 */
#include "jobs_providers.h"
#include "devos_actions.h"
#include "devos_mqtt.h"
#include "devos_events.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* devos_net's resolver asks the tailnet first; not here */
int devos_tailnet_resolve(const char *name, char *out_ip, size_t out_len)
{
    (void)name; (void)out_ip; (void)out_len;
    return -1;
}

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int s_port;
static volatile int s_stop, s_puback = 1, s_drop;
static char s_sub_topics[16][96];
static volatile int s_sub_n;
static char s_pub_topic[96], s_pub_payload[256];
static volatile int s_got_publish;
static volatile int s_ingress;
static char s_ing_topic[96], s_ing_payload[256];
static char s_ing_recv_topic[96], s_ing_recv_payload[256];
static volatile int s_ing_recv;

static int recv_exact(int fd, uint8_t *b, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, b + got, n - got, 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}
static void send_all(int fd, const uint8_t *b, size_t n) { (void)!send(fd, b, n, MSG_NOSIGNAL); }

static void handle_one(int fd, uint8_t hdr)
{
    size_t rem = 0;
    int shift = 0;
    uint8_t lb;
    do {
        if (recv_exact(fd, &lb, 1) < 0) return;
        rem |= (size_t)(lb & 0x7f) << shift;
        shift += 7;
    } while ((lb & 0x80) && shift < 28);
    uint8_t body[1024];
    if (rem > sizeof(body)) return;
    if (rem && recv_exact(fd, body, rem) < 0) return;
    uint8_t type = hdr >> 4;
    switch (type) {
    case 1: { uint8_t c[4] = { 0x20, 0x02, 0x00, 0x00 }; send_all(fd, c, 4); break; }
    case 8: {                                            /* SUBSCRIBE -> SUBACK */
        unsigned pid = ((unsigned)body[0] << 8) | body[1];
        uint8_t sb[80];
        size_t n = 0;
        sb[n++] = (uint8_t)(pid >> 8); sb[n++] = (uint8_t)pid;
        size_t o = 2;
        while (o + 2 <= rem) {
            size_t tl = ((size_t)body[o] << 8) | body[o + 1];
            o += 2;
            if (tl >= 96 || o + tl + 1 > rem) break;
            char t[96];
            memcpy(t, body + o, tl); t[tl] = '\0';
            if (s_sub_n < 16) snprintf(s_sub_topics[s_sub_n++], 96, "%s", t);
            o += tl + 1;
            sb[n++] = 0x00;
        }
        uint8_t out[96];
        out[0] = 0x90; out[1] = (uint8_t)(n - 2);
        memcpy(out + 2, sb, n);
        send_all(fd, out, 2 + n);
        break;
    }
    case 10: {                                           /* UNSUBSCRIBE -> UNSUBACK */
        unsigned pid = ((unsigned)body[0] << 8) | body[1];
        uint8_t out[4] = { 0xB0, 0x02, (uint8_t)(pid >> 8), (uint8_t)pid };
        send_all(fd, out, 4);
        break;
    }
    case 3: {                                            /* PUBLISH from the client */
        unsigned qos = (hdr >> 1) & 3;
        size_t tl = ((size_t)body[0] << 8) | body[1];
        size_t off = 2 + tl + (qos ? 2 : 0);
        if (tl < sizeof(s_pub_topic)) snprintf(s_pub_topic, sizeof(s_pub_topic), "%.*s", (int)tl, body + 2);
        snprintf(s_pub_payload, sizeof(s_pub_payload), "%.*s", (int)(rem > off ? rem - off : 0), body + off);
        s_got_publish++;
        if (qos == 1 && s_puback) {
            unsigned pid = ((unsigned)body[2 + tl] << 8) | body[3 + tl];
            uint8_t a[4] = { 0x40, 0x02, (uint8_t)(pid >> 8), (uint8_t)pid };
            send_all(fd, a, 4);
        }
        break;
    }
    case 12: { uint8_t p[2] = { 0xD0, 0x00 }; send_all(fd, p, 2); break; }   /* PINGREQ */
    default: break;
    }
}

static void broker_conn(int fd)
{
    for (;;) {
        if (s_stop || s_drop) { close(fd); return; }
        if (s_ingress) {                                 /* test asks us to send an inbound PUBLISH */
            char topic[96], payload[256];
            snprintf(topic, sizeof(topic), "%s", s_ing_topic);
            snprintf(payload, sizeof(payload), "%s", s_ing_payload);
            s_ingress = 0;
            size_t tl = strlen(topic), pl = strlen(payload);
            uint8_t out[512];
            size_t o = 0;
            out[o++] = 0x30;
            out[o++] = (uint8_t)(2 + tl + pl);
            out[o++] = (uint8_t)(tl >> 8); out[o++] = (uint8_t)tl;
            memcpy(out + o, topic, tl); o += tl;
            memcpy(out + o, payload, pl); o += pl;
            send_all(fd, out, o);
        }
        struct pollfd pfd = { fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, 40);
        if (pr < 0) { if (errno == EINTR) continue; close(fd); return; }
        if (pr > 0 && (pfd.revents & POLLIN)) {
            uint8_t hdr;
            if (recv_exact(fd, &hdr, 1) < 0) { close(fd); return; }
            handle_one(fd, hdr);
        }
    }
}

static void *broker_thread(void *arg)
{
    (void)arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = 0 };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(ls, (struct sockaddr *)&a, sizeof(a));
    socklen_t al = sizeof(a);
    getsockname(ls, (struct sockaddr *)&a, &al);
    s_port = ntohs(a.sin_port);
    listen(ls, 8);
    while (!s_stop) {
        struct timeval tv = { 0, 200000 };
        setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        broker_conn(fd);
    }
    close(ls);
    return NULL;
}

static int has_sub(const char *t)
{
    for (int i = 0; i < s_sub_n; i++) if (strcmp(s_sub_topics[i], t) == 0) return 1;
    return 0;
}

/* Poll a ticket to a terminal state (up to ~3 s). */
static devos_mqtt_ticket_state_t wait_ticket(uint32_t id)
{
    devos_mqtt_ticket_state_t st = DEVOS_MQTT_TICKET_QUEUED;
    for (int i = 0; i < 120; i++) {
        usleep(25 * 1000);
        if (!devos_mqtt_ticket_poll(id, &st)) break;
        if (st != DEVOS_MQTT_TICKET_QUEUED && st != DEVOS_MQTT_TICKET_SENT) break;
    }
    return st;
}

static void ingress_cb(const devos_event_t *ev, void *user)
{
    (void)user;
    snprintf(s_ing_recv_topic, sizeof(s_ing_recv_topic), "%s", ev->source);
    uint32_t n = ev->payload_len < sizeof(s_ing_recv_payload) - 1 ? ev->payload_len : sizeof(s_ing_recv_payload) - 1;
    if (ev->payload) memcpy(s_ing_recv_payload, ev->payload, n);
    s_ing_recv_payload[n] = '\0';
    s_ing_recv = 1;
}

int main(void)
{
    pthread_t srv;
    pthread_create(&srv, NULL, broker_thread, NULL);
    while (!s_port) usleep(10 * 1000);

    devos_mqtt_init();
    devos_events_init();
    CHECK(devos_events_subscribe("mqtt.message", ingress_cb, NULL) > 0);
    jobs_mqtt_register();
    CHECK(devos_actions_find("mqtt.publish") != NULL);

    /* not connected: a tracked publish resolves as FAILED, never a delivery */
    {
        uint32_t id = devos_mqtt_publish_ticket("x/y", "hi", 2, 0, false, 1000);
        devos_mqtt_ticket_state_t st;
        CHECK(id != 0 && devos_mqtt_ticket_poll(id, &st) && st == DEVOS_MQTT_TICKET_FAILED);
        devos_mqtt_ticket_release(id);
    }
    /* QoS 2 is rejected outright */
    CHECK(devos_mqtt_publish_ticket("x/y", "hi", 2, 2, false, 1000) == 0);

    /* configure the fake broker; the user's own subscription must survive */
    devos_mqtt_config_t cfg;
    devos_mqtt_get_config(&cfg);
    snprintf(cfg.host, sizeof(cfg.host), "127.0.0.1");
    cfg.port = s_port;
    cfg.subs[0][0] = '\0';
    snprintf(cfg.subs[0], sizeof(cfg.subs[0]), "user/#");
    devos_mqtt_set_config(&cfg);

    /* a Jobs-owned subscription acquired before connect is in the SUBSCRIBE */
    int own = devos_mqtt_subscribe_owned("home/doorbell");
    CHECK(own > 0);
    CHECK(devos_mqtt_subscribe_owned("home/doorbell") == -1);      /* duplicate rejected */
    CHECK(devos_mqtt_subscribe_owned("user/#") == -1);             /* clashes with the user's */

    CHECK(devos_mqtt_start() == 0);
    for (int i = 0; i < 120 && devos_mqtt_state() != DEVOS_MQTT_UP; i++) usleep(25 * 1000);
    CHECK(devos_mqtt_state() == DEVOS_MQTT_UP);
    for (int i = 0; i < 40 && !has_sub("home/doorbell"); i++) usleep(25 * 1000);
    CHECK(has_sub("user/#") && has_sub("home/doorbell"));          /* both present */

    /* effective list = user + owned */
    {
        char eff[DEVOS_MQTT_MAX_SUBS + DEVOS_MQTT_MAX_OWNED][DEVOS_MQTT_TOPIC_MAX];
        int n = devos_mqtt_effective_subs(eff, DEVOS_MQTT_MAX_SUBS + DEVOS_MQTT_MAX_OWNED);
        CHECK(n == 2);
    }

    /* QoS 0: SENT is terminal, no acknowledgement */
    {
        uint32_t id = devos_mqtt_publish_ticket("home/light", "on", 2, 0, false, 1000);
        CHECK(id != 0);
        CHECK(wait_ticket(id) == DEVOS_MQTT_TICKET_SENT);
        CHECK(strcmp(s_pub_topic, "home/light") == 0 && strcmp(s_pub_payload, "on") == 0);
        devos_mqtt_ticket_release(id);
    }
    /* QoS 1 with PUBACK: ACKED */
    {
        uint32_t id = devos_mqtt_publish_ticket("home/light", "off", 3, 1, false, 2000);
        CHECK(id != 0);
        CHECK(wait_ticket(id) == DEVOS_MQTT_TICKET_ACKED);
        devos_mqtt_ticket_release(id);
    }
    /* QoS 1 with the PUBACK withheld: TIMEOUT, not a false ack */
    {
        s_puback = 0;
        uint32_t id = devos_mqtt_publish_ticket("home/light", "x", 1, 1, false, 500);
        CHECK(id != 0);
        CHECK(wait_ticket(id) == DEVOS_MQTT_TICKET_TIMEOUT);
        s_puback = 1;
        devos_mqtt_ticket_release(id);
    }

    /* inbound message -> mqtt.message event with source + payload */
    snprintf(s_ing_topic, sizeof(s_ing_topic), "home/temp");
    snprintf(s_ing_payload, sizeof(s_ing_payload), "21.5");
    s_ingress = 1;
    for (int i = 0; i < 80 && !s_ing_recv; i++) { usleep(25 * 1000); devos_events_drain(0); }
    CHECK(s_ing_recv == 1);
    CHECK(strcmp(s_ing_recv_topic, "home/temp") == 0 && strcmp(s_ing_recv_payload, "21.5") == 0);

    /* the mqtt.publish action runs through the devos_actions runtime */
    {
        devos_value_t a[5];
        a[0].type = DEVOS_VAL_STR; a[0].v.str.s = "home/light"; a[0].v.str.len = 10;
        a[1].type = DEVOS_VAL_STR; a[1].v.str.s = "auto"; a[1].v.str.len = 4;
        a[2].type = DEVOS_VAL_BOOL; a[2].v.b = false;
        a[3].type = DEVOS_VAL_INT; a[3].v.i = 1;
        a[4].type = DEVOS_VAL_DURATION; a[4].v.ms = 2000;
        devos_action_args_t args = { .args = a, .arg_count = 5 };
        devos_action_handle_t h;
        CHECK(devos_action_start("mqtt.publish", &args, NULL, &h) == DEVOS_OK);
        devos_action_state_t st = DEVOS_ACT_PENDING;
        devos_action_result_t res;
        memset(&res, 0, sizeof(res));
        for (int i = 0; i < 80 && st == DEVOS_ACT_PENDING; i++) { usleep(25 * 1000); devos_action_poll(h, &st, &res); }
        CHECK(st == DEVOS_ACT_DONE);
        CHECK(res.out_count == 2 && res.outs[0].v.b == true && res.outs[1].v.b == true);
        devos_action_release(h);
    }

    /* drop the session: an unfinished QoS 1 publish resolves as LOST */
    {
        s_puback = 0;                                    /* so it stays SENT, not ACKED */
        uint32_t id = devos_mqtt_publish_ticket("home/light", "lost", 4, 1, false, 5000);
        CHECK(id != 0);
        usleep(150 * 1000);                              /* let it reach SENT */
        s_drop = 1;
        devos_mqtt_ticket_state_t st;
        int ok = 0;
        for (int i = 0; i < 120; i++) {
            usleep(25 * 1000);
            if (devos_mqtt_ticket_poll(id, &st) && st == DEVOS_MQTT_TICKET_LOST) { ok = 1; break; }
        }
        CHECK(ok);
        devos_mqtt_ticket_release(id);
        s_drop = 0;
        s_puback = 1;
    }

    /* releasing an owned subscription frees its slot */
    devos_mqtt_unsubscribe_owned(own);
    CHECK(devos_mqtt_subscribe_owned("home/other") > 0);

    devos_mqtt_stop();
    s_stop = 1;
    pthread_join(srv, NULL);
    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}

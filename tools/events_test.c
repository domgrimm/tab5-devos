/* Host test for devos_events (components/devos_events): topic schemas, the
 * bounded queue, wildcard matching, truncation, drops and unsubscribe. Also
 * drives the jobs_events bridge (main/jobs_providers/jobs_events.c) so its
 * persisted boot_id and drop counter are covered.
 *
 *   gcc -O2 -Icomponents/devos_events -Icomponents/devos_err \
 *       -Icomponents/devos_config/include tools/events_test.c \
 *       components/devos_events/devos_events.c main/jobs_providers/jobs_events.c \
 *       -lpthread -o /tmp/events_test && /tmp/events_test
 */
#include "devos_events.h"
#include "jobs_providers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int s_hits;
static char s_last[64];
static bool s_last_truncated;
static void cb(const devos_event_t *ev, void *user)
{
    (void)user;
    s_hits++;
    snprintf(s_last, sizeof(s_last), "%s", ev->topic);
    s_last_truncated = ev->truncated;
}

static char s_boot_payload[128];
static void boot_cb(const devos_event_t *ev, void *user)
{
    (void)user;
    uint32_t n = ev->payload_len < sizeof(s_boot_payload) - 1 ? ev->payload_len : sizeof(s_boot_payload) - 1;
    if (ev->payload) memcpy(s_boot_payload, ev->payload, n);
    s_boot_payload[n] = '\0';
}

int main(void)
{
    CHECK(devos_events_init() == DEVOS_OK);

    const devos_event_schema_t SC = { .topic = "mqtt.message", .fields = "topic:string,payload:string,retain:boolean" };
    CHECK(devos_events_register_topic(&SC) == DEVOS_OK);
    CHECK(devos_events_register_topic(&SC) == DEVOS_OK);            /* idempotent */
    CHECK(devos_events_topic_schema("mqtt.message") != NULL);
    CHECK(devos_events_topic_schema("nope") == NULL);

    int sub = devos_events_subscribe("home/+/state", cb, NULL);
    int sub_all = devos_events_subscribe("#", cb, NULL);
    CHECK(sub > 0 && sub_all > 0);
    CHECK(devos_events_subscribe("bad", NULL, NULL) == -1);         /* no callback */

    s_hits = 0;
    devos_event_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.topic, sizeof(ev.topic), "home/kitchen/state");
    CHECK(devos_events_publish(&ev, "on", 2) == DEVOS_OK);
    CHECK(devos_events_drain(0) == 1);
    CHECK(s_hits == 2);                                             /* + and # both match */
    CHECK(strcmp(s_last, "home/kitchen/state") == 0);

    /* '+' is exactly one level; '#' matches the remainder */
    s_hits = 0;
    snprintf(ev.topic, sizeof(ev.topic), "home/a/b/state");
    CHECK(devos_events_publish(&ev, NULL, 0) == DEVOS_OK);
    devos_events_drain(0);
    CHECK(s_hits == 1);

    /* payload cap marks truncation */
    static char big[DEVOS_EVENTS_PAYLOAD_MAX + 32];
    memset(big, 'x', sizeof(big));
    s_last_truncated = false;
    snprintf(ev.topic, sizeof(ev.topic), "home/x/state");
    CHECK(devos_events_publish(&ev, big, sizeof(big)) == DEVOS_OK);
    devos_events_drain(0);
    CHECK(s_last_truncated);

    /* unsubscribing stops delivery */
    devos_events_unsubscribe(sub);
    devos_events_unsubscribe(sub_all);
    s_hits = 0;
    snprintf(ev.topic, sizeof(ev.topic), "home/y/state");
    devos_events_publish(&ev, NULL, 0);
    devos_events_drain(0);
    CHECK(s_hits == 0);

    /* the queue is bounded and drops are counted, not fatal */
    devos_events_stats_t st;
    for (int i = 0; i < DEVOS_EVENTS_QUEUE + 5; i++) devos_events_publish(&ev, "z", 1);
    devos_events_stats(&st);
    CHECK(st.dropped >= 5);
    devos_events_drain(0);
    devos_events_stats(&st);
    CHECK(st.queued == 0);

    /* jobs_events bridge: a real, persisted boot_id (not a hardcoded 0) */
    {
        mkdir("sim_sdcard", 0755);
        mkdir("sim_sdcard/.devos", 0755);
        FILE *f = fopen("sim_sdcard/.devos/boot_id", "wb");
        if (f) { fputs("41\n", f); fclose(f); }
        int bsub = devos_events_subscribe("system.boot", boot_cb, NULL);
        CHECK(bsub > 0);
        jobs_events_register();                          /* topic schema (idempotent) */
        s_boot_payload[0] = '\0';
        jobs_events_publish_boot(false);
        CHECK(jobs_events_boot_id() == 42);              /* read, incremented */
        devos_events_drain(0);
        CHECK(strstr(s_boot_payload, "\"boot_id\":42") != NULL);
        CHECK(strstr(s_boot_payload, "\"recovery\":false") != NULL);
        char buf[16] = "";
        f = fopen("sim_sdcard/.devos/boot_id", "rb");
        if (f) { if (fgets(buf, sizeof(buf), f)) {} fclose(f); }
        CHECK(atoi(buf) == 42);                          /* persisted for the next boot */

        /* a publish the bounded queue rejects is counted, never silent */
        unsigned before = devos_jobs_events_dropped();
        for (int i = 0; i < DEVOS_EVENTS_QUEUE + 2; i++) {
            devos_event_t e;
            memset(&e, 0, sizeof(e));
            snprintf(e.topic, sizeof(e.topic), "fill/%d", i);
            devos_events_publish(&e, NULL, 0);
        }
        jobs_events_publish_boot(false);
        CHECK(devos_jobs_events_dropped() == before + 1);
        devos_events_drain(0);
        devos_events_unsubscribe(bsub);
    }

    /* A full batch delivered to a full subscriber table: this is exactly the
     * path the Jobs scheduler runs on every tick, and the drain's scratch used
     * to live on that task's 8 KB stack, so the first received event rebooted
     * the device. */
    {
        int subs[DEVOS_EVENTS_SUBS_MAX];
        int nsubs = 0;
        while (nsubs < DEVOS_EVENTS_SUBS_MAX) {
            int s = devos_events_subscribe("#", cb, NULL);
            if (s <= 0) break;
            subs[nsubs++] = s;
        }
        CHECK(nsubs > 0);

        s_hits = 0;
        int pub = 0;
        for (int i = 0; i < DEVOS_EVENTS_QUEUE; i++) {
            devos_event_t e;
            memset(&e, 0, sizeof(e));
            snprintf(e.topic, sizeof(e.topic), "bulk/%d", i);
            if (devos_events_publish(&e, "payload", 7) != DEVOS_OK) break;
            pub++;
        }
        CHECK(pub == DEVOS_EVENTS_QUEUE);                /* queue accepted them all */
        CHECK(devos_events_drain(0) == DEVOS_EVENTS_QUEUE);
        CHECK(s_hits == pub * nsubs);                    /* every sub saw every event */
        devos_events_stats(&st);
        CHECK(st.queued == 0);

        for (int i = 0; i < nsubs; i++) devos_events_unsubscribe(subs[i]);
        s_hits = 0;
        devos_events_publish(&ev, NULL, 0);
        devos_events_drain(0);
        CHECK(s_hits == 0);
    }

    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}

/* Host test for devos_events (components/devos_events): topic schemas, the
 * bounded queue, wildcard matching, truncation, drops and unsubscribe.
 *
 *   gcc -O2 -Icomponents/devos_events -Icomponents/devos_err tools/events_test.c \
 *       components/devos_events/devos_events.c -lpthread -o /tmp/events_test && /tmp/events_test
 */
#include "devos_events.h"

#include <stdio.h>
#include <string.h>

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

    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}

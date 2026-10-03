#pragma once

/* devos_events: bounded, typed event delivery for Jobs triggers
 * (AGENTS.md "Jobs-compatible actions and events", PLAN.md Jobs section).
 *
 * CONTRACT (Phase 0). The implementation lands in Phase 2 (core) and Phase 6
 * (system producers) / Phase 7 (MQTT ingress). It has no UI, Jobs or app
 * dependency and never executes a job inline: publish() is nonblocking and
 * drops/coalesces under pressure with counters. Producers must emit outside
 * their own locks, with explicit bounded copies - never a pointer into a ring
 * that will be overwritten. Payloads larger than the cap are marked truncated;
 * a job that needs a whole JSON body fails clearly rather than seeing half of
 * one. ISR producers need the separate ISR-safe API (fixed payloads); v1
 * producers are task context only. */

#include "devos_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVOS_EVENTS_MAX_TOPICS  32
#define DEVOS_EVENTS_QUEUE       16      /* queued ingress events */
#define DEVOS_EVENTS_TOPIC_MAX   48
#define DEVOS_EVENTS_PAYLOAD_MAX 4096    /* per-event copy cap (PSRAM) */
#define DEVOS_EVENTS_SUBS_MAX    32

typedef struct {
    char topic[DEVOS_EVENTS_TOPIC_MAX];
    uint32_t seq;              /* per-topic monotonic sequence */
    int64_t mono_ms;           /* monotonic timestamp */
    int64_t wall_s;            /* wall time, 0/!wall_valid when unset */
    bool wall_valid;
    char provider[24];         /* originating engine uid */
    uint32_t corr;             /* correlation id, 0 = none */
    const uint8_t *payload;    /* owned copy, may be NULL */
    uint32_t payload_len;
    bool truncated;            /* producer payload exceeded the cap */
} devos_event_t;

/* Register a topic's schema so the Builder/validator understand its fields.
 * The schema text is a compact "field:type,..." list (metadata only). */
typedef struct {
    const char *topic;
    const char *fields;        /* e.g. "topic:string,payload:string,retain:boolean" */
    const char *description;
} devos_event_schema_t;

devos_err_t devos_events_init(void);
devos_err_t devos_events_register_topic(const devos_event_schema_t *schema);
const devos_event_schema_t *devos_events_topic_schema(const char *topic);

/* Nonblocking publish; copies the payload into PSRAM. Returns DEVOS_OK when
 * queued, DEVOS_ERR_NO_MEM when the queue is full (a drop is counted). */
devos_err_t devos_events_publish(const devos_event_t *ev, const void *payload, uint32_t len);

typedef void (*devos_event_cb_t)(const devos_event_t *ev, void *user);
/* Subscribe to an exact topic or a wildcard ("home/+/state", terminal "#").
 * Returns a subscription id > 0, or -1. Delivery is on the caller's task. */
int devos_events_subscribe(const char *pattern, devos_event_cb_t cb, void *user);
void devos_events_unsubscribe(int sub_id);
/* Deliver up to `max` queued events (0 = all) to the subscribers present at
 * the start of the call. Callbacks run on this task, outside the lock. Returns
 * the number of events drained. */
int devos_events_drain(int max);

typedef struct {
    uint32_t published, delivered, dropped, coalesced;
    int queued;
} devos_events_stats_t;
void devos_events_stats(devos_events_stats_t *out);

#ifdef __cplusplus
}
#endif

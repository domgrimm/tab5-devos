/* Host test for the Phase 6 Jobs triggers (components/devos_jobs
 * jobs_schedule.c + jobs_runtime.c) with a fake monotonic clock and a fake
 * wall clock: daily/weekdays occurrences, DST gap skip and fall-back run-once,
 * invalid clock blocking, a backward clock jump not repeating a claimed date,
 * a timezone change recomputing the deadline, and event triggers (where
 * filter, debounce, no match). Also the system-event bridge (Wi-Fi transition
 * only, battery invalid/absent never firing).
 *
 * Run from an ISOLATED CWD (the store writes ./sim_sdcard):
 *
 *   mkdir -p /tmp/jobs_sched_test && cd /tmp/jobs_sched_test
 *   gcc -O2 -I$REPO/components/devos_jobs -I$REPO/components/devos_actions \
 *       -I$REPO/components/devos_err -I$REPO/components/devos_json \
 *       -I$REPO/components/devos_events -I$REPO/main/jobs_providers \
 *       $REPO/tools/jobs_schedule_test.c \
 *       $REPO/components/devos_jobs/jobs_model.c $REPO/components/devos_jobs/jobs_parse.c \
 *       $REPO/components/devos_jobs/jobs_validate.c $REPO/components/devos_jobs/jobs_serialize.c \
 *       $REPO/components/devos_jobs/jobs_platform.c $REPO/components/devos_jobs/jobs_runtime.c \
 *       $REPO/components/devos_jobs/jobs_schedule.c $REPO/components/devos_jobs/jobs_store.c \
 *       $REPO/components/devos_actions/devos_actions.c $REPO/components/devos_json/devos_json.c \
 *       $REPO/components/devos_events/devos_events.c \
 *       $REPO/main/jobs_providers/jobs_system.c $REPO/main/jobs_providers/jobs_events.c \
 *       -lpthread -o /tmp/jobs_schedule_test && /tmp/jobs_schedule_test
 */
#include "devos_jobs.h"
#include "jobs_internal.h"
#include "jobs_platform.h"
#include "devos_events.h"
#include "devos_actions.h"
#include "jobs_providers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- fake clocks ---- */
static int64_t s_now;
static int64_t fake_now(void *u) { (void)u; return s_now; }

static devos_jobs_system_t s_sys;
static void set_wall(int64_t utc, int32_t off)
{
    s_sys.wall_unix_s = utc;
    s_sys.tz_offset_s = off;
    s_sys.time_valid = true;
    devos_jobs_set_system(&s_sys);
}

/* A fake DST function: `s_off_before` until `switch_utc`, `s_off_after` after. */
static int64_t s_switch;
static int32_t s_off_before, s_off_after;
static int32_t fake_dst(int64_t u, void *user) { (void)user; return u >= s_switch ? s_off_after : s_off_before; }

static int64_t mk(int y, int mo, int d, int h, int mi)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
    t.tm_hour = h; t.tm_min = mi; t.tm_isdst = 0;
    return (int64_t)timegm(&t);
}

static const char *last_log(void)
{
    static char line[JOBS_SYSTEM_LOG_LINE];
    int n = jobs_system_log_tail(line, 1);
    return n == 1 ? line : "";
}
static int log_count(void) { return jobs_system_log_count(); }

/* Disable every job so a section's expected counts are not perturbed by an
 * earlier job that is still enabled and overdue. */
static void disable_all(void)
{
    devos_job_summary_t s;
    for (int i = 0; devos_jobs_summary_at(i, &s); i++) devos_jobs_set_enabled(s.id, false);
}

static void pub_event(const char *topic, const char *source, const char *payload, bool retain)
{
    devos_event_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.topic, sizeof(ev.topic), "%s", topic);
    snprintf(ev.source, sizeof(ev.source), "%s", source);
    ev.retain = retain;
    devos_events_publish(&ev, payload, (uint32_t)strlen(payload));
}

/* Fake MQTT subscription hooks: the engine must acquire/release through them. */
static int s_sub_calls, s_unsub_calls;
static char s_sub_topic[JOBS_SUB_TOPIC_MAX];
static int fake_sub(const char *topic, void *user)
{
    (void)user;
    s_sub_calls++;
    snprintf(s_sub_topic, sizeof(s_sub_topic), "%s", topic);
    return 7;
}
static void fake_unsub(int handle, void *user) { (void)user; (void)handle; s_unsub_calls++; }

static void apply_ok(const char *id, const char *src)
{
    uint32_t rev = 0;
    devos_err_t rc = devos_jobs_apply(id, src, strlen(src), &rev);
    if (rc != DEVOS_OK) { printf("FAIL apply %s: %d\n", id, rc); fails++; }
    checks++;
}

int main(void)
{
    jobs_platform_set_clock(fake_now, NULL);
    s_now = 1000;
    CHECK(devos_jobs_init());
    CHECK(devos_events_init() == DEVOS_OK);
    jobs_system_register();
    jobs_events_register();

    /* ================= daily ================= */
    /* 2026-01-05 is a Monday. Local offset +0 for these. */
    set_wall(mk(2026, 1, 5, 7, 0), 0);
    apply_ok("daily", "version 1;\njob \"daily\" {\n trigger daily \"08:00\";\n system.log(message: \"daily\");\n}\n");
    CHECK(devos_jobs_set_enabled("daily", true) == DEVOS_OK);
    int base = log_count();
    s_now = 1000; devos_jobs_tick();                 /* 07:00 -> not due */
    CHECK(log_count() == base);
    set_wall(mk(2026, 1, 5, 8, 0), 0);
    devos_jobs_tick();                               /* due at 08:00 */
    CHECK(log_count() == base + 1);
    devos_jobs_tick();                               /* same instant: no repeat */
    CHECK(log_count() == base + 1);

    /* backward clock jump within the claimed local date must not repeat */
    set_wall(mk(2026, 1, 5, 7, 30), 0);
    devos_jobs_tick();
    CHECK(log_count() == base + 1);
    /* next day's 08:00 runs */
    set_wall(mk(2026, 1, 6, 8, 0), 0);
    devos_jobs_tick();
    CHECK(log_count() == base + 2);

    /* ================= invalid clock blocks ================= */
    s_sys.time_valid = false;
    devos_jobs_set_system(&s_sys);
    base = log_count();
    s_now += 10000; devos_jobs_tick();
    CHECK(log_count() == base);                      /* no wall time: blocked */
    set_wall(mk(2026, 1, 7, 8, 0), 0);               /* time valid again */
    devos_jobs_tick();
    CHECK(log_count() == base + 1);

    /* ================= weekdays ================= */
    /* 2026-01-09 is a Friday; next weekday 08:00 is Monday 2026-01-12. */
    disable_all();
    set_wall(mk(2026, 1, 9, 9, 0), 0);
    apply_ok("wd", "version 1;\njob \"wd\" {\n trigger weekdays \"08:00\";\n system.log(message: \"wd\");\n}\n");
    CHECK(devos_jobs_set_enabled("wd", true) == DEVOS_OK);
    base = log_count();
    s_now += 1000; devos_jobs_tick();
    CHECK(log_count() == base);                      /* Friday 09:00 already past */
    set_wall(mk(2026, 1, 11, 8, 0), 0);              /* Sunday */
    devos_jobs_tick();
    CHECK(log_count() == base);                      /* weekend skipped */
    set_wall(mk(2026, 1, 12, 8, 0), 0);              /* Monday */
    devos_jobs_tick();
    CHECK(log_count() == base + 1);

    /* ================= custom days ================= */
    /* Weekend-only: fires Saturday, skips Monday. */
    disable_all();
    set_wall(mk(2026, 1, 9, 9, 0), 0);               /* Friday 09:00 */
    apply_ok("we", "version 1;\njob \"we\" {\n trigger weekdays \"08:00\" days \"Sat,Sun\";\n system.log(message: \"we\");\n}\n");
    CHECK(devos_jobs_set_enabled("we", true) == DEVOS_OK);
    base = log_count();
    s_now += 1000; devos_jobs_tick();
    CHECK(log_count() == base);
    set_wall(mk(2026, 1, 10, 8, 0), 0);              /* Saturday */
    devos_jobs_tick();
    CHECK(log_count() == base + 1);
    set_wall(mk(2026, 1, 11, 8, 0), 0);              /* Sunday */
    devos_jobs_tick();
    CHECK(log_count() == base + 2);
    set_wall(mk(2026, 1, 12, 8, 0), 0);              /* Monday: skipped */
    devos_jobs_tick();
    CHECK(log_count() == base + 2);
    set_wall(mk(2026, 1, 18, 8, 0), 0);              /* next Sunday */
    devos_jobs_tick();
    CHECK(log_count() == base + 3);
    /* a single day narrows to it */
    CHECK(devos_jobs_set_enabled("we", false) == DEVOS_OK);
    apply_ok("we2", "version 1;\njob \"we2\" {\n trigger weekdays \"08:00\" days \"Wed\";\n system.log(message: \"we2\");\n}\n");
    CHECK(devos_jobs_set_enabled("we2", true) == DEVOS_OK);
    base = log_count();
    set_wall(mk(2026, 1, 19, 8, 0), 0);              /* Monday: skipped */
    devos_jobs_tick();
    CHECK(log_count() == base);
    set_wall(mk(2026, 1, 21, 8, 0), 0);              /* Wednesday */
    devos_jobs_tick();
    CHECK(log_count() == base + 1);

    /* ================= timezone change recomputes ================= */
    disable_all();
    set_wall(mk(2026, 1, 13, 7, 0), 0);
    apply_ok("tz", "version 1;\njob \"tz\" {\n trigger daily \"08:00\";\n system.log(message: \"tz\");\n}\n");
    CHECK(devos_jobs_set_enabled("tz", true) == DEVOS_OK);
    base = log_count();
    s_now += 1000; devos_jobs_tick();
    CHECK(log_count() == base);
    s_sys.tz_generation++;                            /* timezone changed */
    set_wall(mk(2026, 1, 13, 7, 0), 3 * 3600);        /* local now 10:00 */
    devos_jobs_tick();
    CHECK(log_count() == base);                       /* 10:00 local: today's 08:00 already passed */
    set_wall(mk(2026, 1, 14, 5, 0), 3 * 3600);        /* local 08:00 next day */
    devos_jobs_tick();
    CHECK(log_count() == base + 1);

    /* ================= DST gap (spring forward) ================= */
    /* America/New_York: 2026-03-08 02:00 local does not exist. Daily 02:30 on
     * that date is skipped; the next occurrence is 2026-03-09 02:30 local
     * (-4h) = 06:30 UTC. */
    s_switch = mk(2026, 3, 8, 7, 0);                  /* 07:00 UTC */
    s_off_before = -5 * 3600;
    s_off_after = -4 * 3600;
    devos_jobs_set_offset_fn(fake_dst, NULL);
    disable_all();
    set_wall(mk(2026, 3, 7, 12, 0), -5 * 3600);
    apply_ok("dst", "version 1;\njob \"dst\" {\n trigger daily \"02:30\";\n system.log(message: \"dst\");\n}\n");
    CHECK(devos_jobs_set_enabled("dst", true) == DEVOS_OK);
    base = log_count();
    s_now += 1000; devos_jobs_tick();
    CHECK(log_count() == base);
    set_wall(mk(2026, 3, 8, 6, 30), -4 * 3600);       /* the nonexistent local time */
    devos_jobs_tick();
    CHECK(log_count() == base);                       /* skipped */
    set_wall(mk(2026, 3, 9, 6, 30), -4 * 3600);       /* next day 02:30 local */
    devos_jobs_tick();
    CHECK(log_count() == base + 1);

    /* ================= DST fall-back runs once ================= */
    s_switch = mk(2026, 11, 1, 6, 0);                 /* 06:00 UTC */
    s_off_before = -4 * 3600;                         /* DST still in effect */
    s_off_after = -5 * 3600;                          /* back to standard time */
    disable_all();
    set_wall(mk(2026, 10, 31, 12, 0), -4 * 3600);
    apply_ok("fb", "version 1;\njob \"fb\" {\n trigger daily \"01:30\";\n system.log(message: \"fb\");\n}\n");
    CHECK(devos_jobs_set_enabled("fb", true) == DEVOS_OK);
    base = log_count();
    s_now += 1000; devos_jobs_tick();
    CHECK(log_count() == base);
    set_wall(mk(2026, 11, 1, 5, 30), -4 * 3600);      /* first 01:30 local */
    devos_jobs_tick();
    CHECK(log_count() == base + 1);
    set_wall(mk(2026, 11, 1, 6, 30), -5 * 3600);      /* repeated 01:30 local */
    devos_jobs_tick();
    CHECK(log_count() == base + 1);                   /* once per local date */
    devos_jobs_set_offset_fn(NULL, NULL);

    /* ================= event triggers ================= */
    disable_all();
    set_wall(mk(2026, 1, 20, 12, 0), 0);
    apply_ok("boot", "version 1;\njob \"boot\" {\n trigger event \"system.boot\";\n system.log(message: \"booted\");\n}\n");
    CHECK(devos_jobs_set_enabled("boot", true) == DEVOS_OK);
    base = log_count();
    devos_jobs_tick();
    CHECK(log_count() == base);                       /* nothing published yet */
    {
        devos_event_t ev; memset(&ev, 0, sizeof(ev));
        snprintf(ev.topic, sizeof(ev.topic), "system.boot");
        snprintf(ev.provider, sizeof(ev.provider), "test");
        CHECK(devos_events_publish(&ev, "{}", 2) == DEVOS_OK);
    }
    devos_jobs_tick();
    CHECK(log_count() == base + 1);
    CHECK(strcmp(last_log(), "booted") == 0);

    /* event data is available in the body too, not only in `where` */
    disable_all();
    apply_ok("body", "version 1;\njob \"body\" {\n trigger event \"system.boot\";\n"
                     " system.log(message: \"p=${event.payload}\");\n}\n");
    CHECK(devos_jobs_set_enabled("body", true) == DEVOS_OK);
    base = log_count();
    { devos_event_t ev; memset(&ev, 0, sizeof(ev)); snprintf(ev.topic, sizeof(ev.topic), "system.boot");
      devos_events_publish(&ev, "xyz", 3); }
    devos_jobs_tick();
    CHECK(strcmp(last_log(), "p=xyz") == 0);
    /* Run now of an event job has a synthetic (empty) event, not a crash */
    CHECK(devos_jobs_run_now("body") == DEVOS_OK);
    devos_jobs_tick();
    CHECK(strcmp(last_log(), "p=") == 0);

    /* where filter: a non-matching payload does not run */
    disable_all();
    apply_ok("filtered", "version 1;\njob \"filtered\" {\n"
                         " trigger event \"system.boot\" where contains(event.payload, \"recovery\");\n"
                         " system.log(message: \"filtered\");\n}\n");
    CHECK(devos_jobs_set_enabled("filtered", true) == DEVOS_OK);
    base = log_count();
    { devos_event_t ev; memset(&ev, 0, sizeof(ev)); snprintf(ev.topic, sizeof(ev.topic), "system.boot");
      devos_events_publish(&ev, "{}", 2); }
    devos_jobs_tick();
    CHECK(log_count() == base);                       /* payload lacks "recovery" */
    { devos_event_t ev; memset(&ev, 0, sizeof(ev)); snprintf(ev.topic, sizeof(ev.topic), "system.boot");
      devos_events_publish(&ev, "{\"recovery\":true}", 17); }
    devos_jobs_tick();
    CHECK(log_count() == base + 1);                   /* matches */

    /* debounce: two events within the window run once */
    disable_all();
    apply_ok("deb", "version 1;\njob \"deb\" {\n"
                    " trigger event \"system.boot\"(debounce: 5s);\n system.log(message: \"deb\");\n}\n");
    CHECK(devos_jobs_set_enabled("deb", true) == DEVOS_OK);
    base = log_count();
    s_now += 100000;
    { devos_event_t ev; memset(&ev, 0, sizeof(ev)); snprintf(ev.topic, sizeof(ev.topic), "system.boot");
      devos_events_publish(&ev, "{}", 2); }
    devos_jobs_tick();
    CHECK(log_count() == base + 1);
    s_now += 1000;                                     /* 1 s later: debounced */
    { devos_event_t ev; memset(&ev, 0, sizeof(ev)); snprintf(ev.topic, sizeof(ev.topic), "system.boot");
      devos_events_publish(&ev, "{}", 2); }
    devos_jobs_tick();
    CHECK(log_count() == base + 1);
    s_now += 6000;                                     /* past the window */
    { devos_event_t ev; memset(&ev, 0, sizeof(ev)); snprintf(ev.topic, sizeof(ev.topic), "system.boot");
      devos_events_publish(&ev, "{}", 2); }
    devos_jobs_tick();
    CHECK(log_count() == base + 2);

    /* ================= system-event bridge ================= */
    /* Wi-Fi: the initial state is not a transition; only a real change emits. */
    {
        devos_jobs_system_t s; memset(&s, 0, sizeof(s));
        s.wifi_connected = false;
        jobs_events_poll(&s);                          /* records initial */
        devos_events_stats_t st; devos_events_stats(&st);
        uint32_t before = st.published;
        jobs_events_poll(&s);                          /* no change */
        devos_events_stats(&st);
        CHECK(st.published == before);
        s.wifi_connected = true;
        snprintf(s.wifi_ssid, sizeof(s.wifi_ssid), "home");
        snprintf(s.local_ip, sizeof(s.local_ip), "10.0.0.2");
        jobs_events_poll(&s);                          /* transition */
        devos_events_stats(&st);
        CHECK(st.published == before + 1);
    }
    /* Battery: invalid or absent never fires; a valid present crossing does. */
    {
        devos_events_stats_t st; devos_events_stats(&st);
        uint32_t before = st.published;
        devos_jobs_system_t s; memset(&s, 0, sizeof(s));
        s.wifi_connected = true;                       /* keep the bridge's Wi-Fi state */
        s.battery_valid = false; s.battery_present = true; s.battery_percent = 5;
        jobs_events_poll(&s);
        devos_events_stats(&st);
        CHECK(st.published == before);                 /* invalid: no event */
        s.battery_valid = true; s.battery_present = false; s.battery_percent = 5;
        jobs_events_poll(&s);
        devos_events_stats(&st);
        CHECK(st.published == before);                 /* absent: no event */
        s.battery_valid = true; s.battery_present = true; s.battery_percent = 15;
        jobs_events_poll(&s);
        devos_events_stats(&st);
        CHECK(st.published == before + 1);             /* below threshold: fires */
        s.battery_percent = 10;
        jobs_events_poll(&s);
        devos_events_stats(&st);
        CHECK(st.published == before + 1);             /* hysteresis: no repeat */
        s.battery_percent = 30;
        jobs_events_poll(&s);                          /* re-arm */
        s.battery_percent = 18;
        jobs_events_poll(&s);
        devos_events_stats(&st);
        CHECK(st.published == before + 2);             /* fires again after re-arm */
    }

    /* ================= VPN event bridge ================= */
    /* Tailscale/WireGuard transitions only; the state was primed false above. */
    {
        devos_events_stats_t st; devos_events_stats(&st);
        uint32_t before = st.published;
        devos_jobs_system_t s; memset(&s, 0, sizeof(s));
        s.wifi_connected = true;                       /* keep Wi-Fi steady */
        s.tailscale_online = true;
        snprintf(s.tailscale_ip, sizeof(s.tailscale_ip), "100.64.0.5");
        snprintf(s.tailscale_hostname, sizeof(s.tailscale_hostname), "tab5");
        jobs_events_poll(&s);
        devos_events_stats(&st);
        CHECK(st.published == before + 1);             /* tailscale connected */
        s.wireguard_online = true;
        snprintf(s.wireguard_name, sizeof(s.wireguard_name), "home");
        snprintf(s.wireguard_address, sizeof(s.wireguard_address), "10.8.0.2/24");
        jobs_events_poll(&s);
        devos_events_stats(&st);
        CHECK(st.published == before + 2);             /* wireguard up */
        s.wireguard_online = false;
        jobs_events_poll(&s);
        devos_events_stats(&st);
        CHECK(st.published == before + 3);             /* wireguard down */
        s.tailscale_online = false;
        jobs_events_poll(&s);
        devos_events_stats(&st);
        CHECK(st.published == before + 4);             /* tailscale disconnected */
        jobs_events_poll(&s);
        devos_events_stats(&st);
        CHECK(st.published == before + 4);             /* steady state: no repeat */
    }

    /* a job can trigger on a VPN topic and read the payload */
    disable_all();
    apply_ok("wgjob", "version 1;\njob \"wgjob\" {\n"
                      " trigger event \"network.wireguard_up\" where contains(event.payload, \"home\");\n"
                      " system.log(message: \"tunnel ${event.topic}\");\n}\n");
    CHECK(devos_jobs_set_enabled("wgjob", true) == DEVOS_OK);
    base = log_count();
    { devos_event_t ev; memset(&ev, 0, sizeof(ev));
      snprintf(ev.topic, sizeof(ev.topic), "network.wireguard_up");
      devos_events_publish(&ev, "{\"name\":\"home\"}", 15); }
    devos_jobs_tick();
    CHECK(strcmp(last_log(), "tunnel network.wireguard_up") == 0);

    /* ================= MQTT trigger ================= */
    devos_jobs_set_mqtt_hooks(fake_sub, fake_unsub, NULL);
    disable_all();
    apply_ok("mqtt", "version 1;\njob \"mqtt\" {\n"
                      " trigger event \"mqtt.message\"(topic: \"home/doorbell\");\n"
                      " system.log(message: \"${event.source}=${event.payload}\");\n}\n");
    CHECK(s_sub_calls == 1 && strcmp(s_sub_topic, "home/doorbell") == 0);
    CHECK(devos_jobs_set_enabled("mqtt", true) == DEVOS_OK);
    base = log_count();
    pub_event("mqtt.message", "other/x", "nope", false);     /* outside the subscription */
    devos_jobs_tick();
    CHECK(log_count() == base);
    pub_event("mqtt.message", "home/doorbell", "ding", true); /* retained: ignored by default */
    devos_jobs_tick();
    CHECK(log_count() == base);
    pub_event("mqtt.message", "home/doorbell", "ding", false);
    devos_jobs_tick();
    CHECK(log_count() == base + 1);
    CHECK(strcmp(last_log(), "home/doorbell=ding") == 0);
    /* include_retained: true lets a retained message through */
    disable_all();
    apply_ok("mqtt2", "version 1;\njob \"mqtt2\" {\n"
                       " trigger event \"mqtt.message\"(topic: \"home/#\", include_retained: true);\n"
                       " system.log(message: \"r=${event.payload}\");\n}\n");
    CHECK(s_sub_calls == 2 && strcmp(s_sub_topic, "home/#") == 0);
    CHECK(devos_jobs_set_enabled("mqtt2", true) == DEVOS_OK);
    base = log_count();
    pub_event("mqtt.message", "home/x", "kept", true);
    devos_jobs_tick();
    CHECK(log_count() == base + 1);
    CHECK(strcmp(last_log(), "r=kept") == 0);
    /* deleting the job releases its broker subscription */
    disable_all();
    int before_unsub = s_unsub_calls;
    CHECK(devos_jobs_delete("mqtt2") == DEVOS_OK);
    CHECK(s_unsub_calls == before_unsub + 1);

    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}

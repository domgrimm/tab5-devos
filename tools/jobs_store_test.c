/* Host test for the Jobs durable store (components/devos_jobs/jobs_store.c):
 * transactional commit, catalog rotation, revision fallback, external edits as
 * candidates, history rotation, and failure injected at each commit boundary
 * (the previous revision must stay authoritative). Also a small engine
 * round-trip (apply -> shutdown -> init -> loaded) and RAM-only behaviour when
 * there is no card.
 *
 *   gcc -O2 -Icomponents/devos_jobs -Icomponents/devos_actions -Icomponents/devos_err \
 *       -Icomponents/devos_json -Imain/jobs_providers tools/jobs_store_test.c \
 *       components/devos_jobs/jobs_model.c components/devos_jobs/jobs_parse.c \
 *       components/devos_jobs/jobs_validate.c components/devos_jobs/jobs_serialize.c \
 *       components/devos_jobs/jobs_platform.c components/devos_jobs/jobs_runtime.c \
 *       components/devos_jobs/jobs_schedule.c components/devos_jobs/jobs_store.c \
 *       components/devos_actions/devos_actions.c components/devos_json/devos_json.c \
 *       main/jobs_providers/jobs_system.c -lpthread -o /tmp/jobs_store_test && /tmp/jobs_store_test
 */
#include "devos_jobs.h"
#include "jobs_store.h"
#include "jobs_platform.h"
#include "devos_actions.h"
#include "jobs_providers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static const char *SRC1 =
    "version 1;\njob \"roundtrip\" {\n trigger every 5m;\n system.log(message: \"one\");\n}\n";
static const char *SRC2 =
    "version 1;\njob \"roundtrip\" {\n trigger every 5m;\n system.log(message: \"two\");\n}\n";
static const char *SRC3 =
    "version 1;\njob \"roundtrip\" {\n trigger every 5m;\n system.log(message: \"three\");\n}\n";

static char s_loaded_id[40], s_loaded_src[512];
static uint32_t s_loaded_rev;
static bool s_loaded_enabled;

static bool rec_cb(const char *id, const char *src, size_t len, bool enabled, uint32_t rev, void *u)
{
    (void)u;
    snprintf(s_loaded_id, sizeof(s_loaded_id), "%s", id);
    size_t n = len < sizeof(s_loaded_src) - 1 ? len : sizeof(s_loaded_src) - 1;
    memcpy(s_loaded_src, src, n);
    s_loaded_src[n] = '\0';
    s_loaded_rev = rev;
    s_loaded_enabled = enabled;
    return true;
}

static bool exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static long fsize(const char *p) { struct stat st; return stat(p, &st) == 0 ? (long)st.st_size : -1; }

int main(void)
{
    char dir[] = "/tmp/devos_jobstest.XXXXXX";
    if (!mkdtemp(dir) || chdir(dir) != 0) { perror("scratch"); return 1; }
    const char *ROOT = "./sim_sdcard";

    CHECK(jobs_store_init(ROOT));
    jobs_store_status_t st;
    jobs_store_status(&st);
    CHECK(st.available && !st.degraded);

    /* 1. first commit: revision file + catalog + public projection */
    CHECK(jobs_store_commit("roundtrip", SRC1, strlen(SRC1), 1, false) == DEVOS_OK);
    CHECK(exists("sim_sdcard/.devos/jobs/revisions/roundtrip/1.job"));
    CHECK(exists("sim_sdcard/.devos/jobs/catalog.json"));
    CHECK(exists("sim_sdcard/jobs/roundtrip.job"));
    CHECK(jobs_store_load(rec_cb, NULL) == 1);
    CHECK(strcmp(s_loaded_id, "roundtrip") == 0 && s_loaded_rev == 1 && !s_loaded_enabled);
    CHECK(strstr(s_loaded_src, "\"one\"") != NULL);

    /* 2. a second revision becomes active */
    CHECK(jobs_store_commit("roundtrip", SRC2, strlen(SRC2), 2, true) == DEVOS_OK);
    CHECK(exists("sim_sdcard/.devos/jobs/catalog.json.prev"));           /* rotation kept the old catalog */
    CHECK(jobs_store_load(rec_cb, NULL) == 1);
    CHECK(s_loaded_rev == 2 && s_loaded_enabled && strstr(s_loaded_src, "\"two\""));

    /* 3. injected failure at each commit boundary: previous revision stays */
    for (int step = 1; step <= 4; step++) {
        jobs_store_fail_after(step);
        CHECK(jobs_store_commit("roundtrip", SRC3, strlen(SRC3), 3, true) != DEVOS_OK);
        jobs_store_reset_fail();
        CHECK(jobs_store_load(rec_cb, NULL) == 1);
        CHECK(s_loaded_rev == 2 && strstr(s_loaded_src, "\"two\""));
    }
    jobs_store_reset_fail();

    /* 4. external edit of the public projection is not authority */
    {
        FILE *f = fopen("sim_sdcard/jobs/roundtrip.job", "w");
        CHECK(f != NULL);
        if (f) { fputs(SRC3, f); fclose(f); }
    }
    CHECK(jobs_store_load(rec_cb, NULL) == 1);
    CHECK(s_loaded_rev == 2 && strstr(s_loaded_src, "\"two\""));   /* still the committed rev */

    /* 5. enable/disable persists */
    CHECK(jobs_store_set_enabled("roundtrip", false) == DEVOS_OK);
    CHECK(jobs_store_load(rec_cb, NULL) == 1 && !s_loaded_enabled);

    /* 6. history appends and rotates */
    for (int i = 0; i < 2000; i++) jobs_store_history_append("roundtrip", "a history line long enough to grow the segment");
    CHECK(exists("sim_sdcard/.devos/jobs/history/roundtrip.jsonl"));
    CHECK(exists("sim_sdcard/.devos/jobs/history/roundtrip.prev.jsonl"));
    CHECK(fsize("sim_sdcard/.devos/jobs/history/roundtrip.jsonl") <= 64 * 1024);

    /* 7. examples are not auto-registered by load */
    mkdir("sim_sdcard/jobs/examples", 0755);
    {
        FILE *f = fopen("sim_sdcard/jobs/examples/example.job", "w");
        if (f) { fputs(SRC1, f); fclose(f); }
    }
    CHECK(jobs_store_load(rec_cb, NULL) == 1);                /* only the catalog job */

    /* 8. remove */
    CHECK(jobs_store_remove("roundtrip") == DEVOS_OK);
    CHECK(jobs_store_load(rec_cb, NULL) == 0);
    CHECK(!exists("sim_sdcard/.devos/jobs/revisions/roundtrip/2.job"));

    /* 9. engine round-trip: apply -> shutdown -> init -> loaded */
    CHECK(devos_jobs_init());
    jobs_system_register();
    uint32_t rev = 0;
    CHECK(devos_jobs_apply("roundtrip", SRC1, strlen(SRC1), &rev) == DEVOS_OK);
    CHECK(devos_jobs_set_enabled("roundtrip", true) == DEVOS_OK);
    devos_jobs_shutdown();
    CHECK(devos_jobs_init());
    CHECK(devos_jobs_count() == 1);
    devos_job_summary_t sum;
    CHECK(devos_jobs_summary_at(0, &sum) && strcmp(sum.id, "roundtrip") == 0);
    CHECK(sum.state == DEVOS_JOB_ENABLED);
    devos_jobs_shutdown();

    /* 10. no card: the store reports unavailable and refuses to commit; the
     * engine still applies RAM-only (it re-inits from the mount point on init) */
    CHECK(devos_jobs_init());                /* loads the roundtrip job */
    CHECK(devos_jobs_count() == 1);
    CHECK(!jobs_store_init("/proc/devos_no_such_dir"));
    jobs_store_status(&st);
    CHECK(!st.available);
    CHECK(jobs_store_commit("x", SRC1, strlen(SRC1), 1, false) == DEVOS_ERR_INVALID_STATE);
    CHECK(devos_jobs_apply("mem", SRC1, strlen(SRC1), NULL) == DEVOS_OK);   /* RAM-only */
    CHECK(devos_jobs_count() == 2);
    devos_jobs_shutdown();

    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}

/* SD crash / power-loss recovery fixture for the Jobs store
 * (components/devos_jobs/jobs_store.c). Unlike the injected-write-failure
 * cases in jobs_store_test, this corrupts files on disk the way a torn FAT
 * write or a pulled card leaves them - truncated/garbage revisions, a bad
 * catalog, a partial .tmp sibling - and re-initialises the store each time.
 * The store must always resolve to a complete, valid generation (or to no
 * job), never partial executable source, and must remain writable afterwards.
 *
 *   gcc -O2 -Icomponents/devos_jobs -Icomponents/devos_actions -Icomponents/devos_err \
 *       -Icomponents/devos_json -Icomponents/devos_events -Imain/jobs_providers tools/jobs_crash_test.c \
 *       components/devos_jobs/jobs_model.c components/devos_jobs/jobs_parse.c \
 *       components/devos_jobs/jobs_validate.c components/devos_jobs/jobs_serialize.c \
 *       components/devos_jobs/jobs_platform.c components/devos_jobs/jobs_runtime.c \
 *       components/devos_jobs/jobs_schedule.c components/devos_jobs/jobs_store.c \
 *       components/devos_actions/devos_actions.c components/devos_json/devos_json.c \
 *       components/devos_events/devos_events.c \
 *       main/jobs_providers/jobs_system.c -lpthread -o /tmp/jobs_crash_test && /tmp/jobs_crash_test
 */
#include "jobs_store.h"
#include "jobs_model.h"
#include "devos_actions.h"
#include "jobs_providers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define ROOT "./sim_sdcard"
static const char *SRC1 = "version 1;\njob \"r\" {\n trigger every 5m;\n system.log(message: \"one\");\n}\n";
static const char *SRC2 = "version 1;\njob \"r\" {\n trigger every 5m;\n system.log(message: \"two\");\n}\n";
static const char *SRC3 = "version 1;\njob \"r\" {\n trigger every 5m;\n system.log(message: \"three\");\n}\n";

static uint32_t s_rev;
static char s_src[1024];

/* The engine's loader validates; the store falls back when it returns false. */
static bool rec_cb(const char *id, const char *src, size_t len, bool enabled, uint32_t rev, void *u)
{
    (void)id; (void)enabled; (void)u;
    jobs_ast_t *ast = jobs_parse(src, len, NULL);
    bool ok = ast && ast->diag_count == 0 && jobs_validate(ast);
    if (ast) jobs_ast_free(ast);
    if (!ok) return false;
    size_t n = len < sizeof(s_src) - 1 ? len : sizeof(s_src) - 1;
    memcpy(s_src, src, n);
    s_src[n] = '\0';
    s_rev = rev;
    return true;
}

/* Reopen the store from disk and load; returns accepted count. */
static int reload(void)
{
    s_rev = 0;
    s_src[0] = '\0';
    jobs_store_init(ROOT);
    return jobs_store_load(rec_cb, NULL);
}

static void build_clean(void)
{
    system("rm -rf ./sim_sdcard");
    CHECK(jobs_store_init(ROOT));
    CHECK(jobs_store_commit("r", SRC1, strlen(SRC1), 1, false) == DEVOS_OK);
    CHECK(jobs_store_commit("r", SRC2, strlen(SRC2), 2, true) == DEVOS_OK);
    CHECK(reload() == 1 && s_rev == 2 && strstr(s_src, "\"two\""));
}

static bool exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static long fsize(const char *p) { struct stat st; return stat(p, &st) == 0 ? (long)st.st_size : -1; }

static void write_all(const char *path, const char *s)
{
    FILE *f = fopen(path, "wb");
    if (f) { fputs(s, f); fclose(f); }
}

static void truncate_to(const char *path, long n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    FILE *w = fopen(path, "wb");
    if (w) { fwrite(b, 1, got, w); fclose(w); }
    free(b);
}

int main(void)
{
    char dir[] = "/tmp/devos_crashtest.XXXXXX";
    if (!mkdtemp(dir) || chdir(dir) != 0) { perror("scratch"); return 1; }

    jobs_system_register();          /* the sources use system.log */

    const char *rev2 = "sim_sdcard/.devos/jobs/revisions/r/2.job";
    const char *cat = "sim_sdcard/.devos/jobs/catalog.json";
    const char *catprev = "sim_sdcard/.devos/jobs/catalog.json.prev";

    /* 1. active revision truncated mid-write -> fall back to the previous generation */
    build_clean();
    truncate_to(rev2, 12);
    CHECK(reload() == 1 && s_rev == 1 && strstr(s_src, "\"one\""));

    /* 2. active revision is garbage -> same fallback, never partial source */
    build_clean();
    write_all(rev2, "THIS IS NOT A JOB {{{");
    CHECK(reload() == 1 && s_rev == 1 && strstr(s_src, "\"one\""));

    /* 3. the active generation is missing entirely -> the previous one is used */
    build_clean();
    remove(rev2);
    CHECK(reload() == 1 && s_rev == 1 && strstr(s_src, "\"one\""));

    /* 4. catalog torn -> recover from catalog.json.prev */
    build_clean();
    write_all(cat, "{\"schema\":1,\"jobs\":[{\"id\":\"r\",\"rev\":2,");
    CHECK(fsize(cat) > 0);
    CHECK(reload() == 1);                                     /* prev catalog names r */
    CHECK(s_rev == 1 && strstr(s_src, "version 1;"));

    /* 5. catalog.json missing -> prev is the authority */
    build_clean();
    remove(cat);
    CHECK(reload() == 1 && s_rev == 1);

    /* 6. both catalogs corrupted -> no jobs, no crash, store still available */
    build_clean();
    write_all(cat, "garbage");
    write_all(catprev, "also garbage");
    CHECK(reload() == 0);
    jobs_store_status_t st;
    jobs_store_status(&st);
    CHECK(st.available && st.jobs == 0);

    /* 7. an empty catalog file falls back to the previous one */
    build_clean();
    write_all(cat, "");
    CHECK(fsize(cat) == 0);
    CHECK(reload() == 1 && s_rev == 1);

    /* 8. a leftover partial .tmp sibling is ignored */
    build_clean();
    write_all("sim_sdcard/.devos/jobs/catalog.json.tmp", "{partial");
    write_all("sim_sdcard/.devos/jobs/revisions/r/99.job.tmp", "{partial");
    CHECK(reload() == 1 && s_rev == 2);

    /* 9. a revision whose source no longer validates is skipped, never executed */
    build_clean();
    {
        const char *BADSRC = "version 1;\njob \"r\" { oops }\n";
        CHECK(jobs_store_commit("r", BADSRC, strlen(BADSRC), 3, true) == DEVOS_OK);
    }
    CHECK(reload() == 1 && s_rev == 2 && strstr(s_src, "\"two\""));

    /* 10. after all that, the store is still writable and recovers cleanly */
    build_clean();
    CHECK(jobs_store_commit("r", SRC3, strlen(SRC3), 3, true) == DEVOS_OK);
    CHECK(reload() == 1 && s_rev == 3 && strstr(s_src, "\"three\""));

    (void)exists;
    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}

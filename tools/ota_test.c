/* Ponytail check: OTA manifest fetch/compare + power state machine.
 *
 * Runs standalone (manifests are written to the CWD and read over file://,
 * so it needs curl). Build from the repository root, run from an isolated CWD:
 *
 *   gcc -o /tmp/ota_test tools/ota_test.c \
 *     components/devos_ota/devos_ota.c \
 *     components/devos_json/devos_json.c \
 *     components/devos_power/devos_power.c \
 *     -Imain/include \
 *     -Icomponents/devos_ota \
 *     -Icomponents/devos_json \
 *     -Icomponents/devos_net \
 *     -Icomponents/devos_power \
 *     -Icomponents/devos_core \
 *     -Icomponents/devos_storage \
 *     -Icomponents/lvgl -lpthread && \
 *   mkdir -p /tmp/devos_otatest && cd /tmp/devos_otatest && /tmp/ota_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "devos_core.h"
static devos_telemetry_t fake_t;
const devos_telemetry_t *devos_telemetry_get(void) { return &fake_t; }
void devos_telemetry_update(const devos_telemetry_t *t) { fake_t = *t; }

int devos_net_http_get(const char *host, int port, const char *path,
                       char *resp, size_t cap, int timeout_ms,
                       int *status_out)
{
    (void)host; (void)path; (void)timeout_ms;
    if (port == 8090) {
        if (status_out) *status_out = 200;
        const char *json = "{\"version\":\"v9.9.9\",\"url\":\"https://example.com/tab5-devos.bin\",\"size\":2097152}";
        snprintf(resp, cap, "%s", json);
        return 0;
    }
    if (status_out) *status_out = 0;
    return -1;
}

#include "devos_ota.h"
#include "devos_power.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } \
} while (0)

int main(void)
{
    /* power machine */
    devos_power_init();
    CHECK(devos_power_mode() == DEVOS_POWER_ACTIVE);
    devos_power_poll(10);
    CHECK(devos_power_idle_s() == 10);
    CHECK(devos_power_brightness() == 100);
    devos_power_poll(119);
    CHECK(devos_power_mode() == DEVOS_POWER_ACTIVE);
    devos_power_poll(120);
    CHECK(devos_power_mode() == DEVOS_POWER_DIMMED);
    CHECK(devos_power_brightness() == 40);
    devos_power_poll(600);
    CHECK(devos_power_mode() == DEVOS_POWER_SLEEP);
    CHECK(devos_power_brightness() == 0);
    devos_power_activity(); /* any key wakes */
    CHECK(devos_power_mode() == DEVOS_POWER_ACTIVE);
    devos_power_sleep_now();
    CHECK(devos_power_mode() == DEVOS_POWER_SLEEP);
    CHECK(strcmp(devos_power_mode_text(), "Sleep") == 0);

    /* OTA manifests: written here, fetched over file:// (the simulator's
     * check uses curl), each check waited for. DEVOS_SIM_BUILD stands in for
     * the running image's build id. */
    devos_ota_init();
    char feed[256], cwd[160];
    devos_ota_get_feed(feed, sizeof(feed));
    CHECK(strstr(feed, "https://") == feed && strstr(feed, "/ota/devos-manifest.json") != NULL);  /* default */
    CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
    /* OTA_TEST_BASE (e.g. http://127.0.0.1:8099 serving this folder) when the
     * CWD path is too long for a feed URL (DEVOS_OTA_FEED_MAX) */
    char base[200];
    if (getenv("OTA_TEST_BASE")) snprintf(base, sizeof(base), "%s", getenv("OTA_TEST_BASE"));
    else snprintf(base, sizeof(base), "file://%s", cwd);
    setenv("DEVOS_SIM_BUILD", "8dc918537bb2c81b00112233445566778899aabbccddeeff0011223344556677", 1);
    long local = devos_ota_parse_version(DEVOS_VERSION_STR);
    char same[32], newer[32];
    snprintf(same, sizeof(same), "%ld.%ld.%ld", local >> 16, (local >> 8) & 255, local & 255);
    snprintf(newer, sizeof(newer), "%ld.%ld.%ld", local >> 16, ((local >> 8) & 255) + 1, 0L);
    struct { const char *name, *ver, *build; bool update; const char *text; } cases[] = {
        { "newer.json", newer, "", true, "available" },
        { "same_build.json", same, "8dc918537bb2c81b", false, "this build" },
        { "other_build.json", same, "0123456789abcdef", true, "New build" },
        { "same_nobuild.json", same, "", false, "Up to date" },
        { "older.json", "0.0.1", "0123456789abcdef", false, "Up to date" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        FILE *f = fopen(cases[i].name, "w");
        CHECK(f != NULL);
        if (!f) continue;
        fprintf(f, "{\"version\":\"%s\",\"url\":\"tab5-devos.bin\",\"size\":2097152%s%s%s}\n", cases[i].ver,
                cases[i].build[0] ? ",\"build\":\"" : "", cases[i].build, cases[i].build[0] ? "\"" : "");
        fclose(f);
        snprintf(feed, sizeof(feed), "%s/%s", base, cases[i].name);
        CHECK(devos_ota_set_feed(feed) == 0);
        CHECK(devos_ota_check() == 0);
        for (int t = 0; t < 100 && devos_ota_busy(); t++) usleep(50000);
        if (devos_ota_has_update() != cases[i].update || !strstr(devos_ota_update_text(), cases[i].text)) {
            printf("FAIL %s: update=%d \"%s\"\n", cases[i].name, devos_ota_has_update(), devos_ota_update_text());
            failures++;
        }
    }
    snprintf(feed, sizeof(feed), "%s/newer.json", base);
    devos_ota_set_feed(feed);
    devos_ota_check();
    for (int t = 0; t < 100 && devos_ota_busy(); t++) usleep(50000);
    CHECK(devos_ota_apply() == 0); /* sim dry run */
    CHECK(strstr(devos_ota_update_text(), "dry run") != NULL);
    CHECK(devos_ota_set_feed("not a url") != 0);
    CHECK(devos_ota_set_feed("http://127.0.0.1:1/nope.json") == 0);
    CHECK(devos_ota_check() == 0);
    for (int t = 0; t < 200 && devos_ota_busy(); t++) usleep(50000);
    CHECK(strstr(devos_ota_update_text(), "unreachable") != NULL);

    if (failures == 0) printf("ota/power unit tests: ALL PASS\n");
    return failures != 0;
}

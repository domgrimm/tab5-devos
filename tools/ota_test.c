/* Ponytail check: OTA manifest fetch/compare + power state machine.
 *
 * Runs standalone with mocked network GET and isolated CWD:
 *
 *   mkdir -p /tmp/opencode/otatest && cd /tmp/opencode/otatest && \
 *   gcc -o ota_test /home/dom/dev/tab5-devos/tools/ota_test.c \
 *     /home/dom/dev/tab5-devos/components/devos_ota/devos_ota.c \
 *     /home/dom/dev/tab5-devos/components/devos_json/devos_json.c \
 *     /home/dom/dev/tab5-devos/components/devos_power/devos_power.c \
 *     -I/home/dom/dev/tab5-devos/main/include \
 *     -I/home/dom/dev/tab5-devos/components/devos_ota \
 *     -I/home/dom/dev/tab5-devos/components/devos_json \
 *     -I/home/dom/dev/tab5-devos/components/devos_net \
 *     -I/home/dom/dev/tab5-devos/components/devos_power \
 *     -I/home/dom/dev/tab5-devos/components/devos_core \
 *     -I/home/dom/dev/tab5-devos/components/devos_storage \
 *     -I/home/dom/dev/tab5-devos/components/lvgl && ./ota_test
 */
#include <stdio.h>
#include <string.h>

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
        const char *json = "{\"version\":\"v9.9.9\",\"url\":\"http://10.2.132.54:8090/tab5-devos.bin\",\"size\":2097152}";
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

    /* OTA against the stub shelf (expects v9.9.9 manifest on :8090) */
    devos_ota_init();
    char feed[128];
    devos_ota_get_feed(feed, sizeof(feed));
    CHECK(strstr(feed, "8090") != NULL);
    CHECK(devos_ota_check() == 0);
    CHECK(strstr(devos_ota_update_text(), "v9.9.9") != NULL);
    CHECK(devos_ota_has_update() == true);
    CHECK(devos_ota_apply() == 0); /* sim dry run */
    CHECK(strstr(devos_ota_update_text(), "dry-run") != NULL);
    CHECK(devos_ota_set_feed("not a url") != 0);
    CHECK(devos_ota_set_feed("http://127.0.0.1:1/nope.json") == 0);
    CHECK(devos_ota_check() != 0);
    CHECK(strstr(devos_ota_update_text(), "unreachable") != NULL);

    if (failures == 0) printf("ota/power unit tests: ALL PASS\n");
    return failures != 0;
}

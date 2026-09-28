/* Host test: OTA check + install (devos_ota over devos_http) and the power
 * state machine.
 *
 * A small HTTP server on a loopback port serves manifests and fake firmware
 * images from the CWD (and can cut a download halfway); the simulator build of
 * devos_ota checks, downloads, verifies and "installs" into
 * sim_sdcard/.devos/ota_slot.bin. Build from the repository root, run from an
 * isolated CWD:
 *
 *   gcc -Wall -o /tmp/ota_test tools/ota_test.c \
 *     components/devos_ota/devos_ota.c components/devos_http/devos_http.c \
 *     components/devos_net/devos_net.c components/devos_crypto/devos_crypto.c \
 *     components/devos_json/devos_json.c components/devos_power/devos_power.c \
 *     -Imain/include -Icomponents/devos_ota -Icomponents/devos_http \
 *     -Icomponents/devos_net -Icomponents/devos_crypto -Icomponents/devos_json \
 *     -Icomponents/devos_power -Icomponents/devos_core -Icomponents/devos_storage \
 *     -Icomponents/devos_tailnet -Icomponents/lvgl -lpthread && \
 *   mkdir -p /tmp/devos_otatest && cd /tmp/devos_otatest && /tmp/ota_test
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "devos_core.h"
#include "devos_crypto.h"
#include "devos_ota.h"
#include "devos_power.h"

static devos_telemetry_t fake_t;
const devos_telemetry_t *devos_telemetry_get(void) { return &fake_t; }
void devos_telemetry_update(const devos_telemetry_t *t) { fake_t = *t; }

/* devos_net's resolver asks the tailnet first; not here */
int devos_tailnet_resolve(const char *name, char *out_ip, size_t out_len)
{
    (void)name; (void)out_ip; (void)out_len;
    return -1;
}

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } \
} while (0)

/* ------------------------------------------------------------------ test server */
#define PORT 18470

/* GET /<file> serves it; GET /cut/<file> promises the whole file but hangs up halfway. */
static void *server(void *arg)
{
    int ls = (int)(intptr_t)arg;
    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) continue;
        char req[1024];
        ssize_t n = recv(c, req, sizeof(req) - 1, 0);
        if (n <= 0) { close(c); continue; }
        req[n] = '\0';
        char path[256] = "";
        sscanf(req, "GET /%255s", path);
        bool cut = !strncmp(path, "cut/", 4);
        const char *name = cut ? path + 4 : path;
        FILE *f = fopen(name, "rb");
        if (!f) {
            const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(c, nf, strlen(nf), 0);
            close(c);
            continue;
        }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        char hdr[160];
        int hl = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n"
                          "Content-Type: application/octet-stream\r\nConnection: close\r\n\r\n", len);
        send(c, hdr, (size_t)hl, 0);
        long left = cut ? len / 2 : len;
        char buf[8192];
        while (left > 0) {
            size_t k = fread(buf, 1, left < (long)sizeof(buf) ? (size_t)left : sizeof(buf), f);
            if (!k || send(c, buf, k, MSG_NOSIGNAL) <= 0) break;
            left -= (long)k;
        }
        fclose(f);
        close(c);
    }
    return NULL;
}

static void start_server(void)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(PORT) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 8) != 0) {
        printf("FAIL: can't listen on port %d\n", PORT);
        exit(1);
    }
    pthread_t th;
    pthread_create(&th, NULL, server, (void *)(intptr_t)ls);
    pthread_detach(th);
}

/* ------------------------------------------------------------------ fixtures */
static uint8_t s_img[300 * 1024];
static const char *BUILD = "8dc918537bb2c81b00112233445566778899aabbccddeeff0011223344556677";

/* A fake app image: ESP image magic, app description magic, the build id where
 * esptool puts the ELF SHA-256. */
static void make_image(void)
{
    for (size_t i = 0; i < sizeof(s_img); i++) s_img[i] = (uint8_t)(i * 7 + 3);
    s_img[0] = 0xE9;
    s_img[32] = 0x32; s_img[33] = 0x54; s_img[34] = 0xCD; s_img[35] = 0xAB;
    for (int i = 0; i < 32; i++) sscanf(BUILD + i * 2, "%2hhx", &s_img[32 + 144 + i]);
    FILE *f = fopen("image.bin", "wb");
    fwrite(s_img, 1, sizeof(s_img), f);
    fclose(f);
    uint8_t junk[4096] = {0};
    f = fopen("junk.bin", "wb");
    fwrite(junk, 1, sizeof(junk), f);
    fclose(f);
}

static void hex(const uint8_t *d, size_t n, char *out)
{
    uint8_t h[32];
    devos_hash(DEVOS_HASH_SHA256, d, n, h);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", h[i]);
}

static void manifest(const char *name, const char *ver, const char *url, long size, const char *sha,
                     const char *build)
{
    FILE *f = fopen(name, "w");
    fprintf(f, "{\"version\":\"%s\",\"url\":\"%s\"", ver, url);
    if (size) fprintf(f, ",\"size\":%ld", size);
    if (sha) fprintf(f, ",\"sha256\":\"%s\"", sha);
    if (build) fprintf(f, ",\"build\":\"%s\"", build);
    fprintf(f, "}\n");
    fclose(f);
}

static void wait_idle(void)
{
    for (int t = 0; t < 200 && devos_ota_busy(); t++) usleep(50000);
}

static void check_feed(const char *name)
{
    char feed[96];
    snprintf(feed, sizeof(feed), "http://127.0.0.1:%d/%s", PORT, name);
    CHECK(devos_ota_set_feed(feed) == 0);
    CHECK(devos_ota_check() == 0);
    wait_idle();
}

/* Check, install, and return the final report. */
static const char *install(const char *name)
{
    check_feed(name);
    if (!devos_ota_has_update()) return devos_ota_update_text();
    CHECK(devos_ota_apply() == 0);
    wait_idle();
    return devos_ota_update_text();
}

static bool has(const char *text, const char *want)
{
    if (strstr(text, want)) return true;
    printf("  got \"%s\", wanted \"%s\"\n", text, want);
    return false;
}

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

    /* OTA */
    mkdir("sim_sdcard", 0755);
    mkdir("sim_sdcard/.devos", 0755);
    remove("sim_sdcard/.devos/ota_nvs.json");
    remove("sim_sdcard/.devos/ota_slot.bin");
    make_image();
    start_server();
    devos_ota_init();
    char feed[256];
    devos_ota_get_feed(feed, sizeof(feed));
    CHECK(strstr(feed, "https://") == feed && strstr(feed, "/ota/devos-manifest.json") != NULL);  /* default */

    /* versions and builds decide what is offered (DEVOS_SIM_BUILD = "our" build) */
    setenv("DEVOS_SIM_BUILD", BUILD, 1);
    long local = devos_ota_parse_version(DEVOS_VERSION_STR);
    char same[32], newer[32];
    snprintf(same, sizeof(same), "%ld.%ld.%ld", local >> 16, (local >> 8) & 255, local & 255);
    snprintf(newer, sizeof(newer), "%ld.%ld.%ld", local >> 16, ((local >> 8) & 255) + 1, 0L);
    struct { const char *name, *ver, *build; bool update; const char *text; } cases[] = {
        { "newer.json", newer, NULL, true, "available" },
        { "same_build.json", same, "8dc918537bb2c81b", false, "this build" },
        { "other_build.json", same, "0123456789abcdef", true, "New build" },
        { "same_nobuild.json", same, NULL, false, "Up to date" },
        { "older.json", "0.0.1", "0123456789abcdef", false, "Up to date" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        manifest(cases[i].name, cases[i].ver, "image.bin", 2097152, NULL, cases[i].build);
        check_feed(cases[i].name);
        if (devos_ota_has_update() != cases[i].update || !has(devos_ota_update_text(), cases[i].text)) {
            printf("FAIL %s\n", cases[i].name);
            failures++;
        }
    }
    unsetenv("DEVOS_SIM_BUILD");

    /* installs: download whole, verify, then write the slot */
    char sha[65], junk_sha[65];
    hex(s_img, sizeof(s_img), sha);
    uint8_t junk[4096] = {0};
    hex(junk, sizeof(junk), junk_sha);
    long sz = (long)sizeof(s_img);
    manifest("good.json", newer, "image.bin", sz, sha, BUILD);
    CHECK(has(install("good.json"), "verified and written"));
    FILE *f = fopen("sim_sdcard/.devos/ota_slot.bin", "rb");
    static uint8_t back[sizeof(s_img) + 16];
    size_t n = f ? fread(back, 1, sizeof(back), f) : 0;
    if (f) fclose(f);
    CHECK(n == sizeof(s_img) && memcmp(back, s_img, n) == 0);
    remove("sim_sdcard/.devos/ota_slot.bin");

    manifest("nosize.json", newer, "image.bin", 0, NULL, NULL);          /* old feeds: no size / sha / build */
    CHECK(has(install("nosize.json"), "verified and written"));
    remove("sim_sdcard/.devos/ota_slot.bin");

    manifest("badsha.json", newer, "image.bin", sz, junk_sha, BUILD);
    CHECK(has(install("badsha.json"), "Checksum mismatch"));
    manifest("otherbuild.json", newer, "image.bin", sz, sha, "0123456789abcdef");
    CHECK(has(install("otherbuild.json"), "different build"));
    manifest("cut.json", newer, "cut/image.bin", sz, sha, BUILD);
    CHECK(has(install("cut.json"), "ended early"));
    manifest("wrongsize.json", newer, "image.bin", sz + 10, NULL, NULL);
    CHECK(has(install("wrongsize.json"), "Size mismatch"));
    manifest("junk.json", newer, "junk.bin", 4096, junk_sha, NULL);
    CHECK(has(install("junk.json"), "not a firmware image"));
    manifest("huge.json", newer, "image.bin", 5L * 1024 * 1024, NULL, NULL);
    CHECK(has(install("huge.json"), "larger than the OTA slot"));
    manifest("missing.json", newer, "nothere.bin", sz, NULL, NULL);
    CHECK(has(install("missing.json"), "HTTP 404"));
    f = fopen("sim_sdcard/.devos/ota_slot.bin", "rb");
    CHECK(f == NULL);                                                    /* no failed install touched the slot */
    if (f) fclose(f);

    CHECK(devos_ota_set_feed("not a url") != 0);
    CHECK(devos_ota_set_feed("http://127.0.0.1:1/nope.json") == 0);
    CHECK(devos_ota_check() == 0);
    wait_idle();
    CHECK(has(devos_ota_update_text(), "unreachable"));

    if (failures == 0) printf("ota/power unit tests: ALL PASS\n");
    return failures != 0;
}

/* Host test for the encrypted credential store (components/devos_secrets):
 * ChaCha20-Poly1305 sealing round trip, persistence across a reload, a wrong
 * device key failing closed (corrupt, no plaintext), provisioning/delete,
 * bounds, and resolve/wipe. Runs from an isolated CWD (files under
 * ./sim_sdcard/.devos).
 *
 *   mkdir -p /tmp/secrets_test && cd /tmp/secrets_test
 *   gcc -O2 -I$REPO/components/devos_secrets -I$REPO/components/devos_crypto \
 *       -I$REPO/components/devos_err -I$REPO/components/devos_config/include \
 *       $REPO/tools/secrets_test.c $REPO/components/devos_secrets/devos_secrets.c \
 *       $REPO/components/devos_crypto/devos_crypto.c -o /tmp/secrets_test && /tmp/secrets_test
 */
#include "devos_secrets.h"
#include "devos_crypto.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static bool run_from_isolated_cwd(void)
{
    mkdir("sim_sdcard", 0755);
    mkdir("sim_sdcard/.devos", 0755);
    return true;
}

int main(void)
{
    run_from_isolated_cwd();
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i + 1);

    /* provision a known device key and store a secret */
    CHECK(devos_secrets_set_device_key(key) == DEVOS_OK);
    CHECK(devos_secrets_init() == DEVOS_OK);
    CHECK(devos_secrets_count() == 0);
    CHECK(!devos_secrets_has("health-token"));
    CHECK(devos_secrets_set("health-token", "s3cr3t-value", "Health check token") == DEVOS_OK);
    CHECK(devos_secrets_count() == 1);
    CHECK(devos_secrets_has("health-token"));
    const devos_secret_info_t *info = devos_secrets_at(0);
    CHECK(info && strcmp(info->name, "health-token") == 0 && strcmp(info->label, "Health check token") == 0);
    CHECK(info && info->version == 1);

    /* resolve into a caller buffer, then wipe it */
    char buf[64];
    int n = devos_secret_resolve("health-token", buf, sizeof(buf));
    CHECK(n == 12 && strcmp(buf, "s3cr3t-value") == 0);
    devos_secret_wipe(buf, sizeof(buf));
    CHECK(buf[0] == 0);
    CHECK(devos_secret_resolve("missing", buf, sizeof(buf)) == -1);
    CHECK(buf[0] == 0);

    /* persistence: reload from the sealed blob */
    devos_secrets_deinit();
    CHECK(devos_secrets_init() == DEVOS_OK);
    CHECK(devos_secrets_count() == 1);
    n = devos_secret_resolve("health-token", buf, sizeof(buf));
    CHECK(n == 12 && strcmp(buf, "s3cr3t-value") == 0);
    devos_secret_wipe(buf, sizeof(buf));
    CHECK(!devos_secrets_corrupt());

    /* replace bumps the version and persists the new value */
    CHECK(devos_secrets_set("health-token", "rotated", "Health check token") == DEVOS_OK);
    devos_secrets_deinit();
    devos_secrets_init();
    CHECK(devos_secret_resolve("health-token", buf, sizeof(buf)) == 7 && strcmp(buf, "rotated") == 0);
    devos_secret_wipe(buf, sizeof(buf));
    CHECK(devos_secrets_at(0) && devos_secrets_at(0)->version == 2);

    /* a wrong device key fails closed: no plaintext, corrupt flagged */
    uint8_t wrong[32];
    memset(wrong, 0xAA, sizeof(wrong));
    CHECK(devos_secrets_set_device_key(wrong) == DEVOS_ERR_FAIL);
    CHECK(devos_secrets_corrupt());
    CHECK(devos_secrets_count() == 0);
    CHECK(devos_secret_resolve("health-token", buf, sizeof(buf)) == -1);

    /* restore the real key: the sealed blob loads again (a wrong key never
     * deletes it, it just fails to open) */
    CHECK(devos_secrets_set_device_key(key) == DEVOS_OK);
    CHECK(!devos_secrets_corrupt());
    CHECK(devos_secrets_set("a", "1", NULL) == DEVOS_OK);
    CHECK(devos_secrets_set("b", "2", NULL) == DEVOS_OK);
    CHECK(devos_secrets_count() == 3);                    /* health-token + a + b */
    CHECK(devos_secrets_delete("a") == DEVOS_OK);
    CHECK(!devos_secrets_has("a") && devos_secrets_has("b"));
    CHECK(devos_secrets_delete("a") == DEVOS_ERR_NOT_FOUND);
    devos_secrets_deinit();
    devos_secrets_init();
    CHECK(devos_secrets_count() == 2 && devos_secrets_has("b"));

    /* provisioning by file: name=value lines are sealed in and the plaintext
     * file is removed */
    {
        FILE *f = fopen("sim_sdcard/.devos/secrets.import", "wb");
        CHECK(f != NULL);
        if (f) { fputs("# a comment\nimported=from-file\n", f); fclose(f); }
        devos_secrets_deinit();
        CHECK(devos_secrets_init() == DEVOS_OK);
        CHECK(devos_secrets_has("imported"));
        CHECK(devos_secret_resolve("imported", buf, sizeof(buf)) == 9 && strcmp(buf, "from-file") == 0);
        devos_secret_wipe(buf, sizeof(buf));
        struct stat st;
        CHECK(stat("sim_sdcard/.devos/secrets.import", &st) != 0);
    }

    /* bounds */
    char longname[DEVOS_SECRET_NAME_MAX + 2];
    memset(longname, 'x', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    CHECK(devos_secrets_set(longname, "v", NULL) == DEVOS_ERR_INVALID_ARG);
    char longval[DEVOS_SECRET_VALUE_MAX + 2];
    memset(longval, 'y', sizeof(longval) - 1);
    longval[sizeof(longval) - 1] = '\0';
    CHECK(devos_secrets_set("ok", longval, NULL) == DEVOS_ERR_INVALID_SIZE);
    CHECK(devos_secrets_set("", "v", NULL) == DEVOS_ERR_INVALID_ARG);
    CHECK(devos_secrets_set(NULL, "v", NULL) == DEVOS_ERR_INVALID_ARG);
    for (int i = devos_secrets_count(); i < DEVOS_SECRETS_MAX; i++) {
        char nm[24];
        snprintf(nm, sizeof(nm), "s%d", i);
        CHECK(devos_secrets_set(nm, "v", NULL) == DEVOS_OK);
    }
    CHECK(devos_secrets_set("overflow", "v", NULL) == DEVOS_ERR_NO_MEM);

    /* the security note names the actual mechanism, not a claim */
    const char *note = devos_secrets_security_note();
    CHECK(note && strstr(note, "ChaCha20-Poly1305") != NULL);

    printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "OK", fails, checks);
    return fails ? 1 : 0;
}

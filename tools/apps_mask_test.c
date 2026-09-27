/* Host-side unit test: the app switches behind Settings > Apps
 * (components/devos_core/devos_apps.c) - the boot mask, the rollback of a
 * mask that never boots, safe start, and the per-app memory figures.
 * Every "boot" runs in a forked child, so it starts with fresh statics and
 * sees only what the previous boots stored (./sim_sdcard/.devos/apps.cfg).
 *
 * Compile and run from an isolated directory:
 *   mkdir -p /tmp/devos_apps_test && cd /tmp/devos_apps_test && \
 *   gcc -o apps_test tools/apps_mask_test.c \
 *     components/devos_core/devos_apps.c \
 *     -Imain/include \
 *     -Icomponents/devos_core \
 *     -Icomponents/lvgl \
 *     && ./apps_test
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "devos_core.h"

typedef void (*boot_fn)(void);

/* One boot in a child; asserts there fail the test. */
static void boot(bool safe, boot_fn fn)
{
    fflush(stdout);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        devos_core_apps_load(safe);
        fn();
        fflush(stdout);
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("FAILED (boot child status %d)\n", st);
        exit(1);
    }
}

static void fresh(void)
{
    assert(devos_core_apps_boot_kind() == DEVOS_APPS_BOOT_NORMAL);
    assert(devos_core_app_enabled("docker") && devos_core_app_enabled("adsb"));
    assert(!devos_core_apps_restart_pending());
    /* the launcher and Settings can't be switched off */
    devos_core_set_app_enabled_next("settings", false);
    devos_core_set_app_enabled_next("launcher", false);
    assert(devos_core_app_enabled_next("settings") && devos_core_app_enabled_next("launcher"));
    assert(!devos_core_apps_restart_pending());
    /* switch Docker off for the next boot */
    devos_core_set_app_enabled_next("docker", false);
    assert(devos_core_app_enabled("docker"));           /* still running this boot */
    assert(!devos_core_app_enabled_next("docker"));
    assert(devos_core_apps_restart_pending());
    /* on and off again: nothing pending */
    devos_core_set_app_enabled_next("adsb", false);
    devos_core_set_app_enabled_next("adsb", true);
    assert(devos_core_apps_restart_pending());          /* docker still differs */
    devos_mem_mark_t m;
    devos_core_mem_mark(&m);
    void *p = malloc(300 * 1024);
    memset(p, 1, 300 * 1024);
    devos_core_app_add_cost("docker", &m);
    int32_t sram, psram;
    bool now;
    assert(devos_core_app_cost("docker", &sram, &psram, &now) && now && psram >= 300 * 1024);
    devos_core_apps_boot_ok();                          /* saves the figure */
}

static void docker_off(void)
{
    assert(devos_core_apps_boot_kind() == DEVOS_APPS_BOOT_NORMAL);
    assert(!devos_core_app_enabled("docker") && devos_core_app_enabled("adsb"));
    assert(!devos_core_apps_restart_pending());
    int32_t sram, psram;
    bool now = true;
    assert(devos_core_app_cost("docker", &sram, &psram, &now) && !now && psram >= 300 * 1024);
    devos_core_apps_boot_ok();                          /* docker-off is now the fallback */
    devos_core_set_app_enabled_next("adsb", false);     /* a new change ... */
}

static void crashes(void)
{
    /* ... that "crashes" before boot_ok */
    assert(devos_core_apps_boot_kind() == DEVOS_APPS_BOOT_NORMAL);
    assert(!devos_core_app_enabled("docker") && !devos_core_app_enabled("adsb"));
}

static void reverted(void)
{
    assert(devos_core_apps_boot_kind() == DEVOS_APPS_BOOT_REVERTED);
    assert(!devos_core_app_enabled("docker"));          /* back to the last good switches */
    assert(devos_core_app_enabled("adsb"));
    assert(!devos_core_apps_restart_pending());
    devos_core_apps_boot_ok();
}

static void still_reverted(void)
{
    assert(devos_core_apps_boot_kind() == DEVOS_APPS_BOOT_NORMAL);
    assert(!devos_core_app_enabled("docker") && devos_core_app_enabled("adsb"));
}

static void safe(void)
{
    assert(devos_core_apps_boot_kind() == DEVOS_APPS_BOOT_SAFE);
    assert(devos_core_app_enabled("docker") && devos_core_app_enabled("adsb"));
    assert(!devos_core_apps_restart_pending());
}

static void after_safe(void)
{
    assert(devos_core_apps_boot_kind() == DEVOS_APPS_BOOT_NORMAL);
    assert(devos_core_app_enabled("docker") && devos_core_app_enabled("adsb"));
}

int main(void)
{
    printf("=== App switch (boot mask) tests ===\n");
    remove(TAB5_SD_MOUNT_POINT "/.devos/apps.cfg");
    mkdir(TAB5_SD_MOUNT_POINT, 0755);
    mkdir(TAB5_SD_MOUNT_POINT "/.devos", 0755);

    boot(false, fresh);
    printf("fresh boot: all on, required apps stay on, a change is pending\n");
    boot(false, docker_off);
    printf("next boot: Docker off, its last memory figure kept\n");
    boot(false, crashes);
    boot(false, crashes);
    printf("a new change failed to boot twice ...\n");
    boot(false, reverted);
    printf("... and the third boot put the last good switches back\n");
    boot(false, still_reverted);
    boot(true, safe);
    printf("safe start switched every app back on\n");
    boot(false, after_safe);
    printf("=== All app switch tests passed ===\n");
    return 0;
}

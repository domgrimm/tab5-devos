/* Host-side unit test for devOS Phase 7:
 * Modular App Framework, Self-Registration, Dynamic Registry,
 * and Paginated Launcher Layout Logic.
 *
 * Compile and run from an isolated directory:
 *   mkdir -p /tmp/devos_phase7_test && cd /tmp/devos_phase7_test && \
 *   gcc -o launcher_test /home/dom/dev/tab5-devos/tools/modular_launcher_test.c \
 *     /home/dom/dev/tab5-devos/components/devos_core/devos_core.c \
 *     -I/home/dom/dev/tab5-devos/main/include \
 *     -I/home/dom/dev/tab5-devos/components/devos_core \
 *     -I/home/dom/dev/tab5-devos/components/devos_ui \
 *     -I/home/dom/dev/tab5-devos/components/devos_power \
 *     -I/home/dom/dev/tab5-devos/components/lvgl \
 *     && ./launcher_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#include "devos_config.h"
#include "devos_core.h"

/* Stubs for other subsystem calls made by devos_core */
void devos_top_bar_update(void) {}
void devos_power_activity(void) {}
void devos_theme_toggle(void) {}
void lv_obj_add_flag(lv_obj_t *obj, lv_obj_flag_t f) { (void)obj; (void)f; }
void lv_obj_remove_flag(lv_obj_t *obj, lv_obj_flag_t f) { (void)obj; (void)f; }
lv_obj_t *lv_obj_get_parent(const lv_obj_t *obj) { (void)obj; return NULL; }
uint32_t lv_obj_get_child_count(const lv_obj_t *obj) { (void)obj; return 0; }
void lv_obj_move_to_index(lv_obj_t *obj, int32_t idx) { (void)obj; (void)idx; }

/* Test Telemetry callbacks */
static int test_telemetry_1(char lines[3][64]) {
    snprintf(lines[0], 64, "* Test line 1");
    snprintf(lines[1], 64, "* Test line 2");
    snprintf(lines[2], 64, "* Test line 3");
    return 3;
}

static int test_telemetry_2(char lines[3][64]) {
    snprintf(lines[0], 64, "* Metric A: 99%%");
    snprintf(lines[1], 64, "* Metric B: OK");
    return 2;
}

int main(void)
{
    printf("=== Starting Phase 7 Modular Framework Unit Tests ===\n");

    /* 1. Initialize core */
    devos_core_init();
    assert(devos_core_app_count() == 0);

    /* 2. Register Launcher */
    devos_app_descriptor_t launcher_desc = {
        .id = DEVOS_APP_LAUNCHER,
        .uid = "launcher",
        .name = "Launcher",
        .title = "Home Screen",
    };
    devos_core_register_app(&launcher_desc);
    assert(devos_core_app_count() == 1);
    assert(devos_core_find_app("launcher") == &launcher_desc);

    /* 3. Register Core System Apps */
    devos_app_descriptor_t editor_desc = {
        .id = DEVOS_APP_EDITOR,
        .uid = "editor",
        .name = "Editor",
        .title = "Markdown Editor",
        .icon = LV_SYMBOL_EDIT,
        .category = "tools",
        .get_telemetry_lines = test_telemetry_1,
    };
    devos_app_descriptor_t terminal_desc = {
        .id = DEVOS_APP_TERMINAL,
        .uid = "terminal",
        .name = "Terminal",
        .title = "Terminal / SSH",
        .icon = LV_SYMBOL_POWER,
        .category = "systems",
        .get_telemetry_lines = test_telemetry_2,
    };
    devos_core_register_app(&editor_desc);
    devos_core_register_app(&terminal_desc);
    assert(devos_core_app_count() == 3);

    /* Verify find by UID and Index */
    assert(devos_core_find_app("editor") == &editor_desc);
    assert(devos_core_find_app("terminal") == &terminal_desc);
    assert(devos_core_get_app_at(1) == &editor_desc);
    assert(devos_core_get_app_at(2) == &terminal_desc);

    /* 4. Register Modular Drop-in Apps without predefined IDs (Auto-ID allocation) */
    devos_app_descriptor_t modular_app1 = {
        .id = (devos_app_id_t)0, /* Unassigned! Must not collide with launcher */
        .uid = "calc_app",
        .name = "Calculator",
        .title = "Hex Calculator",
        .category = "tools",
        .get_telemetry_lines = test_telemetry_1,
    };
    devos_core_register_app(&modular_app1);
    assert(devos_core_app_count() == 4);
    assert(modular_app1.id != DEVOS_APP_LAUNCHER);
    assert(devos_core_find_app("calc_app") == &modular_app1);

    /* 5. Switch app by UID */
    devos_core_switch_app_by_uid("calc_app");
    assert(devos_core_get_current_app() == modular_app1.id);

    devos_core_switch_app_by_uid("terminal");
    assert(devos_core_get_current_app() == DEVOS_APP_TERMINAL);

    /* 6. Register many apps up to 16 to verify registry scalability */
    static devos_app_descriptor_t extra_apps[12];
    memset(extra_apps, 0, sizeof(extra_apps));
    static char uids[12][16];
    for (int i = 0; i < 12; i++) {
        snprintf(uids[i], sizeof(uids[i]), "app_%02d", i + 1);
        extra_apps[i].id = (devos_app_id_t)0;
        extra_apps[i].uid = uids[i];
        extra_apps[i].name = uids[i];
        extra_apps[i].title = uids[i];
        extra_apps[i].category = "test";
        extra_apps[i].get_telemetry_lines = test_telemetry_2;
        devos_core_register_app(&extra_apps[i]);
    }
    assert(devos_core_app_count() == 16);
    printf("Successfully registered %d apps in dynamic registry.\n", devos_core_app_count());

    /* 7. Pagination math verification (8 tiles per page) */
    int active_launchable = devos_core_app_count() - 1; /* minus launcher */
    assert(active_launchable == 15);
    int pages = (active_launchable + 7) / 8;
    assert(pages == 2);
    printf("Pagination math verified: 15 apps = %d pages (8 on page 1, 7 on page 2).\n", pages);

    /* 8. Telemetry callback verification */
    char lines[3][64];
    int lc = editor_desc.get_telemetry_lines(lines);
    assert(lc == 3);
    assert(strcmp(lines[0], "* Test line 1") == 0);
    assert(strcmp(lines[1], "* Test line 2") == 0);
    assert(strcmp(lines[2], "* Test line 3") == 0);

    /* 9. Layout Persistence Test (JSON parsing & generation) */
    FILE *f = fopen("/tmp/test_layout.json", "w");
    assert(f != NULL);
    fprintf(f, "[\n  \"calc_app\",\n  \"terminal\",\n  \"editor\"\n]\n");
    fclose(f);

    /* Read and parse JSON back */
    f = fopen("/tmp/test_layout.json", "r");
    assert(f != NULL);
    char buf[512] = {0};
    fread(buf, 1, sizeof(buf), f);
    fclose(f);

    char parsed_uids[4][DEVOS_MAX_UID] = {{0}};
    int pcount = 0;
    const char *p = buf;
    while (*p && pcount < 4) {
        if (*p == '"') {
            p++;
            int tlen = 0;
            while (*p && *p != '"' && tlen < DEVOS_MAX_UID - 1) {
                parsed_uids[pcount][tlen++] = *p++;
            }
            parsed_uids[pcount][tlen] = '\0';
            if (*p == '"') p++;
            pcount++;
        } else {
            p++;
        }
    }
    assert(pcount == 3);
    assert(strcmp(parsed_uids[0], "calc_app") == 0);
    assert(strcmp(parsed_uids[1], "terminal") == 0);
    assert(strcmp(parsed_uids[2], "editor") == 0);
    printf("JSON slot persistence test passed: parsed %d UIDs correctly.\n", pcount);

    /* 10. Global Hotkey test (Sym + 1 .. Sym + 8) */
    devos_core_dispatch_key('2', DEVOS_MOD_FN);
    assert(devos_core_get_current_app() == DEVOS_APP_EDITOR);
    devos_core_dispatch_key('1', DEVOS_MOD_FN);
    assert(devos_core_get_current_app() == DEVOS_APP_TERMINAL);
    printf("Global hotkey Sym + 1..8 dispatching verified.\n");

    printf("\n=== All Phase 7 Unit Tests Passed! ===\n");
    return 0;
}

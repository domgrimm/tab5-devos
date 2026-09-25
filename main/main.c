#include "lvgl.h"
#include "devos_config.h"
#include "devos_theme.h"
#include "devos_core.h"
#include "devos_top_bar.h"
#include "devos_storage.h"
#include "devos_power.h"
#include "devos_ota.h"
#include "bsp_tab5.h"
#include "tab5_keyboard.h"
#include "devos_net.h"
#include "devos_sysmon.h"
#include "microlink.h"
#include "libssh2_port.h"

/* Apps */
#include "apps/app_launcher/app_launcher.h"
#include "apps/app_opendev/app_opendev.h"
#include "apps/app_terminal/app_terminal.h"
#include "apps/app_editor/app_editor.h"
#include "apps/app_tailscale/app_tailscale.h"
#include "apps/app_antigravity/app_antigravity.h"
#include "apps/app_settings/app_settings.h"
#include "apps/app_template/app_template.h"

#include <stdio.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_err.h"

#if LV_USE_LOG
static void lvgl_log_cb(lv_log_level_t level, const char *buf)
{
    (void)level;
    printf("[LVGL] %s\n", buf);
}
#endif

static uint32_t esp_tick_get_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void gui_task(void *arg)
{
    LV_UNUSED(arg);
    printf("[devOS] GUI presentation loop active on Core %d\n", xPortGetCoreID());
    uint32_t last_telemetry_tick = 0;
    uint32_t sim_seconds = 0;

    lv_obj_invalidate(lv_screen_active());

    while (1) {
        /* Dispatch keyboard events here (not on the keyboard task): app
         * handle_key()s run LVGL, which must stay single-threaded, and they
         * need this task's larger stack. Bounded per iteration. */
        uint32_t key;
        uint8_t mods;
        for (int n = 0; n < 16 && tab5_keyboard_get_key(&key, &mods); n++) {
            devos_core_dispatch_key(key, mods);
        }

        uint32_t step = lv_timer_handler();
        if (step == LV_NO_TIMER_READY || step > 30) step = 30;
        if (step < 5) step = 5;

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        if (now != last_telemetry_tick) {
            devos_sysmon_apply();
            devos_power_poll(++sim_seconds);
            devos_top_bar_update();
            app_launcher_update_telemetry();
            if (sim_seconds % 3 == 0) {
                printf("[devOS] GUI loop running, uptime: %lu s, free heap: %lu B\n",
                       (unsigned long)sim_seconds, (unsigned long)esp_get_free_heap_size());
            }
            last_telemetry_tick = now;
        }

        vTaskDelay(pdMS_TO_TICKS(step));
    }
}
#endif

#ifndef ESP_PLATFORM
#include <SDL2/SDL.h>
#include "src/drivers/sdl/lv_sdl_window.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "src/drivers/sdl/lv_sdl_keyboard.h"

static int sdl_event_watcher(void *userdata, SDL_Event *event)
{
    LV_UNUSED(userdata);
    if (event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_FINGERDOWN) {
        devos_power_activity();
        return 1;
    }
    if (event->type == SDL_KEYDOWN) {
        SDL_Keycode sym = event->key.keysym.sym;
        Uint16 mod = event->key.keysym.mod;
        uint8_t devos_mods = DEVOS_MOD_NONE;

        if (mod & KMOD_CTRL) devos_mods |= DEVOS_MOD_CTRL;
        if (mod & KMOD_SHIFT) devos_mods |= DEVOS_MOD_SHIFT;
        if (mod & KMOD_ALT) devos_mods |= DEVOS_MOD_ALT;

        /* Map F-keys or Alt/Ctrl keys as Fn */
        if (sym == SDLK_F1 || ((devos_mods & DEVOS_MOD_CTRL) && sym == SDLK_t)) {
            /* Simulate Fn + T */
            devos_core_dispatch_key('t', DEVOS_MOD_FN);
            return 0;
        }
        if (sym == SDLK_F2 || ((devos_mods & DEVOS_MOD_CTRL) && sym == SDLK_f)) {
            /* Simulate Fn + F */
            devos_core_dispatch_key('f', DEVOS_MOD_FN);
            return 0;
        }
        if (sym == SDLK_F3 || ((devos_mods & DEVOS_MOD_CTRL) && sym == SDLK_LEFTBRACKET)) {
            /* Simulate Sym + L (left sidebar) */
            devos_core_dispatch_key('l', DEVOS_MOD_FN);
            return 0;
        }
        if (sym == SDLK_F4 || ((devos_mods & DEVOS_MOD_CTRL) && sym == SDLK_RIGHTBRACKET)) {
            /* Simulate Sym + R (right inspector) */
            devos_core_dispatch_key('r', DEVOS_MOD_FN);
            return 0;
        }
        if (sym == SDLK_F5 || ((devos_mods & DEVOS_MOD_CTRL) && sym == SDLK_e)) {
            /* Simulate Fn + E (Arrange Mode) */
            devos_core_dispatch_key('e', DEVOS_MOD_FN);
            return 0;
        }
        if (sym == SDLK_ESCAPE || sym == SDLK_HOME) {
            devos_core_dispatch_key(LV_KEY_ESC, DEVOS_MOD_NONE);
            return 0;
        }

        /* Ctrl + 1..8 simulates Fn + 1..8 (Global App Switcher) */
        if ((devos_mods & DEVOS_MOD_CTRL) && sym >= SDLK_1 && sym <= SDLK_8) {
            devos_core_dispatch_key((uint32_t)('0' + (sym - SDLK_0)), DEVOS_MOD_FN);
            return 0;
        }

        /* Alt + 1..9 (Multi-Session SSH Switcher) */
        if ((devos_mods & DEVOS_MOD_ALT) && sym >= SDLK_1 && sym <= SDLK_9) {
            devos_core_dispatch_key((uint32_t)('0' + (sym - SDLK_0)), DEVOS_MOD_ALT);
            return 0;
        }

        /* Control sequences */
        if (devos_mods & DEVOS_MOD_CTRL) {
            if (sym == SDLK_c) { devos_core_dispatch_key(0x03, devos_mods); return 0; }
            if (sym == SDLK_d) { devos_core_dispatch_key(0x04, devos_mods); return 0; }
            if (sym == SDLK_z) { devos_core_dispatch_key(0x1a, devos_mods); return 0; }
            if (sym == SDLK_l) { devos_core_dispatch_key(0x0c, devos_mods); return 0; }
        }

        /* Special terminal keys */
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
            devos_core_dispatch_key('\r', devos_mods);
            return 0;
        }
        /* ponytail: PageUp/PageDown have no ASCII; distinct codes for pager */
        if (sym == SDLK_PAGEUP) {
            devos_core_dispatch_key(DEVOS_KEY_PGUP, DEVOS_MOD_NONE);
            return 0;
        }
        if (sym == SDLK_PAGEDOWN) {
            devos_core_dispatch_key(DEVOS_KEY_PGDN, DEVOS_MOD_NONE);
            return 0;
        }
        if (sym == SDLK_BACKSPACE) {
            devos_core_dispatch_key('\b', devos_mods);
            return 0;
        }
        if (sym == SDLK_TAB) {
            devos_core_dispatch_key('\t', devos_mods);
            return 0;
        }
        if (sym == SDLK_UP) {
            devos_core_dispatch_key(LV_KEY_UP, devos_mods);
            return 0;
        }
        if (sym == SDLK_DOWN) {
            devos_core_dispatch_key(LV_KEY_DOWN, devos_mods);
            return 0;
        }
        /* ponytail: no Fn key on PC keyboards; Ctrl+Arrow stands in for
         * Fn+Arrow (page flip on the launcher) in the simulator */
        if (sym == SDLK_LEFT) {
            devos_core_dispatch_key(LV_KEY_LEFT,
                (devos_mods & DEVOS_MOD_CTRL) ? DEVOS_MOD_FN : devos_mods);
            return 0;
        }
        if (sym == SDLK_RIGHT) {
            devos_core_dispatch_key(LV_KEY_RIGHT,
                (devos_mods & DEVOS_MOD_CTRL) ? DEVOS_MOD_FN : devos_mods);
            return 0;
        }

        /* Printable ASCII characters */
        if (sym >= 32 && sym <= 126) {
            /* If Shift is held and alpha, SDL sym is lowercase unless handled */
            char c = (char)sym;
            if ((devos_mods & DEVOS_MOD_SHIFT) && c >= 'a' && c <= 'z') {
                c -= 32;
            }
            devos_core_dispatch_key((uint32_t)c, devos_mods);
            return 0;
        }
    }
    return 1;
}
#endif

static void power_backlight_cb(int percent)
{
    bsp_tab5_set_brightness((uint8_t)percent);
}

static void devos_system_bringup(void)
{
    printf("\n==================================================\n");
    printf("  %s (%s)\n", DEVOS_VERSION_STR, DEVOS_BUILD_CODENAME);
    printf("  M5Stack Tab5 + A164 70-Key Keyboard Firmware\n");
    printf("==================================================\n\n");

    /* 1. Hardware Board Support Package bring-up */
    printf("[devOS] 1/8 Initializing BSP (I2C, Display buffers)...\n");
    bsp_tab5_init();

    /* 2. MicroSD auto-scaffolding & VFS mount */
    printf("[devOS] 2/8 Initializing Storage...\n");
    devos_storage_init();

    /* 3. A164 Keyboard bring-up */
    printf("[devOS] 3/8 Initializing A164 Keyboard...\n");
    tab5_keyboard_init();

    /* 4. devOS Theme Engine initialization */
    printf("[devOS] 4/8 Initializing Theme Engine...\n");
    devos_theme_init();

    /* 5. devOS Core engine initialization */
    printf("[devOS] 5/8 Initializing Core Event Bus...\n");
    devos_core_init();

    /* 5b. Power-mode state machine: drives the real backlight; touch and keys
     * count as activity; Sym+-/+ step the brightness. */
    devos_power_init();
    devos_power_set_backlight_cb(power_backlight_cb);
    devos_power_load_prefs();
    bsp_tab5_set_touch_activity_cb(devos_power_activity);
    devos_core_set_brightness_step_cb(devos_power_step_brightness);

    /* 5c. OTA feed config */
    devos_ota_init();

    /* 6. Network & Transparent Socket Routing bring-up */
    printf("[devOS] 6/8 Initializing Network Stack...\n");
    devos_net_init();

    /* 6b. System monitor: live battery/Wi-Fi/SD/memory/CPU telemetry and the
     * wall clock (RTC at boot, NTP once online). */
    devos_sysmon_init();

    /* 7. SSH & PTY Engine bring-up */
    printf("[devOS] 7/8 Initializing SSH Subsystem...\n");
    ssh_port_init();

    /* 8. Register all applications */
    printf("[devOS] 8/8 Registering Applications...\n");
    printf("[devOS]   - Registering OpenDev...\n");
    devos_core_register_app(app_opendev_get_descriptor());
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    printf("[devOS]   - Registering Terminal...\n");
    devos_core_register_app(app_terminal_get_descriptor());
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    printf("[devOS]   - Registering Editor...\n");
    devos_core_register_app(app_editor_get_descriptor());
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    printf("[devOS]   - Registering Tailscale...\n");
    devos_core_register_app(app_tailscale_get_descriptor());
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    printf("[devOS]   - Registering Antigravity...\n");
    devos_core_register_app(app_antigravity_get_descriptor());
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    printf("[devOS]   - Registering Settings...\n");
    devos_core_register_app(app_settings_get_descriptor());
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
#ifndef ESP_PLATFORM
    /* Placeholder tiles with invented telemetry: simulator only (exercises
     * launcher pagination), never on real hardware. */
    printf("[devOS]   - Registering Demo Apps...\n");
    app_template_register_demo_apps();
#endif
    printf("[devOS]   - Registering Launcher...\n");
    devos_core_register_app(app_launcher_get_descriptor());
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(10));
#endif

    /* Create persistent Top Status Bar */
    printf("[devOS] Creating Top Bar...\n");
    devos_top_bar_create(lv_layer_top());
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(10));
#endif

    /* Launch Home Screen / Dashboard */
    printf("[devOS] Switching to Launcher...\n");
    devos_core_switch_app(DEVOS_APP_LAUNCHER);

    printf("[devOS] System bring-up complete. Home Screen active.\n");
}

#ifdef ESP_PLATFORM
/* devOS bring-up + LVGL init, then hand off to the GUI task.
 *
 * Runs in its own task rather than in app_main because IDF creates the main
 * task (CONFIG_ESP_MAIN_TASK_STACK_SIZE) *before* the scheduler starts, when
 * only the small low internal-RAM region (~66-69 KB, shared with ESP-Hosted's
 * early tasks/queues and IDF's own tasks) is available; the large regions are
 * only added once main_task runs. A 16 KB main stack left so little headroom
 * that the FreeRTOS idle-task stacks fell through to TCM and tripped
 * xPortcheckValidStackMem (boot loop). This task is created after the
 * scheduler starts, so its stack comes from the big region; it stays pinned
 * to Core 0 so every driver interrupt lands on the same core as before. */
static void devos_boot_task(void *arg)
{
    LV_UNUSED(arg);

    /* Initialize LVGL */
    lv_init();
#if LV_USE_LOG
    lv_log_register_print_cb(lvgl_log_cb);
#endif

    /* Allocate 8 MB from external PSRAM to expand LVGL memory pool (Rule 2) */
    size_t lv_pool_size = 8 * 1024 * 1024;
    uint8_t *lv_psram_pool = (uint8_t *)heap_caps_malloc(lv_pool_size, MALLOC_CAP_SPIRAM);
    if (lv_psram_pool) {
        size_t chunk_size = 250 * 1024;
        int pools_added = 0;
        for (size_t offset = 0; offset + chunk_size <= lv_pool_size; offset += chunk_size) {
            if (lv_mem_add_pool(lv_psram_pool + offset, chunk_size)) {
                pools_added++;
            }
        }
        printf("[devOS] Added %d PSRAM memory pools (%zu KB total) to LVGL\n",
               pools_added, (pools_added * chunk_size) / 1024);
    } else {
        printf("[devOS] Warning: Failed to allocate 8MB PSRAM pool for LVGL\n");
    }

    lv_tick_set_cb(esp_tick_get_cb);

    /* Target board bringup */
    devos_system_bringup();
    printf("[devOS] Boot task stack headroom: %u bytes\n",
           (unsigned)uxTaskGetStackHighWaterMark(NULL));

    /* Pin GUI presentation loop to Core 1 (Rule 1) */
    xTaskCreatePinnedToCore(gui_task, "gui_task", 16384, NULL, 5, NULL, DEVOS_CORE_UI_INPUT);
    vTaskDelete(NULL);
}

void app_main(void)
{
    printf("[devOS] Booting app_main on Core %d...\n", xPortGetCoreID());

    /* Initialize NVS early: persistent config (OTA feed, Tailscale auth key,
     * opendev/agy tokens) opens the "nvs" partition, and nvs_open() fails with
     * ESP_ERR_NVS_NOT_INITIALIZED until this runs. Kept non-fatal so a corrupt
     * or version-bumped partition can't block boot. */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        printf("[devOS] NVS needs erase (%s); reformatting nvs partition...\n",
               esp_err_to_name(nvs_ret));
        if (nvs_flash_erase() == ESP_OK) {
            nvs_ret = nvs_flash_init();
        }
    }
    if (nvs_ret != ESP_OK) {
        printf("[devOS] Warning: nvs_flash_init failed (%s); config will not persist\n",
               esp_err_to_name(nvs_ret));
    }

    /* Bring-up runs in a post-scheduler task (see devos_boot_task). */
    xTaskCreatePinnedToCore(devos_boot_task, "devos_boot", 20480, NULL, 1, NULL, DEVOS_CORE_NET_CRYPTO);
}
#else
int main(int argc, char **argv)
{
    LV_UNUSED(argc);
    LV_UNUSED(argv);

    /* Initialize LVGL v9 */
    lv_init();

    /* Create 1280x720 SDL Display Window matching Tab5 geometry */
    lv_display_t *disp = lv_sdl_window_create(DEVOS_SCREEN_WIDTH, DEVOS_SCREEN_HEIGHT);
    lv_sdl_window_set_title(disp, "devOS - M5Stack Tab5 Cyberdeck Simulator (1280x720)");

    /* Initialize Mouse (touch emulation) and Keyboard */
    lv_sdl_mouse_create();
    lv_sdl_keyboard_create();

    /* Add SDL key watcher for Tab5 hotkeys */
    SDL_AddEventWatch(sdl_event_watcher, NULL);

    /* devOS system initialization */
    devos_system_bringup();

    uint32_t last_telemetry_tick = SDL_GetTicks();
    uint32_t sim_seconds = 0;

    /* Simulator 60 FPS Event Loop */
    while (1) {
        lv_timer_handler();

        uint32_t now = SDL_GetTicks();
        if (now - last_telemetry_tick >= 1000) {
            devos_sysmon_apply();
            devos_power_poll(++sim_seconds);
            devos_top_bar_update();
            app_launcher_update_telemetry();
            last_telemetry_tick = now;
        }

        SDL_Delay(16); /* ~60 FPS */
    }

    return 0;
}
#endif

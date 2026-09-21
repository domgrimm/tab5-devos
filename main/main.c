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

#include <stdio.h>
#include <unistd.h>

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
        if (sym == SDLK_F3 || ((devos_mods & DEVOS_MOD_CTRL) && sym == SDLK_b) || sym == SDLK_LEFTBRACKET) {
            /* Simulate Fn + [ */
            if ((devos_mods & DEVOS_MOD_CTRL) || sym == SDLK_F3) {
                devos_core_dispatch_key('[', DEVOS_MOD_FN);
                return 0;
            }
        }
        if (sym == SDLK_F4 || ((devos_mods & DEVOS_MOD_CTRL) && sym == SDLK_RIGHTBRACKET)) {
            /* Simulate Fn + ] */
            devos_core_dispatch_key(']', DEVOS_MOD_FN);
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

        /* Ctrl + 1..6 or F1..F6 simulates Fn + 1..6 (Global App Switcher) */
        if ((devos_mods & DEVOS_MOD_CTRL) && sym >= SDLK_1 && sym <= SDLK_6) {
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
        if (sym == SDLK_LEFT) {
            devos_core_dispatch_key(LV_KEY_LEFT, devos_mods);
            return 0;
        }
        if (sym == SDLK_RIGHT) {
            devos_core_dispatch_key(LV_KEY_RIGHT, devos_mods);
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

static void devos_system_bringup(void)
{
    printf("\n==================================================\n");
    printf("  %s (%s)\n", DEVOS_VERSION_STR, DEVOS_BUILD_CODENAME);
    printf("  M5Stack Tab5 + A164 70-Key Keyboard Firmware\n");
    printf("==================================================\n\n");

    /* 1. Hardware Board Support Package bring-up */
    bsp_tab5_init();

    /* 2. MicroSD auto-scaffolding & VFS mount */
    devos_storage_init();

    /* 3. A164 Keyboard bring-up */
    tab5_keyboard_init();

    /* 4. devOS Theme Engine initialization */
    devos_theme_init();

    /* 5. devOS Core engine initialization */
    devos_core_init();

    /* 5b. Power-mode state machine */
    devos_power_init();

    /* 5c. OTA feed config */
    devos_ota_init();

    /* 6. Network & Transparent Socket Routing bring-up */
    devos_net_init();

    /* 7. SSH & PTY Engine bring-up */
    ssh_port_init();

    /* 8. Register all applications */
    devos_core_register_app(app_launcher_get_descriptor());
    devos_core_register_app(app_opendev_get_descriptor());
    devos_core_register_app(app_terminal_get_descriptor());
    devos_core_register_app(app_editor_get_descriptor());
    devos_core_register_app(app_tailscale_get_descriptor());
    devos_core_register_app(app_antigravity_get_descriptor());
    devos_core_register_app(app_settings_get_descriptor());

    /* 7. Create persistent Top Status Bar */
    devos_top_bar_create(lv_layer_top());

    /* 8. Launch Home Screen / Dashboard */
    devos_core_switch_app(DEVOS_APP_LAUNCHER);

    printf("[devOS] System bring-up complete. Home Screen active.\n");
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    /* Initialize LVGL */
    lv_init();

    /* Target board bringup */
    devos_system_bringup();

    /* Main presentation loop pinned to Core 1 */
    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
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
            devos_telemetry_tick_sim();
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

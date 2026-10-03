#include "lvgl.h"
#include "devos_config.h"
#include "devos_theme.h"
#include "devos_core.h"
#include "devos_top_bar.h"
#include "devos_cmdpal.h"
#include "devos_hud.h"
#include "devos_shortcuts.h"
#include "devos_toast.h"
#include "devos_powerdlg.h"
#include "devos_storage.h"
#include "devos_fileshare.h"
#include "devos_power.h"
#include "devos_ota.h"
#include "bsp_tab5.h"
#include "tab5_keyboard.h"
#include "devos_net.h"
#include "devos_sysmon.h"
#include "devos_tailnet.h"
#include "devos_wireguard.h"
#include "devos_mqtt.h"
#include "libssh2_port.h"

/* Apps */
#include "apps/app_launcher/app_launcher.h"
#include "apps/app_terminal/app_terminal.h"
#include "apps/app_editor/app_editor.h"
#include "apps/app_tailscale/app_tailscale.h"
#include "apps/app_mqtt/app_mqtt.h"
#include "apps/app_netdiag/app_netdiag.h"
#include "apps/app_rest/app_rest.h"
#include "apps/app_docker/app_docker.h"
#include "apps/app_adsb/app_adsb.h"
#include "apps/app_cricket/app_cricket.h"
#include "apps/app_totp/app_totp.h"
#include "apps/app_wireguard/app_wireguard.h"
#include "apps/app_settings/app_settings.h"
#include "apps/app_template/app_template.h"
#include "apps/app_coder/app_coder.h"
#include "apps/app_jobs/app_jobs.h"
#include "jobs_providers/jobs_providers.h"
#include "devos_jobs.h"
#include "devos_events.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
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

static void jobs_sync_system(void);   /* defined below; used by the 1 Hz tick */

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
            jobs_sync_system();
            devos_power_poll(++sim_seconds);
            devos_top_bar_update();
            devos_toast_watch();                /* Wi-Fi / VPN / battery notices */
            app_launcher_update_telemetry();
            devos_tailnet_housekeeping();       /* peer cache -> flash, on this core */
            /* A new OTA image that has run the UI for 15 s is good: cancel
             * the bootloader's pending rollback. */
            if (sim_seconds == 15) {
                devos_ota_mark_boot_ok();
                devos_core_apps_boot_ok();      /* these app switches boot fine */
            }
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

/* US-layout Shift mapping for the digit and punctuation keys (SDL hands us the
 * unshifted keysym, so the shifted symbol has to be derived here). */
static char sim_shift_char(char c)
{
    switch (c) {
    case '1': return '!';  case '2': return '@';  case '3': return '#';
    case '4': return '$';  case '5': return '%';  case '6': return '^';
    case '7': return '&';  case '8': return '*';  case '9': return '(';
    case '0': return ')';
    case '-': return '_';  case '=': return '+';
    case '[': return '{';  case ']': return '}';
    case '\\': return '|';
    case ';': return ':';  case '\'': return '"';
    case ',': return '<';  case '.': return '>';  case '/': return '?';
    case '`': return '~';
    default:  return c;
    }
}

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

        /* F-keys stand in for Sym shortcuts. (Ctrl+T / F / E used to as well, but
         * they are real app shortcuts: editor find, timestamp ...) */
        if (sym == SDLK_F1) {
            /* Simulate Sym + T */
            devos_core_dispatch_key('t', DEVOS_MOD_FN);
            return 0;
        }
        if (sym == SDLK_F2) {
            /* Simulate Sym + F */
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
        if (sym == SDLK_F5) {
            /* Simulate Sym + E (Arrange Mode) */
            devos_core_dispatch_key('e', DEVOS_MOD_FN);
            return 0;
        }
        if (sym == SDLK_F6 || ((devos_mods & DEVOS_MOD_CTRL) && sym == SDLK_SPACE)) {
            /* Sym + Space (command palette) */
            devos_core_dispatch_key(' ', DEVOS_MOD_FN);
            return 0;
        }
        if (sym == SDLK_F7) {
            /* Sym + I (system info) */
            devos_core_dispatch_key('i', DEVOS_MOD_FN);
            return 0;
        }
        if (sym == SDLK_F8) {
            /* Sym + S (keyboard shortcuts) */
            devos_core_dispatch_key('s', DEVOS_MOD_FN);
            return 0;
        }
        if (sym == SDLK_F9) {
            /* Sym + V (paste) */
            devos_core_dispatch_key('v', DEVOS_MOD_FN);
            return 0;
        }
        /* Super (Windows / Cmd) + a letter, digit or Space is Sym + it */
        if ((mod & KMOD_GUI) && ((sym >= SDLK_a && sym <= SDLK_z) || (sym >= SDLK_0 && sym <= SDLK_9) ||
                                 sym == SDLK_SPACE)) {
            devos_core_dispatch_key((uint32_t)sym, DEVOS_MOD_FN | (devos_mods & DEVOS_MOD_SHIFT));
            return 0;
        }
        if (sym == SDLK_ESCAPE || sym == SDLK_HOME) {
            devos_core_dispatch_key(LV_KEY_ESC, DEVOS_MOD_NONE);
            return 0;
        }

        /* Ctrl + 1..8 simulates Sym + 1..8 (Global App Switcher) */
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
        /* ponytail: no Sym key on PC keyboards; Ctrl+Arrow stands in for
         * Sym+Arrow (page flip on the launcher) in the simulator */
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
            /* SDL reports the unshifted key here (Shift+- arrives as '-'), so
             * apply the US-layout shift for punctuation; fold Shift into
             * alphabet case. Drop the Shift modifier so LVGL doesn't treat the
             * character as a shortcut. */
            char c = (char)sym;
            if (devos_mods & DEVOS_MOD_SHIFT) {
                if (c >= 'a' && c <= 'z') c -= 32;
                else c = sim_shift_char(c);
            }
            devos_core_dispatch_key((uint32_t)c, devos_mods & (uint8_t)~DEVOS_MOD_SHIFT);
            return 0;
        }
    }
    return 1;
}
#endif

static void power_backlight_cb(int percent)
{
    bsp_tab5_set_brightness((uint8_t)percent);
    tab5_keyboard_lights_suspend(percent == 0);   /* keyboard lights sleep too */
}

/* The global power shortcuts: Sym+P sleeps, Sym+Shift+R restarts, Sym+Shift+Q
 * shuts down. Sleep is reversible and acts at once; the other two raise the
 * same confirm/cancel dialog as the Settings > Power buttons
 * (devos_powerdlg), which runs the restart check first. */
static void sys_action_cb(devos_sys_action_t action)
{
    switch (action) {
    case DEVOS_SYS_SLEEP:    devos_power_sleep_now(); break;
    case DEVOS_SYS_RESTART:  devos_powerdlg_restart(); break;
    case DEVOS_SYS_SHUTDOWN: devos_powerdlg_shutdown(); break;
    }
}

/* The keyboard lights' "theme accent" colour follows the theme. */
static void kbd_accent_cb(const devos_palette_t *palette, void *user_data)
{
    (void)user_data;
    tab5_keyboard_set_accent(lv_color_to_u32(palette->accent_primary));
}

/* OpenDev and Antigravity were removed: forget the server tokens they kept
 * (OpenChamber device token, bridge token). Only writes when there is
 * something left to erase, so this costs nothing after the first boot. */
static void forget_removed_apps(void)
{
#ifdef ESP_PLATFORM
    static const char *const ns[] = {"opendev", "agy"};
    for (size_t i = 0; i < sizeof(ns) / sizeof(ns[0]); i++) {
        nvs_iterator_t it = NULL;
        bool used = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns[i], NVS_TYPE_ANY, &it) == ESP_OK;
        nvs_release_iterator(it);
        nvs_handle_t h;
        if (used && nvs_open(ns[i], NVS_READWRITE, &h) == ESP_OK) {
            if (nvs_erase_all(h) == ESP_OK) nvs_commit(h);
            nvs_close(h);
            printf("[devOS] Erased saved %s settings (app removed)\n", ns[i]);
        }
    }
#else
    remove(TAB5_SD_MOUNT_POINT "/.devos/opendev_nvs.json");
    remove(TAB5_SD_MOUNT_POINT "/.devos/agy_nvs.json");
#endif
}

/* Start an app's engine only if the app is switched on, and count the
 * memory it takes towards the app (Settings > Apps). */
#define START_ENGINE(uid, call)                                         \
    do {                                                                \
        if (devos_core_app_enabled(uid)) {                              \
            devos_mem_mark_t m_;                                        \
            devos_core_mem_mark(&m_);                                   \
            call;                                                       \
            devos_core_app_add_cost(uid, &m_);                          \
        } else {                                                        \
            printf("[devOS]   %s engine not started (switched off)\n", uid); \
        }                                                               \
    } while (0)

/* DST-correct local UTC offset for the Jobs scheduler, from the ambient TZ
 * (sysmon sets it from the persisted zone). Standard C: off = u - mktime(gmtime(u)). */
static int32_t jobs_tz_offset_at(int64_t utc_s, void *user)
{
    (void)user;
    time_t u = (time_t)utc_s;
    struct tm ut;
    if (gmtime_r(&u, &ut) == NULL) return 0;
    ut.tm_isdst = -1;
    return (int32_t)((int64_t)utc_s - (int64_t)mktime(&ut));
}

/* MQTT subscription ownership for Jobs: an mqtt.message trigger acquires a
 * broker subscription through devos_mqtt without clobbering the user's own. */
static int jobs_mqtt_sub(const char *topic, void *user)
{
    (void)user;
    return devos_mqtt_subscribe_owned(topic);
}
static void jobs_mqtt_unsub(int handle, void *user)
{
    (void)user;
    devos_mqtt_unsubscribe_owned(handle);
}

/* Copy the compact sysmon snapshot into the Jobs engine (no LVGL, no I2C from
 * the engine). Called from the 1 Hz GUI tick. */
static void jobs_sync_system(void)
{
    devos_sysmon_snapshot_t s;
    devos_sysmon_get_snapshot(&s);
    devos_jobs_system_t j;
    memset(&j, 0, sizeof(j));
    j.battery_valid = s.battery_valid;
    j.battery_present = s.battery_present;
    j.charging = s.charging;
    j.battery_percent = s.battery_percent;
    j.wifi_connected = s.wifi_connected;
    memcpy(j.wifi_ssid, s.wifi_ssid, sizeof(j.wifi_ssid));
    memcpy(j.local_ip, s.local_ip, sizeof(j.local_ip));
    j.wifi_rssi = s.wifi_rssi;
    /* VPN status straight from the owning engines' thread-safe getters (safe
     * when the engine is off / never initialised). */
    devos_ts_info_t ts;
    devos_tailnet_get_info(&ts);
    j.tailscale_online = (ts.state == DEVOS_TS_CONNECTED);
    snprintf(j.tailscale_ip, sizeof(j.tailscale_ip), "%s", ts.ip);
    snprintf(j.tailscale_hostname, sizeof(j.tailscale_hostname), "%s", ts.hostname);
    devos_wg_info_t wg;
    devos_wg_get_info(&wg);
    j.wireguard_online = (wg.state == DEVOS_WG_UP);
    snprintf(j.wireguard_name, sizeof(j.wireguard_name), "%s", wg.name);
    snprintf(j.wireguard_address, sizeof(j.wireguard_address), "%s", wg.address);
    j.time_valid = s.time_valid;
    j.wall_unix_s = s.wall_unix_s;
    j.tz_offset_s = s.tz_offset_s;
    j.tz_generation = s.tz_generation;
    j.uptime_s = s.uptime_s;
    j.cpu_core0 = s.cpu_core0;
    j.cpu_core1 = s.cpu_core1;
    j.psram_free_kb = s.psram_free_kb;
    j.sram_free_kb = s.sram_free_kb;
    j.sram_largest_kb = s.sram_largest_kb;
    devos_jobs_set_system(&j);
    jobs_events_poll(&j);     /* Wi-Fi/battery transitions -> typed events */
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
    forget_removed_apps();

    /* 2b. App switches (Settings > Apps): read once, before any engine
     * starts. A finger held on the screen at power-on switches every app
     * back on (safe start). */
    devos_core_apps_load(bsp_tab5_touch_held());

    /* 3. A164 Keyboard bring-up */
    printf("[devOS] 3/8 Initializing A164 Keyboard...\n");
    tab5_keyboard_init();

    /* 4. devOS Theme Engine initialization */
    printf("[devOS] 4/8 Initializing Theme Engine...\n");
    devos_theme_init();
    devos_theme_add_listener(kbd_accent_cb, NULL);
    kbd_accent_cb(devos_theme_get(), NULL);

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
    devos_core_set_sys_action_cb(sys_action_cb);   /* Sym+P / Sym+Shift+R / Sym+Shift+Q */

    /* 5c. OTA feed config */
    devos_ota_init();

    /* 6. Network & Transparent Socket Routing bring-up */
    printf("[devOS] 6/8 Initializing Network Stack...\n");
    devos_net_init();
    devos_fileshare_init();   /* SD card over the network: off until Settings > File Sharing */

    /* 6a. Tailscale (MicroLink): connects in the background once Wi-Fi is up
     * if this device is enrolled and auto-connect is on. */
    START_ENGINE("tailscale", devos_tailnet_init());
    START_ENGINE("wireguard", devos_wg_init());   /* before sysmon, which polls its state */

    /* 6b. System monitor: live battery/Wi-Fi/SD/memory/CPU telemetry and the
     * wall clock (RTC at boot, NTP once online). */
    devos_sysmon_init();

    /* 6c. Jobs action providers: static typed schemas with lazy handlers,
     * registered independently of whether the Jobs app is switched on
     * (AGENTS.md invariant 10). Nothing here starts a task or a radio. The
     * event registry must exist first so topic schemas can be registered. */
    devos_events_init();
    jobs_providers_register_all();

    /* 6d. Jobs engine: scheduler + durable storage. Started only when the app
     * is on; automatic runs stay paused after a safe/reverted boot, and a
     * restart reports a running job. The DST-correct offset comes from the
     * device's POSIX zone, which sysmon has already applied. */
    devos_jobs_set_offset_fn(jobs_tz_offset_at, NULL);
    devos_jobs_set_mqtt_hooks(jobs_mqtt_sub, jobs_mqtt_unsub, NULL);
    START_ENGINE("jobs", devos_jobs_init());
    if (devos_core_apps_boot_kind() != DEVOS_APPS_BOOT_NORMAL) devos_jobs_set_safe_pause(true);
    devos_core_add_restart_check(devos_jobs_restart_check);

    /* Ready barrier passed and automatic execution is not recovery-paused:
     * emit system.boot exactly once so boot-triggered jobs can run. During a
     * safe/reverted boot the event is not emitted at all. */
    if (devos_jobs_state() == DEVOS_JOBS_READY && !devos_jobs_safe_paused())
        jobs_events_publish_boot(false);

    /* 7. SSH & PTY Engine bring-up */
    printf("[devOS] 7/8 Initializing SSH Subsystem...\n");
    START_ENGINE("terminal", ssh_port_init());

    /* 8. Register all applications */
    printf("[devOS] 8/8 Registering Applications...\n");
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
    printf("[devOS]   - Registering WireGuard...\n");
    devos_core_register_app(app_wireguard_get_descriptor());
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    printf("[devOS]   - Registering MQTT...\n");
    devos_core_register_app(app_mqtt_get_descriptor());
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    printf("[devOS]   - Registering Settings...\n");
    devos_core_register_app(app_settings_get_descriptor());
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    /* the newer apps after Settings: the first page keeps its familiar tiles */
    printf("[devOS]   - Registering Network...\n");
    devos_core_register_app(app_netdiag_get_descriptor());
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    printf("[devOS]   - Registering REST...\n");
    devos_core_register_app(app_rest_get_descriptor());
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    printf("[devOS]   - Registering Docker...\n");
    devos_core_register_app(app_docker_get_descriptor());
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    printf("[devOS]   - Registering ADS-B...\n");
    devos_core_register_app(app_adsb_get_descriptor());
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    printf("[devOS]   - Registering Cricket...\n");
    devos_core_register_app(app_cricket_get_descriptor());
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    printf("[devOS]   - Registering Authenticator...\n");
    devos_core_register_app(app_totp_get_descriptor());
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    printf("[devOS]   - Registering Coder's Toolkit...\n");
    devos_core_register_app(app_coder_get_descriptor());
    devos_core_register_app(app_jobs_get_descriptor());
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
    /* Sym + Space command palette, Sym + I info panel and Sym + S shortcut
     * sheet, over any app; the restart / shutdown confirmation (Sym+Shift+R /
     * Sym+Shift+Q) is registered first so it owns the keyboard while open */
    devos_powerdlg_init();
    devos_cmdpal_init();
    devos_hud_init();
    devos_shortcuts_init();
    devos_toast_init();                     /* notices below the top bar */
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

    /* Give LVGL 8 MB of PSRAM as ONE pool (Rule 2), so large allocations
     * (layers, big labels) can be contiguous. Needs the TLSF cap raised via
     * CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES (sdkconfig.defaults). */
    size_t lv_pool_size = 8 * 1024 * 1024;
    uint8_t *lv_psram_pool = (uint8_t *)heap_caps_malloc(lv_pool_size, MALLOC_CAP_SPIRAM);
    if (lv_psram_pool && lv_mem_add_pool(lv_psram_pool, lv_pool_size)) {
        printf("[devOS] Added %zu KB PSRAM pool to LVGL\n", lv_pool_size / 1024);
    } else {
        printf("[devOS] Warning: failed to add the 8 MB PSRAM pool to LVGL\n");
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
     * SSH keys) opens the "nvs" partition, and nvs_open() fails with
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
/* Settings > Apps restarts: close the window and run this binary again. */
static char **s_argv;

static void sim_restart(void)
{
    SDL_Quit();
    unsetenv("DEVOS_SAFE_START");       /* a power-on thing, not every restart */
    execv("/proc/self/exe", s_argv);
    perror("[devOS] restart");
    exit(1);
}

int main(int argc, char **argv)
{
    LV_UNUSED(argc);
    s_argv = argv;
    devos_core_set_restart_cb(sim_restart);

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
            jobs_sync_system();
            devos_power_poll(++sim_seconds);
            devos_top_bar_update();
            devos_toast_watch();                /* Wi-Fi / VPN / battery notices */
            app_launcher_update_telemetry();
            devos_tailnet_housekeeping();       /* peer cache -> flash, on this core */
            if (sim_seconds == 15) devos_core_apps_boot_ok();
            last_telemetry_tick = now;
        }

        SDL_Delay(16); /* ~60 FPS */
    }

    return 0;
}
#endif

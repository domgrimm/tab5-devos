#include "app_launcher.h"
#include "devos_config.h"
#include "devos_theme.h"
#include <stdio.h>

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;

/* Telemetry labels */
static lv_obj_t *telemetry_box = NULL;
static lv_obj_t *lbl_clock_date = NULL;
static lv_obj_t *lbl_net_power = NULL;
static lv_obj_t *lbl_mem_cpu = NULL;

/* 6 App Cards */
static lv_obj_t *cards[6] = {NULL};
static lv_obj_t *card_titles[6] = {NULL};
static lv_obj_t *card_subtitles[6] = {NULL};
static lv_obj_t *card_line1[6] = {NULL};
static lv_obj_t *card_line2[6] = {NULL};
static lv_obj_t *card_line3[6] = {NULL};

/* Bottom hint bar */
static lv_obj_t *bottom_bar = NULL;
static lv_obj_t *lbl_bottom_hint = NULL;

static void card_click_cb(lv_event_t *e)
{
    int app_id = (int)(intptr_t)lv_event_get_user_data(e);
    devos_core_switch_app((devos_app_id_t)app_id);
}

static void apply_theme(const devos_palette_t *p, void *user_data)
{
    LV_UNUSED(user_data);
    if (!screen) return;

    lv_obj_set_style_bg_color(screen, p->bg, 0);

    /* Telemetry strip */
    lv_obj_set_style_bg_color(telemetry_box, p->telemetry_bg, 0);
    lv_obj_set_style_border_color(telemetry_box, p->surface_border, 0);
    lv_obj_set_style_text_color(lbl_clock_date, p->accent_primary, 0);
    lv_obj_set_style_text_color(lbl_net_power, p->text_primary, 0);
    lv_obj_set_style_text_color(lbl_mem_cpu, p->text_secondary, 0);

    /* Cards */
    for (int i = 0; i < 6; i++) {
        if (cards[i]) {
            lv_obj_set_style_bg_color(cards[i], p->surface, 0);
            lv_obj_set_style_border_color(cards[i], p->surface_border, 0);
            lv_obj_set_style_text_color(card_titles[i], p->accent_primary, 0);
            lv_obj_set_style_text_color(card_subtitles[i], p->text_secondary, 0);
            lv_obj_set_style_text_color(card_line1[i], p->text_primary, 0);
            lv_obj_set_style_text_color(card_line2[i], p->text_primary, 0);
            lv_obj_set_style_text_color(card_line3[i], p->text_secondary, 0);
        }
    }

    /* Bottom Bar */
    if (bottom_bar) {
        lv_obj_set_style_bg_color(bottom_bar, p->bottom_bar_bg, 0);
        lv_obj_set_style_border_color(bottom_bar, p->surface_border, 0);
        lv_obj_set_style_text_color(lbl_bottom_hint, p->text_secondary, 0);
    }
}

void app_launcher_update_telemetry(void)
{
    if (!screen) return;

    const devos_telemetry_t *t = devos_telemetry_get();

    char buf[128];
    snprintf(buf, sizeof(buf), "%02d:%02d   %s", t->rtc_hour, t->rtc_min, t->rtc_date_str);
    lv_label_set_text(lbl_clock_date, buf);

    snprintf(buf, sizeof(buf), "Tailnet: %s (%s)  |  Battery: %.1fV (%.1fW, ~%.1fh left)  |  SD: %.1f GB Free",
             t->tailscale_ip,
             t->tailscale_online ? "Online" : "Offline",
             t->battery_voltage_mv / 1000.0f,
             t->battery_power_mw / 1000.0f,
             t->runtime_minutes_left / 60.0f,
             t->sd_free_mb / 1024.0f);
    lv_label_set_text(lbl_net_power, buf);

    snprintf(buf, sizeof(buf), "Memory: %.1f MB Free PSRAM, %d KB SRAM  |  CPU: Core 0: %d%% | Core 1: %d%%",
             t->free_psram_kb / 1024.0f,
             t->free_sram_kb,
             t->cpu_load_core0,
             t->cpu_load_core1);
    lv_label_set_text(lbl_mem_cpu, buf);

    /* Update dynamic card lines */
    snprintf(buf, sizeof(buf), "* Status: %s", t->opendev_status);
    lv_label_set_text(card_line1[0], buf);
    snprintf(buf, sizeof(buf), "* Model: %s", t->opendev_model);
    lv_label_set_text(card_line2[0], buf);

    snprintf(buf, sizeof(buf), "* %d Session (active)", t->terminal_sessions);
    lv_label_set_text(card_line1[1], buf);
    snprintf(buf, sizeof(buf), "* Host: %s", t->terminal_host);
    lv_label_set_text(card_line2[1], buf);

    snprintf(buf, sizeof(buf), "* %s", t->editor_file);
    lv_label_set_text(card_line1[2], buf);
    snprintf(buf, sizeof(buf), "* %d KB", t->editor_file_kb);
    lv_label_set_text(card_line2[2], buf);

    snprintf(buf, sizeof(buf), "* Peers: %d Online", t->tailscale_peers_online);
    lv_label_set_text(card_line1[3], buf);
    snprintf(buf, sizeof(buf), "* DERP: %s", t->tailscale_derp);
    lv_label_set_text(card_line2[3], buf);

    snprintf(buf, sizeof(buf), "* Bridge: %s", t->agy_bridge_online ? "Online (:8420)" : "Offline");
    lv_label_set_text(card_line1[4], buf);
    snprintf(buf, sizeof(buf), "* Subagents: %d Active", t->agy_subagents_count);
    lv_label_set_text(card_line2[4], buf);
}

static void launcher_init(void)
{
    const devos_palette_t *p = devos_theme_get();

    /* Screen root container */
    screen = lv_obj_create(lv_screen_active());
    app_descriptor.screen = screen;
    lv_obj_set_size(screen, DEVOS_SCREEN_WIDTH, DEVOS_SCREEN_HEIGHT);
    lv_obj_set_pos(screen, 0, 0);
    lv_obj_set_style_bg_color(screen, p->bg, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* 1. Telemetry Strip (y: 42, height: 76) */
    telemetry_box = lv_obj_create(screen);
    lv_obj_set_size(telemetry_box, DEVOS_SCREEN_WIDTH - 32, 74);
    lv_obj_set_pos(telemetry_box, 16, DEVOS_TOP_BAR_HEIGHT + 6);
    lv_obj_set_style_bg_color(telemetry_box, p->telemetry_bg, 0);
    lv_obj_set_style_border_color(telemetry_box, p->surface_border, 0);
    lv_obj_set_style_border_width(telemetry_box, 1, 0);
    lv_obj_set_style_radius(telemetry_box, 6, 0);
    lv_obj_set_style_pad_left(telemetry_box, 16, 0);
    lv_obj_set_style_pad_right(telemetry_box, 16, 0);
    lv_obj_set_style_pad_top(telemetry_box, 6, 0);
    lv_obj_set_style_pad_bottom(telemetry_box, 6, 0);
    lv_obj_clear_flag(telemetry_box, LV_OBJ_FLAG_SCROLLABLE);

    lbl_clock_date = lv_label_create(telemetry_box);
    lv_obj_set_pos(lbl_clock_date, 0, 0);
    lv_obj_set_style_text_font(lbl_clock_date, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl_clock_date, p->accent_primary, 0);

    lbl_net_power = lv_label_create(telemetry_box);
    lv_obj_set_pos(lbl_net_power, 0, 24);
    lv_obj_set_style_text_font(lbl_net_power, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_net_power, p->text_primary, 0);

    lbl_mem_cpu = lv_label_create(telemetry_box);
    lv_obj_set_pos(lbl_mem_cpu, 0, 44);
    lv_obj_set_style_text_font(lbl_mem_cpu, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_mem_cpu, p->text_secondary, 0);

    /* 2. 2x3 App Cards Grid */
    const char *titles[6] = {
        "[1] OpenDev",
        "[2] Terminal/SSH",
        "[3] Markdown",
        "[4] Tailscale",
        "[5] Antigravity",
        "[6] Settings"
    };

    const char *subtitles[6] = {
        "AI Coding Agents",
        "ANSI PTY Shell",
        "Notes & Docs",
        "Mesh Network",
        "Native AGY Client",
        "System & Config"
    };

    const char *default_line1[6] = {
        "* Status: Idle",
        "* 1 Session (bash)",
        "* welcome.md",
        "* Peers: 6 Online",
        "* Bridge: Online (:8420)",
        "* Wi-Fi & Display"
    };

    const char *default_line2[6] = {
        "* Model: Sonnet 3.7",
        "* Host: 100.77.11.92",
        "* 14.2 KB",
        "* DERP: Sydney (18ms)",
        "* Subagents: 2 Active",
        "* Battery & INA226"
    };

    const char *default_line3[6] = {
        "* REST/SSE :4096",
        "* 160x45 Cols (SIGWINCH)",
        "* Split Markdown View",
        "* IP: 100.77.11.92",
        "* /goal /plan /boost",
        "* NVS & Storage"
    };

    const int card_w = 398;
    const int card_h = 245;
    const int start_y = 126;
    const int gap_x = 22;
    const int gap_y = 18;
    const int start_x = 18;

    for (int i = 0; i < 6; i++) {
        int row = i / 3;
        int col = i % 3;
        int x = start_x + col * (card_w + gap_x);
        int y = start_y + row * (card_h + gap_y);

        cards[i] = lv_button_create(screen);
        lv_obj_set_size(cards[i], card_w, card_h);
        lv_obj_set_pos(cards[i], x, y);
        lv_obj_set_style_bg_color(cards[i], p->surface, 0);
        lv_obj_set_style_border_color(cards[i], p->surface_border, 0);
        lv_obj_set_style_border_width(cards[i], 1, 0);
        lv_obj_set_style_radius(cards[i], 8, 0);
        lv_obj_set_style_pad_all(cards[i], 16, 0);
        lv_obj_clear_flag(cards[i], LV_OBJ_FLAG_SCROLLABLE);

        /* Hover / Focused style */
        lv_obj_set_style_border_color(cards[i], p->border_highlight, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(cards[i], 2, LV_STATE_FOCUSED);

        /* Connect click handler */
        int target_app = i + 1; /* 1..6 */
        lv_obj_add_event_cb(cards[i], card_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)target_app);

        /* Card Header Title */
        card_titles[i] = lv_label_create(cards[i]);
        lv_label_set_text(card_titles[i], titles[i]);
        lv_obj_set_pos(card_titles[i], 0, 0);
        lv_obj_set_style_text_font(card_titles[i], &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(card_titles[i], p->accent_primary, 0);

        /* Subtitle */
        card_subtitles[i] = lv_label_create(cards[i]);
        lv_label_set_text(card_subtitles[i], subtitles[i]);
        lv_obj_set_pos(card_subtitles[i], 0, 26);
        lv_obj_set_style_text_font(card_subtitles[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(card_subtitles[i], p->text_secondary, 0);

        /* Divider line */
        lv_obj_t *divider = lv_obj_create(cards[i]);
        lv_obj_set_size(divider, card_w - 32, 1);
        lv_obj_set_pos(divider, 0, 52);
        lv_obj_set_style_bg_color(divider, p->surface_border, 0);
        lv_obj_set_style_border_width(divider, 0, 0);

        /* Bullets */
        card_line1[i] = lv_label_create(cards[i]);
        lv_label_set_text(card_line1[i], default_line1[i]);
        lv_obj_set_pos(card_line1[i], 4, 62);
        lv_obj_set_style_text_font(card_line1[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(card_line1[i], p->text_primary, 0);

        card_line2[i] = lv_label_create(cards[i]);
        lv_label_set_text(card_line2[i], default_line2[i]);
        lv_obj_set_pos(card_line2[i], 4, 88);
        lv_obj_set_style_text_font(card_line2[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(card_line2[i], p->text_primary, 0);

        card_line3[i] = lv_label_create(cards[i]);
        lv_label_set_text(card_line3[i], default_line3[i]);
        lv_obj_set_pos(card_line3[i], 4, 114);
        lv_obj_set_style_text_font(card_line3[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(card_line3[i], p->text_secondary, 0);
    }

    /* 3. Bottom Navigation Hint Bar */
    bottom_bar = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar, DEVOS_SCREEN_WIDTH, DEVOS_BOTTOM_BAR_HEIGHT);
    lv_obj_set_pos(bottom_bar, 0, DEVOS_SCREEN_HEIGHT - DEVOS_BOTTOM_BAR_HEIGHT);
    lv_obj_set_style_bg_color(bottom_bar, p->bottom_bar_bg, 0);
    lv_obj_set_style_border_color(bottom_bar, p->surface_border, 0);
    lv_obj_set_style_border_width(bottom_bar, 1, 0);
    lv_obj_set_style_border_side(bottom_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(bottom_bar, 0, 0);
    lv_obj_set_style_pad_all(bottom_bar, 0, 0);
    lv_obj_clear_flag(bottom_bar, LV_OBJ_FLAG_SCROLLABLE);

    lbl_bottom_hint = lv_label_create(bottom_bar);
    lv_label_set_text(lbl_bottom_hint, "[Enter/Tap] Launch  |  [1-6] Quick Key  |  [Fn+H] Home  |  [Alt+Tab] Switch  |  [Fn+T] Theme");
    lv_obj_center(lbl_bottom_hint);
    lv_obj_set_style_text_font(lbl_bottom_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_bottom_hint, p->text_secondary, 0);

    /* Hook theme updates */
    devos_theme_add_listener(apply_theme, NULL);

    /* Initial telemetry fill */
    app_launcher_update_telemetry();
}

static void launcher_show(void)
{
    app_launcher_update_telemetry();
}

static void launcher_hide(void)
{
}

devos_app_descriptor_t *app_launcher_get_descriptor(void)
{
    app_descriptor.id = DEVOS_APP_LAUNCHER;
    app_descriptor.name = "Launcher";
    app_descriptor.title = "Home Screen";
    app_descriptor.subtitle = "devOS Dashboard";
    app_descriptor.screen = screen;
    app_descriptor.init = launcher_init;
    app_descriptor.show = launcher_show;
    app_descriptor.hide = launcher_hide;
    app_descriptor.handle_key = NULL;

    return &app_descriptor;
}

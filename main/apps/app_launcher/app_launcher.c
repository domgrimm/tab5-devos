#include "app_launcher.h"
#include "devos_config.h"
#include "devos_theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAYOUT_CONFIG_FILE TAB5_SD_MOUNT_POINT "/.devos/launcher_layout.json"

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;

/* Telemetry labels */
static lv_obj_t *telemetry_box = NULL;
static lv_obj_t *lbl_clock_date = NULL;
static lv_obj_t *lbl_net_power = NULL;
static lv_obj_t *lbl_mem_cpu = NULL;

/* Arrange Mode Controls */
static lv_obj_t *btn_arrange_toggle = NULL;
static lv_obj_t *lbl_arrange_btn = NULL;
static lv_obj_t *btn_arrange_reset = NULL;
static lv_obj_t *lbl_arrange_reset = NULL;
static lv_obj_t *banner_arrange = NULL;
static lv_obj_t *lbl_arrange_banner = NULL;

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

/* Slot Mapping State (slot 0..5 -> app_id 1..6) */
static int slot_to_app[6] = {1, 2, 3, 4, 5, 6};
static bool arrange_mode = false;
static int selected_slot = -1;

typedef struct {
    int x;
    int y;
} slot_coord_t;

static const slot_coord_t slot_coords[6] = {
    {18, 126},                      /* Slot 0: Top-Left */
    {18 + 1 * (398 + 22), 126},     /* Slot 1: Top-Center */
    {18 + 2 * (398 + 22), 126},     /* Slot 2: Top-Right */
    {18, 126 + 245 + 18},           /* Slot 3: Bottom-Left */
    {18 + 1 * (398 + 22), 126 + 245 + 18}, /* Slot 4: Bottom-Center */
    {18 + 2 * (398 + 22), 126 + 245 + 18}, /* Slot 5: Bottom-Right */
};

static const char *app_names[6] = {
    "OpenDev",
    "Terminal/SSH",
    "Markdown",
    "Tailscale",
    "Antigravity",
    "Settings"
};

static const char *subtitles[6] = {
    "AI Coding Agents",
    "ANSI PTY Shell",
    "Notes & Docs",
    "Mesh Network",
    "Native AGY Client",
    "System & Config"
};

static const char *default_line1[6] = {
    "* Status: Idle",
    "* 1 Session (bash)",
    "* welcome.md",
    "* Peers: 6 Online",
    "* Bridge: Online (:8420)",
    "* Wi-Fi & Display"
};

static const char *default_line2[6] = {
    "* Model: Sonnet 3.7",
    "* Host: 100.77.11.92",
    "* 14.2 KB",
    "* DERP: Sydney (18ms)",
    "* Subagents: 2 Active",
    "* Battery & INA226"
};

static const char *default_line3[6] = {
    "* REST/SSE :4096",
    "* 160x45 Cols (SIGWINCH)",
    "* Split Markdown View",
    "* IP: 100.77.11.92",
    "* /goal /plan /boost",
    "* NVS & Storage"
};

/* --------------------------------------------------------------------------
 * Persistence: Load and Save Layout
 * -------------------------------------------------------------------------- */
static void save_layout(void)
{
    FILE *f = fopen(LAYOUT_CONFIG_FILE, "w");
    if (f) {
        fprintf(f, "[\n  %d, %d, %d,\n  %d, %d, %d\n]\n",
                slot_to_app[0], slot_to_app[1], slot_to_app[2],
                slot_to_app[3], slot_to_app[4], slot_to_app[5]);
        fclose(f);
    }
}

static void load_layout(void)
{
    FILE *f = fopen(LAYOUT_CONFIG_FILE, "r");
    if (!f) return;

    int a[6];
    int count = 0;
    int c;
    while ((c = fgetc(f)) != EOF && count < 6) {
        if (c >= '1' && c <= '6') {
            a[count++] = c - '0';
        }
    }
    fclose(f);

    if (count == 6) {
        /* Validate permutation of 1..6 */
        bool seen[7] = {false};
        bool valid = true;
        for (int i = 0; i < 6; i++) {
            if (a[i] < 1 || a[i] > 6 || seen[a[i]]) {
                valid = false;
                break;
            }
            seen[a[i]] = true;
        }
        if (valid) {
            for (int i = 0; i < 6; i++) {
                slot_to_app[i] = a[i];
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Card Positioning & Visual Refresh
 * -------------------------------------------------------------------------- */
static void refresh_card_positions(void)
{
    const devos_palette_t *p = devos_theme_get();

    for (int slot = 0; slot < 6; slot++) {
        int app_id = slot_to_app[slot];
        int card_idx = app_id - 1;
        if (card_idx >= 0 && card_idx < 6 && cards[card_idx]) {
            lv_obj_set_pos(cards[card_idx], slot_coords[slot].x, slot_coords[slot].y);

            char title_buf[64];
            if (arrange_mode) {
                if (selected_slot == slot) {
                    snprintf(title_buf, sizeof(title_buf), "[⇄ Slot %d] %s", slot + 1, app_names[card_idx]);
                } else {
                    snprintf(title_buf, sizeof(title_buf), "[Slot %d] %s", slot + 1, app_names[card_idx]);
                }
            } else {
                snprintf(title_buf, sizeof(title_buf), "[%d] %s", slot + 1, app_names[card_idx]);
            }
            lv_label_set_text(card_titles[card_idx], title_buf);

            /* Border styling */
            if (arrange_mode && selected_slot == slot) {
                lv_obj_set_style_border_color(cards[card_idx], p->accent_warning, 0);
                lv_obj_set_style_border_width(cards[card_idx], 3, 0);
            } else if (arrange_mode) {
                lv_obj_set_style_border_color(cards[card_idx], p->border_highlight, 0);
                lv_obj_set_style_border_width(cards[card_idx], 1, 0);
            } else {
                lv_obj_set_style_border_color(cards[card_idx], p->surface_border, 0);
                lv_obj_set_style_border_width(cards[card_idx], 1, 0);
            }
        }
    }

    /* Update Arrange Banner and Buttons */
    if (banner_arrange) {
        if (arrange_mode) {
            lv_obj_remove_flag(banner_arrange, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(btn_arrange_reset, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lbl_arrange_btn, "✓ Done");
            lv_obj_set_style_bg_color(btn_arrange_toggle, p->accent_secondary, 0);

            if (selected_slot >= 0) {
                char b_buf[128];
                int app_id = slot_to_app[selected_slot];
                snprintf(b_buf, sizeof(b_buf), "⇋ Slot %d (%s) selected. Tap destination tile to swap!",
                         selected_slot + 1, app_names[app_id - 1]);
                lv_label_set_text(lbl_arrange_banner, b_buf);
            } else {
                lv_label_set_text(lbl_arrange_banner,
                    "⇋ ARRANGE MODE: Tap a tile to select, then tap destination to swap | [1-6] Swap Slot | [R] Reset");
            }
        } else {
            lv_obj_add_flag(banner_arrange, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(btn_arrange_reset, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lbl_arrange_btn, "⇋ Arrange");
            lv_obj_set_style_bg_color(btn_arrange_toggle, p->surface_active, 0);
        }
    }

    /* Update Bottom Hint Bar */
    if (lbl_bottom_hint) {
        if (arrange_mode) {
            lv_label_set_text(lbl_bottom_hint,
                "[Tap/Click] Select & Swap  |  [1-6] Swap Keys  |  [R] Reset Default  |  [Esc/Done] Exit Arrange Mode");
        } else {
            lv_label_set_text(lbl_bottom_hint,
                "[Enter/Tap] Launch  |  [1-6] Quick Key  |  [Fn+H] Home  |  [Fn+E/E] Arrange Tiles  |  [Fn+T] Theme");
        }
    }
}

void app_launcher_swap_slots(int slot_a, int slot_b)
{
    if (slot_a < 0 || slot_a >= 6 || slot_b < 0 || slot_b >= 6 || slot_a == slot_b) {
        return;
    }

    int temp = slot_to_app[slot_a];
    slot_to_app[slot_a] = slot_to_app[slot_b];
    slot_to_app[slot_b] = temp;

    save_layout();
    refresh_card_positions();
}

void app_launcher_reset_layout(void)
{
    for (int i = 0; i < 6; i++) {
        slot_to_app[i] = i + 1;
    }
    selected_slot = -1;
    save_layout();
    refresh_card_positions();
}

int app_launcher_get_app_in_slot(int slot)
{
    if (slot >= 0 && slot < 6) {
        return slot_to_app[slot];
    }
    return -1;
}

void app_launcher_set_arrange_mode(bool active)
{
    if (arrange_mode != active) {
        arrange_mode = active;
        selected_slot = -1;
        refresh_card_positions();
    }
}

void app_launcher_toggle_arrange_mode(void)
{
    app_launcher_set_arrange_mode(!arrange_mode);
}

bool app_launcher_is_arrange_mode(void)
{
    return arrange_mode;
}

/* --------------------------------------------------------------------------
 * Card and Button Event Handlers
 * -------------------------------------------------------------------------- */
static void card_click_cb(lv_event_t *e)
{
    int app_id = (int)(intptr_t)lv_event_get_user_data(e);

    if (!arrange_mode) {
        /* Normal Mode: Launch the application */
        devos_core_switch_app((devos_app_id_t)app_id);
    } else {
        /* Arrange Mode: Select or Swap */
        int clicked_slot = -1;
        for (int i = 0; i < 6; i++) {
            if (slot_to_app[i] == app_id) {
                clicked_slot = i;
                break;
            }
        }

        if (clicked_slot >= 0) {
            if (selected_slot == -1) {
                /* First tile clicked: Pick up for swap */
                selected_slot = clicked_slot;
                refresh_card_positions();
            } else if (selected_slot == clicked_slot) {
                /* Same tile clicked: Deselect */
                selected_slot = -1;
                refresh_card_positions();
            } else {
                /* Second tile clicked: Swap! */
                app_launcher_swap_slots(selected_slot, clicked_slot);
                selected_slot = -1;
            }
        }
    }
}

static void arrange_toggle_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    app_launcher_toggle_arrange_mode();
}

static void arrange_reset_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    app_launcher_reset_layout();
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

    /* Arrange buttons */
    if (btn_arrange_toggle) {
        lv_obj_set_style_border_color(btn_arrange_toggle, p->surface_border, 0);
        lv_obj_set_style_text_color(lbl_arrange_btn, p->text_primary, 0);
    }
    if (btn_arrange_reset) {
        lv_obj_set_style_bg_color(btn_arrange_reset, p->surface, 0);
        lv_obj_set_style_border_color(btn_arrange_reset, p->surface_border, 0);
        lv_obj_set_style_text_color(lbl_arrange_reset, p->accent_danger, 0);
    }
    if (banner_arrange) {
        lv_obj_set_style_bg_color(banner_arrange, p->surface_active, 0);
        lv_obj_set_style_border_color(banner_arrange, p->accent_primary, 0);
        lv_obj_set_style_text_color(lbl_arrange_banner, p->accent_primary, 0);
    }

    /* Cards */
    for (int i = 0; i < 6; i++) {
        if (cards[i]) {
            lv_obj_set_style_bg_color(cards[i], p->surface, 0);
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

    refresh_card_positions();
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

static bool launcher_handle_key(uint32_t key, uint8_t modifiers)
{
    /* 1. Toggle Arrange Mode: 'e' / 'E' or Fn + E */
    if (key == 'e' || key == 'E' || ((modifiers & DEVOS_MOD_FN) && (key == 'e' || key == 'E'))) {
        app_launcher_toggle_arrange_mode();
        return true;
    }

    /* 2. In Arrange Mode */
    if (arrange_mode) {
        if (key == LV_KEY_ESC) {
            app_launcher_set_arrange_mode(false);
            return true;
        }

        if (key == 'r' || key == 'R') {
            app_launcher_reset_layout();
            return true;
        }

        if (key >= '1' && key <= '6') {
            int slot = key - '1';
            if (selected_slot == -1) {
                selected_slot = slot;
                refresh_card_positions();
            } else if (selected_slot == slot) {
                selected_slot = -1;
                refresh_card_positions();
            } else {
                app_launcher_swap_slots(selected_slot, slot);
                selected_slot = -1;
            }
            return true;
        }
    } else {
        /* 3. Normal Mode: Quick launch by current slot index */
        if (modifiers == DEVOS_MOD_NONE && key >= '1' && key <= '6') {
            int slot = key - '1';
            devos_app_id_t target = (devos_app_id_t)slot_to_app[slot];
            devos_core_switch_app(target);
            return true;
        }
    }

    return false;
}

static void launcher_init(void)
{
    const devos_palette_t *p = devos_theme_get();

    /* Load saved layout order if available */
    load_layout();

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

    /* Arrange Mode Toggle Button */
    btn_arrange_toggle = lv_button_create(telemetry_box);
    lv_obj_set_size(btn_arrange_toggle, 110, 30);
    lv_obj_align(btn_arrange_toggle, LV_ALIGN_RIGHT_MID, 0, -12);
    lv_obj_set_style_bg_color(btn_arrange_toggle, p->surface_active, 0);
    lv_obj_set_style_border_color(btn_arrange_toggle, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_arrange_toggle, 1, 0);
    lv_obj_set_style_radius(btn_arrange_toggle, 4, 0);
    lv_obj_add_event_cb(btn_arrange_toggle, arrange_toggle_cb, LV_EVENT_CLICKED, NULL);

    lbl_arrange_btn = lv_label_create(btn_arrange_toggle);
    lv_label_set_text(lbl_arrange_btn, "⇋ Arrange");
    lv_obj_center(lbl_arrange_btn);
    lv_obj_set_style_text_font(lbl_arrange_btn, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_arrange_btn, p->text_primary, 0);

    /* Arrange Reset Button (Hidden unless arrange mode active) */
    btn_arrange_reset = lv_button_create(telemetry_box);
    lv_obj_set_size(btn_arrange_reset, 110, 24);
    lv_obj_align(btn_arrange_reset, LV_ALIGN_RIGHT_MID, 0, 18);
    lv_obj_set_style_bg_color(btn_arrange_reset, p->surface, 0);
    lv_obj_set_style_border_color(btn_arrange_reset, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_arrange_reset, 1, 0);
    lv_obj_set_style_radius(btn_arrange_reset, 4, 0);
    lv_obj_add_event_cb(btn_arrange_reset, arrange_reset_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(btn_arrange_reset, LV_OBJ_FLAG_HIDDEN);

    lbl_arrange_reset = lv_label_create(btn_arrange_reset);
    lv_label_set_text(lbl_arrange_reset, "↺ Defaults");
    lv_obj_center(lbl_arrange_reset);
    lv_obj_set_style_text_font(lbl_arrange_reset, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_arrange_reset, p->accent_danger, 0);

    /* Arrange Active Banner */
    banner_arrange = lv_obj_create(screen);
    lv_obj_set_size(banner_arrange, DEVOS_SCREEN_WIDTH - 32, 28);
    lv_obj_set_pos(banner_arrange, 16, 120);
    lv_obj_set_style_bg_color(banner_arrange, p->surface_active, 0);
    lv_obj_set_style_border_color(banner_arrange, p->accent_primary, 0);
    lv_obj_set_style_border_width(banner_arrange, 1, 0);
    lv_obj_set_style_radius(banner_arrange, 4, 0);
    lv_obj_set_style_pad_all(banner_arrange, 2, 0);
    lv_obj_clear_flag(banner_arrange, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(banner_arrange, LV_OBJ_FLAG_HIDDEN);

    lbl_arrange_banner = lv_label_create(banner_arrange);
    lv_label_set_text(lbl_arrange_banner,
        "⇋ ARRANGE MODE: Tap a tile to select, then tap destination to swap | [1-6] Keys | [R] Reset");
    lv_obj_align(lbl_arrange_banner, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(lbl_arrange_banner, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_arrange_banner, p->accent_primary, 0);

    /* 2. 2x3 App Cards */
    const int card_w = 398;
    const int card_h = 245;

    for (int i = 0; i < 6; i++) {
        cards[i] = lv_button_create(screen);
        lv_obj_set_size(cards[i], card_w, card_h);
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
        int app_id = i + 1; /* 1..6 */
        lv_obj_add_event_cb(cards[i], card_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)app_id);

        /* Card Header Title */
        card_titles[i] = lv_label_create(cards[i]);
        lv_label_set_text(card_titles[i], app_names[i]);
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
    lv_label_set_text(lbl_bottom_hint,
        "[Enter/Tap] Launch  |  [1-6] Quick Key  |  [Fn+H] Home  |  [Fn+E/E] Arrange Tiles  |  [Fn+T] Theme");
    lv_obj_center(lbl_bottom_hint);
    lv_obj_set_style_text_font(lbl_bottom_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_bottom_hint, p->text_secondary, 0);

    /* Hook theme updates */
    devos_theme_add_listener(apply_theme, NULL);

    /* Set up initial positions and telemetry */
    load_layout();
    refresh_card_positions();
    app_launcher_update_telemetry();
}

static void launcher_show(void)
{
    selected_slot = -1;
    refresh_card_positions();
    app_launcher_update_telemetry();
}

static void launcher_hide(void)
{
    arrange_mode = false;
    selected_slot = -1;
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
    app_descriptor.handle_key = launcher_handle_key;

    return &app_descriptor;
}

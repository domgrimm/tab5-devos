#include "app_terminal.h"
#include "devos_config.h"
#include "devos_theme.h"
#include <stdio.h>
#include <string.h>

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;

static lv_obj_t *sidebar = NULL;
static lv_obj_t *terminal_container = NULL;
static lv_obj_t *term_header = NULL;
static lv_obj_t *lbl_term_info = NULL;
static lv_obj_t *lbl_term_cols = NULL;
static lv_obj_t *term_body = NULL;
static lv_obj_t *lbl_terminal_text = NULL;
static lv_obj_t *term_footer = NULL;
static lv_obj_t *lbl_term_footer = NULL;

static bool sidebar_visible = true;
static uint16_t current_cols = DEVOS_TERM_COLS_COLLAPSED; /* 128 */
static uint16_t current_rows = DEVOS_TERM_ROWS;           /* 45 */

static const char *DEMO_TERMINAL_OUTPUT =
"\033[1;36mLinux workstation 6.12.1-arch1-1 #1 SMP PREEMPT_DYNAMIC x86_64\033[0m\n"
"Welcome to Arch Linux (Tailscale IP: 100.77.11.92)!\n"
"System load: 0.14, 0.22, 0.18 | Memory: 8.2 GiB / 64.0 GiB | Uptime: 14d 6h\n"
"\n"
"\033[1;32mdom@workstation\033[0m:\033[1;34m~/dev/tab5-devos\033[0m$ git status\n"
"On branch main\n"
"Your branch is up to date with 'origin/main'.\n"
"\n"
"Changes to be committed:\n"
"  (use \"git restore --staged <file>...\" to unstage)\n"
"    \033[32mnew file:   components/devos_core/devos_core.c\033[0m\n"
"    \033[32mnew file:   components/devos_ui/devos_theme.c\033[0m\n"
"    \033[32mnew file:   components/devos_ui/devos_agent_viewport.c\033[0m\n"
"    \033[32mnew file:   main/apps/app_launcher/app_launcher.c\033[0m\n"
"    \033[32mnew file:   main/apps/app_terminal/app_terminal.c\033[0m\n"
"\n"
"\033[1;32mdom@workstation\033[0m:\033[1;34m~/dev/tab5-devos\033[0m$ agy --version\n"
"\033[1;35mAntigravity CLI v2.4.0 (Autonomous Agent Engine)\033[0m\n"
"Connected to agy-bridge daemon on \033[1;33m100.77.11.92:8420\033[0m [OK]\n"
"\n"
"\033[1;32mdom@workstation\033[0m:\033[1;34m~/dev/tab5-devos\033[0m$ ";

void app_terminal_resize_pty(uint16_t cols, uint16_t rows)
{
    current_cols = cols;
    current_rows = rows;

    if (lbl_term_cols) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%dx%d Cols", cols, rows);
        lv_label_set_text(lbl_term_cols, buf);
    }
}

void app_terminal_toggle_sidebar(void)
{
    sidebar_visible = !sidebar_visible;

    if (sidebar_visible) {
        lv_obj_remove_flag(sidebar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(terminal_container, DEVOS_PANE_LEFT_WIDTH, 0);
        lv_obj_set_size(terminal_container, DEVOS_SCREEN_WIDTH - DEVOS_PANE_LEFT_WIDTH, DEVOS_CONTENT_HEIGHT);
        app_terminal_resize_pty(DEVOS_TERM_COLS_COLLAPSED, DEVOS_TERM_ROWS);
    } else {
        lv_obj_add_flag(sidebar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(terminal_container, 0, 0);
        lv_obj_set_size(terminal_container, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
        app_terminal_resize_pty(DEVOS_TERM_COLS_EXPANDED, DEVOS_TERM_ROWS);
    }
}

static void toggle_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    app_terminal_toggle_sidebar();
}

static void apply_theme(const devos_palette_t *p, void *user_data)
{
    LV_UNUSED(user_data);
    if (!screen) return;

    lv_obj_set_style_bg_color(screen, p->bg, 0);

    /* Sidebar */
    lv_obj_set_style_bg_color(sidebar, p->surface, 0);
    lv_obj_set_style_border_color(sidebar, p->surface_border, 0);

    /* Terminal container */
    lv_obj_set_style_bg_color(term_header, p->top_bar_bg, 0);
    lv_obj_set_style_border_color(term_header, p->surface_border, 0);
    lv_obj_set_style_text_color(lbl_term_info, p->accent_primary, 0);
    lv_obj_set_style_text_color(lbl_term_cols, p->text_secondary, 0);

    lv_obj_set_style_bg_color(term_body, p->code_bg, 0);
    lv_obj_set_style_text_color(lbl_terminal_text, p->text_primary, 0);

    lv_obj_set_style_bg_color(term_footer, p->bottom_bar_bg, 0);
    lv_obj_set_style_border_color(term_footer, p->surface_border, 0);
    lv_obj_set_style_text_color(lbl_term_footer, p->text_secondary, 0);
}

static bool terminal_handle_key(uint32_t key, uint8_t modifiers)
{
    if ((modifiers & DEVOS_MOD_FN) && (key == '[')) {
        app_terminal_toggle_sidebar();
        return true;
    }
    return false;
}

static void terminal_init(void)
{
    const devos_palette_t *p = devos_theme_get();

    /* Screen root */
    screen = lv_obj_create(lv_screen_active());
    app_descriptor.screen = screen;
    lv_obj_set_size(screen, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(screen, 0, DEVOS_TOP_BAR_HEIGHT);
    lv_obj_set_style_bg_color(screen, p->bg, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);

    /* 1. Left Collapsible Sidebar (260px) */
    sidebar = lv_obj_create(screen);
    lv_obj_set_size(sidebar, DEVOS_PANE_LEFT_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(sidebar, 0, 0);
    lv_obj_set_style_bg_color(sidebar, p->surface, 0);
    lv_obj_set_style_border_color(sidebar, p->surface_border, 0);
    lv_obj_set_style_border_width(sidebar, 1, 0);
    lv_obj_set_style_border_side(sidebar, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_radius(sidebar, 0, 0);
    lv_obj_set_style_pad_all(sidebar, 10, 0);

    /* Sidebar Title */
    lv_obj_t *lbl_side_title = lv_label_create(sidebar);
    lv_label_set_text(lbl_side_title, "CONNECTIONS (Fn+[)");
    lv_obj_set_pos(lbl_side_title, 4, 4);
    lv_obj_set_style_text_font(lbl_side_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_side_title, p->text_secondary, 0);

    /* Active Sessions List */
    lv_obj_t *lbl_sec1 = lv_label_create(sidebar);
    lv_label_set_text(lbl_sec1, "ACTIVE SESSIONS");
    lv_obj_set_pos(lbl_sec1, 4, 30);
    lv_obj_set_style_text_font(lbl_sec1, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_sec1, p->accent_primary, 0);

    lv_obj_t *btn_sess1 = lv_button_create(sidebar);
    lv_obj_set_size(btn_sess1, DEVOS_PANE_LEFT_WIDTH - 28, 36);
    lv_obj_set_pos(btn_sess1, 4, 52);
    lv_obj_set_style_bg_color(btn_sess1, p->surface_active, 0);
    lv_obj_set_style_border_color(btn_sess1, p->accent_primary, 0);
    lv_obj_set_style_border_width(btn_sess1, 1, 0);
    lv_obj_set_style_radius(btn_sess1, 4, 0);

    lv_obj_t *lbl_sess1 = lv_label_create(btn_sess1);
    lv_label_set_text(lbl_sess1, LV_SYMBOL_BULLET " 1: workstation (bash)");
    lv_obj_align(lbl_sess1, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_text_color(lbl_sess1, p->accent_primary, 0);
    lv_obj_set_style_text_font(lbl_sess1, &lv_font_montserrat_12, 0);

    lv_obj_t *btn_sess2 = lv_button_create(sidebar);
    lv_obj_set_size(btn_sess2, DEVOS_PANE_LEFT_WIDTH - 28, 36);
    lv_obj_set_pos(btn_sess2, 4, 94);
    lv_obj_set_style_bg_color(btn_sess2, p->surface, 0);
    lv_obj_set_style_border_color(btn_sess2, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_sess2, 1, 0);
    lv_obj_set_style_radius(btn_sess2, 4, 0);

    lv_obj_t *lbl_sess2 = lv_label_create(btn_sess2);
    lv_label_set_text(lbl_sess2, "- 2: prod-vps (htop)");
    lv_obj_align(lbl_sess2, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_text_color(lbl_sess2, p->text_secondary, 0);
    lv_obj_set_style_text_font(lbl_sess2, &lv_font_montserrat_12, 0);

    /* Saved Bookmarks */
    lv_obj_t *lbl_sec2 = lv_label_create(sidebar);
    lv_label_set_text(lbl_sec2, "SAVED BOOKMARKS");
    lv_obj_set_pos(lbl_sec2, 4, 150);
    lv_obj_set_style_text_font(lbl_sec2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_sec2, p->accent_primary, 0);

    const char *bm_names[3] = {"Workstation (100.77.11.92)", "Dev Cluster (100.64.1.2)", "Home NAS (100.80.3.15)"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn_bm = lv_button_create(sidebar);
        lv_obj_set_size(btn_bm, DEVOS_PANE_LEFT_WIDTH - 28, 34);
        lv_obj_set_pos(btn_bm, 4, 172 + i * 40);
        lv_obj_set_style_bg_color(btn_bm, p->surface, 0);
        lv_obj_set_style_border_color(btn_bm, p->surface_border, 0);
        lv_obj_set_style_border_width(btn_bm, 1, 0);
        lv_obj_set_style_radius(btn_bm, 4, 0);

        lv_obj_t *lbl_bm = lv_label_create(btn_bm);
        lv_label_set_text(lbl_bm, bm_names[i]);
        lv_obj_align(lbl_bm, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_set_style_text_color(lbl_bm, p->text_primary, 0);
        lv_obj_set_style_text_font(lbl_bm, &lv_font_montserrat_12, 0);
    }

    /* 2. Main Terminal Container */
    terminal_container = lv_obj_create(screen);
    lv_obj_set_size(terminal_container, DEVOS_SCREEN_WIDTH - DEVOS_PANE_LEFT_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(terminal_container, DEVOS_PANE_LEFT_WIDTH, 0);
    lv_obj_set_style_bg_color(terminal_container, p->bg, 0);
    lv_obj_set_style_radius(terminal_container, 0, 0);
    lv_obj_set_style_border_width(terminal_container, 0, 0);
    lv_obj_set_style_pad_all(terminal_container, 0, 0);
    lv_obj_clear_flag(terminal_container, LV_OBJ_FLAG_SCROLLABLE);

    /* Terminal Header Bar */
    term_header = lv_obj_create(terminal_container);
    lv_obj_set_size(term_header, lv_pct(100), 32);
    lv_obj_set_pos(term_header, 0, 0);
    lv_obj_set_style_bg_color(term_header, p->top_bar_bg, 0);
    lv_obj_set_style_border_color(term_header, p->surface_border, 0);
    lv_obj_set_style_border_width(term_header, 1, 0);
    lv_obj_set_style_border_side(term_header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(term_header, 0, 0);
    lv_obj_set_style_pad_left(term_header, 10, 0);
    lv_obj_set_style_pad_right(term_header, 10, 0);
    lv_obj_clear_flag(term_header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_toggle = lv_button_create(term_header);
    lv_obj_set_size(btn_toggle, 96, 24);
    lv_obj_align(btn_toggle, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_toggle, p->surface, 0);
    lv_obj_set_style_border_color(btn_toggle, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_toggle, 1, 0);
    lv_obj_set_style_radius(btn_toggle, 3, 0);
    lv_obj_add_event_cb(btn_toggle, toggle_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_toggle = lv_label_create(btn_toggle);
    lv_label_set_text(lbl_toggle, LV_SYMBOL_BARS " Sidebar");
    lv_obj_center(lbl_toggle);
    lv_obj_set_style_text_color(lbl_toggle, p->text_primary, 0);
    lv_obj_set_style_text_font(lbl_toggle, &lv_font_montserrat_12, 0);

    lbl_term_info = lv_label_create(term_header);
    lv_label_set_text(lbl_term_info, "SSH: workstation (100.77.11.92:22) - libssh2 PTY");
    lv_obj_align(lbl_term_info, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(lbl_term_info, p->accent_primary, 0);
    lv_obj_set_style_text_font(lbl_term_info, &lv_font_montserrat_12, 0);

    lbl_term_cols = lv_label_create(term_header);
    lv_label_set_text(lbl_term_cols, "128x45 Cols");
    lv_obj_align(lbl_term_cols, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_color(lbl_term_cols, p->text_secondary, 0);
    lv_obj_set_style_text_font(lbl_term_cols, &lv_font_montserrat_12, 0);

    /* Terminal Body Canvas */
    term_body = lv_obj_create(terminal_container);
    lv_obj_set_size(term_body, lv_pct(100), DEVOS_CONTENT_HEIGHT - 32 - 28);
    lv_obj_set_pos(term_body, 0, 32);
    lv_obj_set_style_bg_color(term_body, p->code_bg, 0);
    lv_obj_set_style_radius(term_body, 0, 0);
    lv_obj_set_style_border_width(term_body, 0, 0);
    lv_obj_set_style_pad_all(term_body, 12, 0);

    lbl_terminal_text = lv_label_create(term_body);
    lv_label_set_text(lbl_terminal_text, DEMO_TERMINAL_OUTPUT);
    lv_obj_set_style_text_color(lbl_terminal_text, p->text_primary, 0);
    lv_obj_set_style_text_font(lbl_terminal_text, &lv_font_montserrat_14, 0);

    /* Terminal Footer Status */
    term_footer = lv_obj_create(terminal_container);
    lv_obj_set_size(term_footer, lv_pct(100), 28);
    lv_obj_set_pos(term_footer, 0, DEVOS_CONTENT_HEIGHT - 28);
    lv_obj_set_style_bg_color(term_footer, p->bottom_bar_bg, 0);
    lv_obj_set_style_border_color(term_footer, p->surface_border, 0);
    lv_obj_set_style_border_width(term_footer, 1, 0);
    lv_obj_set_style_border_side(term_footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(term_footer, 0, 0);
    lv_obj_set_style_pad_left(term_footer, 10, 0);
    lv_obj_clear_flag(term_footer, LV_OBJ_FLAG_SCROLLABLE);

    lbl_term_footer = lv_label_create(term_footer);
    lv_label_set_text(lbl_term_footer, "Connected | Latency: 2ms | PTY: TIOCSWINSZ OK | Alt+1..9 Switch | Fn+[ Toggle Sidebar");
    lv_obj_align(lbl_term_footer, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(lbl_term_footer, p->text_secondary, 0);
    lv_obj_set_style_text_font(lbl_term_footer, &lv_font_montserrat_12, 0);

    /* Register theme listener */
    devos_theme_add_listener(apply_theme, NULL);
}

static void terminal_show(void)
{
}

static void terminal_hide(void)
{
}

devos_app_descriptor_t *app_terminal_get_descriptor(void)
{
    app_descriptor.id = DEVOS_APP_TERMINAL;
    app_descriptor.name = "Terminal";
    app_descriptor.title = "Terminal / SSH";
    app_descriptor.subtitle = "Multi-Session ANSI PTY Shell";
    app_descriptor.screen = screen;
    app_descriptor.init = terminal_init;
    app_descriptor.show = terminal_show;
    app_descriptor.hide = terminal_hide;
    app_descriptor.handle_key = terminal_handle_key;

    return &app_descriptor;
}

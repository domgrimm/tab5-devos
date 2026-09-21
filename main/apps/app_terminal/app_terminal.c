#include "app_terminal.h"
#include "libssh2_port.h"
#include "devos_config.h"
#include "devos_theme.h"
#include "devos_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TERM_BUFFER_MAX 16384
#define TERM_MAX_BOOKMARKS 8

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;

/* Left Sidebar (260px) */
static lv_obj_t *sidebar = NULL;
static lv_obj_t *btn_tab_active = NULL;
static lv_obj_t *lbl_tab_active = NULL;
static lv_obj_t *btn_tab_bookmarks = NULL;
static lv_obj_t *lbl_tab_bookmarks = NULL;
static lv_obj_t *container_active = NULL;
static lv_obj_t *container_bookmarks = NULL;

static lv_obj_t *sess_cards[SSH_MAX_SESSIONS] = {NULL};
static lv_obj_t *sess_labels[SSH_MAX_SESSIONS] = {NULL};
static lv_obj_t *sess_sub_labels[SSH_MAX_SESSIONS] = {NULL};
static lv_obj_t *sess_close_btns[SSH_MAX_SESSIONS] = {NULL};

static lv_obj_t *bm_cards[TERM_MAX_BOOKMARKS] = {NULL};
static lv_obj_t *bm_labels[TERM_MAX_BOOKMARKS] = {NULL};
static lv_obj_t *bm_sub_labels[TERM_MAX_BOOKMARKS] = {NULL};
static lv_obj_t *bm_conn_btns[TERM_MAX_BOOKMARKS] = {NULL};

/* Modals */
static lv_obj_t *modal_connect = NULL;
static lv_obj_t *ta_host = NULL;
static lv_obj_t *ta_user = NULL;
static lv_obj_t *ta_port = NULL;
static lv_obj_t *s_focused_ta = NULL;

/* Main Terminal Container */
static lv_obj_t *terminal_container = NULL;
static lv_obj_t *term_header = NULL;
static lv_obj_t *btn_toggle_sidebar = NULL;
static lv_obj_t *lbl_toggle_sidebar = NULL;
static lv_obj_t *lbl_term_info = NULL;
static lv_obj_t *lbl_term_latency = NULL;
static lv_obj_t *lbl_term_cols = NULL;

static lv_obj_t *term_body = NULL;
static lv_obj_t *lbl_terminal_text = NULL;

static lv_obj_t *term_footer = NULL;
static lv_obj_t *lbl_term_footer = NULL;

static lv_timer_t *term_poll_timer = NULL;

/* State */
static bool sidebar_visible = true;
static int s_active_session_id = 1;
static int s_sidebar_tab = 0; /* 0: Active, 1: Bookmarks */
static uint16_t current_cols = DEVOS_TERM_COLS_COLLAPSED; /* 128 */
static uint16_t current_rows = DEVOS_TERM_ROWS;           /* 45 */

/* Screen text buffer per session */
static char s_term_buffers[SSH_MAX_SESSIONS][TERM_BUFFER_MAX];
static size_t s_term_lens[SSH_MAX_SESSIONS];

/* Forward declarations */
static void refresh_sidebar(void);
static void refresh_header(void);
static void apply_theme(const devos_palette_t *p, void *user_data);
static void app_terminal_switch_session(int session_id);
static void append_to_screen_buffer(int sess_idx, const char *raw_data, size_t len);

/* --------------------------------------------------------------------------
 * ANSI VT100 Parser & Terminal Buffer
 * -------------------------------------------------------------------------- */
static void append_to_screen_buffer(int sess_idx, const char *raw_data, size_t len)
{
    if (sess_idx < 0 || sess_idx >= SSH_MAX_SESSIONS || !raw_data || len == 0) return;

    char *dest = s_term_buffers[sess_idx];
    size_t *dlen = &s_term_lens[sess_idx];

    for (size_t i = 0; i < len; i++) {
        char ch = raw_data[i];

        /* ANSI Escape Sequence Handling */
        if (ch == '\033' && i + 1 < len && raw_data[i + 1] == '[') {
            i += 2;
            /* Parse until terminating letter */
            while (i < len && !(raw_data[i] >= 'A' && raw_data[i] <= 'Z') &&
                   !(raw_data[i] >= 'a' && raw_data[i] <= 'z') && raw_data[i] != '~') {
                i++;
            }
            if (i < len) {
                char term_cmd = raw_data[i];
                if (term_cmd == 'J' || term_cmd == 'H') {
                    /* Clear screen or Home */
                    *dlen = 0;
                    dest[0] = '\0';
                }
            }
            continue;
        }

        if (ch == '\r') {
            /* Carriage return */
            continue;
        }

        if (ch == '\b') {
            /* Backspace: remove last character if not at start of line */
            if (*dlen > 0 && dest[*dlen - 1] != '\n') {
                (*dlen)--;
                dest[*dlen] = '\0';
            }
            continue;
        }

        /* Check buffer capacity, scroll if full (drop top 20%) */
        if (*dlen >= TERM_BUFFER_MAX - 128) {
            size_t drop = TERM_BUFFER_MAX / 5;
            char *nl = strchr(dest + drop, '\n');
            if (nl) {
                size_t offset = nl - dest + 1;
                memmove(dest, nl + 1, *dlen - offset);
                *dlen -= offset;
                dest[*dlen] = '\0';
            } else {
                *dlen = 0;
                dest[0] = '\0';
            }
        }

        /* Append character */
        dest[(*dlen)++] = ch;
        dest[*dlen] = '\0';
    }
}

/* --------------------------------------------------------------------------
 * Callbacks & Navigation
 * -------------------------------------------------------------------------- */
void app_terminal_resize_pty(uint16_t cols, uint16_t rows)
{
    current_cols = cols;
    current_rows = rows;

    if (lbl_term_cols) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%dx%d Cols", cols, rows);
        lv_label_set_text(lbl_term_cols, buf);
    }

    ssh_port_resize_pty(s_active_session_id, cols, rows);
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

static void toggle_sidebar_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    app_terminal_toggle_sidebar();
}

static void tab_active_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_sidebar_tab = 0;
    refresh_sidebar();
}

static void tab_bookmarks_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_sidebar_tab = 1;
    refresh_sidebar();
}

static void session_card_cb(lv_event_t *e)
{
    int sess_id = (int)(intptr_t)lv_event_get_user_data(e);
    app_terminal_switch_session(sess_id);
}

static void session_close_cb(lv_event_t *e)
{
    int sess_id = (int)(intptr_t)lv_event_get_user_data(e);
    ssh_port_close_session(sess_id);

    /* If closed active session, pick another active session */
    if (sess_id == s_active_session_id) {
        int active_ids[SSH_MAX_SESSIONS];
        int count = ssh_port_get_active_sessions(active_ids, SSH_MAX_SESSIONS);
        if (count > 0) {
            app_terminal_switch_session(active_ids[0]);
        } else {
            s_active_session_id = 1;
            refresh_header();
        }
    }
    refresh_sidebar();
}

static void bookmark_connect_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ssh_bookmark_t bms[TERM_MAX_BOOKMARKS];
    int count = 0;
    if (ssh_port_load_bookmarks(bms, TERM_MAX_BOOKMARKS, &count) == 0 && idx < count) {
        int new_id = ssh_port_create_session(bms[idx].alias, bms[idx].host, bms[idx].port,
                                             bms[idx].user, bms[idx].auth_type,
                                             bms[idx].key_path, current_cols, current_rows);
        if (new_id > 0) {
            s_sidebar_tab = 0; /* Switch to active tab */
            app_terminal_switch_session(new_id);
        }
    }
}

static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target(e);
    s_focused_ta = target;
    const devos_palette_t *p = devos_theme_get();

    if (ta_host) {
        lv_obj_clear_state(ta_host, LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(ta_host, p->surface_border, 0);
    }
    if (ta_port) {
        lv_obj_clear_state(ta_port, LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(ta_port, p->surface_border, 0);
    }
    if (ta_user) {
        lv_obj_clear_state(ta_user, LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(ta_user, p->surface_border, 0);
    }

    if (target) {
        lv_obj_add_state(target, LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(target, p->accent_primary, 0);
    }
}

static void open_connect_modal_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (modal_connect) {
        lv_obj_remove_flag(modal_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(modal_connect);

        s_focused_ta = ta_host;
        const devos_palette_t *p = devos_theme_get();
        if (ta_port) {
            lv_obj_clear_state(ta_port, LV_STATE_FOCUSED);
            lv_obj_set_style_border_color(ta_port, p->surface_border, 0);
        }
        if (ta_user) {
            lv_obj_clear_state(ta_user, LV_STATE_FOCUSED);
            lv_obj_set_style_border_color(ta_user, p->surface_border, 0);
        }
        if (ta_host) {
            lv_obj_add_state(ta_host, LV_STATE_FOCUSED);
            lv_obj_set_style_border_color(ta_host, p->accent_primary, 0);
        }
    }
}

static void close_connect_modal_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (modal_connect) {
        lv_obj_add_flag(modal_connect, LV_OBJ_FLAG_HIDDEN);
        s_focused_ta = NULL;
    }
}

static void connect_modal_submit_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!ta_host || !ta_user || !ta_port) return;

    const char *host = lv_textarea_get_text(ta_host);
    const char *user = lv_textarea_get_text(ta_user);
    const char *port_s = lv_textarea_get_text(ta_port);
    int port = atoi(port_s);
    if (port <= 0) port = 22;
    if (!user || strlen(user) == 0) user = "root";

    if (host && strlen(host) > 0) {
        int new_id = ssh_port_create_session(host, host, port, user, SSH_AUTH_KEY, NULL, current_cols, current_rows);
        if (new_id > 0) {
            s_sidebar_tab = 0;
            app_terminal_switch_session(new_id);
        }
    }

    if (modal_connect) {
        lv_obj_add_flag(modal_connect, LV_OBJ_FLAG_HIDDEN);
        s_focused_ta = NULL;
    }
}

static void app_terminal_switch_session(int session_id)
{
    if (session_id < 1 || session_id > SSH_MAX_SESSIONS) return;
    s_active_session_id = session_id;

    /* Notify PTY size */
    ssh_port_resize_pty(s_active_session_id, current_cols, current_rows);

    /* Update screen display with buffered text */
    if (lbl_terminal_text) {
        lv_label_set_text(lbl_terminal_text, s_term_buffers[s_active_session_id - 1]);
    }

    ssh_session_t *sess = ssh_port_get_session(s_active_session_id);
    if (sess) {
        int active_ids[SSH_MAX_SESSIONS];
        int count = ssh_port_get_active_sessions(active_ids, SSH_MAX_SESSIONS);

        devos_telemetry_t t;
        memcpy(&t, devos_telemetry_get(), sizeof(devos_telemetry_t));
        t.terminal_sessions = (uint8_t)count;
        snprintf(t.terminal_host, sizeof(t.terminal_host), "%s (%s)", sess->alias, sess->command);
        devos_telemetry_update(&t);
    }

    refresh_sidebar();
    refresh_header();
}

/* --------------------------------------------------------------------------
 * Timer & Realtime Polling
 * -------------------------------------------------------------------------- */
static void terminal_poll_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (!screen || lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN)) return;

    char buf[1024];
    int bytes = ssh_port_recv(s_active_session_id, buf, sizeof(buf) - 1);
    if (bytes > 0) {
        buf[bytes] = '\0';
        append_to_screen_buffer(s_active_session_id - 1, buf, (size_t)bytes);

        if (lbl_terminal_text) {
            lv_label_set_text(lbl_terminal_text, s_term_buffers[s_active_session_id - 1]);
        }
        if (term_body) {
            lv_obj_scroll_to_y(term_body, LV_COORD_MAX, LV_ANIM_OFF);
        }
    }
}

/* --------------------------------------------------------------------------
 * UI Refresh Helpers
 * -------------------------------------------------------------------------- */
static void refresh_header(void)
{
    if (!screen) return;
    ssh_session_t *sess = ssh_port_get_session(s_active_session_id);
    const devos_palette_t *p = devos_theme_get();

    if (sess && sess->state == SSH_SESSION_CONNECTED) {
        char buf[128];
        snprintf(buf, sizeof(buf), "SSH: %s (%s:%d) - %s",
                 sess->alias, sess->host, sess->port, sess->command);
        lv_label_set_text(lbl_term_info, buf);

        snprintf(buf, sizeof(buf), "%dms", sess->ping_ms);
        lv_label_set_text(lbl_term_latency, buf);
        lv_obj_set_style_text_color(lbl_term_latency, p->accent_secondary, 0);
    } else {
        lv_label_set_text(lbl_term_info, "SSH: Disconnected");
        lv_label_set_text(lbl_term_latency, "--");
        lv_obj_set_style_text_color(lbl_term_latency, p->accent_danger, 0);
    }

    char col_buf[32];
    snprintf(col_buf, sizeof(col_buf), "%dx%d Cols", current_cols, current_rows);
    lv_label_set_text(lbl_term_cols, col_buf);
}

static void refresh_sidebar(void)
{
    if (!screen) return;
    const devos_palette_t *p = devos_theme_get();

    if (s_sidebar_tab == 0) {
        /* Active Sessions Tab */
        lv_obj_remove_flag(container_active, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(container_bookmarks, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_style_bg_color(btn_tab_active, p->surface_active, 0);
        lv_obj_set_style_text_color(lbl_tab_active, p->accent_primary, 0);
        lv_obj_set_style_bg_color(btn_tab_bookmarks, p->surface, 0);
        lv_obj_set_style_text_color(lbl_tab_bookmarks, p->text_secondary, 0);

        int active_count = 0;
        for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
            ssh_session_t *sess = ssh_port_get_session(i + 1);
            if (sess && sess->state == SSH_SESSION_CONNECTED) {
                active_count++;
                lv_obj_remove_flag(sess_cards[i], LV_OBJ_FLAG_HIDDEN);

                char title[64];
                snprintf(title, sizeof(title), "%d: %s (%s)", sess->id, sess->alias, sess->command);
                lv_label_set_text(sess_labels[i], title);

                char sub[64];
                snprintf(sub, sizeof(sub), "%s | %dms", sess->host, sess->ping_ms);
                lv_label_set_text(sess_sub_labels[i], sub);

                if (sess->id == s_active_session_id) {
                    lv_obj_set_style_border_color(sess_cards[i], p->accent_primary, 0);
                    lv_obj_set_style_bg_color(sess_cards[i], p->surface_active, 0);
                    lv_obj_set_style_text_color(sess_labels[i], p->accent_primary, 0);
                } else {
                    lv_obj_set_style_border_color(sess_cards[i], p->surface_border, 0);
                    lv_obj_set_style_bg_color(sess_cards[i], p->surface, 0);
                    lv_obj_set_style_text_color(sess_labels[i], p->text_primary, 0);
                }
            } else {
                lv_obj_add_flag(sess_cards[i], LV_OBJ_FLAG_HIDDEN);
            }
        }

        char tab_title[32];
        snprintf(tab_title, sizeof(tab_title), "Active (%d)", active_count);
        lv_label_set_text(lbl_tab_active, tab_title);

    } else {
        /* Bookmarks Tab */
        lv_obj_add_flag(container_active, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(container_bookmarks, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_style_bg_color(btn_tab_active, p->surface, 0);
        lv_obj_set_style_text_color(lbl_tab_active, p->text_secondary, 0);
        lv_obj_set_style_bg_color(btn_tab_bookmarks, p->surface_active, 0);
        lv_obj_set_style_text_color(lbl_tab_bookmarks, p->accent_primary, 0);

        ssh_bookmark_t bms[TERM_MAX_BOOKMARKS];
        int bm_count = 0;
        ssh_port_load_bookmarks(bms, TERM_MAX_BOOKMARKS, &bm_count);

        for (int i = 0; i < TERM_MAX_BOOKMARKS; i++) {
            if (i < bm_count) {
                lv_obj_remove_flag(bm_cards[i], LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(bm_labels[i], bms[i].alias);

                char sub[64];
                snprintf(sub, sizeof(sub), "%s@%s:%d (%s)",
                         bms[i].user, bms[i].host, bms[i].port,
                         bms[i].auth_type == SSH_AUTH_KEY ? "key" : "pwd");
                lv_label_set_text(bm_sub_labels[i], sub);
            } else {
                lv_obj_add_flag(bm_cards[i], LV_OBJ_FLAG_HIDDEN);
            }
        }

        char tab_title[32];
        snprintf(tab_title, sizeof(tab_title), "Bookmarks (%d)", bm_count);
        lv_label_set_text(lbl_tab_bookmarks, tab_title);
    }
}

static void apply_theme(const devos_palette_t *p, void *user_data)
{
    LV_UNUSED(user_data);
    if (!screen) return;

    lv_obj_set_style_bg_color(screen, p->bg, 0);

    /* Sidebar */
    lv_obj_set_style_bg_color(sidebar, p->surface, 0);
    lv_obj_set_style_border_color(sidebar, p->surface_border, 0);

    /* Header */
    lv_obj_set_style_bg_color(term_header, p->top_bar_bg, 0);
    lv_obj_set_style_border_color(term_header, p->surface_border, 0);
    lv_obj_set_style_text_color(lbl_term_info, p->accent_primary, 0);
    lv_obj_set_style_text_color(lbl_term_cols, p->text_secondary, 0);

    /* Body */
    lv_obj_set_style_bg_color(term_body, p->code_bg, 0);
    lv_obj_set_style_text_color(lbl_terminal_text, p->text_primary, 0);

    /* Footer */
    lv_obj_set_style_bg_color(term_footer, p->bottom_bar_bg, 0);
    lv_obj_set_style_border_color(term_footer, p->surface_border, 0);
    lv_obj_set_style_text_color(lbl_term_footer, p->text_secondary, 0);

    refresh_sidebar();
    refresh_header();
}

/* --------------------------------------------------------------------------
 * Key Handling
 * -------------------------------------------------------------------------- */
static bool terminal_handle_key(uint32_t key, uint8_t modifiers)
{
    /* 0. If Quick Connect modal is open, capture ALL keystrokes for text inputs */
    if (modal_connect && !lv_obj_has_flag(modal_connect, LV_OBJ_FLAG_HIDDEN)) {
        if (!s_focused_ta) {
            s_focused_ta = ta_host;
        }

        /* Escape closes modal */
        if (key == LV_KEY_ESC) {
            close_connect_modal_cb(NULL);
            return true;
        }

        /* Tab cycles between text fields */
        if (key == '\t') {
            if (s_focused_ta == ta_host) {
                s_focused_ta = ta_port;
            } else if (s_focused_ta == ta_port) {
                s_focused_ta = ta_user;
            } else {
                s_focused_ta = ta_host;
            }
            const devos_palette_t *p = devos_theme_get();
            if (ta_host) {
                lv_obj_clear_state(ta_host, LV_STATE_FOCUSED);
                lv_obj_set_style_border_color(ta_host, p->surface_border, 0);
            }
            if (ta_port) {
                lv_obj_clear_state(ta_port, LV_STATE_FOCUSED);
                lv_obj_set_style_border_color(ta_port, p->surface_border, 0);
            }
            if (ta_user) {
                lv_obj_clear_state(ta_user, LV_STATE_FOCUSED);
                lv_obj_set_style_border_color(ta_user, p->surface_border, 0);
            }
            if (s_focused_ta) {
                lv_obj_add_state(s_focused_ta, LV_STATE_FOCUSED);
                lv_obj_set_style_border_color(s_focused_ta, p->accent_primary, 0);
            }
            return true;
        }

        /* Enter connects / submits */
        if (key == '\r' || key == '\n') {
            connect_modal_submit_cb(NULL);
            return true;
        }

        /* Backspace */
        if (key == '\b' || key == 0x7F) {
            if (s_focused_ta) {
                lv_textarea_delete_char(s_focused_ta);
            }
            return true;
        }

        /* Arrow navigation */
        if (key == LV_KEY_LEFT) {
            if (s_focused_ta) lv_textarea_cursor_left(s_focused_ta);
            return true;
        }
        if (key == LV_KEY_RIGHT) {
            if (s_focused_ta) lv_textarea_cursor_right(s_focused_ta);
            return true;
        }

        /* Printable characters */
        if (key >= 32 && key <= 126) {
            if (s_focused_ta) {
                lv_textarea_add_char(s_focused_ta, (char)key);
            }
            return true;
        }

        /* Absorb all other keys so terminal behind never receives them */
        return true;
    }

    /* 1. Toggle Sidebar: Fn + [ */
    if ((modifiers & DEVOS_MOD_FN) && (key == '[')) {
        app_terminal_toggle_sidebar();
        return true;
    }

    /* 2. Switch Sessions: Alt + 1..9 */
    if ((modifiers & DEVOS_MOD_ALT) && (key >= '1' && key <= '9')) {
        int sess_id = key - '0';
        app_terminal_switch_session(sess_id);
        return true;
    }

    /* 3. VT100 Arrow Keys */
    if (key == LV_KEY_UP) {
        ssh_port_send(s_active_session_id, "\033[A", 3);
        return true;
    }
    if (key == LV_KEY_DOWN) {
        ssh_port_send(s_active_session_id, "\033[B", 3);
        return true;
    }
    if (key == LV_KEY_RIGHT) {
        ssh_port_send(s_active_session_id, "\033[C", 3);
        return true;
    }
    if (key == LV_KEY_LEFT) {
        ssh_port_send(s_active_session_id, "\033[D", 3);
        return true;
    }

    /* 4. Control Sequences */
    if (key == 0x03 || key == 0x04 || key == 0x1A || key == 0x0C) {
        char ch = (char)key;
        ssh_port_send(s_active_session_id, &ch, 1);
        return true;
    }

    /* 5. Enter, Backspace, Tab */
    if (key == '\r' || key == '\n') {
        ssh_port_send(s_active_session_id, "\r", 1);
        return true;
    }
    if (key == '\b' || key == 0x7F) {
        ssh_port_send(s_active_session_id, "\b", 1);
        return true;
    }
    if (key == '\t') {
        ssh_port_send(s_active_session_id, "\t", 1);
        return true;
    }

    /* 6. Printable ASCII characters */
    if (key >= 32 && key <= 126) {
        char ch = (char)key;
        ssh_port_send(s_active_session_id, &ch, 1);
        return true;
    }

    return false;
}

/* --------------------------------------------------------------------------
 * Initialization & Layout
 * -------------------------------------------------------------------------- */
static void terminal_init(void)
{
    const devos_palette_t *p = devos_theme_get();

    memset(s_term_buffers, 0, sizeof(s_term_buffers));
    memset(s_term_lens, 0, sizeof(s_term_lens));

    /* Pre-fill initial banner from libssh2_port for sessions 1 and 2 */
    for (int i = 0; i < 2; i++) {
        char initial_chunk[2048];
        int n = ssh_port_recv(i + 1, initial_chunk, sizeof(initial_chunk) - 1);
        if (n > 0) {
            initial_chunk[n] = '\0';
            append_to_screen_buffer(i, initial_chunk, (size_t)n);
        }
    }

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
    lv_obj_set_style_pad_all(sidebar, 8, 0);
    lv_obj_clear_flag(sidebar, LV_OBJ_FLAG_SCROLLABLE);

    /* Top Tab Buttons in Sidebar */
    btn_tab_active = lv_button_create(sidebar);
    lv_obj_set_size(btn_tab_active, 118, 28);
    lv_obj_set_pos(btn_tab_active, 0, 0);
    lv_obj_set_style_radius(btn_tab_active, 4, 0);
    lv_obj_add_event_cb(btn_tab_active, tab_active_cb, LV_EVENT_CLICKED, NULL);

    lbl_tab_active = lv_label_create(btn_tab_active);
    lv_label_set_text(lbl_tab_active, "Active (2)");
    lv_obj_center(lbl_tab_active);
    lv_obj_set_style_text_font(lbl_tab_active, &lv_font_montserrat_12, 0);

    btn_tab_bookmarks = lv_button_create(sidebar);
    lv_obj_set_size(btn_tab_bookmarks, 118, 28);
    lv_obj_set_pos(btn_tab_bookmarks, 126, 0);
    lv_obj_set_style_radius(btn_tab_bookmarks, 4, 0);
    lv_obj_add_event_cb(btn_tab_bookmarks, tab_bookmarks_cb, LV_EVENT_CLICKED, NULL);

    lbl_tab_bookmarks = lv_label_create(btn_tab_bookmarks);
    lv_label_set_text(lbl_tab_bookmarks, "Bookmarks (2)");
    lv_obj_center(lbl_tab_bookmarks);
    lv_obj_set_style_text_font(lbl_tab_bookmarks, &lv_font_montserrat_12, 0);

    /* 1.A Active Sessions Container */
    container_active = lv_obj_create(sidebar);
    lv_obj_set_size(container_active, DEVOS_PANE_LEFT_WIDTH - 16, DEVOS_CONTENT_HEIGHT - 80);
    lv_obj_set_pos(container_active, 0, 36);
    lv_obj_set_style_bg_color(container_active, p->surface, 0);
    lv_obj_set_style_border_width(container_active, 0, 0);
    lv_obj_set_style_pad_all(container_active, 0, 0);

    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        sess_cards[i] = lv_button_create(container_active);
        lv_obj_set_size(sess_cards[i], DEVOS_PANE_LEFT_WIDTH - 20, 48);
        lv_obj_set_pos(sess_cards[i], 0, i * 54);
        lv_obj_set_style_radius(sess_cards[i], 4, 0);
        lv_obj_set_style_border_width(sess_cards[i], 1, 0);
        lv_obj_set_style_pad_all(sess_cards[i], 6, 0);
        lv_obj_add_event_cb(sess_cards[i], session_card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));

        sess_labels[i] = lv_label_create(sess_cards[i]);
        lv_obj_set_pos(sess_labels[i], 2, 2);
        lv_obj_set_style_text_font(sess_labels[i], &lv_font_montserrat_12, 0);

        sess_sub_labels[i] = lv_label_create(sess_cards[i]);
        lv_obj_set_pos(sess_sub_labels[i], 2, 22);
        lv_obj_set_style_text_font(sess_sub_labels[i], &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(sess_sub_labels[i], p->text_secondary, 0);

        sess_close_btns[i] = lv_button_create(sess_cards[i]);
        lv_obj_set_size(sess_close_btns[i], 24, 24);
        lv_obj_align(sess_close_btns[i], LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(sess_close_btns[i], p->surface, 0);
        lv_obj_set_style_border_width(sess_close_btns[i], 0, 0);
        lv_obj_set_style_radius(sess_close_btns[i], 3, 0);
        lv_obj_add_event_cb(sess_close_btns[i], session_close_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));

        lv_obj_t *lbl_x = lv_label_create(sess_close_btns[i]);
        lv_label_set_text(lbl_x, "x");
        lv_obj_center(lbl_x);
        lv_obj_set_style_text_color(lbl_x, p->accent_danger, 0);
    }

    /* Add Session Button at bottom of active list */
    lv_obj_t *btn_add_sess = lv_button_create(sidebar);
    lv_obj_set_size(btn_add_sess, DEVOS_PANE_LEFT_WIDTH - 16, 32);
    lv_obj_set_pos(btn_add_sess, 0, DEVOS_CONTENT_HEIGHT - 44);
    lv_obj_set_style_bg_color(btn_add_sess, p->surface, 0);
    lv_obj_set_style_border_color(btn_add_sess, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_add_sess, 1, 0);
    lv_obj_set_style_radius(btn_add_sess, 4, 0);
    lv_obj_add_event_cb(btn_add_sess, open_connect_modal_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_add_sess = lv_label_create(btn_add_sess);
    lv_label_set_text(lbl_add_sess, LV_SYMBOL_PLUS " Quick Connect / New Session");
    lv_obj_center(lbl_add_sess);
    lv_obj_set_style_text_font(lbl_add_sess, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_add_sess, p->accent_primary, 0);

    /* 1.B Bookmarks Container */
    container_bookmarks = lv_obj_create(sidebar);
    lv_obj_set_size(container_bookmarks, DEVOS_PANE_LEFT_WIDTH - 16, DEVOS_CONTENT_HEIGHT - 48);
    lv_obj_set_pos(container_bookmarks, 0, 36);
    lv_obj_set_style_bg_color(container_bookmarks, p->surface, 0);
    lv_obj_set_style_border_width(container_bookmarks, 0, 0);
    lv_obj_set_style_pad_all(container_bookmarks, 0, 0);
    lv_obj_add_flag(container_bookmarks, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < TERM_MAX_BOOKMARKS; i++) {
        bm_cards[i] = lv_obj_create(container_bookmarks);
        lv_obj_set_size(bm_cards[i], DEVOS_PANE_LEFT_WIDTH - 20, 56);
        lv_obj_set_pos(bm_cards[i], 0, i * 62);
        lv_obj_set_style_bg_color(bm_cards[i], p->surface, 0);
        lv_obj_set_style_border_color(bm_cards[i], p->surface_border, 0);
        lv_obj_set_style_border_width(bm_cards[i], 1, 0);
        lv_obj_set_style_radius(bm_cards[i], 4, 0);
        lv_obj_set_style_pad_all(bm_cards[i], 6, 0);
        lv_obj_clear_flag(bm_cards[i], LV_OBJ_FLAG_SCROLLABLE);

        bm_labels[i] = lv_label_create(bm_cards[i]);
        lv_obj_set_pos(bm_labels[i], 2, 2);
        lv_obj_set_style_text_font(bm_labels[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(bm_labels[i], p->accent_primary, 0);

        bm_sub_labels[i] = lv_label_create(bm_cards[i]);
        lv_obj_set_pos(bm_sub_labels[i], 2, 24);
        lv_obj_set_style_text_font(bm_sub_labels[i], &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(bm_sub_labels[i], p->text_secondary, 0);

        bm_conn_btns[i] = lv_button_create(bm_cards[i]);
        lv_obj_set_size(bm_conn_btns[i], 58, 24);
        lv_obj_align(bm_conn_btns[i], LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(bm_conn_btns[i], p->accent_primary, 0);
        lv_obj_set_style_radius(bm_conn_btns[i], 3, 0);
        lv_obj_add_event_cb(bm_conn_btns[i], bookmark_connect_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl_c = lv_label_create(bm_conn_btns[i]);
        lv_label_set_text(lbl_c, "Open");
        lv_obj_center(lbl_c);
        lv_obj_set_style_text_color(lbl_c, lv_color_black(), 0);
        lv_obj_set_style_text_font(lbl_c, &lv_font_montserrat_10, 0);
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

    /* Header Bar */
    term_header = lv_obj_create(terminal_container);
    lv_obj_set_size(term_header, lv_pct(100), 34);
    lv_obj_set_pos(term_header, 0, 0);
    lv_obj_set_style_bg_color(term_header, p->top_bar_bg, 0);
    lv_obj_set_style_border_color(term_header, p->surface_border, 0);
    lv_obj_set_style_border_width(term_header, 1, 0);
    lv_obj_set_style_border_side(term_header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(term_header, 0, 0);
    lv_obj_set_style_pad_left(term_header, 10, 0);
    lv_obj_set_style_pad_right(term_header, 10, 0);
    lv_obj_clear_flag(term_header, LV_OBJ_FLAG_SCROLLABLE);

    btn_toggle_sidebar = lv_button_create(term_header);
    lv_obj_set_size(btn_toggle_sidebar, 110, 24);
    lv_obj_align(btn_toggle_sidebar, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_toggle_sidebar, p->surface, 0);
    lv_obj_set_style_border_color(btn_toggle_sidebar, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_toggle_sidebar, 1, 0);
    lv_obj_set_style_radius(btn_toggle_sidebar, 3, 0);
    lv_obj_add_event_cb(btn_toggle_sidebar, toggle_sidebar_cb, LV_EVENT_CLICKED, NULL);

    lbl_toggle_sidebar = lv_label_create(btn_toggle_sidebar);
    lv_label_set_text(lbl_toggle_sidebar, LV_SYMBOL_BARS " Sidebar");
    lv_obj_center(lbl_toggle_sidebar);
    lv_obj_set_style_text_color(lbl_toggle_sidebar, p->text_primary, 0);
    lv_obj_set_style_text_font(lbl_toggle_sidebar, &lv_font_montserrat_12, 0);

    lbl_term_info = lv_label_create(term_header);
    lv_label_set_text(lbl_term_info, "SSH: workstation (100.77.11.92:22) - bash");
    lv_obj_align(lbl_term_info, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(lbl_term_info, p->accent_primary, 0);
    lv_obj_set_style_text_font(lbl_term_info, &lv_font_montserrat_12, 0);

    lbl_term_latency = lv_label_create(term_header);
    lv_label_set_text(lbl_term_latency, "2ms");
    lv_obj_align(lbl_term_latency, LV_ALIGN_RIGHT_MID, -110, 0);
    lv_obj_set_style_text_font(lbl_term_latency, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_term_latency, p->accent_secondary, 0);

    lbl_term_cols = lv_label_create(term_header);
    lv_label_set_text(lbl_term_cols, "128x45 Cols");
    lv_obj_align(lbl_term_cols, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_color(lbl_term_cols, p->text_secondary, 0);
    lv_obj_set_style_text_font(lbl_term_cols, &lv_font_montserrat_12, 0);

    /* Terminal Body Canvas */
    term_body = lv_obj_create(terminal_container);
    lv_obj_set_size(term_body, lv_pct(100), DEVOS_CONTENT_HEIGHT - 34 - 28);
    lv_obj_set_pos(term_body, 0, 34);
    lv_obj_set_style_bg_color(term_body, p->code_bg, 0);
    lv_obj_set_style_radius(term_body, 0, 0);
    lv_obj_set_style_border_width(term_body, 0, 0);
    lv_obj_set_style_pad_all(term_body, 12, 0);

    lbl_terminal_text = lv_label_create(term_body);
    lv_label_set_text(lbl_terminal_text, s_term_buffers[0]);
    lv_obj_set_style_text_color(lbl_terminal_text, p->text_primary, 0);
    lv_obj_set_style_text_font(lbl_terminal_text, &lv_font_montserrat_14, 0);

    /* Footer Status */
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
    lv_label_set_text(lbl_term_footer,
                      "Connected | PTY: TIOCSWINSZ OK | Alt+1..9 Switch | Fn+[ Toggle Sidebar | Ctrl+C Interrupt");
    lv_obj_align(lbl_term_footer, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(lbl_term_footer, p->text_secondary, 0);
    lv_obj_set_style_text_font(lbl_term_footer, &lv_font_montserrat_12, 0);

    /* 3. Quick Connect Modal (Redesigned: 540x240, spacious layout) */
    modal_connect = lv_obj_create(screen);
    lv_obj_set_size(modal_connect, 540, 240);
    lv_obj_center(modal_connect);
    lv_obj_set_style_bg_color(modal_connect, p->surface, 0);
    lv_obj_set_style_border_color(modal_connect, p->accent_primary, 0);
    lv_obj_set_style_border_width(modal_connect, 2, 0);
    lv_obj_set_style_radius(modal_connect, 8, 0);
    lv_obj_set_style_pad_all(modal_connect, 18, 0);
    lv_obj_clear_flag(modal_connect, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal_connect, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *lbl_mtitle = lv_label_create(modal_connect);
    lv_label_set_text(lbl_mtitle, LV_SYMBOL_SETTINGS " Quick Connect New SSH Session");
    lv_obj_set_pos(lbl_mtitle, 0, 0);
    lv_obj_set_style_text_font(lbl_mtitle, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_mtitle, p->accent_primary, 0);

    /* Row 1: Target Host / IP and Port */
    lv_obj_t *lbl_l_host = lv_label_create(modal_connect);
    lv_label_set_text(lbl_l_host, "Target Host or Tailscale IP:");
    lv_obj_set_pos(lbl_l_host, 0, 28);
    lv_obj_set_style_text_font(lbl_l_host, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_l_host, p->text_secondary, 0);

    ta_host = lv_textarea_create(modal_connect);
    lv_obj_set_size(ta_host, 380, 36);
    lv_obj_set_pos(ta_host, 0, 48);
    lv_textarea_set_placeholder_text(ta_host, "100.x.y.z or hostname");
    lv_textarea_set_one_line(ta_host, true);
    lv_obj_set_style_bg_color(ta_host, p->code_bg, 0);
    lv_obj_set_style_text_color(ta_host, p->text_primary, 0);
    lv_obj_set_style_border_color(ta_host, p->accent_primary, 0);
    lv_obj_set_style_border_width(ta_host, 1, 0);
    lv_obj_set_style_radius(ta_host, 4, 0);
    lv_obj_set_style_pad_all(ta_host, 8, 0);
    lv_obj_add_event_cb(ta_host, ta_focus_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_l_port = lv_label_create(modal_connect);
    lv_label_set_text(lbl_l_port, "Port:");
    lv_obj_set_pos(lbl_l_port, 396, 28);
    lv_obj_set_style_text_font(lbl_l_port, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_l_port, p->text_secondary, 0);

    ta_port = lv_textarea_create(modal_connect);
    lv_obj_set_size(ta_port, 108, 36);
    lv_obj_set_pos(ta_port, 396, 48);
    lv_textarea_set_text(ta_port, "22");
    lv_textarea_set_one_line(ta_port, true);
    lv_obj_set_style_bg_color(ta_port, p->code_bg, 0);
    lv_obj_set_style_text_color(ta_port, p->text_primary, 0);
    lv_obj_set_style_border_color(ta_port, p->surface_border, 0);
    lv_obj_set_style_border_width(ta_port, 1, 0);
    lv_obj_set_style_radius(ta_port, 4, 0);
    lv_obj_set_style_pad_all(ta_port, 8, 0);
    lv_obj_add_event_cb(ta_port, ta_focus_cb, LV_EVENT_CLICKED, NULL);

    /* Row 2: Username & Authentication */
    lv_obj_t *lbl_l_user = lv_label_create(modal_connect);
    lv_label_set_text(lbl_l_user, "Username:");
    lv_obj_set_pos(lbl_l_user, 0, 94);
    lv_obj_set_style_text_font(lbl_l_user, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_l_user, p->text_secondary, 0);

    ta_user = lv_textarea_create(modal_connect);
    lv_obj_set_size(ta_user, 240, 36);
    lv_obj_set_pos(ta_user, 0, 114);
    lv_textarea_set_text(ta_user, "root");
    lv_textarea_set_placeholder_text(ta_user, "root");
    lv_textarea_set_one_line(ta_user, true);
    lv_obj_set_style_bg_color(ta_user, p->code_bg, 0);
    lv_obj_set_style_text_color(ta_user, p->text_primary, 0);
    lv_obj_set_style_border_color(ta_user, p->surface_border, 0);
    lv_obj_set_style_border_width(ta_user, 1, 0);
    lv_obj_set_style_radius(ta_user, 4, 0);
    lv_obj_set_style_pad_all(ta_user, 8, 0);
    lv_obj_add_event_cb(ta_user, ta_focus_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_l_auth = lv_label_create(modal_connect);
    lv_label_set_text(lbl_l_auth, "Auth Credentials:");
    lv_obj_set_pos(lbl_l_auth, 258, 94);
    lv_obj_set_style_text_font(lbl_l_auth, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_l_auth, p->text_secondary, 0);

    lv_obj_t *lbl_auth_desc = lv_label_create(modal_connect);
    lv_label_set_text(lbl_auth_desc, LV_SYMBOL_OK " Key (/sdcard/.ssh/) & Pwd");
    lv_obj_set_pos(lbl_auth_desc, 258, 122);
    lv_obj_set_style_text_font(lbl_auth_desc, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_auth_desc, p->accent_secondary, 0);

    /* Row 3: Action Buttons (Right-aligned) */
    lv_obj_t *btn_conn_cancel = lv_button_create(modal_connect);
    lv_obj_set_size(btn_conn_cancel, 100, 34);
    lv_obj_set_pos(btn_conn_cancel, 276, 168);
    lv_obj_set_style_bg_color(btn_conn_cancel, p->surface, 0);
    lv_obj_set_style_border_color(btn_conn_cancel, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_conn_cancel, 1, 0);
    lv_obj_set_style_radius(btn_conn_cancel, 4, 0);
    lv_obj_add_event_cb(btn_conn_cancel, close_connect_modal_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_can = lv_label_create(btn_conn_cancel);
    lv_label_set_text(lbl_can, "Cancel");
    lv_obj_center(lbl_can);
    lv_obj_set_style_text_color(lbl_can, p->text_primary, 0);

    lv_obj_t *btn_conn_sub = lv_button_create(modal_connect);
    lv_obj_set_size(btn_conn_sub, 120, 34);
    lv_obj_set_pos(btn_conn_sub, 384, 168);
    lv_obj_set_style_bg_color(btn_conn_sub, p->accent_primary, 0);
    lv_obj_set_style_radius(btn_conn_sub, 4, 0);
    lv_obj_add_event_cb(btn_conn_sub, connect_modal_submit_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_sub = lv_label_create(btn_conn_sub);
    lv_label_set_text(lbl_sub, LV_SYMBOL_OK " Connect");
    lv_obj_center(lbl_sub);
    lv_obj_set_style_text_color(lbl_sub, lv_color_black(), 0);

    /* 4. Timer for polling terminal I/O */
    term_poll_timer = lv_timer_create(terminal_poll_cb, 30, NULL);

    /* 5. Theme and initial display */
    devos_theme_add_listener(apply_theme, NULL);
    refresh_sidebar();
    refresh_header();
}

static void terminal_show(void)
{
    /* Check if Tailscale peer or another app requested an SSH session to a specific host */
    const devos_telemetry_t *t = devos_telemetry_get();
    if (t && t->terminal_requested_host[0] != '\0') {
        char target_host[64];
        strncpy(target_host, t->terminal_requested_host, sizeof(target_host) - 1);
        target_host[sizeof(target_host) - 1] = '\0';

        /* Clear requested target in telemetry */
        devos_telemetry_t updated;
        memcpy(&updated, t, sizeof(devos_telemetry_t));
        updated.terminal_requested_host[0] = '\0';
        devos_telemetry_update(&updated);

        /* Check if existing session matches this host */
        int found_sess = -1;
        for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
            ssh_session_t *sess = ssh_port_get_session(i + 1);
            if (sess && sess->state == SSH_SESSION_CONNECTED && strcmp(sess->host, target_host) == 0) {
                found_sess = sess->id;
                break;
            }
        }

        if (found_sess > 0) {
            app_terminal_switch_session(found_sess);
        } else {
            /* Create new session */
            int new_id = ssh_port_create_session(target_host, target_host, 22, "dom",
                                                 SSH_AUTH_KEY, NULL, current_cols, current_rows);
            if (new_id > 0) {
                s_sidebar_tab = 0;
                app_terminal_switch_session(new_id);
            }
        }
    }

    refresh_sidebar();
    refresh_header();
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

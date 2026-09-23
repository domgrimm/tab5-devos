#include "app_tailscale.h"
#include "microlink.h"
#include "devos_net.h"
#include "devos_config.h"
#include "devos_theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;

/* Status Card Objects */
static lv_obj_t *status_card = NULL;
static lv_obj_t *lbl_title = NULL;
static lv_obj_t *lbl_details = NULL;
static lv_obj_t *lbl_keys = NULL;
static lv_obj_t *btn_toggle = NULL;
static lv_obj_t *lbl_toggle = NULL;
static lv_obj_t *btn_ping_derp = NULL;
static lv_obj_t *lbl_ping_derp = NULL;
static lv_obj_t *btn_auth_key = NULL;
static lv_obj_t *lbl_auth_key = NULL;

/* Section Header */
static lv_obj_t *lbl_peers_h = NULL;
static lv_obj_t *btn_refresh = NULL;
static lv_obj_t *lbl_refresh = NULL;

/* Scrollable Peer List */
static lv_obj_t *peer_list_scroll = NULL;
static lv_obj_t *peer_cards[MICROLINK_MAX_PEERS] = {NULL};
static lv_obj_t *peer_title_lbls[MICROLINK_MAX_PEERS] = {NULL};
static lv_obj_t *peer_sub_lbls[MICROLINK_MAX_PEERS] = {NULL};
static lv_obj_t *peer_tray[MICROLINK_MAX_PEERS] = {NULL};

/* Auth Modal */
static lv_obj_t *modal_auth = NULL;
static lv_obj_t *ta_auth_key = NULL;
static lv_obj_t *lbl_m_title = NULL;
static lv_obj_t *lbl_m_desc = NULL;
static lv_obj_t *btn_enroll = NULL;
static lv_obj_t *lbl_enroll = NULL;
static lv_obj_t *btn_cancel = NULL;
static lv_obj_t *lbl_cancel = NULL;

/* Peer tray buttons (per-peer, for theme updates) */
static lv_obj_t *peer_ssh_btns[MICROLINK_MAX_PEERS] = {NULL};
static lv_obj_t *peer_ssh_lbls[MICROLINK_MAX_PEERS] = {NULL};
static lv_obj_t *peer_ping_btns[MICROLINK_MAX_PEERS] = {NULL};
static lv_obj_t *peer_ping_lbls[MICROLINK_MAX_PEERS] = {NULL};

static int s_selected_peer = -1;
static bool s_filter_direct_only = false;

/* Forward declarations */
static void refresh_ui(void);
static void apply_theme(const devos_palette_t *p, void *user_data);

/* --------------------------------------------------------------------------
 * Callbacks
 * -------------------------------------------------------------------------- */
static void toggle_conn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (microlink_get_state() == MICROLINK_STATE_CONNECTED) {
        microlink_disconnect();
    } else {
        microlink_connect();
    }
    refresh_ui();
}

static void ping_derp_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int latency = 0;
    if (microlink_ping_derp(&latency) == 0) {
        refresh_ui();
    }
}

static void refresh_peers_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    microlink_refresh_peers();
    refresh_ui();
}

static void peer_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_selected_peer == idx) {
        s_selected_peer = -1; /* Toggle off */
    } else {
        s_selected_peer = idx;
    }
    refresh_ui();
}

static void open_peer_ssh(int idx)
{
    microlink_status_t st;
    if (microlink_get_status(&st) == 0 && idx >= 0 && idx < st.peer_count) {
        /* Update telemetry with selected host and launch Terminal */
        devos_telemetry_t t;
        memcpy(&t, devos_telemetry_get(), sizeof(t));
        snprintf(t.terminal_requested_host, sizeof(t.terminal_requested_host), "%s", st.peers[idx].ip);
        devos_telemetry_update(&t);

        devos_core_switch_app(DEVOS_APP_TERMINAL);
    }
}

static void peer_ssh_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    open_peer_ssh(idx);
}

static void peer_ping_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int lat = 0;
    microlink_ping_peer(idx, &lat);
    refresh_ui();
}

static void open_auth_modal_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (modal_auth) {
        char key[MICROLINK_MAX_KEY_LEN] = {0};
        microlink_get_auth_key(key, sizeof(key));
        if (ta_auth_key) {
            lv_textarea_set_text(ta_auth_key, key);
            lv_obj_add_state(ta_auth_key, LV_STATE_FOCUSED);
            lv_obj_set_style_border_color(ta_auth_key,
                devos_theme_get()->accent_primary, 0);
        }
        lv_obj_remove_flag(modal_auth, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(modal_auth);
    }
}

static void close_auth_modal_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (modal_auth) {
        lv_obj_add_flag(modal_auth, LV_OBJ_FLAG_HIDDEN);
    }
}

static void enroll_auth_key_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (ta_auth_key) {
        const char *text = lv_textarea_get_text(ta_auth_key);
        if (text && strlen(text) > 0) {
            microlink_set_auth_key(text);
            microlink_connect();
        }
    }
    if (modal_auth) {
        lv_obj_add_flag(modal_auth, LV_OBJ_FLAG_HIDDEN);
    }
    refresh_ui();
}

static void on_microlink_state_change(microlink_state_t new_state, void *user_data)
{
    LV_UNUSED(new_state);
    LV_UNUSED(user_data);
    refresh_ui();
}

/* --------------------------------------------------------------------------
 * UI Refresh
 * -------------------------------------------------------------------------- */
static void refresh_ui(void)
{
    if (!screen) return;

    const devos_palette_t *p = devos_theme_get();
    microlink_status_t st;
    if (microlink_get_status(&st) != 0) return;

    /* 1. Header Card State */
    char buf[128];
    if (st.state == MICROLINK_STATE_CONNECTED) {
        lv_label_set_text(lbl_title, LV_SYMBOL_BULLET " Tailscale WireGuard Mesh: CONNECTED");
        lv_obj_set_style_text_color(lbl_title, p->accent_secondary, 0);
        lv_label_set_text(lbl_toggle, "Disconnect");
        lv_obj_set_style_bg_color(btn_toggle, p->surface, 0);
        lv_obj_set_style_border_color(btn_toggle, p->accent_danger, 0);
        lv_obj_set_style_text_color(lbl_toggle, p->accent_danger, 0);
    } else if (st.state == MICROLINK_STATE_CONNECTING) {
        lv_label_set_text(lbl_title, LV_SYMBOL_BULLET " Tailscale WireGuard Mesh: CONNECTING...");
        lv_obj_set_style_text_color(lbl_title, p->accent_warning, 0);
        lv_label_set_text(lbl_toggle, "Cancel");
        lv_obj_set_style_bg_color(btn_toggle, p->surface, 0);
        lv_obj_set_style_border_color(btn_toggle, p->accent_warning, 0);
        lv_obj_set_style_text_color(lbl_toggle, p->accent_warning, 0);
    } else {
        lv_label_set_text(lbl_title, "- Tailscale WireGuard Mesh: DISCONNECTED");
        lv_obj_set_style_text_color(lbl_title, p->text_muted, 0);
        lv_label_set_text(lbl_toggle, "Connect");
        lv_obj_set_style_bg_color(btn_toggle, p->surface_active, 0);
        lv_obj_set_style_border_color(btn_toggle, p->accent_secondary, 0);
        lv_obj_set_style_text_color(lbl_toggle, p->accent_secondary, 0);
    }

    snprintf(buf, sizeof(buf), "Node IP: %s  |  Relay: %s %dms  |  MagicDNS: %s",
             st.state == MICROLINK_STATE_CONNECTED ? st.assigned_ip : "None",
             st.derp_relay_name, st.derp_ping_ms, st.tailnet_domain);
    lv_label_set_text(lbl_details, buf);

    snprintf(buf, sizeof(buf), "Auth: Persistent Private Key (NVS)  |  MTU: %d  |  Rx: %.1f MB | Tx: %.1f MB",
             st.mtu, st.total_rx_bytes / 1048576.0f, st.total_tx_bytes / 1048576.0f);
    lv_label_set_text(lbl_keys, buf);

    /* 2. Peers Header */
    int online_count = 0;
    for (int i = 0; i < st.peer_count; i++) {
        if (st.peers[i].is_online) online_count++;
    }
    snprintf(buf, sizeof(buf), "ACTIVE TAILNET PEERS (%d Online)", online_count);
    lv_label_set_text(lbl_peers_h, buf);

    /* 3. Peer Cards */
    int card_y = 0;
    for (int i = 0; i < MICROLINK_MAX_PEERS; i++) {
        if (!peer_cards[i]) continue;

        if (i >= st.peer_count) {
            lv_obj_add_flag(peer_cards[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        lv_obj_remove_flag(peer_cards[i], LV_OBJ_FLAG_HIDDEN);
        microlink_peer_t *peer = &st.peers[i];

        /* Title Line */
        snprintf(buf, sizeof(buf), "%s [%d] %s (%s) - %s",
                 peer->is_online ? LV_SYMBOL_BULLET : "-",
                 i + 1,
                 peer->name,
                 peer->ip,
                 peer->os_desc);
        lv_label_set_text(peer_title_lbls[i], buf);
        lv_obj_set_style_text_color(peer_title_lbls[i],
                                    peer->is_online ? p->text_primary : p->text_muted, 0);

        /* Subtitle Line */
        char sub_buf[128];
        snprintf(sub_buf, sizeof(sub_buf), "%s (%dms)  |  Rx: %u KB, Tx: %u KB  |  %s",
                 peer->is_direct ? "Direct P2P" : "DERP Relay",
                 peer->ping_ms,
                 (unsigned int)(peer->rx_bytes / 1024),
                 (unsigned int)(peer->tx_bytes / 1024),
                 peer->fqdn);
        lv_label_set_text(peer_sub_lbls[i], sub_buf);
        lv_obj_set_style_text_color(peer_sub_lbls[i],
                                    peer->is_direct ? p->accent_secondary : p->accent_warning, 0);

        /* Card positioning and selection expansion */
        bool is_sel = (s_selected_peer == i);
        int h = is_sel ? 96 : 56;
        lv_obj_set_size(peer_cards[i], DEVOS_SCREEN_WIDTH - 36, h);
        lv_obj_set_pos(peer_cards[i], 0, card_y);
        card_y += h + 8;

        if (is_sel) {
            lv_obj_set_style_border_color(peer_cards[i], p->accent_warning, 0);
            lv_obj_set_style_border_width(peer_cards[i], 2, 0);
            lv_obj_set_style_bg_color(peer_cards[i], p->surface_active, 0);
            if (peer_tray[i]) {
                lv_obj_remove_flag(peer_tray[i], LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_set_style_border_color(peer_cards[i], p->surface_border, 0);
            lv_obj_set_style_border_width(peer_cards[i], 1, 0);
            lv_obj_set_style_bg_color(peer_cards[i], p->surface, 0);
            if (peer_tray[i]) {
                lv_obj_add_flag(peer_tray[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * App Initialization
 * -------------------------------------------------------------------------- */
static void tailscale_init(void)
{
    const devos_palette_t *p = devos_theme_get();

    /* Initialize MicroLink Engine */
    microlink_init(NULL);
    microlink_add_state_listener(on_microlink_state_change, NULL);

    screen = lv_obj_create(lv_screen_active());
    app_descriptor.screen = screen;
    lv_obj_set_size(screen, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(screen, 0, DEVOS_TOP_BAR_HEIGHT);
    lv_obj_set_style_bg_color(screen, p->bg, 0);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 16, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);

    /* 1. Status Card (y: 0, height: 96) */
    status_card = lv_obj_create(screen);
    lv_obj_set_size(status_card, DEVOS_SCREEN_WIDTH - 32, 96);
    lv_obj_set_pos(status_card, 0, 0);
    lv_obj_set_style_bg_color(status_card, p->surface, 0);
    lv_obj_set_style_border_color(status_card, p->surface_border, 0);
    lv_obj_set_style_border_width(status_card, 1, 0);
    lv_obj_set_style_radius(status_card, 6, 0);
    lv_obj_set_style_pad_all(status_card, 12, 0);
    lv_obj_clear_flag(status_card, LV_OBJ_FLAG_SCROLLABLE);

    lbl_title = lv_label_create(status_card);
    lv_label_set_text(lbl_title, LV_SYMBOL_BULLET " Tailscale WireGuard Mesh: CONNECTED");
    lv_obj_set_pos(lbl_title, 0, 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_title, p->accent_secondary, 0);

    lbl_details = lv_label_create(status_card);
    lv_obj_set_pos(lbl_details, 0, 26);
    lv_obj_set_style_text_font(lbl_details, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_details, p->text_primary, 0);

    lbl_keys = lv_label_create(status_card);
    lv_obj_set_pos(lbl_keys, 0, 48);
    lv_obj_set_style_text_font(lbl_keys, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_keys, p->text_secondary, 0);

    /* Action Buttons inside Status Card */
    btn_toggle = lv_button_create(status_card);
    lv_obj_set_size(btn_toggle, 105, 30);
    lv_obj_align(btn_toggle, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_radius(btn_toggle, 4, 0);
    lv_obj_set_style_border_width(btn_toggle, 1, 0);
    lv_obj_add_event_cb(btn_toggle, toggle_conn_cb, LV_EVENT_CLICKED, NULL);

    lbl_toggle = lv_label_create(btn_toggle);
    lv_label_set_text(lbl_toggle, "Disconnect");
    lv_obj_center(lbl_toggle);
    lv_obj_set_style_text_font(lbl_toggle, &lv_font_montserrat_12, 0);

    btn_ping_derp = lv_button_create(status_card);
    lv_obj_set_size(btn_ping_derp, 110, 30);
    lv_obj_align_to(btn_ping_derp, btn_toggle, LV_ALIGN_OUT_LEFT_MID, -10, 0);
    lv_obj_set_style_bg_color(btn_ping_derp, p->surface, 0);
    lv_obj_set_style_border_color(btn_ping_derp, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_ping_derp, 1, 0);
    lv_obj_set_style_radius(btn_ping_derp, 4, 0);
    lv_obj_add_event_cb(btn_ping_derp, ping_derp_cb, LV_EVENT_CLICKED, NULL);

    lbl_ping_derp = lv_label_create(btn_ping_derp);
    lv_label_set_text(lbl_ping_derp, LV_SYMBOL_REFRESH " Ping DERP");
    lv_obj_center(lbl_ping_derp);
    lv_obj_set_style_text_font(lbl_ping_derp, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_ping_derp, p->text_primary, 0);

    btn_auth_key = lv_button_create(status_card);
    lv_obj_set_size(btn_auth_key, 105, 30);
    lv_obj_align_to(btn_auth_key, btn_ping_derp, LV_ALIGN_OUT_LEFT_MID, -10, 0);
    lv_obj_set_style_bg_color(btn_auth_key, p->surface, 0);
    lv_obj_set_style_border_color(btn_auth_key, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_auth_key, 1, 0);
    lv_obj_set_style_radius(btn_auth_key, 4, 0);
    lv_obj_add_event_cb(btn_auth_key, open_auth_modal_cb, LV_EVENT_CLICKED, NULL);

    lbl_auth_key = lv_label_create(btn_auth_key);
    lv_label_set_text(lbl_auth_key, LV_SYMBOL_SETTINGS " Auth Key");
    lv_obj_center(lbl_auth_key);
    lv_obj_set_style_text_font(lbl_auth_key, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_auth_key, p->text_primary, 0);

    /* 2. Peers Section Header (y: 104) */
    lbl_peers_h = lv_label_create(screen);
    lv_label_set_text(lbl_peers_h, "ACTIVE TAILNET PEERS (6 Online)");
    lv_obj_set_pos(lbl_peers_h, 2, 106);
    lv_obj_set_style_text_font(lbl_peers_h, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_peers_h, p->accent_primary, 0);

    btn_refresh = lv_button_create(screen);
    lv_obj_set_size(btn_refresh, 95, 24);
    lv_obj_set_pos(btn_refresh, DEVOS_SCREEN_WIDTH - 32 - 95, 102);
    lv_obj_set_style_bg_color(btn_refresh, p->surface, 0);
    lv_obj_set_style_border_color(btn_refresh, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_refresh, 1, 0);
    lv_obj_set_style_radius(btn_refresh, 4, 0);
    lv_obj_add_event_cb(btn_refresh, refresh_peers_cb, LV_EVENT_CLICKED, NULL);

    lbl_refresh = lv_label_create(btn_refresh);
    lv_label_set_text(lbl_refresh, LV_SYMBOL_REFRESH " Refresh");
    lv_obj_center(lbl_refresh);
    lv_obj_set_style_text_font(lbl_refresh, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_refresh, p->text_primary, 0);

    /* 3. Scrollable Peer List (y: 132, height: 500) */
    peer_list_scroll = lv_obj_create(screen);
    lv_obj_set_size(peer_list_scroll, DEVOS_SCREEN_WIDTH - 28, DEVOS_CONTENT_HEIGHT - 138);
    lv_obj_set_pos(peer_list_scroll, 0, 132);
    lv_obj_set_style_bg_color(peer_list_scroll, p->bg, 0);
    lv_obj_set_style_border_width(peer_list_scroll, 0, 0);
    lv_obj_set_style_pad_all(peer_list_scroll, 0, 0);

    microlink_status_t st;
    for (int i = 0; i < MICROLINK_MAX_PEERS; i++) {
        peer_cards[i] = lv_button_create(peer_list_scroll);
        lv_obj_set_size(peer_cards[i], DEVOS_SCREEN_WIDTH - 36, 56);
        lv_obj_set_style_bg_color(peer_cards[i], p->surface, 0);
        lv_obj_set_style_border_color(peer_cards[i], p->surface_border, 0);
        lv_obj_set_style_border_width(peer_cards[i], 1, 0);
        lv_obj_set_style_radius(peer_cards[i], 4, 0);
        lv_obj_set_style_pad_left(peer_cards[i], 12, 0);
        lv_obj_set_style_pad_right(peer_cards[i], 12, 0);
        lv_obj_set_style_pad_top(peer_cards[i], 8, 0);
        lv_obj_set_style_pad_bottom(peer_cards[i], 8, 0);
        lv_obj_add_event_cb(peer_cards[i], peer_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        peer_title_lbls[i] = lv_label_create(peer_cards[i]);
        lv_obj_set_pos(peer_title_lbls[i], 0, 0);
        lv_obj_set_style_text_font(peer_title_lbls[i], &lv_font_montserrat_14, 0);

        peer_sub_lbls[i] = lv_label_create(peer_cards[i]);
        lv_obj_set_pos(peer_sub_lbls[i], 0, 22);
        lv_obj_set_style_text_font(peer_sub_lbls[i], &lv_font_montserrat_12, 0);

        /* Action Tray (hidden unless selected) */
        peer_tray[i] = lv_obj_create(peer_cards[i]);
        lv_obj_set_size(peer_tray[i], 320, 32);
        lv_obj_set_pos(peer_tray[i], 0, 48);
        lv_obj_set_style_bg_color(peer_tray[i], p->surface_active, 0);
        lv_obj_set_style_border_width(peer_tray[i], 0, 0);
        lv_obj_set_style_pad_all(peer_tray[i], 0, 0);
        lv_obj_clear_flag(peer_tray[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(peer_tray[i], LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *btn_ssh = peer_ssh_btns[i] = lv_button_create(peer_tray[i]);
        lv_obj_set_size(btn_ssh, 110, 28);
        lv_obj_set_pos(btn_ssh, 0, 2);
        lv_obj_set_style_bg_color(btn_ssh, p->accent_primary, 0);
        lv_obj_set_style_radius(btn_ssh, 4, 0);
        lv_obj_add_event_cb(btn_ssh, peer_ssh_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl_ssh = peer_ssh_lbls[i] = lv_label_create(btn_ssh);
        lv_label_set_text(lbl_ssh, LV_SYMBOL_POWER " SSH Shell");
        lv_obj_center(lbl_ssh);
        lv_obj_set_style_text_color(lbl_ssh,
            devos_theme_is_dark() ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl_ssh, &lv_font_montserrat_12, 0);

        lv_obj_t *btn_ping = peer_ping_btns[i] = lv_button_create(peer_tray[i]);
        lv_obj_set_size(btn_ping, 90, 28);
        lv_obj_set_pos(btn_ping, 118, 2);
        lv_obj_set_style_bg_color(btn_ping, p->surface, 0);
        lv_obj_set_style_border_color(btn_ping, p->surface_border, 0);
        lv_obj_set_style_border_width(btn_ping, 1, 0);
        lv_obj_set_style_radius(btn_ping, 4, 0);
        lv_obj_add_event_cb(btn_ping, peer_ping_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl_ping = peer_ping_lbls[i] = lv_label_create(btn_ping);
        lv_label_set_text(lbl_ping, LV_SYMBOL_SHUFFLE " Ping");
        lv_obj_center(lbl_ping);
        lv_obj_set_style_text_color(lbl_ping, p->text_primary, 0);
        lv_obj_set_style_text_font(lbl_ping, &lv_font_montserrat_12, 0);
    }

    /* 4. Auth Key Modal Dialog */
    modal_auth = lv_obj_create(screen);
    lv_obj_set_size(modal_auth, 540, 200);
    lv_obj_align(modal_auth, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(modal_auth, p->surface, 0);
    lv_obj_set_style_border_color(modal_auth, p->accent_primary, 0);
    lv_obj_set_style_border_width(modal_auth, 2, 0);
    lv_obj_set_style_radius(modal_auth, 8, 0);
    lv_obj_set_style_pad_all(modal_auth, 16, 0);
    lv_obj_clear_flag(modal_auth, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal_auth, LV_OBJ_FLAG_HIDDEN);

    lbl_m_title = lv_label_create(modal_auth);
    lv_label_set_text(lbl_m_title, LV_SYMBOL_SETTINGS " Tailscale Auth Key Configuration");
    lv_obj_set_pos(lbl_m_title, 0, 0);
    lv_obj_set_style_text_font(lbl_m_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_m_title, p->accent_primary, 0);

    lbl_m_desc = lv_label_create(modal_auth);
    lv_label_set_text(lbl_m_desc, "Enter an ephemeral or pre-authenticated key (tskey-auth-...):");
    lv_obj_set_pos(lbl_m_desc, 0, 26);
    lv_obj_set_style_text_font(lbl_m_desc, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_m_desc, p->text_secondary, 0);

    ta_auth_key = lv_textarea_create(modal_auth);
    lv_textarea_set_placeholder_text(ta_auth_key, "tskey-auth-k1234567890abcdef...");
    lv_textarea_set_one_line(ta_auth_key, true);
    lv_obj_set_size(ta_auth_key, 508, 36);
    lv_obj_set_pos(ta_auth_key, 0, 50);
    lv_obj_set_style_bg_color(ta_auth_key, p->bg_alt, 0);
    lv_obj_set_style_border_color(ta_auth_key, p->surface_border, 0);
    lv_obj_set_style_border_width(ta_auth_key, 1, 0);
    lv_obj_set_style_radius(ta_auth_key, 4, 0);
    lv_obj_set_style_pad_all(ta_auth_key, 8, 0);
    lv_obj_set_style_text_color(ta_auth_key, p->text_primary, 0);

    btn_enroll = lv_button_create(modal_auth);
    lv_obj_set_size(btn_enroll, 130, 34);
    lv_obj_set_pos(btn_enroll, 240, 110);
    lv_obj_set_style_bg_color(btn_enroll, p->accent_primary, 0);
    lv_obj_set_style_radius(btn_enroll, 4, 0);
    lv_obj_add_event_cb(btn_enroll, enroll_auth_key_cb, LV_EVENT_CLICKED, NULL);

    lbl_enroll = lv_label_create(btn_enroll);
    lv_label_set_text(lbl_enroll, LV_SYMBOL_OK " Enroll Node");
    lv_obj_center(lbl_enroll);
    lv_obj_set_style_text_color(lbl_enroll,
        devos_theme_is_dark() ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_enroll, &lv_font_montserrat_12, 0);

    btn_cancel = lv_button_create(modal_auth);
    lv_obj_set_size(btn_cancel, 110, 34);
    lv_obj_set_pos(btn_cancel, 380, 110);
    lv_obj_set_style_bg_color(btn_cancel, p->surface, 0);
    lv_obj_set_style_border_color(btn_cancel, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_cancel, 1, 0);
    lv_obj_set_style_radius(btn_cancel, 4, 0);
    lv_obj_add_event_cb(btn_cancel, close_auth_modal_cb, LV_EVENT_CLICKED, NULL);

    lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_center(lbl_cancel);
    lv_obj_set_style_text_color(lbl_cancel, p->text_primary, 0);
    lv_obj_set_style_text_font(lbl_cancel, &lv_font_montserrat_12, 0);

    /* Theme updates */
    devos_theme_add_listener(apply_theme, NULL);

    refresh_ui();
}

static void apply_theme(const devos_palette_t *p, void *user_data)
{
    LV_UNUSED(user_data);
    if (!screen) return;
    /* ponytail: black on neon cyan (dark) / white on cobalt (light) */
    lv_color_t on_accent = devos_theme_is_dark() ? lv_color_black() : lv_color_white();

    lv_obj_set_style_bg_color(screen, p->bg, 0);
    lv_obj_set_style_bg_color(status_card, p->surface, 0);
    lv_obj_set_style_border_color(status_card, p->surface_border, 0);
    lv_obj_set_style_text_color(lbl_details, p->text_primary, 0);
    lv_obj_set_style_text_color(lbl_keys, p->text_secondary, 0);

    /* Status action buttons */
    if (btn_ping_derp) {
        lv_obj_set_style_bg_color(btn_ping_derp, p->surface, 0);
        lv_obj_set_style_border_color(btn_ping_derp, p->surface_border, 0);
    }
    if (lbl_ping_derp) lv_obj_set_style_text_color(lbl_ping_derp, p->text_primary, 0);
    if (btn_auth_key) {
        lv_obj_set_style_bg_color(btn_auth_key, p->surface, 0);
        lv_obj_set_style_border_color(btn_auth_key, p->surface_border, 0);
    }
    if (lbl_auth_key) lv_obj_set_style_text_color(lbl_auth_key, p->text_primary, 0);

    lv_obj_set_style_text_color(lbl_peers_h, p->accent_primary, 0);
    lv_obj_set_style_bg_color(btn_refresh, p->surface, 0);
    lv_obj_set_style_border_color(btn_refresh, p->surface_border, 0);
    lv_obj_set_style_text_color(lbl_refresh, p->text_primary, 0);

    /* Peer list container + action trays */
    if (peer_list_scroll) lv_obj_set_style_bg_color(peer_list_scroll, p->bg, 0);
    for (int i = 0; i < MICROLINK_MAX_PEERS; i++) {
        if (peer_tray[i]) lv_obj_set_style_bg_color(peer_tray[i], p->surface_active, 0);
        if (peer_ssh_btns[i]) {
            lv_obj_set_style_bg_color(peer_ssh_btns[i], p->accent_primary, 0);
        }
        if (peer_ssh_lbls[i]) {
            lv_obj_set_style_text_color(peer_ssh_lbls[i], on_accent, 0);
        }
        if (peer_ping_btns[i]) {
            lv_obj_set_style_bg_color(peer_ping_btns[i], p->surface, 0);
            lv_obj_set_style_border_color(peer_ping_btns[i], p->surface_border, 0);
        }
        if (peer_ping_lbls[i]) {
            lv_obj_set_style_text_color(peer_ping_lbls[i], p->text_primary, 0);
        }
    }

    /* Auth modal */
    if (modal_auth) {
        lv_obj_set_style_bg_color(modal_auth, p->surface, 0);
        lv_obj_set_style_border_color(modal_auth, p->accent_primary, 0);
    }
    if (lbl_m_title) lv_obj_set_style_text_color(lbl_m_title, p->accent_primary, 0);
    if (lbl_m_desc) lv_obj_set_style_text_color(lbl_m_desc, p->text_secondary, 0);
    if (ta_auth_key) {
        lv_obj_set_style_bg_color(ta_auth_key, p->bg_alt, 0);
        lv_obj_set_style_border_color(ta_auth_key, p->surface_border, 0);
        lv_obj_set_style_text_color(ta_auth_key, p->text_primary, 0);
    }
    if (btn_enroll) lv_obj_set_style_bg_color(btn_enroll, p->accent_primary, 0);
    if (lbl_enroll) lv_obj_set_style_text_color(lbl_enroll, on_accent, 0);
    if (btn_cancel) {
        lv_obj_set_style_bg_color(btn_cancel, p->surface, 0);
        lv_obj_set_style_border_color(btn_cancel, p->surface_border, 0);
    }
    if (lbl_cancel) lv_obj_set_style_text_color(lbl_cancel, p->text_primary, 0);

    refresh_ui();
}

static void tailscale_show(void)
{
    s_selected_peer = -1;
    refresh_ui();
}

static void tailscale_hide(void)
{
    if (modal_auth) {
        lv_obj_add_flag(modal_auth, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool tailscale_handle_key(uint32_t key, uint8_t modifiers)
{
    /* If modal is open, capture ALL keystrokes for the auth key field
       (same pattern as terminal Quick Connect modal) */
    if (modal_auth && !lv_obj_has_flag(modal_auth, LV_OBJ_FLAG_HIDDEN)) {
        if (key == LV_KEY_ESC) {
            lv_obj_add_flag(modal_auth, LV_OBJ_FLAG_HIDDEN);
            return true;
        }
        if (key == '\r' || key == '\n') {
            enroll_auth_key_cb(NULL);
            return true;
        }
        if (key == '\b' || key == 0x7F) {
            if (ta_auth_key) lv_textarea_delete_char(ta_auth_key);
            return true;
        }
        if (key == LV_KEY_LEFT) {
            if (ta_auth_key) lv_textarea_cursor_left(ta_auth_key);
            return true;
        }
        if (key == LV_KEY_RIGHT) {
            if (ta_auth_key) lv_textarea_cursor_right(ta_auth_key);
            return true;
        }
        if (key >= 32 && key <= 126) {
            if (ta_auth_key) lv_textarea_add_char(ta_auth_key, (char)key);
            return true;
        }
        /* Absorb all other keys so the screen behind never receives them */
        return true;
    }

    if (key == 'c' || key == 'C') {
        toggle_conn_cb(NULL);
        return true;
    }

    if (key == 'p' || key == 'P') {
        ping_derp_cb(NULL);
        return true;
    }

    if (key == 'r' || key == 'R') {
        refresh_peers_cb(NULL);
        return true;
    }

    if (modifiers == DEVOS_MOD_NONE && key >= '1' && key <= '9') {
        int idx = key - '1';
        microlink_status_t st;
        if (microlink_get_status(&st) == 0 && idx < st.peer_count) {
            s_selected_peer = (s_selected_peer == idx) ? -1 : idx;
            refresh_ui();
            return true;
        }
    }

    if ((key == LV_KEY_ENTER || key == '\r' || key == '\n') && s_selected_peer >= 0) {
        open_peer_ssh(s_selected_peer);
        return true;
    }

    if (key == LV_KEY_ESC && s_selected_peer >= 0) {
        s_selected_peer = -1;
        refresh_ui();
        return true;
    }

    return false;
}

static int tailscale_telemetry_lines(char lines[3][64])
{
    microlink_status_t st;
    if (microlink_get_status(&st) != 0 ||
        st.state != MICROLINK_STATE_CONNECTED) {
        snprintf(lines[0], sizeof(lines[0]), "* Disconnected");
        snprintf(lines[1], sizeof(lines[1]), "* Mesh inactive");
        snprintf(lines[2], sizeof(lines[2]), "* Direct LAN routing");
        return 3;
    }
    int online = 0;
    for (int i = 0; i < st.peer_count; i++) {
        if (st.peers[i].is_online) online++;
    }
    snprintf(lines[0], sizeof(lines[0]), "* Peers: %d online", online);
    snprintf(lines[1], sizeof(lines[1]), "* DERP: %s", st.derp_relay_name);
    snprintf(lines[2], sizeof(lines[2]), "* IP: %s", st.assigned_ip);
    return 3;
}

devos_app_descriptor_t *app_tailscale_get_descriptor(void)
{
    app_descriptor.id = DEVOS_APP_TAILSCALE;
    app_descriptor.uid = "tailscale";
    app_descriptor.icon = LV_SYMBOL_LOOP;
    app_descriptor.category = "network";
    app_descriptor.name = "Tailscale";
    app_descriptor.title = "Tailscale Mesh";
    app_descriptor.subtitle = "WireGuard Private Network";
    app_descriptor.screen = screen;
    app_descriptor.init = tailscale_init;
    app_descriptor.show = tailscale_show;
    app_descriptor.hide = tailscale_hide;
    app_descriptor.handle_key = tailscale_handle_key;
    app_descriptor.get_telemetry_lines = tailscale_telemetry_lines;

    return &app_descriptor;
}

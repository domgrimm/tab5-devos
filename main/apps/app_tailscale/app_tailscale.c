#include "app_tailscale.h"
#include "devos_config.h"
#include "devos_theme.h"
#include <stdio.h>

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;

static void tailscale_init(void)
{
    const devos_palette_t *p = devos_theme_get();

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

    /* Status Card */
    lv_obj_t *status_card = lv_obj_create(screen);
    lv_obj_set_size(status_card, DEVOS_SCREEN_WIDTH - 32, 90);
    lv_obj_set_pos(status_card, 0, 0);
    lv_obj_set_style_bg_color(status_card, p->surface, 0);
    lv_obj_set_style_border_color(status_card, p->surface_border, 0);
    lv_obj_set_style_border_width(status_card, 1, 0);
    lv_obj_set_style_radius(status_card, 6, 0);
    lv_obj_set_style_pad_all(status_card, 12, 0);

    lv_obj_t *lbl_title = lv_label_create(status_card);
    lv_label_set_text(lbl_title, LV_SYMBOL_BULLET " Tailscale WireGuard Mesh: CONNECTED");
    lv_obj_set_pos(lbl_title, 0, 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_title, p->accent_secondary, 0);

    lv_obj_t *lbl_details = lv_label_create(status_card);
    lv_label_set_text(lbl_details, "Node IP: 100.77.11.92  |  Relay: DERP-19 (Sydney) 18ms  |  MagicDNS: devos.tailnet");
    lv_obj_set_pos(lbl_details, 0, 28);
    lv_obj_set_style_text_font(lbl_details, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_details, p->text_primary, 0);

    lv_obj_t *lbl_keys = lv_label_create(status_card);
    lv_label_set_text(lbl_keys, "Auth: Persistent Private Key in Encrypted NVS  |  MTU: 1280  |  ChaCha20-Poly1305 HW");
    lv_obj_set_pos(lbl_keys, 0, 48);
    lv_obj_set_style_text_font(lbl_keys, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_keys, p->text_secondary, 0);

    /* Peers Section */
    lv_obj_t *lbl_peers_h = lv_label_create(screen);
    lv_label_set_text(lbl_peers_h, "ACTIVE TAILNET PEERS (6 Online)");
    lv_obj_set_pos(lbl_peers_h, 0, 106);
    lv_obj_set_style_text_font(lbl_peers_h, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_peers_h, p->accent_primary, 0);

    const char *peers[4] = {
        LV_SYMBOL_BULLET " workstation.tailnet  (100.64.1.2)   - Linux x86_64 - Direct (2ms)",
        LV_SYMBOL_BULLET " macbook-pro.tailnet  (100.77.11.90) - macOS Sonoma - Direct (4ms)",
        LV_SYMBOL_BULLET " home-nas.tailnet     (100.80.3.15)  - TrueNAS Core - DERP (18ms)",
        LV_SYMBOL_BULLET " prod-cluster.tailnet (100.99.20.1)  - Ubuntu 24.04 - Direct (12ms)"
    };

    for (int i = 0; i < 4; i++) {
        lv_obj_t *peer_card = lv_obj_create(screen);
        lv_obj_set_size(peer_card, DEVOS_SCREEN_WIDTH - 32, 46);
        lv_obj_set_pos(peer_card, 0, 134 + i * 54);
        lv_obj_set_style_bg_color(peer_card, p->surface, 0);
        lv_obj_set_style_border_color(peer_card, p->surface_border, 0);
        lv_obj_set_style_border_width(peer_card, 1, 0);
        lv_obj_set_style_radius(peer_card, 4, 0);
        lv_obj_set_style_pad_all(peer_card, 10, 0);

        lv_obj_t *lp = lv_label_create(peer_card);
        lv_label_set_text(lp, peers[i]);
        lv_obj_align(lp, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_text_font(lp, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lp, p->text_primary, 0);
    }
}

static void tailscale_show(void) {}
static void tailscale_hide(void) {}

devos_app_descriptor_t *app_tailscale_get_descriptor(void)
{
    app_descriptor.id = DEVOS_APP_TAILSCALE;
    app_descriptor.name = "Tailscale";
    app_descriptor.title = "Tailscale Mesh";
    app_descriptor.subtitle = "WireGuard Private Network";
    app_descriptor.screen = screen;
    app_descriptor.init = tailscale_init;
    app_descriptor.show = tailscale_show;
    app_descriptor.hide = tailscale_hide;
    app_descriptor.handle_key = NULL;

    return &app_descriptor;
}

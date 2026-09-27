#pragma once
/* Network app internals: the shell (app_netdiag.c) hosts five tool views. */
#include "lvgl.h"
#include "devos_config.h"
#include "devos_core.h"
#include "devos_focus.h"
#include "devos_theme.h"
#include "devos_widgets.h"
#include "devos_netdiag.h"

#define ND_VIEW_Y  DEVOS_W_BAR_H
#define ND_VIEW_H  (DEVOS_CONTENT_HEIGHT - DEVOS_W_BAR_H - DEVOS_W_KEYS_H)
#define ND_ROW_H   22

typedef struct {
    const char *name;
    void (*create)(lv_obj_t *parent);           /* parent: the view's container */
    void (*show)(void);
    void (*hide)(void);
    bool (*key)(uint32_t key, uint8_t mods);
    void (*tick)(void);                         /* every 200 ms while shown */
    const char *(*keys)(void);                  /* footer hint */
} nd_view_t;

extern const nd_view_t nd_view_ping, nd_view_dns, nd_view_scan, nd_view_wifi, nd_view_mdns;

enum { ND_PING = 0, ND_DNS, ND_SCAN, ND_WIFI, ND_MDNS, ND_VIEWS };

void nd_goto(int view);
void nd_flash(const char *msg);                 /* a short message in the bar */
/* cross-tool actions */
void nd_ping_host(const char *host);
void nd_scan_host(const char *host);
void nd_dns_name(const char *name);

/* Keys for a form (devos_focus) that ends in a list: while the list's
 * scroll container has the focus, arrows / PgUp / PgDn / Enter drive it. */
bool nd_form_key(devos_focus_t *f, devos_vlist_t *v, uint32_t key, uint8_t mods);
/* Our Wi-Fi subnet as a.b.c.0/24 (or "" if unknown). */
void nd_local_subnet(char *out, size_t cap);

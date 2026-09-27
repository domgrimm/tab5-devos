#include "app_launcher.h"
#include "devos_config.h"
#include "devos_theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAYOUT_CONFIG_FILE TAB5_SD_MOUNT_POINT "/.devos/launcher_layout.json"

#define TILES_PER_PAGE  8
#define GRID_COLS       4
#define GRID_ROWS       2
#define TILE_WIDTH      280
#define TILE_HEIGHT     210
#define COL_GAP         30
#define ROW_GAP         14
#define MARGIN_X        35
#define ROW_Y0          6
#define ROW_Y1          (ROW_Y0 + TILE_HEIGHT + ROW_GAP) /* 230 */
#define CAROUSEL_HEIGHT 444
#define FOOTER_HEIGHT   44
#define FOOTER_Y        (DEVOS_SCREEN_HEIGHT - DEVOS_BOTTOM_BAR_HEIGHT - 4 - FOOTER_HEIGHT)
#define INFO_BOTTOM     (DEVOS_TOP_BAR_HEIGHT + 4 + 66)
#define CAROUSEL_Y      (INFO_BOTTOM + (FOOTER_Y - INFO_BOTTOM - CAROUSEL_HEIGHT) / 2)

static devos_app_descriptor_t app_descriptor;
static lv_obj_t *screen = NULL;

/* Telemetry header strip */
static lv_obj_t *telemetry_box = NULL;
static lv_obj_t *lbl_clock_date = NULL;
static lv_obj_t *lbl_net_power = NULL;
static lv_obj_t *lbl_mem_cpu = NULL;

/* Arrange Banner */
static lv_obj_t *banner_arrange = NULL;
static lv_obj_t *lbl_arrange_banner = NULL;

/* Multi-Page Carousel & Container */
static lv_obj_t *carousel = NULL;
static lv_obj_t *page_objs[4] = {NULL};

/* Compact Card Widgets */
typedef struct {
    lv_obj_t *card_btn;
    lv_obj_t *lbl_icon;
    lv_obj_t *lbl_title;
    lv_obj_t *lbl_subtitle;
    lv_obj_t *divider;
    lv_obj_t *lbl_line1;
    lv_obj_t *lbl_line2;
    lv_obj_t *lbl_line3;
    lv_obj_t *btn_hide;         /* arrange mode: hide / show this card */
    lv_obj_t *lbl_hide;
    int slot_idx;
} card_widget_t;

static card_widget_t cards[DEVOS_MAX_APPS];

/* Pagination Footer Bar */
static lv_obj_t *footer_bar = NULL;
static lv_obj_t *lbl_apps_count = NULL;
static lv_obj_t *btn_prev = NULL;
static lv_obj_t *lbl_prev = NULL;
static lv_obj_t *dots_cont = NULL;
static lv_obj_t *dot_btns[4] = {NULL};
static lv_obj_t *lbl_page_text = NULL;
static lv_obj_t *btn_next = NULL;
static lv_obj_t *lbl_next = NULL;
static lv_obj_t *btn_arrange_toggle = NULL;
static lv_obj_t *lbl_arrange_btn = NULL;
static lv_obj_t *btn_arrange_reset = NULL;
static lv_obj_t *lbl_arrange_reset = NULL;
static int last_registered_count = -1;

/* Bottom Navigation Hint Bar: every key of the screen (keyboard first,
 * AGENTS.md invariant 9). Each touch control has a key: Prev / Next / dots =
 * PgUp / PgDn (Sym+Up/Down, Sym+Left/Right), Arrange / Done = E, Defaults = R,
 * eye = H. */
static lv_obj_t *bottom_bar = NULL;
static lv_obj_t *lbl_bottom_hint = NULL;
#define HINT_NORMAL \
    "[Arrows/Tab] Move  |  [Enter/Tap] Launch  |  [1-8] Launch tile  |  [Sym+" LV_SYMBOL_UP "/" LV_SYMBOL_DOWN \
    "] Page  |  [E] Arrange  |  [Sym+T] Theme  |  [Sym+H] Home from any app"
#define HINT_ARRANGE \
    "[Arrows/Tab] Move  |  [Enter/Tap] Pick up / drop  |  [1-8] Pick / swap slot  |  [H] Hide / show  |  " \
    "[R] Defaults  |  [Sym+" LV_SYMBOL_UP "/" LV_SYMBOL_DOWN "] Page  |  [Esc] Cancel / done"

/* Slot Mapping & Carousel State */
/* slot_uids is what the carousel shows: every app in arrange mode, only the
 * visible ones otherwise. order_uids is the full saved order. */
static char slot_uids[DEVOS_MAX_APPS][DEVOS_MAX_UID];
static int active_app_count = 0;
static char order_uids[DEVOS_MAX_APPS][DEVOS_MAX_UID];
static int order_count = 0;
static char hidden_uids[DEVOS_MAX_APPS][DEVOS_MAX_UID];
static int hidden_count = 0;
static char s_hide_notice[80] = "";
static int current_page = 0;
static int total_pages = 1;
static bool arrange_mode = false;
static int selected_slot = -1;
static int focused_slot = 0;

/* Forward Declarations */
static void refresh_cards(void);
static void update_pagination_ui(void);
static void rebuild_card_widgets(void);

/* --------------------------------------------------------------------------
 * Persistence: order + hidden cards
 *   {"order": ["terminal", ...], "hidden": ["template"]}
 * Older files are a bare array of uids (or legacy ints 1..6). Uids of apps
 * that are no longer registered are dropped.
 * -------------------------------------------------------------------------- */
static bool is_hidden(const char *uid)
{
    for (int i = 0; i < hidden_count; i++) {
        if (strcmp(hidden_uids[i], uid) == 0) return true;
    }
    return false;
}

static void set_hidden(const char *uid, bool hide)
{
    int at = -1;
    for (int i = 0; i < hidden_count; i++) {
        if (strcmp(hidden_uids[i], uid) == 0) { at = i; break; }
    }
    if (hide && at < 0 && hidden_count < DEVOS_MAX_APPS) {
        snprintf(hidden_uids[hidden_count++], DEVOS_MAX_UID, "%s", uid);
    } else if (!hide && at >= 0) {
        memmove(hidden_uids[at], hidden_uids[at + 1], (size_t)(hidden_count - at - 1) * DEVOS_MAX_UID);
        hidden_count--;
    }
}

/* Apps in the order that are switched on (Settings > Apps), and how many of
 * those are hidden. */
static int count_on(bool hidden_only)
{
    int n = 0;
    for (int i = 0; i < order_count; i++) {
        if (devos_core_find_app(order_uids[i]) && (!hidden_only || is_hidden(order_uids[i]))) n++;
    }
    return n;
}

/* What the carousel shows (see slot_uids). */
static void rebuild_view(void)
{
    active_app_count = 0;
    for (int i = 0; i < order_count; i++) {
        if (!devos_core_find_app(order_uids[i])) continue;   /* switched off: keeps its place */
        if (arrange_mode || !is_hidden(order_uids[i])) {
            snprintf(slot_uids[active_app_count++], DEVOS_MAX_UID, "%s", order_uids[i]);
        }
    }
    total_pages = (active_app_count + TILES_PER_PAGE - 1) / TILES_PER_PAGE;
    if (total_pages < 1) total_pages = 1;
    if (total_pages > 4) total_pages = 4;
    if (current_page >= total_pages) current_page = total_pages - 1;
    if (focused_slot >= active_app_count) focused_slot = active_app_count > 0 ? active_app_count - 1 : 0;
}

static void save_layout(void)
{
    FILE *f = fopen(LAYOUT_CONFIG_FILE, "w");
    if (!f) return;
    fprintf(f, "{\n  \"order\": [");
    for (int i = 0; i < order_count; i++) fprintf(f, "%s\"%s\"", i ? ", " : "", order_uids[i]);
    fprintf(f, "],\n  \"hidden\": [");
    for (int i = 0; i < hidden_count; i++) fprintf(f, "%s\"%s\"", i ? ", " : "", hidden_uids[i]);
    fprintf(f, "]\n}\n");
    fclose(f);
}

static const char *legacy_id_to_uid(int id)
{
    switch (id) {
        case 1: return "opendev";
        case 2: return "terminal";
        case 3: return "editor";
        case 4: return "tailscale";
        case 5: return "antigravity";
        case 6: return "settings";
        default: return NULL;
    }
}

static int index_of(char list[][DEVOS_MAX_UID], int n, const char *uid)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i], uid) == 0) return i;
    }
    return -1;
}

/* Parse the uid strings of the JSON array starting at p (stops at ']').
 * Legacy ints use the old numbering (1 OpenDev .. 6 Settings). */
static void parse_uid_array(const char *p, char out[][DEVOS_MAX_UID], int *n,
                            char reg[][DEVOS_MAX_UID], int reg_count)
{
    while (*p && *p != ']' && *n < DEVOS_MAX_APPS) {
        if (*p == '"') {
            p++;
            char token[DEVOS_MAX_UID] = {0};
            int tlen = 0;
            while (*p && *p != '"' && tlen < (int)sizeof(token) - 1) token[tlen++] = *p++;
            if (*p == '"') p++;
            if (index_of(reg, reg_count, token) >= 0 && index_of(out, *n, token) < 0) {
                snprintf(out[(*n)++], DEVOS_MAX_UID, "%s", token);
            }
        } else if (*p >= '1' && *p <= '6') {
            const char *luid = legacy_id_to_uid(*p - '0');
            if (luid && index_of(reg, reg_count, luid) >= 0 && index_of(out, *n, luid) < 0)
                snprintf(out[(*n)++], DEVOS_MAX_UID, "%s", luid);
            p++;
        } else {
            p++;
        }
    }
}

static void load_layout(void)
{
    /* 1. Known apps (excluding the launcher itself). Switched-off ones stay
     * in the order so they come back to the same place. */
    char registered_uids[DEVOS_MAX_APPS][DEVOS_MAX_UID];
    int reg_count = 0;
    int core_count = devos_core_known_app_count();
    for (int i = 0; i < core_count && reg_count < DEVOS_MAX_APPS; i++) {
        devos_app_descriptor_t *app = devos_core_known_app_at(i);
        /* by uid: a switched-off app was never given its id */
        if (!app || !app->uid || strcmp(app->uid, "launcher") == 0) continue;
        if (*app->uid) snprintf(registered_uids[reg_count++], DEVOS_MAX_UID, "%s", app->uid);
    }

    /* 2. Saved order + hidden set */
    order_count = 0;
    hidden_count = 0;
    FILE *f = fopen(LAYOUT_CONFIG_FILE, "r");
    if (f) {
        char file_buf[2048] = {0};
        size_t n = fread(file_buf, 1, sizeof(file_buf) - 1, f);
        fclose(f);
        file_buf[n] = '\0';
        const char *ord = strstr(file_buf, "\"order\"");
        const char *hid = strstr(file_buf, "\"hidden\"");
        if (ord) ord = strchr(ord, '[');
        else ord = strchr(file_buf, '[');                        /* legacy bare array */
        if (ord) parse_uid_array(ord + 1, order_uids, &order_count, registered_uids, reg_count);
        if (hid && (hid = strchr(hid, '[')) != NULL) {
            parse_uid_array(hid + 1, hidden_uids, &hidden_count, registered_uids, reg_count);
        }
    }

    /* 3. Apps not in the file yet go at the end (visible) */
    for (int i = 0; i < reg_count && order_count < DEVOS_MAX_APPS; i++) {
        if (index_of(order_uids, order_count, registered_uids[i]) < 0) {
            snprintf(order_uids[order_count++], DEVOS_MAX_UID, "%s", registered_uids[i]);
        }
    }
    /* never hide everything */
    if (count_on(true) >= count_on(false)) hidden_count = 0;
    rebuild_view();
}

/* --------------------------------------------------------------------------
 * Carousel Page Navigation
 * -------------------------------------------------------------------------- */
int app_launcher_get_current_page(void) { return current_page; }
int app_launcher_get_total_pages(void) { return total_pages; }
int app_launcher_get_active_app_count(void) { return active_app_count; }

void app_launcher_set_page(int page, bool animate)
{
    if (page < 0) page = 0;
    if (page >= total_pages) page = total_pages - 1;

    current_page = page;
    if (carousel) {
        lv_obj_scroll_to_x(carousel, current_page * DEVOS_SCREEN_WIDTH,
                           animate ? LV_ANIM_ON : LV_ANIM_OFF);
    }
    update_pagination_ui();
    refresh_cards();
}

static void carousel_scroll_end_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (!obj) return;

    int32_t scroll_x = lv_obj_get_scroll_x(obj);
    int new_page = (scroll_x + (DEVOS_SCREEN_WIDTH / 2)) / DEVOS_SCREEN_WIDTH;
    if (new_page < 0) new_page = 0;
    if (new_page >= total_pages) new_page = total_pages - 1;

    if (new_page != current_page) {
        current_page = new_page;
        update_pagination_ui();
        refresh_cards();
    }
}

static void prev_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (current_page > 0) {
        app_launcher_set_page(current_page - 1, true);
    }
}

static void next_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (current_page < total_pages - 1) {
        app_launcher_set_page(current_page + 1, true);
    }
}

static void dot_click_cb(lv_event_t *e)
{
    int page = (int)(intptr_t)lv_event_get_user_data(e);
    app_launcher_set_page(page, true);
}

/* --------------------------------------------------------------------------
 * Arrange Mode Operations
 * -------------------------------------------------------------------------- */
void app_launcher_swap_slots(int slot_a, int slot_b)
{
    if (slot_a < 0 || slot_a >= active_app_count ||
        slot_b < 0 || slot_b >= active_app_count ||
        slot_a == slot_b) {
        selected_slot = -1;
        refresh_cards();
        return;
    }

    int oa = index_of(order_uids, order_count, slot_uids[slot_a]);
    int ob = index_of(order_uids, order_count, slot_uids[slot_b]);
    if (oa >= 0 && ob >= 0) {
        char tmp[DEVOS_MAX_UID];
        strlcpy(tmp, order_uids[oa], sizeof(tmp));
        strlcpy(order_uids[oa], order_uids[ob], sizeof(order_uids[oa]));
        strlcpy(order_uids[ob], tmp, sizeof(order_uids[ob]));
    }
    selected_slot = -1;
    rebuild_view();
    save_layout();
    refresh_cards();
}

void app_launcher_reset_layout(void)
{
    /* Reset to core registration order, every card visible */
    int reg_count = 0;
    int core_count = devos_core_known_app_count();
    for (int i = 0; i < core_count && reg_count < DEVOS_MAX_APPS; i++) {
        devos_app_descriptor_t *app = devos_core_known_app_at(i);
        if (!app || !app->uid || strcmp(app->uid, "launcher") == 0) continue;
        snprintf(order_uids[reg_count++], DEVOS_MAX_UID, "%s", app->uid);
    }
    order_count = reg_count;
    hidden_count = 0;
    selected_slot = -1;
    current_page = 0;
    rebuild_view();
    save_layout();
    rebuild_card_widgets();
    app_launcher_set_page(0, false);
    refresh_cards();
}

int app_launcher_get_app_in_slot(int slot)
{
    if (slot >= 0 && slot < active_app_count) {
        devos_app_descriptor_t *app = devos_core_find_app(slot_uids[slot]);
        if (app) return app->id;
    }
    return -1;
}

const char *app_launcher_get_app_uid_in_slot(int slot)
{
    if (slot >= 0 && slot < active_app_count) {
        return slot_uids[slot];
    }
    return NULL;
}

void app_launcher_set_arrange_mode(bool active)
{
    if (arrange_mode != active) {
        arrange_mode = active;
        selected_slot = -1;
        s_hide_notice[0] = '\0';
        rebuild_view();                     /* arrange shows hidden cards too */
        rebuild_card_widgets();
        app_launcher_set_page(current_page, false);
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

/* Arrange mode: hide or show the card in `slot`. */
static void toggle_hidden(int slot)
{
    if (slot < 0 || slot >= active_app_count) return;
    const char *uid = slot_uids[slot];
    bool hide = !is_hidden(uid);
    if (hide && count_on(true) + 1 >= count_on(false)) {
        snprintf(s_hide_notice, sizeof(s_hide_notice), "Keep at least one app visible.");
        refresh_cards();
        return;
    }
    set_hidden(uid, hide);
    devos_app_descriptor_t *app = devos_core_find_app(uid);
    snprintf(s_hide_notice, sizeof(s_hide_notice), "%s %s on the home screen.",
             app && app->name ? app->name : uid, hide ? "hidden" : "shown");
    save_layout();
    refresh_cards();
}

static void hide_btn_cb(lv_event_t *e)
{
    toggle_hidden((int)(intptr_t)lv_event_get_user_data(e));
}

static void card_click_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    focused_slot = slot;                    /* keyboard focus follows taps */

    if (!arrange_mode) {
        /* Normal Mode: Launch the selected app */
        devos_app_descriptor_t *app = devos_core_find_app(slot_uids[slot]);
        if (app) {
            devos_core_switch_app(app->id);
        }
    } else {
        /* Arrange Mode: Select or Swap across pages */
        lv_obj_t *target_obj = (lv_obj_t *)lv_event_get_target(e);
        if (target_obj) {
            lv_obj_remove_state(target_obj, LV_STATE_FOCUSED | LV_STATE_PRESSED);
        }

        if (selected_slot == -1) {
            /* First tile picked up */
            selected_slot = slot;
            refresh_cards();
        } else if (selected_slot == slot) {
            /* Deselect */
            selected_slot = -1;
            refresh_cards();
        } else {
            /* Second tile clicked -> Swap! */
            int src = selected_slot;
            selected_slot = -1;
            app_launcher_swap_slots(src, slot);
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

/* --------------------------------------------------------------------------
 * Visual Card & Pagination Refresh
 * -------------------------------------------------------------------------- */
static void update_pagination_ui(void)
{
    const devos_palette_t *p = devos_theme_get();

    /* 1. Apps Count badge */
    if (lbl_apps_count) {
        char ac_buf[32];
        int on = count_on(false), hid = count_on(true);
        if (hid) snprintf(ac_buf, sizeof(ac_buf), "%d apps, %d hidden", on, hid);
        else snprintf(ac_buf, sizeof(ac_buf), "%d Apps Installed", on);
        lv_label_set_text(lbl_apps_count, ac_buf);
        lv_obj_set_style_text_color(lbl_apps_count, p->text_secondary, 0);
    }

    /* 2. Graphical interactive dot indicators */
    for (int i = 0; i < 4; i++) {
        if (i < total_pages) {
            if (!dot_btns[i] && dots_cont) {
                dot_btns[i] = lv_button_create(dots_cont);
                lv_obj_add_event_cb(dot_btns[i], dot_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            }
            if (dot_btns[i]) {
                lv_obj_remove_flag(dot_btns[i], LV_OBJ_FLAG_HIDDEN);
                if (i == current_page) {
                    lv_obj_set_size(dot_btns[i], 18, 8);
                    lv_obj_set_style_bg_color(dot_btns[i], p->accent_primary, 0);
                } else {
                    lv_obj_set_size(dot_btns[i], 8, 8);
                    lv_obj_set_style_bg_color(dot_btns[i], p->surface_border, 0);
                }
                lv_obj_set_style_radius(dot_btns[i], LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_border_width(dot_btns[i], 0, 0);
                lv_obj_set_style_pad_all(dot_btns[i], 0, 0);
            }
        } else if (dot_btns[i]) {
            lv_obj_add_flag(dot_btns[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 3. Page text (Page 1 / 2) */
    if (lbl_page_text) {
        char pbuf[32];
        snprintf(pbuf, sizeof(pbuf), "Page %d / %d", current_page + 1, total_pages);
        lv_label_set_text(lbl_page_text, pbuf);
    }

    /* 3. Prev / Next Button styling & opacity */
    if (btn_prev) {
        if (current_page > 0) {
            lv_obj_set_style_opa(btn_prev, LV_OPA_COVER, 0);
            lv_obj_add_flag(btn_prev, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_set_style_opa(btn_prev, LV_OPA_40, 0);
            lv_obj_clear_flag(btn_prev, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    if (btn_next) {
        if (current_page < total_pages - 1) {
            lv_obj_set_style_opa(btn_next, LV_OPA_COVER, 0);
            lv_obj_add_flag(btn_next, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_set_style_opa(btn_next, LV_OPA_40, 0);
            lv_obj_clear_flag(btn_next, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    /* 4. Arrange & Reset buttons */
    if (lbl_arrange_btn && btn_arrange_toggle) {
        if (arrange_mode) {
            lv_label_set_text(lbl_arrange_btn, LV_SYMBOL_OK " Done");
            lv_obj_set_style_bg_color(btn_arrange_toggle, p->accent_secondary, 0);
        } else {
            lv_label_set_text(lbl_arrange_btn, LV_SYMBOL_SHUFFLE " Arrange");
            lv_obj_set_style_bg_color(btn_arrange_toggle, p->surface_active, 0);
        }
    }
    if (btn_arrange_reset) {
        if (arrange_mode) {
            lv_obj_remove_flag(btn_arrange_reset, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(btn_arrange_reset, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 5. Arrange Banner */
    if (banner_arrange) {
        if (arrange_mode) {
            lv_obj_remove_flag(banner_arrange, LV_OBJ_FLAG_HIDDEN);
            if (selected_slot >= 0 && selected_slot < active_app_count) {
                char b_buf[192];
                devos_app_descriptor_t *sel_app = devos_core_find_app(slot_uids[selected_slot]);
                snprintf(b_buf, sizeof(b_buf),
                         LV_SYMBOL_SHUFFLE " Slot %d (%s) picked up: arrows to its new place (any page) + Enter, "
                         "or 1-8 / tap the card to swap with  |  [Esc] Cancel",
                         selected_slot + 1, sel_app ? sel_app->name : "App");
                lv_label_set_text(lbl_arrange_banner, b_buf);
            } else {
                char b_buf[192];
                snprintf(b_buf, sizeof(b_buf), "%s",
                         s_hide_notice[0] ? s_hide_notice
                                          : LV_SYMBOL_SHUFFLE " ARRANGE: Enter or tap a card, then Enter or tap where it goes  |  "
                                            "[H] / " LV_SYMBOL_EYE_OPEN " hides / shows a card  |  [R] Defaults  |  [E] / [Esc] Done");
                lv_label_set_text(lbl_arrange_banner, b_buf);
            }
        } else {
            lv_obj_add_flag(banner_arrange, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 6. Bottom Hint Bar */
    if (lbl_bottom_hint) {
        lv_label_set_text(lbl_bottom_hint, arrange_mode ? HINT_ARRANGE : HINT_NORMAL);
    }
}

static void refresh_cards(void)
{
    const devos_palette_t *p = devos_theme_get();

    for (int i = 0; i < active_app_count; i++) {
        card_widget_t *c = &cards[i];
        if (!c->card_btn) continue;

        devos_app_descriptor_t *app = devos_core_find_app(slot_uids[i]);
        if (!app) continue;

        /* Icon */
        if (c->lbl_icon) {
            lv_label_set_text(c->lbl_icon, app->icon ? app->icon : LV_SYMBOL_FILE);
            lv_obj_set_style_text_color(c->lbl_icon, p->accent_primary, 0);
        }

        /* Title with active-page slot index (1..8) or global slot in arrange mode */
        if (c->lbl_title) {
            char tbuf[64];
            int page_slot = (i % TILES_PER_PAGE) + 1;
            if (arrange_mode) {
                if (selected_slot == i) {
                    snprintf(tbuf, sizeof(tbuf), "[" LV_SYMBOL_SHUFFLE " %d] %s", i + 1, app->name ? app->name : app->uid);
                } else {
                    snprintf(tbuf, sizeof(tbuf), "[%d] %s", i + 1, app->name ? app->name : app->uid);
                }
            } else {
                snprintf(tbuf, sizeof(tbuf), "[%d] %s", page_slot, app->name ? app->name : app->uid);
            }
            lv_label_set_text(c->lbl_title, tbuf);
            lv_obj_set_style_text_color(c->lbl_title, p->accent_primary, 0);
        }

        /* Subtitle / Category */
        if (c->lbl_subtitle) {
            const char *sub = (app->subtitle && *app->subtitle) ? app->subtitle :
                              (app->category ? app->category : "App");
            lv_label_set_text(c->lbl_subtitle, sub);
            lv_obj_set_style_text_color(c->lbl_subtitle, p->text_secondary, 0);
        }

        /* Dynamic Telemetry Lines */
        char lines[3][64] = {{0}};
        int line_count = 0;
        if (app->get_telemetry_lines) {
            line_count = app->get_telemetry_lines(lines);
        }

        if (c->lbl_line1) {
            if (line_count > 0 && lines[0][0]) {
                lv_label_set_text(c->lbl_line1, lines[0]);
            } else {
                lv_label_set_text(c->lbl_line1, "* Status: Active");
            }
            lv_obj_set_style_text_color(c->lbl_line1, p->text_primary, 0);
        }
        if (c->lbl_line2) {
            if (line_count > 1 && lines[1][0]) {
                lv_label_set_text(c->lbl_line2, lines[1]);
            } else {
                lv_label_set_text(c->lbl_line2, app->category ? app->category : "");
            }
            lv_obj_set_style_text_color(c->lbl_line2, p->text_primary, 0);
        }
        if (c->lbl_line3) {
            if (line_count > 2 && lines[2][0]) {
                lv_label_set_text(c->lbl_line3, lines[2]);
            } else {
                lv_label_set_text(c->lbl_line3, "");
            }
            lv_obj_set_style_text_color(c->lbl_line3, p->text_secondary, 0);
        }

        /* Hidden cards (only listed in arrange mode) */
        bool hid = is_hidden(slot_uids[i]);
        lv_obj_set_style_opa(c->card_btn, (arrange_mode && hid) ? LV_OPA_50 : LV_OPA_COVER, 0);
        if (c->btn_hide) {
            lv_label_set_text(c->lbl_hide, hid ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
            lv_obj_set_style_bg_color(c->btn_hide, hid ? p->surface : p->surface_active, 0);
            lv_obj_set_style_border_color(c->btn_hide, hid ? p->accent_warning : p->surface_border, 0);
            lv_obj_set_style_text_color(c->lbl_hide, hid ? p->accent_warning : p->text_primary, 0);
        }
        if (arrange_mode && hid && c->lbl_subtitle) {
            lv_label_set_text(c->lbl_subtitle, "Hidden from the home screen");
            lv_obj_set_style_text_color(c->lbl_subtitle, p->accent_warning, 0);
        }

        /* Border & Card styling */
        lv_obj_set_style_bg_color(c->card_btn, p->surface, 0);
        if (c->divider) lv_obj_set_style_bg_color(c->divider, p->surface_border, 0);

        if (arrange_mode && selected_slot == i) {
            lv_obj_set_style_border_color(c->card_btn, p->accent_warning, 0);
            lv_obj_set_style_border_color(c->card_btn, p->accent_warning, LV_STATE_FOCUSED);
            lv_obj_set_style_border_width(c->card_btn, 3, 0);
            lv_obj_set_style_border_width(c->card_btn, 3, LV_STATE_FOCUSED);
        } else if (arrange_mode) {
            /* the keyboard focus (what Enter / H act on) gets a heavy border */
            int bw = focused_slot == i ? 3 : 1;
            lv_obj_set_style_border_color(c->card_btn, p->border_highlight, 0);
            lv_obj_set_style_border_color(c->card_btn, p->border_highlight, LV_STATE_FOCUSED);
            lv_obj_set_style_border_width(c->card_btn, bw, 0);
            lv_obj_set_style_border_width(c->card_btn, bw, LV_STATE_FOCUSED);
        } else if (focused_slot == i) {
            lv_obj_set_style_border_color(c->card_btn, p->border_highlight, 0);
            lv_obj_set_style_border_color(c->card_btn, p->border_highlight, LV_STATE_FOCUSED);
            lv_obj_set_style_border_width(c->card_btn, 2, 0);
            lv_obj_set_style_border_width(c->card_btn, 2, LV_STATE_FOCUSED);
        } else {
            lv_obj_set_style_border_color(c->card_btn, p->surface_border, 0);
            lv_obj_set_style_border_color(c->card_btn, p->border_highlight, LV_STATE_FOCUSED);
            lv_obj_set_style_border_width(c->card_btn, 1, 0);
            lv_obj_set_style_border_width(c->card_btn, 2, LV_STATE_FOCUSED);
        }
    }

    update_pagination_ui();
}

void app_launcher_update_telemetry(void)
{
    if (!screen || lv_obj_has_flag(screen, LV_OBJ_FLAG_HIDDEN)) return;

    const devos_telemetry_t *t = devos_telemetry_get();

    /* System Telemetry in Top Strip (all live: devos_sysmon) */
    char buf[160];
    if (t->time_valid) {
        snprintf(buf, sizeof(buf), "%02d:%02d   %s", t->rtc_hour, t->rtc_min, t->rtc_date_str);
    } else {
        snprintf(buf, sizeof(buf), "--:--   Clock not set (connect to Wi-Fi to sync)");
    }
    if (lbl_clock_date) lv_label_set_text(lbl_clock_date, buf);

    char ts[48] = "", wg[48] = "", bat[72], sd[48], net[224];
    if (t->tailscale_online) {
        snprintf(ts, sizeof(ts), "Tailscale: %s  |  ", t->tailscale_ip);
    }
    if (t->wireguard_online) {
        snprintf(wg, sizeof(wg), "WireGuard: %.19s  |  ", t->wireguard_ip);
    }
    if (!t->battery_valid) {
        snprintf(bat, sizeof(bat), "Battery: n/a");
    } else if (!t->battery_present) {
        snprintf(bat, sizeof(bat), "Power: USB (no battery)");
    } else if (t->battery_charging) {
        snprintf(bat, sizeof(bat), "Battery: %d%% (%.2fV, charging)",
                 t->battery_percent, t->battery_voltage_mv / 1000.0f);
    } else if (t->runtime_minutes_left > 0) {
        snprintf(bat, sizeof(bat), "Battery: %d%% (%.2fV, %.1fW, ~%dh%02dm left)",
                 t->battery_percent, t->battery_voltage_mv / 1000.0f,
                 t->battery_power_mw / 1000.0f,
                 t->runtime_minutes_left / 60, t->runtime_minutes_left % 60);
    } else {
        snprintf(bat, sizeof(bat), "Battery: %d%% (%.2fV, %.1fW)",
                 t->battery_percent, t->battery_voltage_mv / 1000.0f,
                 t->battery_power_mw / 1000.0f);
    }
    if (!t->sd_mounted) {
        snprintf(sd, sizeof(sd), "SD: not inserted");
    } else if (t->sd_total_mb == 0) {
        snprintf(sd, sizeof(sd), "SD: reading...");
    } else {
        snprintf(sd, sizeof(sd), "SD: %.1f GB free of %.1f GB",
                 t->sd_free_mb / 1024.0f, t->sd_total_mb / 1024.0f);
    }
    snprintf(net, sizeof(net), "%s%s%s  |  %s", ts, wg, bat, sd);
    if (lbl_net_power) lv_label_set_text(lbl_net_power, net);

    snprintf(buf, sizeof(buf), "Memory: %.1f MB PSRAM free, %u KB SRAM free  |  CPU: Core 0 %d%%  |  Core 1 %d%%  |  Up %luh%02lum",
             t->free_psram_kb / 1024.0f,
             (unsigned int)t->free_sram_kb,
             t->cpu_load_core0,
             t->cpu_load_core1,
             (unsigned long)(t->uptime_s / 3600), (unsigned long)((t->uptime_s / 60) % 60));
    if (lbl_mem_cpu) lv_label_set_text(lbl_mem_cpu, buf);

    /* Update dynamic app card telemetry lines */
    for (int i = 0; i < active_app_count; i++) {
        card_widget_t *c = &cards[i];
        if (!c->card_btn) continue;
        devos_app_descriptor_t *app = devos_core_find_app(slot_uids[i]);
        if (!app || !app->get_telemetry_lines) continue;

        char lines[3][64] = {{0}};
        int count = app->get_telemetry_lines(lines);
        if (count > 0 && c->lbl_line1 && lines[0][0]) lv_label_set_text(c->lbl_line1, lines[0]);
        if (count > 1 && c->lbl_line2 && lines[1][0]) lv_label_set_text(c->lbl_line2, lines[1]);
        if (count > 2 && c->lbl_line3 && lines[2][0]) lv_label_set_text(c->lbl_line3, lines[2]);
    }
}

/* --------------------------------------------------------------------------
 * Physical Keyboard Navigation & Shortcuts
 * -------------------------------------------------------------------------- */
static bool launcher_handle_key(uint32_t key, uint8_t modifiers)
{
    /* 1. Toggle Arrange Mode: 'e' / 'E' or Sym + E */
    if (key == 'e' || key == 'E' || ((modifiers & DEVOS_MOD_FN) && (key == 'e' || key == 'E'))) {
        app_launcher_toggle_arrange_mode();
        return true;
    }

    /* 2. Page Navigation: PageUp / PageDown, Fn + Left / Right */
    if (key == DEVOS_KEY_PGUP || ((modifiers & DEVOS_MOD_FN) && key == LV_KEY_LEFT)) {
        if (current_page > 0) {
            app_launcher_set_page(current_page - 1, true);
            focused_slot = current_page * TILES_PER_PAGE;
            refresh_cards();
        }
        return true;
    }
    if (key == DEVOS_KEY_PGDN || ((modifiers & DEVOS_MOD_FN) && key == LV_KEY_RIGHT)) {
        if (current_page < total_pages - 1) {
            app_launcher_set_page(current_page + 1, true);
            focused_slot = current_page * TILES_PER_PAGE;
            if (focused_slot >= active_app_count) focused_slot = active_app_count - 1;
            refresh_cards();
        }
        return true;
    }

    /* 3. Arrange Mode Key Controls */
    if (arrange_mode) {
        if (key == LV_KEY_ESC) {            /* one level: drop the picked card, then leave */
            if (selected_slot >= 0) {
                selected_slot = -1;
                refresh_cards();
            } else {
                app_launcher_set_arrange_mode(false);
            }
            return true;
        }
        if (key == 'r' || key == 'R') {
            app_launcher_reset_layout();
            return true;
        }
        if ((key == 'h' || key == 'H') && !(modifiers & (DEVOS_MOD_CTRL | DEVOS_MOD_ALT | DEVOS_MOD_FN))) {
            toggle_hidden(selected_slot >= 0 ? selected_slot : focused_slot);
            return true;
        }
        /* 1..8 on active page: select or swap slot */
        if (key >= '1' && key <= '8') {
            int slot = current_page * TILES_PER_PAGE + (key - '1');
            if (slot < active_app_count) {
                focused_slot = slot;
                if (selected_slot == -1) {
                    selected_slot = slot;
                    refresh_cards();
                } else if (selected_slot == slot) {
                    selected_slot = -1;
                    refresh_cards();
                } else {
                    int src = selected_slot;
                    selected_slot = -1;
                    app_launcher_swap_slots(src, slot);
                }
            }
            return true;
        }
    } else {
        /* 4. Normal Mode: Direct Launch (1..8 on active page) */
        if (modifiers == DEVOS_MOD_NONE && key >= '1' && key <= '8') {
            int slot = current_page * TILES_PER_PAGE + (key - '1');
            if (slot < active_app_count) {
                devos_app_descriptor_t *app = devos_core_find_app(slot_uids[slot]);
                if (app) {
                    devos_core_switch_app(app->id);
                }
            }
            return true;
        }
    }

    /* 5a. Tab / Aa+Tab: next / previous tile in order, across pages */
    if (key == '\t' && !(modifiers & (DEVOS_MOD_CTRL | DEVOS_MOD_ALT | DEVOS_MOD_FN))) {
        if (active_app_count > 0) {
            int dir = (modifiers & DEVOS_MOD_SHIFT) ? -1 : 1;
            focused_slot = (focused_slot + dir + active_app_count) % active_app_count;
            if (focused_slot / TILES_PER_PAGE != current_page) {
                app_launcher_set_page(focused_slot / TILES_PER_PAGE, true);
            }
            refresh_cards();
        }
        return true;
    }

    /* 5b. Continuous Arrow Key Navigation across page boundaries */
    int cur_col = (focused_slot % TILES_PER_PAGE) % GRID_COLS;
    int cur_row = (focused_slot % TILES_PER_PAGE) / GRID_COLS;

    if (key == LV_KEY_RIGHT) {
        if (cur_col < GRID_COLS - 1) {
            if (focused_slot + 1 < active_app_count) {
                focused_slot++;
            }
        } else {
            /* Cross right boundary to next page! */
            if (current_page < total_pages - 1) {
                app_launcher_set_page(current_page + 1, true);
                int next_slot = (current_page) * TILES_PER_PAGE + (cur_row * GRID_COLS);
                if (next_slot < active_app_count) focused_slot = next_slot;
                else focused_slot = active_app_count - 1;
            }
        }
        refresh_cards();
        return true;
    }
    if (key == LV_KEY_LEFT) {
        if (cur_col > 0) {
            focused_slot--;
        } else {
            /* Cross left boundary to previous page! */
            if (current_page > 0) {
                app_launcher_set_page(current_page - 1, true);
                int prev_slot = (current_page) * TILES_PER_PAGE + (cur_row * GRID_COLS) + (GRID_COLS - 1);
                if (prev_slot < active_app_count) focused_slot = prev_slot;
                else focused_slot = active_app_count - 1;
            }
        }
        refresh_cards();
        return true;
    }
    if (key == LV_KEY_DOWN) {
        if (cur_row == 0 && focused_slot + GRID_COLS < active_app_count) {
            focused_slot += GRID_COLS;
            refresh_cards();
        }
        return true;
    }
    if (key == LV_KEY_UP) {
        if (cur_row == 1) {
            focused_slot -= GRID_COLS;
            refresh_cards();
        }
        return true;
    }

    /* 6. Enter to Launch or Swap */
    if (key == LV_KEY_ENTER || key == '\r') {
        if (arrange_mode) {
            if (selected_slot == -1) {
                selected_slot = focused_slot;
                refresh_cards();
            } else if (selected_slot == focused_slot) {
                selected_slot = -1;
                refresh_cards();
            } else {
                int src = selected_slot;
                selected_slot = -1;
                app_launcher_swap_slots(src, focused_slot);
            }
        } else {
            if (focused_slot < active_app_count) {
                devos_app_descriptor_t *app = devos_core_find_app(slot_uids[focused_slot]);
                if (app) {
                    devos_core_switch_app(app->id);
                }
            }
        }
        return true;
    }

    return false;
}

/* --------------------------------------------------------------------------
 * Card & Carousel Construction
 * -------------------------------------------------------------------------- */
static void rebuild_card_widgets(void)
{
    const devos_palette_t *p = devos_theme_get();

    /* Delete old pages if any */
    for (int i = 0; i < 4; i++) {
        if (page_objs[i]) {
            lv_obj_delete(page_objs[i]);
            page_objs[i] = NULL;
        }
    }
    memset(cards, 0, sizeof(cards));

    /* Build pages inside carousel */
    for (int page = 0; page < total_pages; page++) {
        lv_obj_t *pg = lv_obj_create(carousel);
        page_objs[page] = pg;
        lv_obj_set_size(pg, DEVOS_SCREEN_WIDTH, CAROUSEL_HEIGHT);
        lv_obj_set_pos(pg, page * DEVOS_SCREEN_WIDTH, 0);
        lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(pg, 0, 0);
        lv_obj_set_style_pad_all(pg, 0, 0);
        lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);

        for (int k = 0; k < TILES_PER_PAGE; k++) {
            int slot = page * TILES_PER_PAGE + k;
            if (slot >= active_app_count) break;

            int col = k % GRID_COLS;
            int row = k / GRID_COLS;
            int x = MARGIN_X + col * (TILE_WIDTH + COL_GAP);
            int y = (row == 0) ? ROW_Y0 : ROW_Y1;

            card_widget_t *c = &cards[slot];
            c->slot_idx = slot;

            /* Card container button */
            c->card_btn = lv_button_create(pg);
            lv_obj_set_size(c->card_btn, TILE_WIDTH, TILE_HEIGHT);
            lv_obj_set_pos(c->card_btn, x, y);
            lv_obj_set_style_bg_color(c->card_btn, p->surface, 0);
            lv_obj_set_style_border_color(c->card_btn, p->surface_border, 0);
            lv_obj_set_style_border_width(c->card_btn, 1, 0);
            lv_obj_set_style_radius(c->card_btn, 8, 0);
            lv_obj_set_style_pad_all(c->card_btn, 12, 0);
            lv_obj_clear_flag(c->card_btn, LV_OBJ_FLAG_SCROLLABLE);

            /* Connect click handler */
            lv_obj_add_event_cb(c->card_btn, card_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)slot);

            /* Icon */
            c->lbl_icon = lv_label_create(c->card_btn);
            lv_obj_set_pos(c->lbl_icon, 0, 0);
            lv_obj_set_style_text_font(c->lbl_icon, &lv_font_montserrat_18, 0);
            lv_obj_set_style_text_color(c->lbl_icon, p->accent_primary, 0);

            /* Title */
            c->lbl_title = lv_label_create(c->card_btn);
            lv_obj_set_pos(c->lbl_title, 26, 0);
            lv_obj_set_width(c->lbl_title, 220);
            lv_label_set_long_mode(c->lbl_title, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(c->lbl_title, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(c->lbl_title, p->accent_primary, 0);

            /* Subtitle / Category */
            c->lbl_subtitle = lv_label_create(c->card_btn);
            lv_obj_set_pos(c->lbl_subtitle, 0, 26);
            lv_obj_set_width(c->lbl_subtitle, 250);
            lv_label_set_long_mode(c->lbl_subtitle, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(c->lbl_subtitle, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(c->lbl_subtitle, p->text_secondary, 0);

            /* Divider */
            c->divider = lv_obj_create(c->card_btn);
            lv_obj_set_size(c->divider, TILE_WIDTH - 24, 1);
            lv_obj_set_pos(c->divider, 0, 50);
            lv_obj_set_style_bg_color(c->divider, p->surface_border, 0);
            lv_obj_set_style_border_width(c->divider, 0, 0);

            /* Telemetry Lines */
            c->lbl_line1 = lv_label_create(c->card_btn);
            lv_obj_set_pos(c->lbl_line1, 2, 60);
            lv_obj_set_width(c->lbl_line1, 250);
            lv_label_set_long_mode(c->lbl_line1, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(c->lbl_line1, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(c->lbl_line1, p->text_primary, 0);

            c->lbl_line2 = lv_label_create(c->card_btn);
            lv_obj_set_pos(c->lbl_line2, 2, 84);
            lv_obj_set_width(c->lbl_line2, 250);
            lv_label_set_long_mode(c->lbl_line2, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(c->lbl_line2, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(c->lbl_line2, p->text_primary, 0);

            c->lbl_line3 = lv_label_create(c->card_btn);
            lv_obj_set_pos(c->lbl_line3, 2, 108);
            lv_obj_set_width(c->lbl_line3, 250);
            lv_label_set_long_mode(c->lbl_line3, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(c->lbl_line3, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(c->lbl_line3, p->text_secondary, 0);

            if (arrange_mode) {
                c->btn_hide = lv_button_create(c->card_btn);
                lv_obj_set_size(c->btn_hide, 40, 30);
                lv_obj_align(c->btn_hide, LV_ALIGN_TOP_RIGHT, 4, -6);
                lv_obj_set_ext_click_area(c->btn_hide, 8);
                lv_obj_set_style_radius(c->btn_hide, 6, 0);
                lv_obj_set_style_shadow_width(c->btn_hide, 0, 0);
                lv_obj_set_style_border_width(c->btn_hide, 1, 0);
                lv_obj_add_event_cb(c->btn_hide, hide_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)slot);
                c->lbl_hide = lv_label_create(c->btn_hide);
                lv_obj_center(c->lbl_hide);
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Theme Propagation
 * -------------------------------------------------------------------------- */
static void apply_theme(const devos_palette_t *p, void *user_data)
{
    LV_UNUSED(user_data);
    if (!screen) return;

    lv_obj_set_style_bg_color(screen, p->bg, 0);

    /* Telemetry strip */
    if (telemetry_box) {
        lv_obj_set_style_bg_color(telemetry_box, p->telemetry_bg, 0);
        lv_obj_set_style_border_color(telemetry_box, p->surface_border, 0);
        lv_obj_set_style_text_color(lbl_clock_date, p->accent_primary, 0);
        lv_obj_set_style_text_color(lbl_net_power, p->text_primary, 0);
        lv_obj_set_style_text_color(lbl_mem_cpu, p->text_secondary, 0);
    }

    /* Arrange Banner */
    if (banner_arrange) {
        lv_obj_set_style_bg_color(banner_arrange, p->surface_active, 0);
        lv_obj_set_style_border_color(banner_arrange, p->accent_primary, 0);
        lv_obj_set_style_text_color(lbl_arrange_banner, p->accent_primary, 0);
    }

    /* Pagination Footer Bar */
    if (footer_bar) {
        lv_obj_set_style_bg_color(footer_bar, p->telemetry_bg, 0);
        if (lbl_apps_count) lv_obj_set_style_text_color(lbl_apps_count, p->text_secondary, 0);
        if (lbl_page_text) lv_obj_set_style_text_color(lbl_page_text, p->text_primary, 0);
        if (btn_prev) {
            lv_obj_set_style_bg_color(btn_prev, p->surface, 0);
            lv_obj_set_style_border_color(btn_prev, p->surface_border, 0);
            lv_obj_set_style_text_color(lbl_prev, p->text_primary, 0);
        }
        if (btn_next) {
            lv_obj_set_style_bg_color(btn_next, p->surface, 0);
            lv_obj_set_style_border_color(btn_next, p->surface_border, 0);
            lv_obj_set_style_text_color(lbl_next, p->text_primary, 0);
        }
        if (btn_arrange_toggle) {
            lv_obj_set_style_border_color(btn_arrange_toggle, p->surface_border, 0);
            lv_obj_set_style_text_color(lbl_arrange_btn, p->text_primary, 0);
        }
        if (btn_arrange_reset) {
            lv_obj_set_style_bg_color(btn_arrange_reset, p->surface, 0);
            lv_obj_set_style_border_color(btn_arrange_reset, p->surface_border, 0);
            lv_obj_set_style_text_color(lbl_arrange_reset, p->accent_danger, 0);
        }
    }

    /* Bottom Hint Bar */
    if (bottom_bar) {
        lv_obj_set_style_bg_color(bottom_bar, p->bottom_bar_bg, 0);
        lv_obj_set_style_border_color(bottom_bar, p->surface_border, 0);
        lv_obj_set_style_text_color(lbl_bottom_hint, p->text_secondary, 0);
    }

    refresh_cards();
}

/* --------------------------------------------------------------------------
 * App Lifecycle (init, show, hide)
 * -------------------------------------------------------------------------- */
static void launcher_init(void)
{
    const devos_palette_t *p = devos_theme_get();

    /* Discover apps and load layout */
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

    /* 1. Telemetry Strip (y: 42, height: 66) */
    telemetry_box = lv_obj_create(screen);
    lv_obj_set_size(telemetry_box, DEVOS_SCREEN_WIDTH - 32, 66);
    lv_obj_set_pos(telemetry_box, 16, DEVOS_TOP_BAR_HEIGHT + 4);
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
    lv_obj_set_pos(lbl_net_power, 0, 22);
    lv_obj_set_style_text_font(lbl_net_power, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_net_power, p->text_primary, 0);

    lbl_mem_cpu = lv_label_create(telemetry_box);
    lv_obj_set_pos(lbl_mem_cpu, 0, 40);
    lv_obj_set_style_text_font(lbl_mem_cpu, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_mem_cpu, p->text_secondary, 0);

    /* 2. Arrange Banner (y: 112, height: 26) */
    banner_arrange = lv_obj_create(screen);
    lv_obj_set_size(banner_arrange, DEVOS_SCREEN_WIDTH - 32, 26);
    lv_obj_set_pos(banner_arrange, 16, 112);
    lv_obj_set_style_bg_color(banner_arrange, p->surface_active, 0);
    lv_obj_set_style_border_color(banner_arrange, p->accent_primary, 0);
    lv_obj_set_style_border_width(banner_arrange, 1, 0);
    lv_obj_set_style_radius(banner_arrange, 4, 0);
    lv_obj_set_style_pad_all(banner_arrange, 0, 0);
    lv_obj_clear_flag(banner_arrange, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(banner_arrange, LV_OBJ_FLAG_HIDDEN);

    lbl_arrange_banner = lv_label_create(banner_arrange);
    lv_obj_center(lbl_arrange_banner);
    lv_obj_set_style_text_font(lbl_arrange_banner, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_arrange_banner, p->accent_primary, 0);

    /* 3. Multi-Page Carousel, centred between the info strip and the footer */
    carousel = lv_obj_create(screen);
    lv_obj_set_size(carousel, DEVOS_SCREEN_WIDTH, CAROUSEL_HEIGHT);
    lv_obj_set_pos(carousel, 0, CAROUSEL_Y);
    lv_obj_set_style_bg_opa(carousel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(carousel, 0, 0);
    lv_obj_set_style_pad_all(carousel, 0, 0);
    lv_obj_set_scroll_dir(carousel, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(carousel, LV_SCROLL_SNAP_CENTER);
    lv_obj_add_flag(carousel, LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_set_scrollbar_mode(carousel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(carousel, carousel_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);

    /* Build card widgets */
    rebuild_card_widgets();

    /* 4. Pagination Footer Bar: mirrors the info strip (4 px above the hint
     *    bar, as the strip sits 4 px below the top bar). */
    footer_bar = lv_obj_create(screen);
    lv_obj_set_size(footer_bar, DEVOS_SCREEN_WIDTH - 32, FOOTER_HEIGHT);
    lv_obj_set_pos(footer_bar, 16, FOOTER_Y);
    lv_obj_set_style_bg_color(footer_bar, p->telemetry_bg, 0);
    lv_obj_set_style_border_color(footer_bar, p->surface_border, 0);
    lv_obj_set_style_border_width(footer_bar, 1, 0);
    lv_obj_set_style_radius(footer_bar, 6, 0);
    lv_obj_set_style_pad_all(footer_bar, 4, 0);
    lv_obj_clear_flag(footer_bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Left: Apps Installed count */
    lbl_apps_count = lv_label_create(footer_bar);
    lv_obj_set_pos(lbl_apps_count, 14, 12);
    lv_obj_set_style_text_font(lbl_apps_count, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_apps_count, p->text_secondary, 0);

    /* Center Pagination Controls */
    btn_prev = lv_button_create(footer_bar);
    lv_obj_set_size(btn_prev, 85, 34);
    lv_obj_set_pos(btn_prev, 460, 1);
    lv_obj_set_style_bg_color(btn_prev, p->surface, 0);
    lv_obj_set_style_border_color(btn_prev, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_prev, 1, 0);
    lv_obj_set_style_radius(btn_prev, 4, 0);
    lv_obj_add_event_cb(btn_prev, prev_btn_cb, LV_EVENT_CLICKED, NULL);

    lbl_prev = lv_label_create(btn_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_LEFT " Prev");
    lv_obj_center(lbl_prev);
    lv_obj_set_style_text_font(lbl_prev, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_prev, p->text_primary, 0);

    /* Interactive graphical dots container */
    dots_cont = lv_obj_create(footer_bar);
    lv_obj_set_size(dots_cont, 95, 34);
    lv_obj_set_pos(dots_cont, 555, 1);
    lv_obj_set_style_bg_opa(dots_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dots_cont, 0, 0);
    lv_obj_set_style_pad_all(dots_cont, 0, 0);
    lv_obj_clear_flag(dots_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(dots_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Page text info */
    lbl_page_text = lv_label_create(footer_bar);
    lv_obj_set_pos(lbl_page_text, 660, 10);
    lv_obj_set_style_text_font(lbl_page_text, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_page_text, p->text_primary, 0);

    /* Next Button */
    btn_next = lv_button_create(footer_bar);
    lv_obj_set_size(btn_next, 85, 34);
    lv_obj_set_pos(btn_next, 755, 1);
    lv_obj_set_style_bg_color(btn_next, p->surface, 0);
    lv_obj_set_style_border_color(btn_next, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_next, 1, 0);
    lv_obj_set_style_radius(btn_next, 4, 0);
    lv_obj_add_event_cb(btn_next, next_btn_cb, LV_EVENT_CLICKED, NULL);

    lbl_next = lv_label_create(btn_next);
    lv_label_set_text(lbl_next, "Next " LV_SYMBOL_RIGHT);
    lv_obj_center(lbl_next);
    lv_obj_set_style_text_font(lbl_next, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_next, p->text_primary, 0);

    /* Arrange Mode Toggle Button */
    btn_arrange_toggle = lv_button_create(footer_bar);
    lv_obj_set_size(btn_arrange_toggle, 110, 34);
    lv_obj_align(btn_arrange_toggle, LV_ALIGN_RIGHT_MID, -2, 0);   /* far right */
    lv_obj_set_style_bg_color(btn_arrange_toggle, p->surface_active, 0);
    lv_obj_set_style_border_color(btn_arrange_toggle, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_arrange_toggle, 1, 0);
    lv_obj_set_style_radius(btn_arrange_toggle, 4, 0);
    lv_obj_add_event_cb(btn_arrange_toggle, arrange_toggle_cb, LV_EVENT_CLICKED, NULL);

    lbl_arrange_btn = lv_label_create(btn_arrange_toggle);
    lv_label_set_text(lbl_arrange_btn, LV_SYMBOL_SHUFFLE " Arrange");
    lv_obj_center(lbl_arrange_btn);
    lv_obj_set_style_text_font(lbl_arrange_btn, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_arrange_btn, p->text_primary, 0);

    /* Arrange Reset Button (Defaults) */
    btn_arrange_reset = lv_button_create(footer_bar);
    lv_obj_set_size(btn_arrange_reset, 105, 34);
    lv_obj_align_to(btn_arrange_reset, btn_arrange_toggle, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    lv_obj_set_style_bg_color(btn_arrange_reset, p->surface, 0);
    lv_obj_set_style_border_color(btn_arrange_reset, p->surface_border, 0);
    lv_obj_set_style_border_width(btn_arrange_reset, 1, 0);
    lv_obj_set_style_radius(btn_arrange_reset, 4, 0);
    lv_obj_add_event_cb(btn_arrange_reset, arrange_reset_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(btn_arrange_reset, LV_OBJ_FLAG_HIDDEN);

    lbl_arrange_reset = lv_label_create(btn_arrange_reset);
    lv_label_set_text(lbl_arrange_reset, LV_SYMBOL_REFRESH " Defaults");
    lv_obj_center(lbl_arrange_reset);
    lv_obj_set_style_text_font(lbl_arrange_reset, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_arrange_reset, p->accent_danger, 0);

    /* 5. Bottom Navigation Hint Bar (y: 688, height: 32) */
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
    lv_label_set_text(lbl_bottom_hint, HINT_NORMAL);
    lv_obj_center(lbl_bottom_hint);
    lv_obj_set_style_text_font(lbl_bottom_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_bottom_hint, p->text_secondary, 0);

    /* Hook theme updates */
    devos_theme_add_listener(apply_theme, NULL);

    /* Initial visual setup */
    refresh_cards();
    app_launcher_update_telemetry();
}

static void launcher_show(void)
{
    selected_slot = -1;
    int current_reg = devos_core_app_count();
    if (current_reg != last_registered_count) {
        last_registered_count = current_reg;
        load_layout();
        rebuild_card_widgets();
    }
    app_launcher_set_page(current_page, false);
    refresh_cards();
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
    app_descriptor.uid = "launcher";
    app_descriptor.icon = LV_SYMBOL_HOME;
    app_descriptor.category = "system";
    app_descriptor.name = "Launcher";
    app_descriptor.title = "Home Screen";
    app_descriptor.subtitle = "devOS Dashboard";
    app_descriptor.screen = screen;
    app_descriptor.init = launcher_init;
    app_descriptor.show = launcher_show;
    app_descriptor.hide = launcher_hide;
    app_descriptor.handle_key = launcher_handle_key;
    app_descriptor.get_telemetry_lines = NULL;

    return &app_descriptor;
}

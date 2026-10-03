/* Cricket: live scores, results and full scorecards from ESPNcricinfo, for
 * today or any day back to the first Test (devos_cricket).
 *
 * Left: the day's matches (today: live first, refreshed every 30 s). Right:
 * the selected match; Enter loads its scorecard - batting and bowling for
 * each innings, one innings at a time.
 *
 * Keys: Up / Down pick a match, Enter scorecard, Tab list / scorecard,
 * Left / Right (or [ / ]) previous / next day, T today, D go to a date,
 * L live only, R refresh, 1-4 innings, Esc back. In the scorecard Up / Down
 * scroll and Left / Right change innings.
 */
#include "app_cricket.h"
#include "devos_config.h"
#include "devos_core.h"
#include "devos_cricket.h"
#include "devos_focus.h"
#include "devos_icons.h"
#include "devos_theme.h"
#include "devos_widgets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
/* below 16 KB malloc stays internal: put these in PSRAM explicitly */
static void *psram_calloc(size_t n, size_t sz)
{
    void *p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM);
    return p ? p : calloc(n, sz);
}
#else
#define psram_calloc calloc
#endif

#define LIST_W   520
#define CARD_X   (LIST_W + 8)
#define CARD_W   (DEVOS_SCREEN_WIDTH - CARD_X - 8)
#define ROW_H    80
#define TEXT_MAX 6144

static devos_app_descriptor_t s_desc;
static lv_obj_t *s_screen, *lbl_status, *lbl_date, *s_keys, *lbl_empty;
static lv_obj_t *card_panel, *lbl_title, *lbl_meta, *tabs[DEVOS_CRICKET_MAX_INN], *card_scroll, *lbl_card;
static devos_vlist_t s_list;
static devos_w_dialog_t s_dlg;
static lv_obj_t *ta_date;
static devos_focus_t s_fdlg;

static devos_cricket_match_t *s_m;          /* DEVOS_CRICKET_MAX_MATCHES, PSRAM */
static int s_n, s_view[DEVOS_CRICKET_MAX_MATCHES], s_nview;
static devos_cricket_card_t *s_card;        /* PSRAM */
static devos_cricket_status_t s_st;
static char *s_text;                        /* scorecard text, TEXT_MAX */
static uint32_t s_gen = 0xffffffff;
static char s_sel_event[12];
static int s_date;                          /* YYYYMMDD, 0 = today */
static int s_inn;                           /* innings shown */
static bool s_card_pane, s_live_only, s_inited;

/* ------------------------------------------------------------------ helpers */
static const devos_cricket_match_t *sel_match(void)
{
    for (int i = 0; i < s_n; i++)
        if (!strcmp(s_m[i].event, s_sel_event)) return &s_m[i];
    return NULL;
}

static int sel_row(void)
{
    for (int i = 0; i < s_nview; i++)
        if (!strcmp(s_m[s_view[i]].event, s_sel_event)) return i;
    return -1;
}

static void fmt_date(int ymd, char *out, size_t cap)
{
    static const char *const MON[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int m = ymd / 100 % 100;
    snprintf(out, cap, "%d %s %d", ymd % 100, MON[(m >= 1 && m <= 12) ? m - 1 : 0], ymd / 10000);
}

static void fmt_start(int64_t t, char *out, size_t cap)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(out, cap, "%a %d %b %H:%M", &tm);
}

static int shown_date(void) { return s_date ? s_date : devos_cricket_today(); }

/* ------------------------------------------------------------------ list */
static void row_draw(devos_vlist_t *v, lv_layer_t *layer, const lv_area_t *row, int idx)
{
    LV_UNUSED(v);
    if (idx >= s_nview) return;
    const devos_palette_t *p = devos_theme_get();
    const devos_cricket_match_t *m = &s_m[s_view[idx]];
    int x = row->x1 + 12, y = row->y1 + 6, w = lv_area_get_width(row) - 24;
    char buf[96];
    snprintf(buf, sizeof(buf), "%s%s%s", m->series, m->title[0] ? "  -  " : "", m->title);
    devos_w_draw_text(layer, &lv_font_montserrat_12, x, y, w - 110, buf, p->text_muted);
    lv_color_t bc = m->state == DEVOS_CRICKET_LIVE ? p->accent_danger : m->state == DEVOS_CRICKET_PRE ? p->accent_secondary : p->text_muted;
    const char *badge = m->state == DEVOS_CRICKET_LIVE ? "LIVE" : m->state == DEVOS_CRICKET_DONE ? "RESULT" : "";
    if (m->state == DEVOS_CRICKET_PRE && m->start) {
        fmt_start(m->start, buf, sizeof(buf));
        badge = buf;
    }
    devos_w_draw_text(layer, &lv_font_montserrat_12, x + w - 104, y, 104, badge, bc);
    for (int s = 0; s < 2; s++) {
        const devos_cricket_side_t *sd = &m->side[s];
        lv_color_t c = m->state == DEVOS_CRICKET_DONE && !sd->winner ? p->text_secondary : p->text_primary;
        devos_w_draw_text(layer, &lv_font_montserrat_16, x, y + 18 + s * 20, 250, sd->name[0] ? sd->name : "-", c);
        devos_w_draw_text(layer, &lv_font_montserrat_16, x + 256, y + 18 + s * 20, w - 256, sd->score, c);
    }
    lv_color_t sc = m->state == DEVOS_CRICKET_LIVE ? p->accent_warning : p->text_secondary;
    devos_w_draw_text(layer, &lv_font_montserrat_12, x, y + 60 - 2, w, m->status, sc);
}

static void build_view(void)
{
    s_nview = 0;
    /* today: live first, then upcoming, then results (the API's series order within each) */
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < s_n; i++) {
            devos_cricket_state_t st = s_m[i].state;
            int rank = s_date ? 0 : st == DEVOS_CRICKET_LIVE ? 0 : st == DEVOS_CRICKET_PRE ? 1 : 2;
            if (rank != pass) continue;
            if (s_live_only && st != DEVOS_CRICKET_LIVE) continue;
            s_view[s_nview++] = i;
        }
        if (s_date) break;
    }
}

/* ------------------------------------------------------------------ scorecard */
static void render_card(void)
{
    const devos_palette_t *p = devos_theme_get();
    const devos_cricket_match_t *m = sel_match();
    bool have = s_card->loaded && m && !strcmp(s_card->event, m->event);
    char meta[512];
    if (!m) {
        devos_w_set_text(lbl_title, "");
        devos_w_set_text(lbl_meta, s_nview ? "Up / Down to pick a match." : "");
        lv_label_set_text(lbl_card, "");
        for (int i = 0; i < DEVOS_CRICKET_MAX_INN; i++) lv_obj_add_flag(tabs[i], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char title[128], when[40] = "";
    snprintf(title, sizeof(title), "%s v %s%s%s", m->side[0].name, m->side[1].name, m->title[0] ? " - " : "", m->title);
    devos_w_set_text(lbl_title, title);
    if (m->start) fmt_start(m->start, when, sizeof(when));
    int o = snprintf(meta, sizeof(meta), "%s%s%s\n%s%s%s", m->series, m->format[0] ? "  -  " : "", m->format,
                     have && s_card->venue[0] ? s_card->venue : m->venue, when[0] ? "  -  " : "", when);
    if (have && s_card->toss[0] && o < (int)sizeof(meta)) o += snprintf(meta + o, sizeof(meta) - (size_t)o, "\nToss: %s", s_card->toss);
    const char *res = have && s_card->result[0] ? s_card->result : m->status;
    if (res[0] && o < (int)sizeof(meta)) snprintf(meta + o, sizeof(meta) - (size_t)o, "\n%s", res);
    devos_w_set_text(lbl_meta, meta);

    for (int i = 0; i < DEVOS_CRICKET_MAX_INN; i++) {
        if (!have || i >= s_card->ninn) {
            lv_obj_add_flag(tabs[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        char t[64];
        const devos_cricket_innings_t *in = &s_card->inn[i];
        snprintf(t, sizeof(t), "[%d] %.24s %.30s", i + 1, in->team, in->total);
        lv_label_set_text(tabs[i], t);
        lv_obj_remove_flag(tabs[i], LV_OBJ_FLAG_HIDDEN);
        devos_w_track(tabs[i], i == s_inn ? DEVOS_W_BADGE : DEVOS_W_TEXT_DIM);
        lv_obj_set_style_bg_opa(tabs[i], i == s_inn ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    LV_UNUSED(p);

    if (!have) {
        const char *msg = s_card->loading && !strcmp(s_card->event, m->event) ? "Loading the scorecard..."
                          : s_card->error[0] && !strcmp(s_card->event, m->event) ? s_card->error
                          : m->state == DEVOS_CRICKET_PRE ? "Not started yet. Enter loads the teams' scorecard when play begins."
                                                          : "Enter: full scorecard";
        lv_label_set_text(lbl_card, msg);
        return;
    }
    if (!s_card->ninn) {
        lv_label_set_text(lbl_card, "No scorecard yet.");
        return;
    }
    if (s_inn >= s_card->ninn) s_inn = s_card->ninn - 1;
    const devos_cricket_innings_t *in = &s_card->inn[s_inn];
    size_t n = 0, cap = TEXT_MAX;
#define OUT(...) do { if (n < cap) n += (size_t)snprintf(s_text + n, cap - n, __VA_ARGS__); } while (0)
    OUT("%s  %s\n\n", in->team, in->total);
    OUT("%-24s %-9s %4s %4s %3s %3s %6s\n", "BATTER", "", "R", "B", "4s", "6s", "SR");
    for (int k = 0; k < in->nbat; k++) {
        const devos_cricket_bat_t *b = &in->bat[k];
        char sr[8] = "-";
        if (b->balls) snprintf(sr, sizeof(sr), "%.1f", 100.0 * b->runs / b->balls);
        OUT("%-24.24s %-9.9s %4d %4d %3d %3d %6s\n", b->name, b->how, b->runs, b->balls, b->fours, b->sixes, sr);
    }
    if (in->nbowl) {
        OUT("\n%-24s %6s %4s %4s %4s %6s\n", "BOWLER", "O", "M", "R", "W", "Econ");
        for (int k = 0; k < in->nbowl; k++) {
            const devos_cricket_bowl_t *w = &in->bowl[k];
            char ex[24] = "";
            if (w->wides && w->noballs) snprintf(ex, sizeof(ex), "  (%dw %dnb)", w->wides, w->noballs);
            else if (w->wides) snprintf(ex, sizeof(ex), "  (%dw)", w->wides);
            else if (w->noballs) snprintf(ex, sizeof(ex), "  (%dnb)", w->noballs);
            OUT("%-24.24s %6s %4d %4d %4d %6.2f%s\n", w->name, w->overs, w->maidens, w->runs, w->wickets, (double)w->econ, ex);
        }
    }
    if (s_card->updated) {
        char t[16];
        time_t u = (time_t)s_card->updated;
        struct tm tm;
        localtime_r(&u, &tm);
        strftime(t, sizeof(t), "%H:%M:%S", &tm);
        OUT("\nESPNcricinfo  -  updated %s%s", t, s_card->live ? "  (live: refreshes every 30 s)" : "");
    }
#undef OUT
    lv_label_set_text(lbl_card, s_text);
}

static void open_card(void)
{
    const devos_cricket_match_t *m = sel_match();
    if (!m) return;
    if (strcmp(s_card->event, m->event)) s_inn = 0;
    devos_cricket_open(m->league, m->event);
    s_card_pane = true;
    s_gen = 0xffffffff;
}

static void set_inn(int i)
{
    if (!s_card->loaded || i < 0 || i >= s_card->ninn) return;
    s_inn = i;
    lv_obj_scroll_to_y(card_scroll, 0, LV_ANIM_OFF);
    render_card();
}

static void tab_cb(lv_event_t *e)
{
    set_inn((int)(intptr_t)lv_event_get_user_data(e));
    s_card_pane = true;
}

/* ------------------------------------------------------------------ list events */
static void on_select(devos_vlist_t *v, int idx)
{
    LV_UNUSED(v);
    if (idx >= 0 && idx < s_nview) snprintf(s_sel_event, sizeof(s_sel_event), "%s", s_m[s_view[idx]].event);
    render_card();
}

static void on_activate(devos_vlist_t *v, int idx)
{
    on_select(v, idx);
    open_card();
}

/* ------------------------------------------------------------------ data */
static void ingest(void)
{
    devos_cricket_status(&s_st);
    s_n = devos_cricket_matches(s_m, DEVOS_CRICKET_MAX_MATCHES);
    devos_cricket_card(s_card);
    build_view();
    devos_vlist_set_count(&s_list, s_nview);
    s_list.sel = sel_row();
    if (s_list.sel < 0 && s_nview) {
        s_list.sel = 0;
        snprintf(s_sel_event, sizeof(s_sel_event), "%s", s_m[s_view[0]].event);
    }
    devos_vlist_redraw(&s_list);
    render_card();
}

static void status_line(void)
{
    char buf[200], d[24];
    char td[20] = "";
    if (s_date) fmt_date(s_date, d, sizeof(d));
    else if (devos_cricket_today()) {
        fmt_date(devos_cricket_today(), td, sizeof(td));
        snprintf(d, sizeof(d), "Today, %.14s", td);
    } else snprintf(d, sizeof(d), "Today");
    devos_w_set_text(lbl_date, d);
    if (!s_st.clock_ok && s_date == 0 && !s_st.updated)
        snprintf(buf, sizeof(buf), "Waiting for the network...");
    else if (s_st.error[0])
        snprintf(buf, sizeof(buf), "%s", s_st.error);
    else if (s_st.loading && !s_st.updated)
        snprintf(buf, sizeof(buf), "Loading from ESPNcricinfo...");
    else
        snprintf(buf, sizeof(buf), "%d match%s%s, %d live%s%s", s_st.count, s_st.count == 1 ? "" : "es",
                 s_date ? "" : " today", s_st.live, s_live_only ? "  -  showing live only (L)" : "",
                 s_st.loading ? "  -  updating..." : "");
    devos_w_set_text(lbl_status, buf);
    devos_w_track(lbl_status, s_st.error[0] ? DEVOS_W_TEXT_ERR : DEVOS_W_TEXT_DIM);
    if (s_nview) {
        lv_obj_add_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
        devos_w_set_text(lbl_empty, s_st.loading || !s_st.updated ? "Loading..."
                                    : s_live_only                 ? "No live matches right now.\nL shows every match."
                                                                  : "No matches on this day.\nLeft / Right: another day,  D: go to a date.");
    }
    /* whichever pane has the keyboard shows it */
    const devos_palette_t *p = devos_theme_get();
    s_list.active = !s_card_pane;
    lv_obj_set_style_border_color(card_panel, s_card_pane ? p->accent_primary : p->surface_border, 0);
}

/* ------------------------------------------------------------------ dates */
static void go_date(int ymd)
{
    int today = devos_cricket_today();
    if (!today && ymd) {
        devos_w_set_text(lbl_status, "The clock isn't set yet (it's set over the network) - can't browse by date.");
        return;
    }
    if (ymd == today) ymd = 0;
    s_date = ymd;
    s_sel_event[0] = '\0';
    s_card_pane = false;
    devos_cricket_open(NULL, NULL);
    devos_cricket_set_date(ymd);
    s_gen = 0xffffffff;
}

static void step_day(int dir)
{
    int d = shown_date();
    if (!d) {
        go_date(1);                                 /* shows the "clock not set" message */
        return;
    }
    go_date(devos_cricket_date_add(d, dir));
}

static void prev_cb(lv_event_t *e) { LV_UNUSED(e); step_day(-1); }
static void next_cb(lv_event_t *e) { LV_UNUSED(e); step_day(1); }
static void today_cb(lv_event_t *e) { LV_UNUSED(e); go_date(0); }

static void dlg_open(void)
{
    char b[40] = "YYYY-MM-DD";
    int d = shown_date();
    if (d) snprintf(b, sizeof(b), "YYYY-MM-DD  (showing %04d-%02d-%02d)", d / 10000, d / 100 % 100, d % 100);
    lv_textarea_set_text(ta_date, "");
    lv_textarea_set_placeholder_text(ta_date, b);
    devos_w_set_text(s_dlg.msg, "");
    devos_w_dialog_show(&s_dlg, true);
    devos_focus_set(&s_fdlg, ta_date);
}

static void dlg_close(void)
{
    devos_w_dialog_show(&s_dlg, false);
    devos_focus_clear(&s_fdlg);
}

static void dlg_ok(void)
{
    int y = 0, m = 0, d = 0;
    const char *t = lv_textarea_get_text(ta_date);
    bool ok = sscanf(t, "%d-%d-%d", &y, &m, &d) == 3 || (strlen(t) == 8 && sscanf(t, "%4d%2d%2d", &y, &m, &d) == 3);
    if (!ok || y < 1877 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31) {
        devos_w_set_text(s_dlg.msg, "Enter a date as YYYY-MM-DD (from 1877, the first Test)");
        return;
    }
    dlg_close();
    go_date(y * 10000 + m * 100 + d);
}

static void dlg_ok_cb(lv_event_t *e) { LV_UNUSED(e); dlg_ok(); }
static void dlg_cancel_cb(lv_event_t *e) { LV_UNUSED(e); dlg_close(); }
static void date_cb(lv_event_t *e) { LV_UNUSED(e); dlg_open(); }

/* ------------------------------------------------------------------ tick + keys */
static const char *keys_text(void)
{
    if (devos_w_dialog_open(&s_dlg)) return "Type a date    Tab / arrows move    Enter goes    Esc cancels";
    if (s_card_pane)
        return "Up / Down scroll    Left / Right or 1-4 innings    Tab / Esc match list    [ / ] day    R refresh    Esc back";
    return "Up / Down pick    Enter scorecard    Tab scorecard    Left / Right day    T today    D date    L live only    R refresh    Esc home";
}

static void tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!s_screen || lv_obj_has_flag(s_screen, LV_OBJ_FLAG_HIDDEN)) return;
    uint32_t g = devos_cricket_generation();
    if (g != s_gen) {
        s_gen = g;
        ingest();
    }
    devos_cricket_status(&s_st);
    status_line();
    devos_w_set_text(s_keys, keys_text());
}

static bool cricket_key(uint32_t key, uint8_t mods)
{
    if (devos_w_dialog_open(&s_dlg)) {
        if (key == LV_KEY_ESC) dlg_close();
        else if (devos_focus_key(&s_fdlg, key, mods)) {}
        else if (key == '\r' || key == '\n') dlg_ok();
        return true;
    }
    if (mods & (DEVOS_MOD_CTRL | DEVOS_MOD_ALT | DEVOS_MOD_FN)) return false;
    if (key == '\t') {
        s_card_pane = !s_card_pane;
        return true;
    }
    if (s_card_pane) {
        switch (key) {
        case LV_KEY_UP: lv_obj_scroll_by_bounded(card_scroll, 0, 3 * 18, LV_ANIM_OFF); return true;
        case LV_KEY_DOWN: lv_obj_scroll_by_bounded(card_scroll, 0, -3 * 18, LV_ANIM_OFF); return true;
        case DEVOS_KEY_PGUP: lv_obj_scroll_by_bounded(card_scroll, 0, lv_obj_get_height(card_scroll) - 40, LV_ANIM_OFF); return true;
        case DEVOS_KEY_PGDN: lv_obj_scroll_by_bounded(card_scroll, 0, -(lv_obj_get_height(card_scroll) - 40), LV_ANIM_OFF); return true;
        case LV_KEY_LEFT: set_inn(s_inn - 1); return true;
        case LV_KEY_RIGHT: set_inn(s_inn + 1); return true;
        case '\r': case '\n': open_card(); return true;
        case LV_KEY_ESC: s_card_pane = false; return true;
        default: break;
        }
    } else {
        if (key == LV_KEY_LEFT) { step_day(-1); return true; }
        if (key == LV_KEY_RIGHT) { step_day(1); return true; }
        if (devos_vlist_key(&s_list, key)) return true;
    }
    switch (key) {
    case '[': step_day(-1); return true;
    case ']': step_day(1); return true;
    case '1': case '2': case '3': case '4': set_inn((int)(key - '1')); s_card_pane = s_card->loaded; return true;
    case 't': case 'T': go_date(0); return true;
    case 'd': case 'D': dlg_open(); return true;
    case 'l': case 'L':
        s_live_only = !s_live_only;
        s_gen = 0xffffffff;
        return true;
    case 'r': case 'R': devos_cricket_refresh(); return true;
    case LV_KEY_ESC: return false;                  /* Home Screen */
    default: return key >= 32 && key < 127;
    }
}

/* ------------------------------------------------------------------ init */
static void cricket_init(void)
{
    if (s_inited) return;
    s_inited = true;
    devos_cricket_init();
    s_m = psram_calloc(DEVOS_CRICKET_MAX_MATCHES, sizeof(*s_m));
    s_card = psram_calloc(1, sizeof(*s_card));
    s_text = psram_calloc(1, TEXT_MAX);

    s_screen = devos_w_screen(&s_desc);
    lv_obj_t *bar = devos_w_bar(s_screen, "Cricket", NULL);
    lbl_status = devos_w_label(bar, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "");
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_status, 700);
    lv_obj_align(lbl_status, LV_ALIGN_LEFT_MID, 110, 0);
    lv_obj_t *bnext = devos_w_btn(bar, LV_SYMBOL_RIGHT, 36, next_cb, NULL, NULL);
    lv_obj_align(bnext, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_t *bdate = devos_w_btn(bar, "", 170, date_cb, NULL, &lbl_date);
    lv_obj_align_to(bdate, bnext, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    lv_obj_t *bprev = devos_w_btn(bar, LV_SYMBOL_LEFT, 36, prev_cb, NULL, NULL);
    lv_obj_align_to(bprev, bdate, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    lv_obj_t *btoday = devos_w_btn(bar, "Today", 70, today_cb, NULL, NULL);
    lv_obj_align_to(btoday, bprev, LV_ALIGN_OUT_LEFT_MID, -12, 0);

    int h = DEVOS_CONTENT_HEIGHT - DEVOS_W_BAR_H - DEVOS_W_KEYS_H;
    lv_obj_t *lp = devos_w_panel(s_screen, 0, DEVOS_W_BAR_H, LIST_W, h, DEVOS_W_CODE);
    lv_obj_set_style_border_side(lp, LV_BORDER_SIDE_RIGHT, 0);
    devos_vlist_create(&s_list, lp, ROW_H, row_draw);
    s_list.on_select = on_select;
    s_list.on_activate = on_activate;
    s_list.active = true;
    lv_obj_set_pos(s_list.scroll, 0, 0);
    lv_obj_set_size(s_list.scroll, LIST_W - 1, h);
    lbl_empty = devos_w_label(lp, &lv_font_montserrat_14, DEVOS_W_TEXT_DIM, "");
    lv_obj_set_style_text_align(lbl_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_empty, LV_ALIGN_CENTER, 0, 0);

    card_panel = devos_w_panel(s_screen, CARD_X, DEVOS_W_BAR_H + 8, CARD_W, h - 16, DEVOS_W_PANEL);
    lv_obj_set_style_radius(card_panel, 6, 0);
    lv_obj_set_style_border_width(card_panel, 2, 0);
    lbl_title = devos_w_label(card_panel, &lv_font_montserrat_20, DEVOS_W_TEXT_ACCENT, "");
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_title, CARD_W - 32);
    lv_obj_set_pos(lbl_title, 14, 10);
    lbl_meta = devos_w_label(card_panel, &lv_font_montserrat_14, DEVOS_W_TEXT_DIM, "");
    lv_label_set_long_mode(lbl_meta, LV_LABEL_LONG_DOT);
    lv_obj_set_size(lbl_meta, CARD_W - 32, 84);
    lv_obj_set_pos(lbl_meta, 14, 40);
    for (int i = 0; i < DEVOS_CRICKET_MAX_INN; i++) {
        tabs[i] = devos_w_label(card_panel, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "");
        lv_obj_set_style_pad_hor(tabs[i], 6, 0);
        lv_obj_set_style_pad_ver(tabs[i], 3, 0);
        lv_obj_set_style_radius(tabs[i], 4, 0);
        lv_obj_set_pos(tabs[i], 14 + (i % 2) * ((CARD_W - 32) / 2), 128 + (i / 2) * 24);
        lv_obj_add_flag(tabs[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(tabs[i], tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    card_scroll = lv_obj_create(card_panel);
    lv_obj_remove_style_all(card_scroll);
    lv_obj_set_pos(card_scroll, 14, 180);
    lv_obj_set_size(card_scroll, CARD_W - 32, h - 16 - 180 - 12);
    lv_obj_set_scroll_dir(card_scroll, LV_DIR_VER);
    lbl_card = devos_w_label(card_scroll, devos_w_mono(), DEVOS_W_TEXT, "");
    lv_obj_set_width(lbl_card, CARD_W - 40);

    s_keys = devos_w_keys(s_screen);

    devos_w_dialog(&s_dlg, s_screen, 520, 220, "Go to a date");
    ta_date = devos_w_field(s_dlg.box, "Date - any day since 1877", 0, 34, 476);
    lv_textarea_set_accepted_chars(ta_date, "0123456789-");
    lv_textarea_set_max_length(ta_date, 10);
    lv_obj_t *bok = devos_w_btn_kind(s_dlg.box, DEVOS_W_BTN_PRIMARY, LV_SYMBOL_OK "  Go", 120, dlg_ok_cb, NULL, NULL);
    lv_obj_set_size(bok, 120, 36);
    lv_obj_align(bok, LV_ALIGN_BOTTOM_RIGHT, -132, 0);
    lv_obj_t *bc = devos_w_btn(s_dlg.box, "Cancel", 120, dlg_cancel_cb, NULL, NULL);
    lv_obj_set_size(bc, 120, 36);
    lv_obj_align(bc, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    devos_focus_init(&s_fdlg);
    devos_focus_add(&s_fdlg, ta_date);
    devos_focus_add(&s_fdlg, bok);
    devos_focus_add(&s_fdlg, bc);

    lv_timer_create(tick_cb, 250, NULL);
}

static void cricket_show(void)
{
    devos_cricket_set_active(true);
    s_gen = 0xffffffff;
}

static void cricket_hide(void) { devos_cricket_set_active(false); }

static int cricket_telemetry(char lines[3][64])
{
    devos_cricket_status_t st;
    devos_cricket_status(&st);
    if (st.updated && st.date == 0) {
        snprintf(lines[0], 64, "* %d live now", st.live);
        snprintf(lines[1], 64, "* %d matches today", st.count);
        return 2;
    }
    snprintf(lines[0], 64, "* Live scores & scorecards");
    snprintf(lines[1], 64, "* Any day since 1877");
    return 2;
}

/* Sym+S sheet (devos_shortcuts.h) */
static const char *cricket_shortcuts(void)
{
    return
        "Matches\n"
        "Up / Down\tPick a match\n"
        "Enter / Tab\tIts scorecard\n"
        "Left / Right, [ / ]\tDay before / after\n"
        "T\tToday\n"
        "D\tGo to a date\n"
        "L\tLive matches only\n"
        "R\tRefresh\n"
        "Scorecard\n"
        "Up / Down, Sym+Up / Down\tScroll\n"
        "Left / Right, 1 ... 4\tInnings\n"
        "Enter\tReload\n"
        "Esc / Tab\tBack to the matches\n";
}

devos_app_descriptor_t *app_cricket_get_descriptor(void)
{
    s_desc.id = DEVOS_APP_LAUNCHER;                 /* auto-assigned */
    s_desc.uid = "cricket";
    s_desc.icon = LV_SYMBOL_LIST;
    s_desc.draw_icon = devos_icon_cricket;
    s_desc.category = "tools";
    s_desc.name = "Cricket";
    s_desc.title = "Cricket";
    s_desc.subtitle = "ESPNcricinfo scores";
    s_desc.init = cricket_init;
    s_desc.show = cricket_show;
    s_desc.hide = cricket_hide;
    s_desc.handle_key = cricket_key;
    s_desc.get_telemetry_lines = cricket_telemetry;
    s_desc.get_shortcuts = cricket_shortcuts;
    s_desc.default_off = true;                      /* hidden: off until enabled in Settings > Apps */
    return &s_desc;
}

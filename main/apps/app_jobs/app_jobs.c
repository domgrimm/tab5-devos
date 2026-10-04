/* app_jobs: the Jobs app (PLAN.md 11). Builder is the default mode; Text is
 * one key away. Both edit one AST/source, so they never diverge.
 *
 *   Builder: a trigger card, a touch-draggable step tree, and an inspector
 *   generated from the action registry. Conditions, `set` values and
 *   expression-capable parameters are editable as full expressions; anything
 *   the Builder cannot render shows a custom-node card.
 *   Text: the raw source, with the same toolbar.
 *
 * Commands are on Sym+<key> so they work even while typing in a field
 * (AGENTS.md invariant 9). Plain letters still work outside a field.
 *
 * Rules (AGENTS.md): widgets/theme (#5), keyboard first (#9) via devos_focus,
 * modular app (#8) - no launcher edits. */
#include "app_jobs.h"
#include "devos_jobs.h"
#include "jobs_build.h"
#include "devos_actions.h"
#include "devos_events.h"
#include "devos_config.h"
#include "devos_widgets.h"
#include "devos_codeview.h"
#include "devos_focus.h"
#include "devos_icons.h"
#include "devos_theme.h"
#include "devos_toast.h"
#include "devos_cmdpal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>

#define OUT_MAX 4096
#define SRCMAX  (JOBS_MAX_SOURCE + 64)
#define STEP_ROWS 32
#define STEP_H 30
#define PARAM_MAX 12                        /* per-parameter form rows */

typedef enum { MODE_TEXT = 0, MODE_BUILDER } ui_mode_t;
typedef enum { INSP_NONE = 0, INSP_ACTION, INSP_IF, INSP_SET, INSP_WAIT, INSP_REPEAT, INSP_CUSTOM } insp_t;
typedef enum { TAB_OVERVIEW = 0, TAB_BUILDER, TAB_TEXT, TAB_RUNS, TAB_COUNT } jobs_tab_t;

typedef struct {
    lv_obj_t *screen;
    /* left job list (P1: DEVOS_PANE_LEFT_WIDTH, Sym+L) */
    lv_obj_t *list_pane;
    lv_obj_t *list_row[DEVOS_JOBS_MAX], *list_lbl[DEVOS_JOBS_MAX];
    bool list_visible;
    /* header */
    lv_obj_t *lbl_header, *lbl_name, *lbl_state, *lbl_mode, *lbl_hint;
    lv_obj_t *btn_pause;
    /* tabs */
    lv_obj_t *tab_btn[TAB_COUNT];
    jobs_tab_t tab;
    /* views */
    lv_obj_t *ov, *runs, *bld;
    lv_obj_t *lbl_ov_trigger, *lbl_ov_next, *lbl_ov_runs, *lbl_ov_policy;
    lv_obj_t *btn_revs;
    lv_obj_t *runs_panel;
    devos_codeview_t runs_cv;
    lv_obj_t *ta_src;
    lv_obj_t *lbl_problems;
    lv_obj_t *btn_new, *btn_validate, *btn_apply, *btn_enable, *btn_run, *btn_cancel, *btn_hist, *btn_del;
    /* Builder */
    lv_obj_t *dd_kind, *ta_trig, *ta_where, *dd_topic, *dd_hour, *dd_min;
    lv_obj_t *step_list, *step_row[STEP_ROWS], *step_lbl[STEP_ROWS];
    lv_obj_t *lbl_settings;
    lv_obj_t *dd_add, *ta_add_filter, *btn_add, *btn_bdel, *btn_up, *btn_dn;
    lv_obj_t *dd_param, *dd_expr, *ta_val, *dd_choice, *lbl_custom;
    /* P2: per-parameter vertical form (one row per action parameter) */
    lv_obj_t *form;                          /* scrollable container */
    lv_obj_t *form_rows[PARAM_MAX];
    lv_obj_t *form_lbl[PARAM_MAX];
    lv_obj_t *form_help[PARAM_MAX];
    lv_obj_t *form_ta[PARAM_MAX];
    lv_obj_t *form_dd[PARAM_MAX];
    lv_obj_t *form_expr[PARAM_MAX];
    const devos_action_param_t *form_p[PARAM_MAX];
    int form_n;
    lv_obj_t *form_out_ta;                   /* "Save result as" */
    lv_obj_t *form_out_lbl;
    lv_obj_t *form_hdr;                      /* action title + effect line */
    /* Job settings dialog (name + policy) */
    devos_w_dialog_t dlg_job;
    lv_obj_t *job_ta_name, *job_ta_timeout, *job_dd_overlap, *job_ta_cooldown;
    devos_focus_t job_focus;
    jobs_build_t build;
    bool build_ok;
    jobs_build_row_t rows[JOBS_BUILD_ROWS];
    int rows_n;
    int bstep, bparam;
    insp_t insp;
    bool insp_expr;
    lv_obj_t *out_scroll;
    devos_codeview_t cv;
    devos_focus_t focus;
    devos_focus_t list_focus;
    char ids[DEVOS_JOBS_MAX][DEVOS_JOBS_ID_MAX];
    int n, sel;
    uint32_t base_rev;
    bool applied_clean;                     /* nothing to apply since the last Apply */
    ui_mode_t mode;
    char out[OUT_MAX];
    bool dirty;                             /* unsaved edits vs the applied revision */
    char cur_name[DEVOS_JOBS_NAME_MAX];
} jobs_ctx_t;

static devos_app_descriptor_t s_desc;
static jobs_ctx_t s_ctx;
static bool s_syncing;                      /* text set programmatically, not a user edit */
static bool s_in_list;                      /* the job list has the keyboard (P1 model) */
static bool s_job_open;                     /* the Job settings dialog has the keyboard */

/* Delete confirmation (P0: never delete a job with one key). */
static devos_w_dialog_t s_del_dlg;
static bool s_del_open;
static char s_del_id[DEVOS_JOBS_ID_MAX];
static char s_del_name[DEVOS_JOBS_NAME_MAX];

/* Event-topic picker: topic ids parallel to the dropdown's options. */
#define TOPIC_OPTS_MAX (DEVOS_EVENTS_MAX_TOPICS + 1)
static char s_topic_ids[TOPIC_OPTS_MAX][DEVOS_EVENTS_TOPIC_MAX];
static int s_topic_n;

/* New-job picker: Blank / Duplicate / jobs/examples starters */
#define NEW_EX_MAX 8
static devos_w_dialog_t s_new_dlg;
static bool s_new_open;
static lv_obj_t *s_new_dd;
static char s_new_files[NEW_EX_MAX][48];
static int s_new_n;
static devos_focus_t s_new_focus;

/* Dry run: async start + tick poll so the UI never blocks. */
static devos_w_dialog_t s_dry_dlg;
static bool s_dry_open;
static bool s_dry_active;
static devos_codeview_t s_dry_cv;
static EXT_RAM_BSS_ATTR char s_dry_text[4096];
static EXT_RAM_BSS_ATTR devos_dryrun_t s_dry_res;
static EXT_RAM_BSS_ATTR char s_dry_src[SRCMAX];

/* Revisions view: list generations, view/diff one, roll back (adds a rev). */
static devos_w_dialog_t s_rev_dlg;
static bool s_rev_open;
static lv_obj_t *s_rev_dd, *s_rev_mode;
static devos_codeview_t s_rev_cv;
static uint32_t s_rev_nums[32];
static int s_rev_n;
static EXT_RAM_BSS_ATTR char s_rev_old[SRCMAX];
static EXT_RAM_BSS_ATTR char s_rev_text[8192];
static devos_focus_t s_rev_focus;

static void set_mode(ui_mode_t m);
static void set_tab(jobs_tab_t t);
static bool selected_summary(devos_job_summary_t *out);
static void refresh_overview(void);
static void refresh_runs(void);
static void refresh_problems(void);
static void update_footer(void);
static void refresh_add_picker(void);
static void form_build(const jobs_node_t *node, const devos_action_descriptor_t *d);
static void form_commit(const jobs_node_t *node);
static void open_job_settings(void);
static void commit_job_settings(void);
static void close_job_settings(void);
static void open_revisions(void);
static void rev_rollback(void);
static void rev_close(void);

static const char *NEW_TEMPLATE =
    "version 1;\n"
    "job \"New job\" {\n"
    "    trigger manual;\n"
    "    system.log(message: \"hello from Jobs\");\n"
    "}\n";

static void say(const char *t)
{
    if (t && t[0]) devos_toast_show(t, DEVOS_TOAST_INFO, 0);
}
static void set_visible(lv_obj_t *o, bool v)
{
    if (!o) return;
    if (v) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void fmt_dur(int64_t ms, char *out, size_t cap)
{
    if (ms != 0 && ms % 3600000 == 0) snprintf(out, cap, "%lldh", (long long)(ms / 3600000));
    else if (ms != 0 && ms % 60000 == 0) snprintf(out, cap, "%lldm", (long long)(ms / 60000));
    else if (ms != 0 && ms % 1000 == 0) snprintf(out, cap, "%llds", (long long)(ms / 1000));
    else snprintf(out, cap, "%lldms", (long long)ms);
}
static int64_t parse_dur(const char *s)
{
    if (!s) return -1;
    char *end;
    long long v = strtoll(s, &end, 10);
    if (end == s) return -1;
    while (*end == ' ') end++;
    if (*end == '\0') return v;
    if (strcmp(end, "ms") == 0) return v;
    if (strcmp(end, "s") == 0) return v * 1000;
    if (strcmp(end, "m") == 0) return v * 60000;
    if (strcmp(end, "h") == 0) return v * 3600000;
    return -1;
}

/* ---- left job list (P1) ---- */
static void list_highlight(void)
{
    const devos_palette_t *p = devos_theme_get();
    for (int i = 0; i < s_ctx.n && i < DEVOS_JOBS_MAX; i++) {
        lv_obj_t *r = s_ctx.list_row[i];
        if (!r) continue;
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        if (i == s_ctx.sel) {
            lv_obj_set_style_border_width(r, 3, 0);
            lv_obj_set_style_border_side(r, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_color(r, p->accent_primary, 0);
        } else {
            lv_obj_set_style_border_width(r, 0, 0);
        }
    }
}

static void list_refresh(void)
{
    devos_job_summary_t sum;
    s_ctx.n = 0;
    for (int i = 0; i < devos_jobs_count() && s_ctx.n < DEVOS_JOBS_MAX; i++) {
        if (!devos_jobs_summary_at(i, &sum)) continue;
        if (sum.id[0] == '~') continue;           /* in-memory dry run, never listed */
        snprintf(s_ctx.ids[s_ctx.n], sizeof(s_ctx.ids[0]), "%s", sum.id);
        char g = sum.running ? '>'
                : sum.state != DEVOS_JOB_ENABLED ? 'o'
                : (sum.last_run_wall_s && !sum.last_ok) ? '!' : '*';
        char detail[96];
        if (sum.running) snprintf(detail, sizeof(detail), "running %lldms",
                                  (long long)0);      /* filled by tick from the run snapshot */
        else if (sum.last_result[0]) snprintf(detail, sizeof(detail), "%s", sum.last_result);
        else snprintf(detail, sizeof(detail), "never run");
        char line2[128];
        if (sum.next_run_in_ms > 0) {
            char d[16];
            fmt_dur(sum.next_run_in_ms, d, sizeof(d));
            snprintf(line2, sizeof(line2), "%s  -  next %s", sum.trigger, d);
        } else {
            snprintf(line2, sizeof(line2), "%s  -  %s", sum.trigger, detail);
        }
        char b[DEVOS_JOBS_NAME_MAX + 160];
        snprintf(b, sizeof(b), "%c %s\n%s", g, sum.name, line2);
        devos_w_set_text(s_ctx.list_lbl[s_ctx.n], b);
        lv_obj_remove_flag(s_ctx.list_row[s_ctx.n], LV_OBJ_FLAG_HIDDEN);
        s_ctx.n++;
    }
    for (int i = s_ctx.n; i < DEVOS_JOBS_MAX; i++)
        if (s_ctx.list_row[i]) lv_obj_add_flag(s_ctx.list_row[i], LV_OBJ_FLAG_HIDDEN);
    if (s_ctx.sel >= s_ctx.n) s_ctx.sel = s_ctx.n > 0 ? s_ctx.n - 1 : 0;
    list_highlight();
    /* header count chip */
    int on = 0, running = 0;
    for (int i = 0; i < s_ctx.n; i++) {
        if (!devos_jobs_summary_at(i, &sum)) continue;
        if (sum.state == DEVOS_JOB_ENABLED) on++;
        if (sum.running) running++;
    }
    if (s_ctx.lbl_header) {
        char h[96];
        const char *pause = devos_jobs_safe_paused() ? "paused (recovery)" :
                            devos_jobs_paused() ? "paused" : "automatic on";
        snprintf(h, sizeof(h), "%d on  -  %d running  -  %s", on, running, pause);
        devos_w_set_text(s_ctx.lbl_header, h);
    }
    if (s_ctx.btn_pause) devos_w_set_text(lv_obj_get_child(s_ctx.btn_pause, 0),
                                          devos_jobs_paused() ? LV_SYMBOL_PLAY "  Resume" : LV_SYMBOL_PAUSE "  Pause");
}

/* Kept name for the many callers. */
static void refresh_list(void) { list_refresh(); }

static const char *cur_source(void) { return lv_textarea_get_text(s_ctx.ta_src); }

/* ---- dirty state and drafts (P0-3) ---- */
static void update_name(void)
{
    if (!s_ctx.lbl_name) return;
    char b[DEVOS_JOBS_NAME_MAX + 24];
    snprintf(b, sizeof(b), "%s%s", s_ctx.cur_name,
             s_ctx.dirty ? "   (unsaved)" : "");
    devos_w_set_text(s_ctx.lbl_name, b);
}
static void set_dirty(bool d)
{
    if (s_ctx.dirty == d) return;
    s_ctx.dirty = d;
    update_name();
}
static void mark_dirty(void) { set_dirty(true); }

static const char *jobs_restart_check(void)
{
    return s_ctx.dirty ? "Jobs: an unsaved draft would be lost" : NULL;
}
static void draft_save(void)
{
    if (s_ctx.n == 0 || !s_ctx.dirty) return;
    devos_jobs_save_draft(s_ctx.ids[s_ctx.sel], cur_source(), strlen(cur_source()));
}

static void load_selected(void)
{
    if (s_ctx.n == 0) {
        s_ctx.cur_name[0] = '\0';
        s_syncing = true;
        lv_textarea_set_text(s_ctx.ta_src, "");
        s_syncing = false;
        devos_w_set_text(s_ctx.lbl_name, "");
        devos_w_set_text(s_ctx.lbl_state, "no jobs - press Sym+N for a new one");
        s_ctx.base_rev = 0;
        set_dirty(false);
        return;
    }
    const char *id = s_ctx.ids[s_ctx.sel];
    devos_job_summary_t sum;
    memset(&sum, 0, sizeof(sum));
    for (int i = 0; i < devos_jobs_count(); i++)
        if (devos_jobs_summary_at(i, &sum) && strcmp(sum.id, id) == 0) break;
    static char src[SRCMAX];
    size_t len = 0;
    const char *applied;
    if (devos_jobs_source(id, src, sizeof(src), &len) == DEVOS_OK) applied = src;
    else { snprintf(src, sizeof(src), "%s", NEW_TEMPLATE); applied = src; }
    s_syncing = true;
    lv_textarea_set_text(s_ctx.ta_src, applied);
    s_syncing = false;
    s_ctx.base_rev = sum.revision;
    snprintf(s_ctx.cur_name, sizeof(s_ctx.cur_name), "%s", sum.name);
    set_dirty(false);
    update_name();
    /* a saved draft (unsaved edits from a previous visit) takes precedence */
    static char draft[SRCMAX];
    size_t dl = 0;
    if (devos_jobs_load_draft(id, draft, sizeof(draft), &dl) == DEVOS_OK && dl > 0 &&
        strcmp(draft, applied) != 0) {
        s_syncing = true;
        lv_textarea_set_text(s_ctx.ta_src, draft);
        s_syncing = false;
        mark_dirty();
        say("Draft restored - press Sym+A to apply.");
    }
    static char st[96];
    snprintf(st, sizeof(st), "%s  |  %s  |  rev %u  |  last: %s",
             sum.state == DEVOS_JOB_ENABLED ? "Enabled" : "Disabled",
             sum.trigger, (unsigned)sum.revision, sum.last_result[0] ? sum.last_result : "-");
    devos_w_set_text(s_ctx.lbl_state, st);
}

/* ---- builder: model sync ---- */
static void step_summary(const jobs_node_t *n, char *out, size_t cap)
{
    if (!n) { snprintf(out, cap, "?"); return; }
    switch (n->kind) {
    case JN_ACTION: {
        const devos_action_descriptor_t *d = devos_actions_find(n->u.str.s);
        int o = snprintf(out, cap, "%s", n->u.str.s);
        if (d) for (int i = 0; i < d->param_count && o < (int)cap - 4; i++) {
            const char *v = jobs_build_arg_text(n, d->params[i].name);
            if (v && v[0]) { snprintf(out + o, cap - (size_t)o, "  %s=%s", d->params[i].name, v); break; }
        }
        break;
    }
    case JN_IF: snprintf(out, cap, "if %s", jobs_build_expr_text(n->a)); break;
    case JN_RUN: snprintf(out, cap, "run %s", n->u.str.s ? n->u.str.s : "?"); break;
    case JN_RETURN: snprintf(out, cap, "return %s", jobs_build_expr_text(n->a)); break;
    case JN_REPEAT: snprintf(out, cap, "repeat %lld as %s { ... }", (long long)n->count,
                             n->u.str.s ? n->u.str.s : "i"); break;
    case JN_WAIT: { char d[24]; fmt_dur(n->u.i, d, sizeof(d)); snprintf(out, cap, "wait %s", d); break; }
    case JN_SET: snprintf(out, cap, "set %s = %s", n->u.str.s, jobs_build_expr_text(n->a)); break;
    default: snprintf(out, cap, "step");
    }
}

static void builder_sync(void)
{
    char src[SRCMAX];
    jobs_build_source(&s_ctx.build, src, sizeof(src));
    s_syncing = true;
    lv_textarea_set_text(s_ctx.ta_src, src);
    s_syncing = false;
    s_ctx.build_ok = jobs_build_revalidate(&s_ctx.build);
    mark_dirty();
}

/* ---- event topic picker (P0-5): only registered topics ---- */
static void refresh_topics(void)
{
    s_topic_n = 0;
    const jobs_node_t *t = jobs_build_trigger(&s_ctx.build);
    const char *cur = t ? jobs_build_trigger_event_topic(t) : NULL;
    char opts[TOPIC_OPTS_MAX * (DEVOS_EVENTS_TOPIC_MAX + 2)];
    size_t o = 0;
    opts[0] = '\0';
    /* keep an unknown current topic selectable so it is not silently changed */
    if (cur && cur[0] && !devos_events_topic_schema(cur)) {
        snprintf(s_topic_ids[s_topic_n], sizeof(s_topic_ids[0]), "%s", cur);
        o += (size_t)snprintf(opts + o, sizeof(opts) - o, "%.48s  (unknown)", cur);
        s_topic_n++;
    }
    for (int i = 0; i < devos_events_topic_count() && s_topic_n < TOPIC_OPTS_MAX; i++) {
        const devos_event_schema_t *sc = devos_events_topic_at(i);
        if (!sc || !sc->topic) continue;
        snprintf(s_topic_ids[s_topic_n], sizeof(s_topic_ids[0]), "%s", sc->topic);
        o += (size_t)snprintf(opts + o, sizeof(opts) - o, "%s%s", s_topic_n ? "\n" : "", sc->topic);
        s_topic_n++;
    }
    if (s_topic_n == 0) snprintf(opts, sizeof(opts), "(no topics registered)");
    lv_dropdown_set_options(s_ctx.dd_topic, opts);
    int sel = 0;
    if (cur) for (int i = 0; i < s_topic_n; i++) if (strcmp(s_topic_ids[i], cur) == 0) sel = i;
    lv_dropdown_set_selected(s_ctx.dd_topic, (uint32_t)sel);
    if (cur && cur[0] && !devos_events_topic_schema(cur))
        say("This event topic is not registered - the trigger will never fire.");
}

static void topic_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int sel = (int)lv_dropdown_get_selected(s_ctx.dd_topic);
    if (sel < 0 || sel >= s_topic_n) return;
    if (jobs_build_set_trigger_event(&s_ctx.build, s_topic_ids[sel])) {
        const devos_event_schema_t *sc = devos_events_topic_schema(s_topic_ids[sel]);
        char b[260];
        snprintf(b, sizeof(b), "Event \"%s\"\n%s\nfields: %s", s_topic_ids[sel],
                 sc && sc->description ? sc->description : "",
                 sc && sc->fields ? sc->fields : "");
        say(b);
        builder_sync();
    }
}

static void builder_inspector(void);

/* Subtle selection: a thin accent bar on the left of the current step; the
 * rest are plain. Re-applied on theme change. */
static void restyle_steps(void)
{
    const devos_palette_t *p = devos_theme_get();
    for (int i = 0; i < s_ctx.rows_n && i < STEP_ROWS; i++) {
        lv_obj_t *row = s_ctx.step_row[i];
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        if (i == s_ctx.bstep) {
            lv_obj_set_style_border_width(row, 3, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_color(row, p->accent_primary, 0);
        } else {
            lv_obj_set_style_border_width(row, 0, 0);
        }
    }
}

static void steps_theme_cb(const devos_palette_t *p, void *user)
{
    (void)p; (void)user;
    if (s_ctx.mode == MODE_BUILDER) restyle_steps();
}

/* (Re)draw the step rows and rebuild the row cache. */
static void refresh_steps(void)
{
    const jobs_node_t *sel_node = (s_ctx.bstep >= 0 && s_ctx.bstep < s_ctx.rows_n) ? s_ctx.rows[s_ctx.bstep].node : NULL;
    s_ctx.rows_n = jobs_build_rows(&s_ctx.build, s_ctx.rows, JOBS_BUILD_ROWS);
    for (int i = 0; i < STEP_ROWS; i++) {
        if (i < s_ctx.rows_n) {
            char s[96], txt[120];
            step_summary(s_ctx.rows[i].node, s, sizeof(s));
            snprintf(txt, sizeof(txt), "%*s%d. %s", s_ctx.rows[i].depth * 2, "", i + 1, s);
            devos_w_set_text(s_ctx.step_lbl[i], txt);
            lv_obj_set_y(s_ctx.step_row[i], i * STEP_H);
            set_visible(s_ctx.step_row[i], true);
        } else {
            set_visible(s_ctx.step_row[i], false);
        }
    }
    /* keep the same node selected across a reorder */
    if (sel_node) for (int i = 0; i < s_ctx.rows_n; i++) if (s_ctx.rows[i].node == sel_node) s_ctx.bstep = i;
    if (s_ctx.bstep >= s_ctx.rows_n) s_ctx.bstep = s_ctx.rows_n ? s_ctx.rows_n - 1 : 0;
    /* fixed layout: the list is a fixed box so the Add row and settings never
     * move; it scrolls internally. */
    restyle_steps();
    if (s_ctx.rows_n > 0)
        lv_obj_scroll_to_y(s_ctx.step_list, s_ctx.bstep * STEP_H, LV_ANIM_OFF);
}

static void builder_refresh(void)
{
    refresh_add_picker();
    jobs_node_t *t = jobs_build_trigger(&s_ctx.build);
    if (t) {
        int kind = t->sub;
        lv_dropdown_set_selected(s_ctx.dd_kind,
            (uint32_t)(kind == JTRIG_EVERY ? 1 : kind == JTRIG_DAILY ? 2 :
                       kind == JTRIG_WEEKDAYS ? 3 : kind == JTRIG_EVENT ? 4 : 0));
        char v[64] = "";
        if (kind == JTRIG_EVERY) fmt_dur(t->u.i, v, sizeof(v));
        else if (kind == JTRIG_DAILY || kind == JTRIG_WEEKDAYS)
            snprintf(v, sizeof(v), "%s", t->u.str.s ? t->u.str.s : "08:00");
        else if (kind == JTRIG_EVENT)
            snprintf(v, sizeof(v), "%s", jobs_build_trigger_event_topic(t) ? jobs_build_trigger_event_topic(t) : "");
        lv_textarea_set_text(s_ctx.ta_trig, v);
        lv_textarea_set_text(s_ctx.ta_where, jobs_build_trigger_where_text(t));
        if (s_ctx.dd_topic) refresh_topics();
        bool ev = kind == JTRIG_EVENT;
        bool hm = kind == JTRIG_DAILY || kind == JTRIG_WEEKDAYS;
        if (hm && s_ctx.dd_hour) {
            int hh = 8, mm = 0;
            sscanf(v, "%d:%d", &hh, &mm);
            if (hh < 0 || hh > 23) hh = 8;
            if (mm < 0 || mm > 59) mm = 0;
            lv_dropdown_set_selected(s_ctx.dd_hour, (uint32_t)hh);
            lv_dropdown_set_selected(s_ctx.dd_min, (uint32_t)mm);
        }
        set_visible(s_ctx.ta_trig, !ev && !hm);
        if (s_ctx.dd_hour) set_visible(s_ctx.dd_hour, hm);
        if (s_ctx.dd_min) set_visible(s_ctx.dd_min, hm);
        if (s_ctx.dd_topic) set_visible(s_ctx.dd_topic, ev);
        if (ev) lv_obj_remove_flag(s_ctx.ta_where, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_ctx.ta_where, LV_OBJ_FLAG_HIDDEN);
    }
    refresh_steps();
    builder_inspector();
}

static const devos_action_descriptor_t *cur_action(void)
{
    if (s_ctx.bstep < 0 || s_ctx.bstep >= s_ctx.rows_n) return NULL;
    const jobs_node_t *n = s_ctx.rows[s_ctx.bstep].node;
    return (n && n->kind == JN_ACTION) ? devos_actions_find(n->u.str.s) : NULL;
}

static void builder_inspector(void)
{
    const jobs_node_t *node = (s_ctx.bstep >= 0 && s_ctx.bstep < s_ctx.rows_n) ? s_ctx.rows[s_ctx.bstep].node : NULL;
    const devos_action_descriptor_t *d = cur_action();
    s_ctx.insp = INSP_NONE;
    s_ctx.insp_expr = false;
    set_visible(s_ctx.dd_param, false);
    set_visible(s_ctx.dd_expr, false);
    set_visible(s_ctx.dd_choice, false);
    set_visible(s_ctx.ta_val, false);
    if (s_ctx.form) set_visible(s_ctx.form, false);

    if (!node) { devos_w_set_text(s_ctx.lbl_custom, "No steps yet - press Sym+U (or the Add button) to add one."); set_visible(s_ctx.lbl_custom, true); return; }

    if (node->kind == JN_IF) {
        s_ctx.insp = INSP_IF;
        devos_w_set_text(s_ctx.lbl_custom, "Condition (e.g. nas.ok && response.status == 200)");
        set_visible(s_ctx.lbl_custom, true);
        set_visible(s_ctx.ta_val, true);
        lv_textarea_set_text(s_ctx.ta_val, jobs_build_expr_text(jobs_build_if_cond(node)));
        return;
    }
    if (node->kind == JN_SET) {
        s_ctx.insp = INSP_SET;
        devos_w_set_text(s_ctx.lbl_custom, "Value expression");
        set_visible(s_ctx.lbl_custom, true);
        set_visible(s_ctx.ta_val, true);
        lv_textarea_set_text(s_ctx.ta_val, jobs_build_expr_text(jobs_build_set_value(node)));
        return;
    }
    if (node->kind == JN_WAIT) {
        s_ctx.insp = INSP_WAIT;
        devos_w_set_text(s_ctx.lbl_custom, "Wait duration (e.g. 2s, 250ms)");
        set_visible(s_ctx.lbl_custom, true);
        set_visible(s_ctx.ta_val, true);
        char v[24];
        fmt_dur(node->u.i, v, sizeof(v));
        lv_textarea_set_text(s_ctx.ta_val, v);
        return;
    }
    if (node->kind == JN_REPEAT) {
        s_ctx.insp = INSP_REPEAT;
        devos_w_set_text(s_ctx.lbl_custom, "Repeat: count (1-32) as index - body is edited in Text");
        set_visible(s_ctx.lbl_custom, true);
        set_visible(s_ctx.ta_val, true);
        char v[48];
        snprintf(v, sizeof(v), "%lld as %s", (long long)node->count, node->u.str.s ? node->u.str.s : "i");
        lv_textarea_set_text(s_ctx.ta_val, v);
        return;
    }
    if (!d || d->param_count == 0) {
        s_ctx.insp = INSP_CUSTOM;
        devos_w_set_text(s_ctx.lbl_custom, "Advanced step - edit it in Text (Sym+M).");
        set_visible(s_ctx.lbl_custom, true);
        return;
    }

    /* action parameters */
    s_ctx.insp = INSP_ACTION;
    form_build(node, d);
}

/* ---- P2: per-parameter vertical form ------------------------------------- */
static const char *param_type_hint(const devos_action_param_t *p)
{
    switch (p->type) {
    case DEVOS_VAL_STR:      return "text";
    case DEVOS_VAL_INT:      return "whole number";
    case DEVOS_VAL_NUM:      return "number";
    case DEVOS_VAL_BOOL:     return "true / false";
    case DEVOS_VAL_DURATION: return "duration (e.g. 2s, 250ms)";
    default:                 return "";
    }
}

/* Names an expression can use here: outputs bound by earlier steps
 * (<name> plus its known <name.field>s), repeat indices, and the event
 * fields when the trigger is an event. Top-level walk; nested steps see
 * the whole top level (a hint, not a scope check). */
static void form_vars_hint(const jobs_node_t *node, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (!out || cap == 0 || !s_ctx.build.ast || !s_ctx.build.ast->root) return;
    size_t o = 0;
    jobs_node_t *body = s_ctx.build.ast->root->c;
    for (jobs_node_t *s = body ? body->a : NULL; s && o + 40 < cap; s = s->next) {
        if (s == node) break;
        if (s->kind == JN_REPEAT && s->u.str.s && s->u.str.s[0]) {
            o += (size_t)snprintf(out + o, cap - o, "%s%s", o ? " " : "", s->u.str.s);
            continue;
        }
        const char *as = (s->kind == JN_ACTION || s->kind == JN_RUN) ? s->u.str.s2 : NULL;
        if (!as || !as[0]) continue;
        o += (size_t)snprintf(out + o, cap - o, "%s%s", o ? " " : "", as);
        const devos_action_descriptor_t *d =
            s->kind == JN_ACTION ? devos_actions_find(s->u.str.s) : NULL;
        if (d) for (int k = 0; k < d->out_count && o + 40 < cap; k++)
            o += (size_t)snprintf(out + o, cap - o, " %s.%s", as, d->outs[k].name);
    }
    jobs_node_t *t = jobs_build_trigger(&s_ctx.build);
    if (t && t->sub == JTRIG_EVENT && o + 64 < cap)
        o += (size_t)snprintf(out + o, cap - o, "%sevent.topic event.payload event.seq",
                              o ? " " : "");
}

/* The value text currently shown for a parameter (literal form), or "". */
static void param_value_text(const jobs_node_t *node, const devos_action_param_t *p, char *out, size_t cap){
    out[0] = '\0';
    if (!node || !p) return;
    if (p->type == DEVOS_VAL_DURATION) {
        int64_t ms = jobs_build_arg_duration(node, p->name);
        if (ms >= 0) fmt_dur(ms, out, cap);
    } else {
        const char *cur = jobs_build_arg_text(node, p->name);
        if (cur) snprintf(out, cap, "%s", cur);
    }
}

/* Build or refresh the form for the selected action node. */
static void form_build(const jobs_node_t *node, const devos_action_descriptor_t *d)
{
    if (!s_ctx.form) return;
    set_visible(s_ctx.form, true);
    set_visible(s_ctx.lbl_custom, false);       /* the form replaces the single-line hint */
    set_visible(s_ctx.ta_val, false);
    if (!node || !d) { s_ctx.form_n = 0; return; }
    s_ctx.form_n = d->param_count > PARAM_MAX ? PARAM_MAX : d->param_count;

    if (s_ctx.form_hdr) {
        char h[160];
        snprintf(h, sizeof(h), "%s  -  %s", d->label ? d->label : d->id,
                 d->effect == DEVOS_EFFECT_MUTATE ? "changes state" :
                 d->effect == DEVOS_EFFECT_NET_SEND ? "sends data" : "read-only");
        devos_w_set_text(s_ctx.form_hdr, h);
    }

    for (int i = 0; i < PARAM_MAX; i++) {
        bool used = i < s_ctx.form_n;
        if (s_ctx.form_rows[i]) set_visible(s_ctx.form_rows[i], used);
        if (!used) continue;
        const devos_action_param_t *p = &d->params[i];
        s_ctx.form_p[i] = p;

        /* label: name (required *) */
        char lb[80];
        snprintf(lb, sizeof(lb), "%s%s", p->name, p->required ? "  *" : "");
        devos_w_set_text(s_ctx.form_lbl[i], lb);
        /* help: help text, then type and default, then in-scope variables */
        char hb[480];
        snprintf(hb, sizeof(hb), "%s%sType: %s%s%s%s",
                 p->help ? p->help : "", p->help ? "\n" : "",
                 param_type_hint(p),
                 p->expression ? ", or an expression" : "",
                 p->def ? "  -  default " : "", p->def ? p->def : "");
        if (p->expression && !p->credential) {
            char vars[256];
            form_vars_hint(node, vars, sizeof(vars));
            size_t hl = strlen(hb);
            snprintf(hb + hl, sizeof(hb) - hl, "\nAvailable: %s",
                     vars[0] ? vars : "(nothing bound yet - add a step with 'Save result as')");
        }
        devos_w_set_text(s_ctx.form_help[i], hb);

        bool choice = p->choices != NULL || p->type == DEVOS_VAL_BOOL;
        bool is_expr = p->expression && jobs_build_arg_is_expr(node, p->name);
        if (is_expr) choice = false;        /* an expression overrides a choice list */

        set_visible(s_ctx.form_ta[i], !choice);
        set_visible(s_ctx.form_dd[i], choice);
        if (s_ctx.form_expr[i]) set_visible(s_ctx.form_expr[i], p->expression && !p->credential);

        if (choice) {
            if (p->choices) {
                static char c[160];
                snprintf(c, sizeof(c), "%s", p->choices);
                for (char *q = c; *q; q++) if (*q == '|') *q = '\n';
                lv_dropdown_set_options(s_ctx.form_dd[i], c);
                const char *cur = jobs_build_arg_text(node, p->name);
                int sel = 0, k = 0;
                char tmp[160];
                snprintf(tmp, sizeof(tmp), "%s", p->choices);
                for (char *tok = strtok(tmp, "|"); tok; tok = strtok(NULL, "|"), k++)
                    if (cur && strcmp(tok, cur) == 0) sel = k;
                lv_dropdown_set_selected(s_ctx.form_dd[i], (uint32_t)sel);
            } else {
                lv_dropdown_set_options(s_ctx.form_dd[i], "false\ntrue");
                bool b = false;
                jobs_build_arg_bool(node, p->name, &b);
                lv_dropdown_set_selected(s_ctx.form_dd[i], b ? 1 : 0);
            }
        } else {
            if (p->credential) {
                const char *n = jobs_build_arg_secret_name(node, p->name);
                lv_textarea_set_text(s_ctx.form_ta[i], n ? n : "");
            } else if (is_expr) {
                lv_textarea_set_text(s_ctx.form_ta[i], jobs_build_expr_text(jobs_build_arg_expr(node, p->name)));
            } else {
                char v[128];
                param_value_text(node, p, v, sizeof(v));
                lv_textarea_set_text(s_ctx.form_ta[i], v);
            }
        }
        if (s_ctx.form_expr[i]) {
            lv_dropdown_set_options(s_ctx.form_expr[i], "Literal\nExpression");
            lv_dropdown_set_selected(s_ctx.form_expr[i], is_expr ? 1 : 0);
        }
    }

    /* "Save result as": any action/run may bind its result to a name */
    if (s_ctx.form_out_ta) {
        set_visible(s_ctx.form_out_ta, true);
        set_visible(s_ctx.form_out_lbl, true);
        const char *o = jobs_build_output(node);
        lv_textarea_set_text(s_ctx.form_out_ta, o ? o : "");
    }
}

/* Commit the whole form for the selected action, then re-serialize once. */
static void form_commit(const jobs_node_t *node)
{
    if (!node) return;
    const devos_action_descriptor_t *d = cur_action();
    if (!d) return;
    bool bad = false;
    for (int i = 0; i < s_ctx.form_n && i < d->param_count; i++) {
        const devos_action_param_t *p = s_ctx.form_p[i];
        if (!p) continue;
        bool choice = (p->choices != NULL || p->type == DEVOS_VAL_BOOL) && !lv_obj_has_flag(s_ctx.form_ta[i], LV_OBJ_FLAG_HIDDEN);
        if (!choice) {
            bool as_expr = s_ctx.form_expr[i] &&
                           lv_dropdown_get_selected(s_ctx.form_expr[i]) == 1;
            const char *val = lv_textarea_get_text(s_ctx.form_ta[i]);
            if (p->credential) {
                jobs_build_set_arg_secret(&s_ctx.build, node, p->name, val);
            } else if (as_expr && p->expression) {
                if (!val[0]) continue;          /* empty expression: leave as-is */
                if (!jobs_build_set_arg_expr(&s_ctx.build, node, p->name, val)) bad = true;
            } else if (p->type == DEVOS_VAL_DURATION) {
                int64_t ms = parse_dur(val);
                if (ms >= 0) jobs_build_set_arg_duration(&s_ctx.build, node, p->name, ms);
            } else if (p->type == DEVOS_VAL_INT) {
                jobs_build_set_arg_int(&s_ctx.build, node, p->name, strtoll(val, NULL, 10));
            } else if (p->type == DEVOS_VAL_NUM) {
                jobs_build_set_arg_expr(&s_ctx.build, node, p->name, val);
            } else {
                jobs_build_set_arg_str(&s_ctx.build, node, p->name, val);
            }
        } else if (p->choices) {
            char opts[160];
            snprintf(opts, sizeof(opts), "%s", p->choices);
            int sel = (int)lv_dropdown_get_selected(s_ctx.form_dd[i]), k = 0;
            for (char *tok = strtok(opts, "|"); tok; tok = strtok(NULL, "|"), k++)
                if (k == sel) { jobs_build_set_arg_str(&s_ctx.build, node, p->name, tok); break; }
        } else {
            jobs_build_set_arg_bool(&s_ctx.build, node, p->name,
                                    lv_dropdown_get_selected(s_ctx.form_dd[i]) == 1);
        }
    }
    if (s_ctx.form_out_ta) {
        const char *o = lv_textarea_get_text(s_ctx.form_out_ta);
        jobs_build_set_output(&s_ctx.build, node, o);
    }
    if (bad) { say(s_ctx.build.diag); return; }
    builder_sync();
    if (s_ctx.tab == TAB_BUILDER) refresh_steps();
}

static void builder_commit_trigger(void)
{
    int kind = (int)lv_dropdown_get_selected(s_ctx.dd_kind);
    jobs_build_set_trigger_kind(&s_ctx.build, kind == 1 ? JTRIG_EVERY : kind == 2 ? JTRIG_DAILY :
                                                 kind == 3 ? JTRIG_WEEKDAYS : kind == 4 ? JTRIG_EVENT : JTRIG_MANUAL);
    const char *v = lv_textarea_get_text(s_ctx.ta_trig);
    if (kind == 1) { int64_t ms = parse_dur(v); if (ms > 0) jobs_build_set_trigger_duration(&s_ctx.build, ms); }
    else if (kind == 2 || kind == 3) {
        if (s_ctx.dd_hour && s_ctx.dd_min) {
            char hm[8];
            snprintf(hm, sizeof(hm), "%02d:%02d", (int)lv_dropdown_get_selected(s_ctx.dd_hour),
                     (int)lv_dropdown_get_selected(s_ctx.dd_min));
            jobs_build_set_trigger_time(&s_ctx.build, hm);
        } else {
            jobs_build_set_trigger_time(&s_ctx.build, v);
        }
    }
    else if (kind == 4) {
        int sel = (int)lv_dropdown_get_selected(s_ctx.dd_topic);
        if (sel >= 0 && sel < s_topic_n) jobs_build_set_trigger_event(&s_ctx.build, s_topic_ids[sel]);
        if (!jobs_build_set_trigger_where(&s_ctx.build, lv_textarea_get_text(s_ctx.ta_where)))
            say(s_ctx.build.diag);
    }
    builder_sync();
}

static void time_spinner_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_ctx.tab == TAB_BUILDER) builder_commit_trigger();
}

static void builder_commit_value(void)
{
    const jobs_node_t *node = (s_ctx.bstep >= 0 && s_ctx.bstep < s_ctx.rows_n) ? s_ctx.rows[s_ctx.bstep].node : NULL;
    if (!node) return;
    const char *val = lv_textarea_get_text(s_ctx.ta_val);

    if (s_ctx.insp == INSP_IF) {
        if (!jobs_build_set_if_cond(&s_ctx.build, node, val)) { say(s_ctx.build.diag); return; }
    } else if (s_ctx.insp == INSP_SET) {
        if (!jobs_build_set_set_value(&s_ctx.build, node, val)) { say(s_ctx.build.diag); return; }
    } else if (s_ctx.insp == INSP_WAIT) {
        int64_t ms = parse_dur(val);
        if (ms > 0) jobs_build_set_wait(&s_ctx.build, node, ms);
    } else if (s_ctx.insp == INSP_REPEAT) {
        long long count = 0;
        char idx[40] = "";
        if (sscanf(val, "%lld as %39s", &count, idx) != 2 || !jobs_build_set_repeat(&s_ctx.build, node, (int64_t)count, idx))
            { say("Repeat: write it as \"3 as i\" (count 1-32)"); return; }
    } else if (s_ctx.insp == INSP_ACTION) {
        form_commit(node);
        return;                                 /* form_commit synced already */
    } else {
        return;
    }
    builder_sync();
    if (s_ctx.mode == MODE_BUILDER) refresh_steps();   /* the row summary changed */
}

static void builder_commit_all(void)
{
    if (s_ctx.mode != MODE_BUILDER) return;
    builder_commit_trigger();
    builder_commit_value();
}

/* ---- Add-step picker (P2): Control entries then actions by category ---- */
#define ADD_MAX (16 + 64)
static char s_add_kind[ADD_MAX];            /* 0=control, 1=action */
static char s_add_id[ADD_MAX][64];          /* action id, or control key */
static int  s_add_n;

/* Case-insensitive substring match over an action's label, id and category
 * (empty filter matches everything). */
static bool add_match(const char *label, const char *id, const char *cat, const char *f)
{
    if (!f || !f[0]) return true;
    const char *hay[3] = { label, id, cat };
    for (int h = 0; h < 3; h++) {
        if (!hay[h]) continue;
        for (const char *p = hay[h]; *p; p++) {
            size_t k = 0;
            while (f[k] && p[k] && tolower((unsigned char)p[k]) == tolower((unsigned char)f[k])) k++;
            if (!f[k]) return true;
        }
    }
    return false;
}

static void add_filter_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_ctx.tab == TAB_BUILDER) refresh_add_picker();
}

static void refresh_add_picker(void)
{
    if (!s_ctx.dd_add) return;
    /* keep the selection across refilters when the entry survives */
    int prev = (int)lv_dropdown_get_selected(s_ctx.dd_add);
    char keep[64] = "";
    if (prev >= 0 && prev < s_add_n) snprintf(keep, sizeof(keep), "%s", s_add_id[prev]);
    const char *f = s_ctx.ta_add_filter ? lv_textarea_get_text(s_ctx.ta_add_filter) : "";
    char opts[ADD_MAX * 76];
    size_t o = 0;
    opts[0] = '\0';
    s_add_n = 0;
    struct { const char *key, *label; } ctl[] = {
        { "if",     "Control: if ... { }" },
        { "wait",   "Control: wait <duration>" },
        { "repeat", "Control: repeat N as i { }" },
        { "set",    "Control: set name = value" },
        { "run",    "Control: run \"job\" as out" },
    };
    for (unsigned i = 0; i < sizeof(ctl) / sizeof(ctl[0]) && s_add_n < ADD_MAX; i++) {
        if (!add_match(ctl[i].label, ctl[i].label, ctl[i].key, f)) continue;
        s_add_kind[s_add_n] = 0;
        snprintf(s_add_id[s_add_n], sizeof(s_add_id[0]), "%s", ctl[i].key);
        o += (size_t)snprintf(opts + o, sizeof(opts) - o, "%s%s", s_add_n ? "\n" : "", ctl[i].label);
        s_add_n++;
    }
    const char *last_cat = NULL;
    for (int i = 0; i < devos_actions_count() && s_add_n < ADD_MAX; i++) {
        const devos_action_descriptor_t *d = devos_actions_at(i);
        if (!d || !d->id) continue;
        if (d->category && (!last_cat || strcmp(d->category, last_cat) != 0)) {
            /* a section heading is just visual; skip it in the selectable list */
            last_cat = d->category;
        }
        if (!add_match(d->label ? d->label : d->id, d->id, d->category, f)) continue;
        char reason[80];
        bool ok = devos_actions_available(d->id, reason, sizeof(reason));
        s_add_kind[s_add_n] = 1;
        snprintf(s_add_id[s_add_n], sizeof(s_add_id[0]), "%s", d->id);
        o += (size_t)snprintf(opts + o, sizeof(opts) - o, "%s%s%s",
                              s_add_n ? "\n" : "",
                              d->label ? d->label : d->id,
                              ok ? "" : "  (unavailable)");
        s_add_n++;
    }
    if (s_add_n == 0) snprintf(opts, sizeof(opts), "(no match - clear the filter)");
    lv_dropdown_set_options(s_ctx.dd_add, opts);
    int sel = 0;
    if (keep[0]) for (int i = 0; i < s_add_n; i++) if (strcmp(s_add_id[i], keep) == 0) sel = i;
    lv_dropdown_set_selected(s_ctx.dd_add, (uint32_t)sel);
}

static void builder_add_step(void)
{
    if (s_ctx.tab != TAB_BUILDER || !s_ctx.build.ast) return;
    const jobs_node_t *block = (s_ctx.bstep >= 0 && s_ctx.bstep < s_ctx.rows_n)
                                   ? s_ctx.rows[s_ctx.bstep].block : s_ctx.build.ast->root->c;
    int sel = (int)lv_dropdown_get_selected(s_ctx.dd_add);
    if (sel < 0 || sel >= s_add_n) return;
    bool ok = false;
    if (s_add_kind[sel]) {
        const devos_action_descriptor_t *d = devos_actions_find(s_add_id[sel]);
        char reason[80];
        if (d && !devos_actions_available(d->id, reason, sizeof(reason))) {
            say(reason[0] ? reason : "That action is unavailable.");
            return;
        }
        ok = jobs_build_add_action(&s_ctx.build, block, s_add_id[sel]);
    } else if (strcmp(s_add_id[sel], "if") == 0) {
        ok = jobs_build_add_if(&s_ctx.build, block, "true");
    } else if (strcmp(s_add_id[sel], "wait") == 0) {
        ok = jobs_build_add_wait(&s_ctx.build, block, 1000);
    } else if (strcmp(s_add_id[sel], "repeat") == 0) {
        ok = jobs_build_add_repeat(&s_ctx.build, block, 3, "i");
    } else if (strcmp(s_add_id[sel], "set") == 0) {
        ok = jobs_build_add_set(&s_ctx.build, block, "n", "0");
    } else if (strcmp(s_add_id[sel], "run") == 0) {
        /* the only job name we can guess safely is the first other job */
        ok = jobs_build_add_run(&s_ctx.build, block, s_ctx.cur_name[0] ? s_ctx.cur_name : "job", "out");
        if (ok) say("Run step added - set the job name in the Step settings field.");
    }
    if (!ok) { say(s_ctx.build.diag[0] ? s_ctx.build.diag : "Couldn't add that step."); return; }
    builder_sync();
    builder_refresh();
    /* keep the newest step selected so its settings show immediately */
    int target = jobs_build_block_count(block) - 1;
    for (int i = 0; i < s_ctx.rows_n; i++)
        if (s_ctx.rows[i].node && s_ctx.rows[i].block == block && s_ctx.rows[i].index == target)
            s_ctx.bstep = i;
    refresh_steps();
    builder_inspector();
}

static void builder_delete_step(void)
{
    if (s_ctx.mode != MODE_BUILDER || s_ctx.bstep < 0 || s_ctx.bstep >= s_ctx.rows_n) return;
    if (jobs_build_delete(&s_ctx.build, s_ctx.rows[s_ctx.bstep].block, s_ctx.rows[s_ctx.bstep].node)) {
        if (s_ctx.bstep > 0) s_ctx.bstep--;
        builder_sync();
        builder_refresh();
    }
}

static void builder_move(int dir)
{
    if (s_ctx.mode != MODE_BUILDER || s_ctx.bstep < 0 || s_ctx.bstep >= s_ctx.rows_n) return;
    if (jobs_build_move(&s_ctx.build, s_ctx.rows[s_ctx.bstep].block, s_ctx.rows[s_ctx.bstep].node, dir)) {
        builder_sync();
        builder_refresh();
    }
}

/* P2: move the selected step into the preceding if/repeat (indent) or out of
 * its block to just after the owner (outdent). Keeps the moved node selected. */
static void builder_reblock(int dir)
{
    if (s_ctx.tab != TAB_BUILDER || s_ctx.bstep < 0 || s_ctx.bstep >= s_ctx.rows_n) return;
    const jobs_node_t *node = s_ctx.rows[s_ctx.bstep].node;
    const jobs_node_t *block = s_ctx.rows[s_ctx.bstep].block;
    bool ok = dir > 0 ? jobs_build_indent(&s_ctx.build, block, node)
                      : jobs_build_outdent(&s_ctx.build, block, node);
    if (!ok) { say(dir > 0 ? "Nothing to indent into (needs an if/repeat above)." : "Already at the top level."); return; }
    builder_sync();
    builder_refresh();
    for (int i = 0; i < s_ctx.rows_n; i++)
        if (s_ctx.rows[i].node == node) { s_ctx.bstep = i; break; }
    refresh_steps();
    builder_inspector();
}

/* ---- touch drag reordering ---- */
static int s_drag_from = -1;
static lv_point_t s_drag_p0;

static void step_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        s_ctx.bstep = idx;
        s_ctx.bparam = 0;
        refresh_steps();
        builder_inspector();
    } else if (code == LV_EVENT_PRESSED) {
        s_drag_from = idx;
        lv_indev_get_point(lv_indev_active(), &s_drag_p0);
    } else if (code == LV_EVENT_PRESSING) {
        if (s_drag_from >= 0 && s_drag_from < s_ctx.rows_n) {
            lv_point_t p;
            lv_indev_get_point(lv_indev_active(), &p);
            lv_obj_set_y(s_ctx.step_row[s_drag_from], s_drag_from * STEP_H + (p.y - s_drag_p0.y));
        }
    } else if (code == LV_EVENT_RELEASED) {
        if (s_drag_from >= 0 && s_drag_from < s_ctx.rows_n) {
            lv_point_t p;
            lv_indev_get_point(lv_indev_active(), &p);
            int delta = (p.y - s_drag_p0.y) / STEP_H;
            if (delta != 0) {
                int target = s_ctx.rows[s_drag_from].index + delta;
                const jobs_node_t *moved = s_ctx.rows[s_drag_from].node;
                if (jobs_build_move_to(&s_ctx.build, s_ctx.rows[s_drag_from].block, moved, target)) {
                    builder_sync();
                    builder_refresh();
                    for (int i = 0; i < s_ctx.rows_n; i++) if (s_ctx.rows[i].node == moved) s_ctx.bstep = i;
                    refresh_steps();
                    builder_inspector();
                }
            }
        }
        s_drag_from = -1;
    }
}

/* ---- actions (shared) ---- */
static void act_validate(void)
{
    const char *src = cur_source();
    char diag[160];
    if (devos_jobs_check(src, strlen(src), diag, sizeof(diag)) == DEVOS_OK) say("Valid - press Sym+A to Apply.");
    else { snprintf(s_ctx.out, OUT_MAX, "Invalid:\n  %s", diag); say(s_ctx.out); }
}

static void act_apply(void)
{
    if (s_ctx.n == 0) { say("Create a job first (Sym+N)."); return; }
    const char *id = s_ctx.ids[s_ctx.sel];
    const char *src = cur_source();
    uint32_t rev = 0;
    devos_err_t rc = devos_jobs_apply_base(id, src, strlen(src), s_ctx.base_rev, true, &rev);
    if (rc == DEVOS_OK) {
        s_ctx.base_rev = rev;
        devos_jobs_save_draft(id, "", 0);        /* the draft is applied now */
        refresh_list();
        load_selected();
        if (s_ctx.mode == MODE_BUILDER) set_mode(MODE_BUILDER);
        devos_toast_show("Applied", DEVOS_TOAST_OK, 0);
        say("Applied. Sym+G enables, Sym+R runs now.");
    } else if (rc == DEVOS_ERR_INVALID_STATE) {
        devos_toast_show("Changed elsewhere - reload first", DEVOS_TOAST_WARN, 0);
        say("Conflict: this job changed since you loaded it.");
    } else {
        char diag[160];
        devos_jobs_check(src, strlen(src), diag, sizeof(diag));
        snprintf(s_ctx.out, OUT_MAX, "Apply failed: %s", diag);
        say(s_ctx.out);
    }
}

static void act_enable(void)
{
    if (s_ctx.n == 0) return;
    const char *id = s_ctx.ids[s_ctx.sel];
    devos_job_summary_t sum;
    bool on = false;
    for (int i = 0; i < devos_jobs_count(); i++)
        if (devos_jobs_summary_at(i, &sum) && strcmp(sum.id, id) == 0) on = sum.state == DEVOS_JOB_ENABLED;
    devos_jobs_set_enabled(id, !on);
    refresh_list();
    load_selected();
    if (s_ctx.mode == MODE_BUILDER) set_mode(MODE_BUILDER);
    devos_toast_show(!on ? "Enabled" : "Disabled", DEVOS_TOAST_OK, 0);
}

static void act_run(void)
{
    if (s_ctx.n == 0) return;
    devos_err_t rc = devos_jobs_run_now(s_ctx.ids[s_ctx.sel]);
    if (rc == DEVOS_OK) devos_toast_show("Running", DEVOS_TOAST_OK, 0);
    else if (rc == DEVOS_ERR_INVALID_STATE) say("Already running (overlap skipped).");
    else say("Can't run this job.");
}

static void act_cancel(void)
{
    if (s_ctx.n == 0) return;
    if (devos_jobs_cancel(s_ctx.ids[s_ctx.sel]) == DEVOS_OK) devos_toast_show("Cancelling", DEVOS_TOAST_WARN, 0);
}

/* Copy line N (1-based) out of the draft source for the trace view. */
static void src_line(const char *src, int n, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if (!src || n < 1 || !out || cap == 0) return;
    const char *p = src;
    for (int i = 1; i < n && *p; i++) {
        p = strchr(p, '\n');
        if (!p) return;
        p++;
    }
    size_t k = 0;
    while (p[k] && p[k] != '\n' && k + 1 < cap) { out[k] = p[k]; k++; }
    out[k] = '\0';
    while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == '\t')) out[--k] = '\0';
    /* strip leading indentation for a compact trace view */
    size_t lead = 0;
    while (out[lead] == ' ' || out[lead] == '\t') lead++;
    if (lead) memmove(out, out + lead, strlen(out + lead) + 1);
}

static void dry_close(void)
{
    s_dry_open = false;
    devos_w_dialog_show(&s_dry_dlg, false);
}

static void dry_show_result(void)
{
    size_t o = 0;
    const char *verdict = s_dry_res.timed_out ? "TIMED OUT" : s_dry_res.ok ? "OK" : "FAILED";
    o += (size_t)snprintf(s_dry_text + o, sizeof(s_dry_text) - o,
                          "Dry run: %s\n%s\n%d steps, %d ms\n",
                          verdict, s_dry_res.message, s_dry_res.steps, (int)s_dry_res.duration_ms);
    const char *src = s_dry_src[0] ? s_dry_src : cur_source();
    for (int i = 0; i < s_dry_res.trace_n && o < sizeof(s_dry_text) - 120; i++) {
        char line[96];
        src_line(src, s_dry_res.trace[i].line, line, sizeof(line));
        o += (size_t)snprintf(s_dry_text + o, sizeof(s_dry_text) - o, "%3d  %-50.50s  %s\n",
                              s_dry_res.trace[i].line, line,
                              s_dry_res.trace[i].result == 2 ? "ERROR" :
                              s_dry_res.trace[i].result == 1 ? "skipped" : "ran");
    }
    devos_codeview_set(&s_dry_cv, s_dry_text);
    s_dry_open = true;
    devos_w_dialog_show(&s_dry_dlg, true);
}

static void dry_ok_cb(lv_event_t *e) { LV_UNUSED(e); dry_close(); }

/* ---- Revisions: view / diff / roll back ---- */
static void rev_close(void)
{
    s_rev_open = false;
    devos_w_dialog_show(&s_rev_dlg, false);
}

static void rev_show(void)
{
    if (s_ctx.n == 0 || s_rev_n == 0) return;
    int sel = (int)lv_dropdown_get_selected(s_rev_dd);
    if (sel < 0 || sel >= s_rev_n) sel = 0;
    int n = devos_jobs_revision_source(s_ctx.ids[s_ctx.sel], s_rev_nums[sel],
                                       s_rev_old, sizeof(s_rev_old));
    if (n <= 0) { devos_codeview_set(&s_rev_cv, "(that generation is gone)"); return; }
    bool diff = lv_dropdown_get_selected(s_rev_mode) == 1;
    s_rev_cv.plain = !diff;
    if (diff) {
        devos_codeview_diff(s_rev_old, cur_source(), s_rev_text, sizeof(s_rev_text));
        devos_codeview_set(&s_rev_cv, s_rev_text);
    } else {
        devos_codeview_set(&s_rev_cv, s_rev_old);
    }
}

static void rev_rollback(void)
{
    if (s_ctx.n == 0 || s_rev_n == 0) { rev_close(); return; }
    int sel = (int)lv_dropdown_get_selected(s_rev_dd);
    if (sel < 0 || sel >= s_rev_n) sel = 0;
    static EXT_RAM_BSS_ATTR char old[SRCMAX];
    int n = devos_jobs_revision_source(s_ctx.ids[s_ctx.sel], s_rev_nums[sel], old, sizeof(old));
    if (n <= 0) { say("That generation is gone."); return; }
    uint32_t rev = 0;
    devos_err_t rc = devos_jobs_apply_base(s_ctx.ids[s_ctx.sel], old, (size_t)n,
                                           s_ctx.base_rev, true, &rev);
    if (rc == DEVOS_OK) {
        s_ctx.base_rev = rev;
        rev_close();
        refresh_list();
        load_selected();
        if (s_ctx.tab == TAB_BUILDER) set_mode(MODE_BUILDER);
        if (s_ctx.tab == TAB_OVERVIEW) refresh_overview();
        devos_toast_show("Rolled back (new revision)", DEVOS_TOAST_OK, 0);
    } else if (rc == DEVOS_ERR_INVALID_STATE) {
        say("Changed elsewhere - reload first.");
    } else {
        say("Couldn't roll back.");
    }
}

static void rev_ok_cb(lv_event_t *e) { LV_UNUSED(e); rev_rollback(); }
static void rev_cancel_cb(lv_event_t *e) { LV_UNUSED(e); rev_close(); }
static void rev_pick_cb(lv_event_t *e) { LV_UNUSED(e); rev_show(); }
static void revs_cb(lv_event_t *e) { LV_UNUSED(e); open_revisions(); }

static void open_revisions(void)
{
    if (s_ctx.n == 0) { say("No job selected."); return; }
    s_rev_n = devos_jobs_revisions(s_ctx.ids[s_ctx.sel], s_rev_nums, 32);
    if (s_rev_n <= 0) { say("No saved revisions yet."); return; }
    char opts[32 * 24];
    size_t o = 0;
    opts[0] = '\0';
    devos_job_summary_t sum;
    uint32_t active = selected_summary(&sum) ? sum.revision : 0;
    for (int i = 0; i < s_rev_n; i++)
        o += (size_t)snprintf(opts + o, sizeof(opts) - o, "%srev %u%s",
                              i ? "\n" : "", (unsigned)s_rev_nums[i],
                              s_rev_nums[i] == active ? "  (active)" : "");
    lv_dropdown_set_options(s_rev_dd, opts);
    lv_dropdown_set_selected(s_rev_dd, 0);
    lv_dropdown_set_selected(s_rev_mode, 0);
    rev_show();
    s_rev_open = true;
    devos_w_dialog_show(&s_rev_dlg, true);
    devos_focus_first(&s_rev_focus);
}

/* Test the draft without applying it: validate, start, poll from tick_cb. */
static void act_dryrun(void)
{
    if (s_dry_active) { say("A dry run is already going."); return; }
    const char *src = cur_source();
    char diag[160];
    if (devos_jobs_check(src, strlen(src), diag, sizeof(diag)) != DEVOS_OK) {
        char b[200];
        snprintf(b, sizeof(b), "Can't dry-run an invalid draft: %s", diag);
        say(b);
        return;
    }
    if (devos_jobs_dry_start(src, strlen(src), diag, sizeof(diag)) != DEVOS_OK) {
        say(diag[0] ? diag : "Couldn't start the dry run.");
        return;
    }
    snprintf(s_dry_src, sizeof(s_dry_src), "%s", src);   /* trace lines map to this text */
    s_dry_active = true;
    devos_toast_show("Dry run started", DEVOS_TOAST_OK, 0);
}

/* Name the duplicate "Copy of <name>" by replacing the job's quoted name. */
static void dup_rename(const char *src, char *out, size_t cap)
{
    snprintf(out, cap, "%s", src);
    char *j = strstr(out, "job \"");
    if (!j) return;
    j += 5;
    char *e = strchr(j, '"');
    if (!e) return;
    char name[DEVOS_JOBS_NAME_MAX];
    size_t old_len = (size_t)(e - j);
    if (old_len > 48) old_len = 48;
    snprintf(name, sizeof(name), "Copy of %.*s", (int)old_len, j);
    size_t head = (size_t)(j - out);
    size_t tail = strlen(e + 1) + 1;                  /* incl. NUL */
    if (head + strlen(name) + 1 + tail > cap) return;  /* name + quote + tail */
    memmove(j + strlen(name) + 1, e + 1, tail);
    memcpy(j, name, strlen(name));
    j[strlen(name)] = '"';
}

static void close_new_dialog(void)
{
    s_new_open = false;
    devos_w_dialog_show(&s_new_dlg, false);
}

static void new_confirmed(void)
{
    close_new_dialog();
    int sel = (int)lv_dropdown_get_selected(s_new_dd);
    /* entry 0 = Blank, 1 = Duplicate (when a job is selected), then examples */
    static char src[SRCMAX];
    bool have = false;
    if (sel == 0) {
        snprintf(src, sizeof(src), "%s", NEW_TEMPLATE);
        have = true;
    } else if (sel == 1 && s_ctx.n > 0) {
        static char cur[SRCMAX];
        size_t len = 0;
        if (devos_jobs_source(s_ctx.ids[s_ctx.sel], cur, sizeof(cur), &len) == DEVOS_OK) {
            dup_rename(cur, src, sizeof(src));
            have = true;
        }
    } else {
        int idx = sel - (s_ctx.n > 0 ? 2 : 1);
        if (idx >= 0 && idx < s_new_n) {
            char path[160];
            snprintf(path, sizeof(path), "%s/jobs/examples/%s", TAB5_SD_MOUNT_POINT, s_new_files[idx]);
            FILE *f = fopen(path, "r");
            if (f) {
                size_t n = fread(src, 1, sizeof(src) - 1, f);
                fclose(f);
                src[n] = '\0';
                have = true;
            }
        }
    }
    if (!have) { say("Couldn't read that starter."); return; }
    char diag[160];
    if (devos_jobs_check(src, strlen(src), diag, sizeof(diag)) != DEVOS_OK) {
        char b[200];
        snprintf(b, sizeof(b), "That starter is invalid: %s", diag);
        say(b);
        return;
    }
    draft_save();
    char id[DEVOS_JOBS_ID_MAX];
    for (int k = 1; k < 1000; k++) {
        snprintf(id, sizeof(id), "job%d", k);
        bool taken = false;
        for (int i = 0; i < s_ctx.n; i++) if (strcmp(s_ctx.ids[i], id) == 0) taken = true;
        if (!taken) break;
    }
    if (devos_jobs_apply(id, src, strlen(src), NULL) != DEVOS_OK) { say("Couldn't create the job."); return; }
    s_ctx.sel = s_ctx.n;
    refresh_list();
    load_selected();
    if (s_ctx.mode == MODE_BUILDER) set_mode(MODE_BUILDER);
    devos_toast_show("New job (disabled)", DEVOS_TOAST_OK, 0);
}

static void new_ok_cb(lv_event_t *e) { LV_UNUSED(e); new_confirmed(); }
static void new_cancel_cb(lv_event_t *e) { LV_UNUSED(e); close_new_dialog(); }

static void act_new(void)
{
    /* scan the examples folder for starters */
    s_new_n = 0;
    char dir[160];
    snprintf(dir, sizeof(dir), "%s/jobs/examples", TAB5_SD_MOUNT_POINT);
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && s_new_n < NEW_EX_MAX) {
            size_t L = strlen(e->d_name);
            if (L < 5 || strcmp(e->d_name + L - 4, ".job") != 0) continue;
            snprintf(s_new_files[s_new_n], sizeof(s_new_files[0]), "%s", e->d_name);
            s_new_n++;
        }
        closedir(d);
    }
    char opts[NEW_EX_MAX * 64 + 128];
    size_t o = 0;
    o += (size_t)snprintf(opts + o, sizeof(opts) - o, "Blank job");
    if (s_ctx.n > 0)
        o += (size_t)snprintf(opts + o, sizeof(opts) - o, "\nDuplicate \"%.*s\"",
                              (int)sizeof(s_ctx.cur_name) - 16, s_ctx.cur_name);
    for (int i = 0; i < s_new_n; i++) {
        char base[48];
        snprintf(base, sizeof(base), "%s", s_new_files[i]);
        base[strlen(base) - 4] = '\0';
        for (char *q = base; *q; q++) if (*q == '-' || *q == '_') *q = ' ';
        o += (size_t)snprintf(opts + o, sizeof(opts) - o, "\nExample: %s", base);
    }
    lv_dropdown_set_options(s_new_dd, opts);
    lv_dropdown_set_selected(s_new_dd, 0);
    s_new_open = true;
    devos_w_dialog_show(&s_new_dlg, true);
    devos_focus_first(&s_new_focus);
}

static void close_delete_dialog(void)
{
    s_del_open = false;
    devos_w_dialog_show(&s_del_dlg, false);
}

static void delete_confirmed(void)
{
    close_delete_dialog();
    if (devos_jobs_delete(s_del_id) != DEVOS_OK) { say("Can't delete a running job - cancel it first."); return; }
    if (s_ctx.sel > 0) s_ctx.sel--;
    s_ctx.dirty = false;
    refresh_list();
    load_selected();
    if (s_ctx.mode == MODE_BUILDER) set_mode(MODE_BUILDER);
    devos_toast_show("Deleted", DEVOS_TOAST_WARN, 0);
}

/* P0: never delete with one key - always ask first (Enter confirms, Esc cancels). */
static void act_delete(void)
{
    if (s_ctx.n == 0) { say("No job selected."); return; }
    snprintf(s_del_id, sizeof(s_del_id), "%s", s_ctx.ids[s_ctx.sel]);
    snprintf(s_del_name, sizeof(s_del_name), "%s", s_ctx.cur_name);
    char t[DEVOS_JOBS_NAME_MAX + 24];
    snprintf(t, sizeof(t), LV_SYMBOL_TRASH "  Delete \"%s\"?", s_del_name);
    lv_label_set_text(s_del_dlg.title, t);
    s_del_open = true;
    devos_w_dialog_show(&s_del_dlg, true);
}
static void del_ok_cb(lv_event_t *e) { LV_UNUSED(e); delete_confirmed(); }
static void del_cancel_cb(lv_event_t *e) { LV_UNUSED(e); close_delete_dialog(); }
static void jobset_cb(lv_event_t *e) { LV_UNUSED(e); open_job_settings(); }
static void dryrun_cb(lv_event_t *e) { LV_UNUSED(e); act_dryrun(); }

/* ---- Job settings dialog (name + policy) ---- */
static bool ensure_build(void)
{
    if (s_ctx.build.ast) return true;
    return jobs_build_load(&s_ctx.build, cur_source(), strlen(cur_source()));
}

static void close_job_settings(void)
{
    s_job_open = false;
    devos_w_dialog_show(&s_ctx.dlg_job, false);
}

static void commit_job_settings(void)
{
    if (s_ctx.n == 0) { close_job_settings(); return; }
    bool had_build = s_ctx.build.ast != NULL;
    if (!ensure_build()) { close_job_settings(); return; }
    const char *nm = lv_textarea_get_text(s_ctx.job_ta_name);
    if (nm[0]) jobs_build_set_job_name(&s_ctx.build, nm);
    int64_t to = parse_dur(lv_textarea_get_text(s_ctx.job_ta_timeout));
    int64_t cd = parse_dur(lv_textarea_get_text(s_ctx.job_ta_cooldown));
    if (to < 0) to = 0;
    if (cd < 0) cd = 0;
    jobs_build_set_policy(&s_ctx.build, to, lv_dropdown_get_selected(s_ctx.job_dd_overlap) == 1, cd);
    builder_sync();                     /* re-serialize to ta_src + mark dirty */
    if (!had_build && s_ctx.tab != TAB_BUILDER) { jobs_build_free(&s_ctx.build); s_ctx.build.ast = NULL; }
    if (nm[0]) snprintf(s_ctx.cur_name, sizeof(s_ctx.cur_name), "%s", nm);
    update_name();
    if (s_ctx.tab == TAB_OVERVIEW) refresh_overview();
    refresh_problems();
    close_job_settings();
    devos_toast_show("Job settings saved - Sym+A to apply", DEVOS_TOAST_OK, 0);
}

static void open_job_settings(void)
{
    if (s_ctx.n == 0) { say("No job selected."); return; }
    if (!ensure_build()) { say("This job can't be edited here - fix it in Text first."); return; }
    jobs_node_t *job = s_ctx.build.ast ? s_ctx.build.ast->root : NULL;
    s_syncing = true;
    lv_textarea_set_text(s_ctx.job_ta_name, job && job->u.str.s ? job->u.str.s : s_ctx.cur_name);
    s_syncing = false;
    char dv[24];
    int64_t to = jobs_build_policy_timeout(job);
    int64_t cd = jobs_build_policy_cooldown(job);
    fmt_dur(to, dv, sizeof(dv));
    lv_textarea_set_text(s_ctx.job_ta_timeout, to > 0 ? dv : "");
    fmt_dur(cd, dv, sizeof(dv));
    lv_textarea_set_text(s_ctx.job_ta_cooldown, cd > 0 ? dv : "");
    lv_dropdown_set_selected(s_ctx.job_dd_overlap, (uint32_t)jobs_build_policy_overlap(job));
    s_job_open = true;
    devos_w_dialog_show(&s_ctx.dlg_job, true);
    devos_focus_first(&s_ctx.job_focus);
}

static void job_ok_cb(lv_event_t *e) { LV_UNUSED(e); commit_job_settings(); }
static void job_cancel_cb(lv_event_t *e) { LV_UNUSED(e); close_job_settings(); }

/* ---- mode ---- */
static void set_mode(ui_mode_t m)
{
    if (m == MODE_BUILDER) {
        if (!jobs_build_load(&s_ctx.build, cur_source(), strlen(cur_source())) && !s_ctx.build.ast) {
            devos_toast_show("Builder needs a valid job", DEVOS_TOAST_WARN, 0);
            m = MODE_TEXT;
        } else {
            s_ctx.build_ok = s_ctx.build.diag[0] == '\0';
            s_ctx.bstep = 0;
            s_ctx.bparam = 0;
            builder_refresh();
            if (!s_ctx.build_ok) { snprintf(s_ctx.out, OUT_MAX, "Builder: %s", s_ctx.build.diag); say(s_ctx.out); }
        }
    } else {
        jobs_build_free(&s_ctx.build);
        s_ctx.build.ast = NULL;
    }
    s_ctx.mode = m;
    bool b = m == MODE_BUILDER;
    set_visible(s_ctx.ta_src, !b);
    set_visible(s_ctx.bld, b);
    devos_w_set_text(s_ctx.lbl_mode, b ? "Builder" : "Text");
}

/* ---- callbacks ---- */
static void validate_cb(lv_event_t *e) { LV_UNUSED(e); act_validate(); }
static void apply_cb(lv_event_t *e) { LV_UNUSED(e); builder_commit_all(); act_apply(); }
static void enable_cb(lv_event_t *e) { LV_UNUSED(e); act_enable(); }
static void new_cb(lv_event_t *e) { LV_UNUSED(e); act_new(); }
static void del_cb(lv_event_t *e) { LV_UNUSED(e); act_delete(); }
static void kind_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int kind = (int)lv_dropdown_get_selected(s_ctx.dd_kind);
    jobs_build_set_trigger_kind(&s_ctx.build, kind == 1 ? JTRIG_EVERY : kind == 2 ? JTRIG_DAILY :
                                                 kind == 3 ? JTRIG_WEEKDAYS : kind == 4 ? JTRIG_EVENT : JTRIG_MANUAL);
    builder_sync();
    builder_refresh();   /* show the new kind's default value/fields */
}
static void param_cb(lv_event_t *e) { LV_UNUSED(e); s_ctx.bparam = (int)lv_dropdown_get_selected(s_ctx.dd_param); builder_inspector(); }
static void val_commit_cb(lv_event_t *e) { LV_UNUSED(e); if (s_ctx.insp != INSP_NONE) builder_commit_value(); }
static void form_row_commit_cb(lv_event_t *e) { LV_UNUSED(e); if (s_ctx.insp == INSP_ACTION) builder_commit_value(); }
static void src_changed_cb(lv_event_t *e) { LV_UNUSED(e); if (!s_syncing) mark_dirty(); }
static void add_cb(lv_event_t *e) { LV_UNUSED(e); builder_add_step(); }
static void bdel_cb(lv_event_t *e) { LV_UNUSED(e); builder_delete_step(); }
static void up_cb(lv_event_t *e) { LV_UNUSED(e); builder_move(-1); }
static void dn_cb(lv_event_t *e) { LV_UNUSED(e); builder_move(1); }

static void tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!s_ctx.screen || lv_obj_has_flag(s_ctx.screen, LV_OBJ_FLAG_HIDDEN)) return;
    list_refresh();
    if (s_ctx.tab == TAB_OVERVIEW) refresh_overview();
    else if (s_ctx.tab == TAB_RUNS) refresh_runs();
    refresh_problems();
    /* state-aware toolbar: Apply only when there is something to apply, and a
     * single Run/Cancel button that follows the job's state */
    if (s_ctx.btn_apply) {
        if (s_ctx.dirty) lv_obj_remove_state(s_ctx.btn_apply, LV_STATE_DISABLED);
        else lv_obj_add_state(s_ctx.btn_apply, LV_STATE_DISABLED);
    }
    devos_job_summary_t sum;
    if (s_ctx.btn_enable) {
        bool en = selected_summary(&sum) && sum.state == DEVOS_JOB_ENABLED;
        devos_w_set_text(lv_obj_get_child(s_ctx.btn_enable, 0), en ? "Disable  [Sym+G]" : "Enable  [Sym+G]");
    }
    if (s_ctx.btn_run) {
        bool run = selected_summary(&sum) && sum.running;
        char rl[40];
        snprintf(rl, sizeof(rl), "%s  [%s]", run ? "Cancel" : "Run now", run ? "Sym+X" : "Sym+R");
        devos_w_set_text(lv_obj_get_child(s_ctx.btn_run, 0), rl);
    }
    /* dry run: poll the engine without blocking the UI */
    if (s_dry_active && devos_jobs_dry_poll(&s_dry_res, 30000)) {
        s_dry_active = false;
        dry_show_result();
    }
}

/* ---- lifecycle ---- */
static lv_obj_t *mk_label(lv_obj_t *parent, const char *text, devos_w_kind_t kind, int x, int y)
{
    lv_obj_t *l = devos_w_label(parent, NULL, kind, text);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* ---- command palette ---- */
static void cmd_new_job(void *ud)
{
    (void)ud;
    devos_core_open_with("jobs", "new", NULL);
}
static const devos_command_t CMD_NEW = {
    .title = "Jobs: New job", .keywords = "automation job new create add",
    .hint = "Jobs", .icon = LV_SYMBOL_PLUS, .run = cmd_new_job,
};

static void cmd_pause_jobs(void *ud)
{
    (void)ud;
    devos_jobs_set_paused(!devos_jobs_paused());
    devos_toast_show(devos_jobs_paused() ? "Automatic jobs paused" : "Automatic jobs resumed",
                     DEVOS_TOAST_OK, 0);
}
static const char *cmd_pause_label(void *ud)
{
    (void)ud;
    return devos_jobs_paused() ? "Jobs: Resume automatic" : "Jobs: Pause automatic";
}
static const devos_command_t CMD_PAUSE = {
    .title = "Jobs: Pause automatic", .label = cmd_pause_label,
    .keywords = "automation jobs pause resume hold stop", .hint = "Jobs",
    .icon = LV_SYMBOL_PAUSE, .run = cmd_pause_jobs,
};

/* ---- tabs, Overview, Runs, Problems, footer (P1) ---- */
static bool selected_summary(devos_job_summary_t *out)
{
    if (s_ctx.n == 0) return false;
    const char *id = s_ctx.ids[s_ctx.sel];
    for (int i = 0; i < devos_jobs_count(); i++)
        if (devos_jobs_summary_at(i, out) && strcmp(out->id, id) == 0) return true;
    return false;
}

static void trigger_words(const char *trig, char *out, size_t cap)
{
    if (!trig) { snprintf(out, cap, "-"); return; }
    if (strncmp(trig, "every ", 6) == 0) {
        snprintf(out, cap, "Every %s", trig + 6);      /* "every 5m" -> "Every 5m" */
    } else if (strncmp(trig, "daily ", 6) == 0) {
        snprintf(out, cap, "Every day at %s", trig + 6);
    } else if (strncmp(trig, "weekdays ", 9) == 0) {
        snprintf(out, cap, "Weekdays at %s", trig + 9);
    } else if (strncmp(trig, "on ", 3) == 0) {
        snprintf(out, cap, "When \"%s\" fires", trig + 3);
    } else {
        snprintf(out, cap, "Only when you press Run now");
    }
}

static void refresh_overview(void)
{
    if (!s_ctx.ov) return;
    devos_job_summary_t sum;
    if (!selected_summary(&sum)) {
        devos_w_set_text(s_ctx.lbl_ov_trigger, "Select a job on the left, or press Sym+N for a new one.");
        devos_w_set_text(s_ctx.lbl_ov_next, "");
        devos_w_set_text(s_ctx.lbl_ov_policy, "");
        devos_w_set_text(s_ctx.lbl_ov_runs, "");
        return;
    }
    char words[64], b[160];
    trigger_words(sum.trigger, words, sizeof(words));
    snprintf(b, sizeof(b), "%s\n%s", words,
             sum.state == DEVOS_JOB_ENABLED ? "Enabled" : "Disabled");
    devos_w_set_text(s_ctx.lbl_ov_trigger, b);

    if (sum.running) snprintf(b, sizeof(b), "Running now");
    else if (sum.next_run_in_ms > 0) {
        char d[16]; fmt_dur(sum.next_run_in_ms, d, sizeof(d));
        snprintf(b, sizeof(b), "Next run in %s", d);
    } else snprintf(b, sizeof(b), "No scheduled next run");
    devos_w_set_text(s_ctx.lbl_ov_next, b);

    /* policy from the source (best effort) */
    snprintf(b, sizeof(b), "Last: %s%s%s", sum.last_result[0] ? sum.last_result : "never run",
             sum.last_cause[0] && strcmp(sum.last_cause, "-") ? "  -  " : "",
             sum.last_cause[0] && strcmp(sum.last_cause, "-") ? sum.last_cause : "");
    devos_w_set_text(s_ctx.lbl_ov_policy, b);

    devos_run_record_t rec[5];
    int n = devos_jobs_history_recent(s_ctx.ids[s_ctx.sel], rec, 5);
    char runs[320];
    size_t o = 0;
    runs[0] = '\0';
    if (n == 0) o += (size_t)snprintf(runs + o, sizeof(runs) - o, "No runs yet.");
    for (int i = 0; i < n && o < sizeof(runs) - 40; i++) {
        char d[16]; fmt_dur(rec[i].duration_ms, d, sizeof(d));
        o += (size_t)snprintf(runs + o, sizeof(runs) - o, "%s  %-8s %s%s\n",
                              rec[i].ok ? "OK  " : "FAIL", rec[i].cause[0] ? rec[i].cause : "-", d,
                              rec[i].error[0] ? rec[i].error : "");
    }
    devos_w_set_text(s_ctx.lbl_ov_runs, runs);
}

static void refresh_runs(void)
{
    if (!s_ctx.runs || !s_ctx.runs_panel) return;
    static EXT_RAM_BSS_ATTR char buf[4096];   /* devos_codeview keeps the pointer */
    if (s_ctx.n == 0) { snprintf(buf, sizeof(buf), "No job selected.\n"); devos_codeview_set(&s_ctx.runs_cv, buf); return; }
    devos_run_record_t rec[16];
    int n = devos_jobs_history_recent(s_ctx.ids[s_ctx.sel], rec, 16);
    size_t o = 0;
    o += (size_t)snprintf(buf + o, sizeof(buf) - o,
                          "%-9s %-9s %-9s %-6s %s\n", "time", "cause", "duration", "result", "error");
    if (n == 0) o += (size_t)snprintf(buf + o, sizeof(buf) - o, "(no runs recorded)\n");
    for (int i = 0; i < n && o < sizeof(buf) - 96; i++) {
        char d[16]; fmt_dur(rec[i].duration_ms, d, sizeof(d));
        char when[24] = "?";
        if (rec[i].wall > 0) {
            time_t t = (time_t)rec[i].wall;
            struct tm tm;
            localtime_r(&t, &tm);
            strftime(when, sizeof(when), "%d %H:%M", &tm);
        }
        o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%-9s %-9s %-9s %-6s %s\n",
                              when, rec[i].cause[0] ? rec[i].cause : "-", d,
                              rec[i].ok ? "ok" : "failed", rec[i].error);
    }
    devos_codeview_set(&s_ctx.runs_cv, buf);
}

/* Tag the Problems strip with the first validation diagnostic (P1). */
static void refresh_problems(void)
{
    if (!s_ctx.lbl_problems) return;
    const char *src = cur_source();
    char diag[160];
    if (s_ctx.n == 0) { devos_w_set_text(s_ctx.lbl_problems, ""); return; }
    if (devos_jobs_check(src, strlen(src), diag, sizeof(diag)) == DEVOS_OK) {
        const char *adv = devos_jobs_topic_advisory();
        if (adv) {
            char b[200];
            snprintf(b, sizeof(b), "Warning: %s", adv);
            devos_w_set_text(s_ctx.lbl_problems, b);
        } else {
            devos_w_set_text(s_ctx.lbl_problems, s_ctx.dirty ? "Ready - unsaved changes (Sym+A to apply)"
                                                             : "Ready - no problems");
        }
    } else {
        char b[200];
        snprintf(b, sizeof(b), "Problem: %s", diag);
        devos_w_set_text(s_ctx.lbl_problems, b);
    }
}

static void update_footer(void)
{
    if (!s_ctx.lbl_hint) return;
    const char *h = s_ctx.tab == TAB_TEXT ? "Typing edits the source  Ctrl+S apply  Sym+O overview  Sym+Y runs  Esc list"
                  : s_ctx.tab == TAB_OVERVIEW ? "Sym+B builder  Sym+M text  Sym+Y runs  Sym+A apply  Sym+G enable  Sym+E settings  Esc list"
                  : s_ctx.tab == TAB_RUNS ? "Up / Down  pick  Sym+O overview  Sym+B builder  Esc list"
                  : "Up / Down  pick a step  Sym+U add  Del delete  Sym+K/Sym+J move  Sym+O overview  Sym+M text";
    if (!s_ctx.list_visible)
        h = s_ctx.tab == TAB_OVERVIEW ? "Sym+L jobs  Sym+B builder  Sym+M text  Sym+Y runs  Sym+A apply"
                                      : "Sym+L jobs  Sym+O overview  Sym+Y runs  Esc";
    devos_w_set_text(s_ctx.lbl_hint, h);
}

static void set_tab(jobs_tab_t t);

static void tab_cb(lv_event_t *e)
{
    set_tab((jobs_tab_t)(intptr_t)lv_event_get_user_data(e));
}

static void list_row_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_ctx.n) return;
    draft_save();
    s_ctx.sel = idx;
    list_highlight();
    load_selected();
    if (s_ctx.tab == TAB_BUILDER) set_mode(MODE_BUILDER);
    if (s_ctx.tab == TAB_OVERVIEW) refresh_overview();
    if (s_ctx.tab == TAB_RUNS) refresh_runs();
}

static void pause_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    devos_jobs_set_paused(!devos_jobs_paused());
    refresh_list();
    devos_toast_show(devos_jobs_paused() ? "Automatic jobs paused" : "Automatic jobs resumed",
                     DEVOS_TOAST_OK, 0);
}

static void run_cancel_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_ctx.n == 0) return;
    devos_job_summary_t sum;
    if (selected_summary(&sum) && sum.running) act_cancel();
    else act_run();
}

/* Move the job-list selection (loads the newly picked job). */
static void list_move(int dir)
{
    if (s_ctx.n == 0) return;
    int ni = s_ctx.sel + dir;
    if (ni < 0) ni = 0;
    if (ni >= s_ctx.n) ni = s_ctx.n - 1;
    if (ni == s_ctx.sel) return;
    draft_save();
    s_ctx.sel = ni;
    list_highlight();
    load_selected();
    if (s_ctx.tab == TAB_BUILDER) set_mode(MODE_BUILDER);
    if (s_ctx.tab == TAB_OVERVIEW) refresh_overview();
    if (s_ctx.tab == TAB_RUNS) refresh_runs();
}

static void list_toggle_enabled(void)
{
    if (s_ctx.n == 0) return;
    devos_job_summary_t sum;
    bool en = selected_summary(&sum) && sum.state == DEVOS_JOB_ENABLED;
    devos_jobs_set_enabled(s_ctx.ids[s_ctx.sel], !en);
    refresh_list();
    load_selected();
    devos_toast_show(!en ? "Enabled" : "Disabled", DEVOS_TOAST_OK, 0);
}

/* Sym+L: show / hide the 260 px job list (invariant 3). */
static void toggle_list(void)
{
    s_ctx.list_visible = !s_ctx.list_visible;
    set_visible(s_ctx.list_pane, s_ctx.list_visible);
    if (!s_ctx.list_visible) { s_in_list = false; devos_focus_first(&s_ctx.focus); }
    else { s_in_list = true; devos_focus_clear(&s_ctx.focus); }
    update_footer();
}

static void set_tab(jobs_tab_t t)
{
    if (t >= TAB_COUNT) t = TAB_OVERVIEW;
    if (t == s_ctx.tab) {
        if (t == TAB_OVERVIEW) refresh_overview();
        return;
    }
    if (s_ctx.tab == TAB_BUILDER && t != TAB_BUILDER && s_ctx.build.ast) builder_commit_all();
    s_ctx.tab = t;
    if (t == TAB_BUILDER) set_mode(MODE_BUILDER);
    else set_mode(MODE_TEXT);               /* Text, Overview and Runs hold no Builder */
    set_visible(s_ctx.bld, t == TAB_BUILDER);
    set_visible(s_ctx.ta_src, t == TAB_TEXT);
    set_visible(s_ctx.ov, t == TAB_OVERVIEW);
    set_visible(s_ctx.runs, t == TAB_RUNS);
    if (t == TAB_OVERVIEW) refresh_overview();
    if (t == TAB_RUNS) refresh_runs();
    for (int i = 0; i < TAB_COUNT; i++) {
        if (!s_ctx.tab_btn[i]) continue;
        if (i == (int)t) lv_obj_add_state(s_ctx.tab_btn[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(s_ctx.tab_btn[i], LV_STATE_CHECKED);
    }
    if (t == TAB_BUILDER) devos_focus_set(&s_ctx.focus, s_ctx.step_list);
    else if (t == TAB_TEXT) devos_focus_set(&s_ctx.focus, s_ctx.ta_src);
    else if (t == TAB_OVERVIEW) devos_focus_set(&s_ctx.focus, s_ctx.btn_apply);
    else if (t == TAB_RUNS) devos_focus_set(&s_ctx.focus, s_ctx.runs_panel);
    s_in_list = false;                      /* a tab switch leaves the job list */
    update_footer();
}


static void jobs_init(void)
{
    lv_obj_t *scr = s_ctx.screen = devos_w_screen(&s_desc);
    devos_w_bar(scr, "Jobs", NULL);

    /* ---- left job list (260 px, invariant 3; Sym+L toggles) ---- */
    s_ctx.list_pane = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ctx.list_pane);
    devos_w_track(s_ctx.list_pane, DEVOS_W_PANEL_ALT);
    lv_obj_set_pos(s_ctx.list_pane, 0, DEVOS_W_BAR_H);
    lv_obj_set_size(s_ctx.list_pane, DEVOS_PANE_LEFT_WIDTH, DEVOS_CONTENT_HEIGHT - DEVOS_W_BAR_H);
    lv_obj_set_style_border_side(s_ctx.list_pane, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_add_flag(s_ctx.list_pane, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_ctx.list_pane, LV_DIR_VER);
    lv_obj_set_style_pad_all(s_ctx.list_pane, 6, 0);
    lv_obj_t *lp = devos_w_label(s_ctx.list_pane, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM,
                                 "JOBS   (Sym+L hides)");
    lv_obj_set_pos(lp, 2, 0);
    for (int i = 0; i < DEVOS_JOBS_MAX; i++) {
        lv_obj_t *r = lv_obj_create(s_ctx.list_pane);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, DEVOS_PANE_LEFT_WIDTH - 24, 46);
        lv_obj_set_pos(r, 0, 22 + i * 48);
        lv_obj_set_style_radius(r, 4, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_set_style_pad_all(r, 2, 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, list_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_ctx.list_row[i] = r;
        s_ctx.list_lbl[i] = devos_w_label(r, &lv_font_montserrat_12, DEVOS_W_TEXT, "");
        lv_obj_set_width(s_ctx.list_lbl[i], DEVOS_PANE_LEFT_WIDTH - 32);
        lv_label_set_long_mode(s_ctx.list_lbl[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(s_ctx.list_lbl[i], 6, 2);
        lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
    }
    s_ctx.list_visible = true;

    const int RX = DEVOS_PANE_LEFT_WIDTH + 8;
    const int RW = DEVOS_SCREEN_WIDTH - RX - 8;

    /* ---- header: name + state + counts + pause / new / delete ---- */
    s_ctx.lbl_name = devos_w_label(scr, NULL, DEVOS_W_TEXT_ACCENT, "");
    lv_obj_set_pos(s_ctx.lbl_name, RX, 46);
    s_ctx.lbl_state = devos_w_label(scr, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "");
    lv_obj_set_pos(s_ctx.lbl_state, RX, 66);
    s_ctx.lbl_header = devos_w_label(scr, &lv_font_montserrat_12, DEVOS_W_TEXT_MUTED, "");
    lv_obj_set_pos(s_ctx.lbl_header, RX + 300, 48);
    s_ctx.btn_pause = devos_w_btn(scr, LV_SYMBOL_PAUSE "  Pause", 110, pause_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_pause, DEVOS_SCREEN_WIDTH - 122, 44);
    s_ctx.btn_new = devos_w_btn(scr, "New  [Sym+N]", 110, new_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_new, DEVOS_SCREEN_WIDTH - 240, 44);
    s_ctx.btn_del = devos_w_btn(scr, "Delete  [Sym+D]", 120, del_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_del, DEVOS_SCREEN_WIDTH - 372, 44);

    /* ---- tabs: Overview / Builder / Text / Runs ---- */
    static const char *tabnames[TAB_COUNT] = { "Overview", "Builder", "Text", "Runs" };
    int tx = RX;
    for (int i = 0; i < TAB_COUNT; i++) {
        s_ctx.tab_btn[i] = devos_w_btn(scr, tabnames[i], 104, tab_cb, (void *)(intptr_t)i, NULL);
        lv_obj_set_pos(s_ctx.tab_btn[i], tx, 84);
        tx += 110;
    }
    s_ctx.lbl_mode = devos_w_label(scr, NULL, DEVOS_W_TEXT_ACCENT, "");
    lv_obj_add_flag(s_ctx.lbl_mode, LV_OBJ_FLAG_HIDDEN);

    const int CY = 118, CH = 424;

    /* ---- Overview ---- */
    s_ctx.ov = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ctx.ov);
    lv_obj_set_pos(s_ctx.ov, RX, CY);
    lv_obj_set_size(s_ctx.ov, RW, CH);
    lv_obj_remove_flag(s_ctx.ov, LV_OBJ_FLAG_SCROLLABLE);
    mk_label(s_ctx.ov, "TRIGGER", DEVOS_W_TEXT_DIM, 0, 0);
    s_ctx.lbl_ov_trigger = devos_w_label(s_ctx.ov, NULL, DEVOS_W_TEXT, "");
    lv_obj_set_pos(s_ctx.lbl_ov_trigger, 0, 22);
    lv_obj_set_width(s_ctx.lbl_ov_trigger, RW);
    mk_label(s_ctx.ov, "NEXT RUN", DEVOS_W_TEXT_DIM, 0, 78);
    s_ctx.lbl_ov_next = devos_w_label(s_ctx.ov, NULL, DEVOS_W_TEXT, "");
    lv_obj_set_pos(s_ctx.lbl_ov_next, 0, 100);
    mk_label(s_ctx.ov, "LAST RUN", DEVOS_W_TEXT_DIM, 0, 134);
    s_ctx.lbl_ov_policy = devos_w_label(s_ctx.ov, NULL, DEVOS_W_TEXT, "");
    lv_obj_set_pos(s_ctx.lbl_ov_policy, 0, 156);
    lv_obj_set_width(s_ctx.lbl_ov_policy, RW);
    mk_label(s_ctx.ov, "RECENT RUNS", DEVOS_W_TEXT_DIM, 0, 198);
    s_ctx.lbl_ov_runs = devos_w_label(s_ctx.ov, devos_w_mono(), DEVOS_W_TEXT, "");
    lv_obj_set_pos(s_ctx.lbl_ov_runs, 0, 220);
    lv_obj_set_width(s_ctx.lbl_ov_runs, RW);
    lv_obj_t *btn_revs = devos_w_btn(s_ctx.ov, "Revisions  [Sym+Z]", 170, revs_cb, NULL, NULL);
    lv_obj_set_pos(btn_revs, 0, CH - 40);
    s_ctx.btn_revs = btn_revs;

    /* ---- Text ---- */
    s_ctx.ta_src = devos_w_ta(scr, false, RW, CH);
    lv_obj_set_pos(s_ctx.ta_src, RX, CY);
    lv_textarea_set_max_length(s_ctx.ta_src, SRCMAX);
    lv_obj_add_event_cb(s_ctx.ta_src, src_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(s_ctx.ta_src, LV_OBJ_FLAG_HIDDEN);

    /* ---- Runs ---- */
    s_ctx.runs = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ctx.runs);
    lv_obj_set_pos(s_ctx.runs, RX, CY);
    lv_obj_set_size(s_ctx.runs, RW, CH);
    lv_obj_remove_flag(s_ctx.runs, LV_OBJ_FLAG_SCROLLABLE);
    s_ctx.runs_panel = lv_obj_create(s_ctx.runs);
    lv_obj_remove_style_all(s_ctx.runs_panel);
    devos_w_track(s_ctx.runs_panel, DEVOS_W_CODE);
    lv_obj_set_size(s_ctx.runs_panel, RW, CH);
    lv_obj_set_pos(s_ctx.runs_panel, 0, 0);
    lv_obj_set_style_pad_all(s_ctx.runs_panel, 8, 0);
    devos_codeview_create(&s_ctx.runs_cv, s_ctx.runs_panel);
    s_ctx.runs_cv.plain = true;
    lv_obj_add_flag(s_ctx.runs, LV_OBJ_FLAG_HIDDEN);

    /* ---- Builder (kept; now one tab) ---- */
    s_ctx.bld = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ctx.bld);
    lv_obj_set_pos(s_ctx.bld, RX, CY);
    lv_obj_set_size(s_ctx.bld, RW, CH);
    lv_obj_remove_flag(s_ctx.bld, LV_OBJ_FLAG_SCROLLABLE);

    mk_label(s_ctx.bld, "Trigger", DEVOS_W_TEXT_DIM, 0, 0);
    s_ctx.dd_kind = devos_w_dd(s_ctx.bld, "Manual\nEvery\nDaily\nWeekdays\nEvent", 150);
    lv_obj_set_pos(s_ctx.dd_kind, 0, 18);
    lv_obj_add_event_cb(s_ctx.dd_kind, kind_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_ctx.ta_trig = devos_w_ta(s_ctx.bld, true, 190, 36);
    lv_obj_set_pos(s_ctx.ta_trig, 160, 18);
    lv_textarea_set_max_length(s_ctx.ta_trig, 48);
    /* P2: hour/minute spinners for Daily/Weekdays (Left/Right change them) */
    {
        static char hours[24 * 3], mins[60 * 3];
        if (!hours[0]) {
            size_t o = 0;
            for (int h = 0; h < 24; h++) o += (size_t)snprintf(hours + o, sizeof(hours) - o, "%s%02d", h ? "\n" : "", h);
            o = 0;
            for (int m = 0; m < 60; m++) o += (size_t)snprintf(mins + o, sizeof(mins) - o, "%s%02d", m ? "\n" : "", m);
        }
        s_ctx.dd_hour = devos_w_dd(s_ctx.bld, hours, 80);
        lv_obj_set_pos(s_ctx.dd_hour, 160, 18);
        lv_obj_add_flag(s_ctx.dd_hour, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(s_ctx.dd_hour, time_spinner_cb, LV_EVENT_VALUE_CHANGED, NULL);
        s_ctx.dd_min = devos_w_dd(s_ctx.bld, mins, 80);
        lv_obj_set_pos(s_ctx.dd_min, 250, 18);
        lv_obj_add_flag(s_ctx.dd_min, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(s_ctx.dd_min, time_spinner_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }
    s_ctx.dd_topic = devos_w_dd(s_ctx.bld, "(no topics registered)", 250);
    lv_obj_set_pos(s_ctx.dd_topic, 160, 18);
    lv_obj_add_flag(s_ctx.dd_topic, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_ctx.dd_topic, topic_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_ctx.ta_where = devos_w_ta(s_ctx.bld, true, RW - 430, 36);
    lv_obj_set_pos(s_ctx.ta_where, 420, 18);
    lv_textarea_set_max_length(s_ctx.ta_where, 160);
    lv_obj_add_flag(s_ctx.ta_where, LV_OBJ_FLAG_HIDDEN);

    mk_label(s_ctx.bld, "Steps  (tap to select, drag to reorder)", DEVOS_W_TEXT_DIM, 0, 56);
    s_ctx.step_list = lv_obj_create(s_ctx.bld);
    lv_obj_set_pos(s_ctx.step_list, 0, 72);
    lv_obj_set_size(s_ctx.step_list, RW, 120);
    lv_obj_set_style_radius(s_ctx.step_list, 4, 0);
    lv_obj_set_style_pad_all(s_ctx.step_list, 2, 0);
    lv_obj_set_style_border_width(s_ctx.step_list, 1, 0);
    devos_w_track(s_ctx.step_list, DEVOS_W_PANEL);
    lv_obj_add_flag(s_ctx.step_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_ctx.step_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_ctx.step_list, LV_DIR_VER);   /* rows are full width */
    for (int i = 0; i < STEP_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(s_ctx.step_list);
        lv_obj_set_size(row, RW - 6, STEP_H - 4);
        lv_obj_set_pos(row, 0, i * STEP_H);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_pad_all(row, 2, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, step_row_cb, LV_EVENT_ALL, (void *)(intptr_t)i);
        s_ctx.step_row[i] = row;
        s_ctx.step_lbl[i] = devos_w_label(row, NULL, DEVOS_W_TEXT, "");
        lv_obj_set_pos(s_ctx.step_lbl[i], 4, 2);
        lv_obj_set_width(s_ctx.step_lbl[i], RW - 20);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }

    s_ctx.dd_add = devos_w_dd(s_ctx.bld, "", 300);
    lv_obj_set_pos(s_ctx.dd_add, 0, 202);
    s_ctx.ta_add_filter = devos_w_ta(s_ctx.bld, true, 150, 36);
    lv_obj_set_pos(s_ctx.ta_add_filter, 745, 202);
    lv_textarea_set_max_length(s_ctx.ta_add_filter, 32);
    lv_textarea_set_placeholder_text(s_ctx.ta_add_filter, "Filter steps");
    lv_obj_add_event_cb(s_ctx.ta_add_filter, add_filter_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_ctx.btn_add = devos_w_btn(s_ctx.bld, "Add  [Sym+U]", 110, add_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_add, 310, 202);
    s_ctx.btn_bdel = devos_w_btn(s_ctx.bld, "Del step", 100, bdel_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_bdel, 425, 202);
    s_ctx.btn_up = devos_w_btn(s_ctx.bld, "Up  [Sym+K]", 100, up_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_up, 530, 202);
    s_ctx.btn_dn = devos_w_btn(s_ctx.bld, "Down  [Sym+J]", 100, dn_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_dn, 635, 202);

    s_ctx.lbl_settings = mk_label(s_ctx.bld, "Step settings", DEVOS_W_TEXT_DIM, 0, 238);
    /* legacy single-value widgets, used by the non-action inspectors */
    s_ctx.dd_param = devos_w_dd(s_ctx.bld, "", 220);
    lv_obj_set_pos(s_ctx.dd_param, 0, 286);
    lv_obj_add_event_cb(s_ctx.dd_param, param_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(s_ctx.dd_param, LV_OBJ_FLAG_HIDDEN);
    s_ctx.dd_expr = devos_w_dd(s_ctx.bld, "Literal\nExpression", 140);
    lv_obj_set_pos(s_ctx.dd_expr, 230, 286);
    lv_obj_add_flag(s_ctx.dd_expr, LV_OBJ_FLAG_HIDDEN);
    s_ctx.dd_choice = devos_w_dd(s_ctx.bld, "", 200);
    lv_obj_set_pos(s_ctx.dd_choice, 380, 286);
    lv_obj_add_flag(s_ctx.dd_choice, LV_OBJ_FLAG_HIDDEN);
    s_ctx.ta_val = devos_w_ta(s_ctx.bld, true, RW, 36);
    lv_obj_set_pos(s_ctx.ta_val, 0, 296);
    lv_textarea_set_max_length(s_ctx.ta_val, 240);
    lv_obj_add_event_cb(s_ctx.ta_val, val_commit_cb, LV_EVENT_DEFOCUSED, NULL);
    s_ctx.lbl_custom = devos_w_label(s_ctx.bld, NULL, DEVOS_W_TEXT_MUTED, "");
    lv_obj_set_pos(s_ctx.lbl_custom, 0, 256);
    lv_obj_set_width(s_ctx.lbl_custom, RW);
    lv_obj_add_flag(s_ctx.lbl_custom, LV_OBJ_FLAG_HIDDEN);

    /* P2: per-parameter form (one labelled row per action parameter) */
    s_ctx.form = lv_obj_create(s_ctx.bld);
    lv_obj_remove_style_all(s_ctx.form);
    lv_obj_set_pos(s_ctx.form, 0, 256);
    lv_obj_set_size(s_ctx.form, RW, CH - 256);
    lv_obj_set_style_pad_all(s_ctx.form, 0, 0);
    lv_obj_set_style_pad_row(s_ctx.form, 6, 0);
    lv_obj_set_flex_flow(s_ctx.form, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_ctx.form, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_ctx.form, LV_DIR_VER);
    s_ctx.form_hdr = devos_w_label(s_ctx.form, NULL, DEVOS_W_TEXT_ACCENT, "");
    for (int i = 0; i < PARAM_MAX; i++) {
        lv_obj_t *row = lv_obj_create(s_ctx.form);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, RW - 4);
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        s_ctx.form_rows[i] = row;
        s_ctx.form_lbl[i] = devos_w_label(row, NULL, DEVOS_W_TEXT, "");
        /* value line: field + Literal/Expr toggle (inside a row) */
        lv_obj_t *line = lv_obj_create(row);
        lv_obj_remove_style_all(line);
        lv_obj_set_width(line, RW - 8);
        lv_obj_set_height(line, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(line, 0, 0);
        lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(line, 8, 0);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        s_ctx.form_ta[i] = devos_w_ta(line, true, RW - 160, 34);
        lv_textarea_set_max_length(s_ctx.form_ta[i], 240);
        lv_obj_add_event_cb(s_ctx.form_ta[i], form_row_commit_cb, LV_EVENT_DEFOCUSED, (void *)(intptr_t)i);
        s_ctx.form_dd[i] = devos_w_dd(line, "", RW - 160);
        lv_obj_add_event_cb(s_ctx.form_dd[i], form_row_commit_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        s_ctx.form_expr[i] = devos_w_dd(line, "Literal\nExpression", 140);
        lv_obj_add_event_cb(s_ctx.form_expr[i], form_row_commit_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        s_ctx.form_help[i] = devos_w_label(row, &lv_font_montserrat_12, DEVOS_W_TEXT_MUTED, "");
        lv_obj_set_width(s_ctx.form_help[i], RW - 12);
        lv_label_set_long_mode(s_ctx.form_help[i], LV_LABEL_LONG_WRAP);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
    /* Save result as */
    lv_obj_t *oline = lv_obj_create(s_ctx.form);
    lv_obj_remove_style_all(oline);
    lv_obj_set_width(oline, RW - 4);
    lv_obj_set_height(oline, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(oline, 0, 0);
    lv_obj_set_flex_flow(oline, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(oline, 8, 0);
    lv_obj_remove_flag(oline, LV_OBJ_FLAG_SCROLLABLE);
    s_ctx.form_out_lbl = devos_w_label(oline, NULL, DEVOS_W_TEXT, "Save result as");
    s_ctx.form_out_ta = devos_w_ta(oline, true, RW - 200, 34);
    lv_textarea_set_max_length(s_ctx.form_out_ta, 39);
    lv_obj_add_event_cb(s_ctx.form_out_ta, form_row_commit_cb, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_flag(s_ctx.form, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ctx.bld, LV_OBJ_FLAG_HIDDEN);

    /* ---- Problems strip + toolbar ---- */
    s_ctx.lbl_problems = devos_w_label(scr, &lv_font_montserrat_12, DEVOS_W_TEXT_WARN, "");
    lv_obj_set_pos(s_ctx.lbl_problems, RX, CY + CH + 6);
    lv_obj_set_width(s_ctx.lbl_problems, RW);

    int ty = CY + CH + 30;
    s_ctx.btn_validate = devos_w_btn(scr, "Validate  [Sym+C]", 140, validate_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_validate, RX, ty);
    s_ctx.btn_apply = devos_w_btn_kind(scr, DEVOS_W_BTN_PRIMARY, LV_SYMBOL_OK " Apply  [Sym+A]", 140, apply_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_apply, RX + 150, ty);
    s_ctx.btn_enable = devos_w_btn(scr, "Enable  [Sym+G]", 140, enable_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_enable, RX + 300, ty);
    s_ctx.btn_run = devos_w_btn(scr, "Run now  [Sym+R]", 140, run_cancel_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_run, RX + 450, ty);
    lv_obj_t *btn_jobset = devos_w_btn(scr, "Settings  [Sym+E]", 150, jobset_cb, NULL, NULL);
    lv_obj_set_pos(btn_jobset, RX + 600, ty);
    lv_obj_t *btn_dry = devos_w_btn(scr, "Dry run  [Sym+W]", 140, dryrun_cb, NULL, NULL);
    lv_obj_set_pos(btn_dry, RX + 760, ty);

    s_ctx.lbl_hint = devos_w_label(scr, &lv_font_montserrat_12, DEVOS_W_TEXT_MUTED, "");
    lv_obj_set_pos(s_ctx.lbl_hint, RX, ty + 34);
    lv_obj_set_width(s_ctx.lbl_hint, RW);

    /* unused legacy widgets kept out of the layout */
    s_ctx.btn_cancel = NULL;
    s_ctx.btn_hist = NULL;
    s_ctx.out_scroll = NULL;

    /* P0: deleting a job always asks first. */
    devos_w_dialog(&s_del_dlg, scr, 560, 170, LV_SYMBOL_TRASH "  Delete job");
    lv_obj_t *dok = devos_w_btn_kind(s_del_dlg.box, DEVOS_W_BTN_DANGER, "Delete", 130, del_ok_cb, NULL, NULL);
    lv_obj_set_size(dok, 130, 36);
    lv_obj_align(dok, LV_ALIGN_BOTTOM_RIGHT, -142, 0);
    lv_obj_t *dcl = devos_w_btn(s_del_dlg.box, "Cancel", 130, del_cancel_cb, NULL, NULL);
    lv_obj_set_size(dcl, 130, 36);
    lv_obj_align(dcl, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    devos_w_set_text(s_del_dlg.msg, "Enter = delete      Esc = cancel");
    devos_core_add_restart_check(jobs_restart_check);

    /* New-job picker: Blank / Duplicate / jobs/examples starters */
    devos_w_dialog(&s_new_dlg, scr, 560, 170, LV_SYMBOL_PLUS "  New job");
    s_new_dd = devos_w_dd(s_new_dlg.box, "Blank job", 480);
    lv_obj_align(s_new_dd, LV_ALIGN_TOP_LEFT, 0, 44);
    devos_w_set_text(s_new_dlg.msg, "Left/Right pick  Enter = create      Esc = cancel");
    lv_obj_t *nok = devos_w_btn_kind(s_new_dlg.box, DEVOS_W_BTN_PRIMARY, "Create", 120, new_ok_cb, NULL, NULL);
    lv_obj_set_size(nok, 120, 36);
    lv_obj_align(nok, LV_ALIGN_BOTTOM_RIGHT, -132, 0);
    lv_obj_t *ncl = devos_w_btn(s_new_dlg.box, "Cancel", 120, new_cancel_cb, NULL, NULL);
    lv_obj_set_size(ncl, 120, 36);
    lv_obj_align(ncl, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    devos_focus_init(&s_new_focus);
    devos_focus_add(&s_new_focus, s_new_dd);
    devos_focus_add(&s_new_focus, nok);
    devos_focus_add(&s_new_focus, ncl);

    /* Dry-run result dialog: verdict + per-step trace as source lines */
    devos_w_dialog(&s_dry_dlg, scr, 760, 420, LV_SYMBOL_PLAY "  Dry run");
    lv_obj_t *dry_panel = lv_obj_create(s_dry_dlg.box);
    lv_obj_remove_style_all(dry_panel);
    devos_w_track(dry_panel, DEVOS_W_CODE);
    lv_obj_set_size(dry_panel, 720, 300);
    lv_obj_align(dry_panel, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_set_style_pad_all(dry_panel, 8, 0);
    devos_codeview_create(&s_dry_cv, dry_panel);
    s_dry_cv.plain = true;
    lv_obj_t *dry_ok = devos_w_btn(s_dry_dlg.box, "Close", 120, dry_ok_cb, NULL, NULL);
    lv_obj_set_size(dry_ok, 120, 36);
    lv_obj_align(dry_ok, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    /* Revisions dialog: generation picker, View/Diff, rollback */
    devos_w_dialog(&s_rev_dlg, scr, 760, 460, LV_SYMBOL_REFRESH "  Revisions");
    s_rev_dd = devos_w_dd(s_rev_dlg.box, "", 220);
    lv_obj_align(s_rev_dd, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_add_event_cb(s_rev_dd, rev_pick_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_rev_mode = devos_w_dd(s_rev_dlg.box, "View\nDiff vs draft", 180);
    lv_obj_align(s_rev_mode, LV_ALIGN_TOP_LEFT, 232, 40);
    lv_obj_add_event_cb(s_rev_mode, rev_pick_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *rev_panel = lv_obj_create(s_rev_dlg.box);
    lv_obj_remove_style_all(rev_panel);
    devos_w_track(rev_panel, DEVOS_W_CODE);
    lv_obj_set_size(rev_panel, 720, 300);
    lv_obj_align(rev_panel, LV_ALIGN_TOP_LEFT, 0, 84);
    lv_obj_set_style_pad_all(rev_panel, 8, 0);
    devos_codeview_create(&s_rev_cv, rev_panel);
    s_rev_cv.plain = true;
    devos_w_set_text(s_rev_dlg.msg, "Enter = roll back (adds a new revision)      Esc = close");
    lv_obj_t *rok = devos_w_btn_kind(s_rev_dlg.box, DEVOS_W_BTN_PRIMARY, "Roll back", 130, rev_ok_cb, NULL, NULL);
    lv_obj_set_size(rok, 130, 36);
    lv_obj_align(rok, LV_ALIGN_BOTTOM_RIGHT, -132, 0);
    lv_obj_t *rcl = devos_w_btn(s_rev_dlg.box, "Close", 120, rev_cancel_cb, NULL, NULL);
    lv_obj_set_size(rcl, 120, 36);
    lv_obj_align(rcl, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    devos_focus_init(&s_rev_focus);
    devos_focus_add(&s_rev_focus, s_rev_dd);
    devos_focus_add(&s_rev_focus, s_rev_mode);
    devos_focus_add(&s_rev_focus, rok);
    devos_focus_add(&s_rev_focus, rcl);

    /* Job settings dialog: name, timeout, overlap, cooldown */
    devos_w_dialog(&s_ctx.dlg_job, scr, 560, 360, LV_SYMBOL_SETTINGS "  Job settings");
    lv_obj_t *jrow = lv_obj_create(s_ctx.dlg_job.box);
    lv_obj_remove_style_all(jrow);
    lv_obj_set_pos(jrow, 0, 42);
    lv_obj_set_size(jrow, 520, 250);
    lv_obj_remove_flag(jrow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(jrow, 0, 0);
    mk_label(jrow, "Name", DEVOS_W_TEXT_DIM, 0, 0);
    s_ctx.job_ta_name = devos_w_ta(jrow, true, 500, 34);
    lv_obj_set_pos(s_ctx.job_ta_name, 0, 20);
    lv_textarea_set_max_length(s_ctx.job_ta_name, DEVOS_JOBS_NAME_MAX - 1);
    mk_label(jrow, "Timeout  (empty = no limit, e.g. 30s)", DEVOS_W_TEXT_DIM, 0, 62);
    s_ctx.job_ta_timeout = devos_w_ta(jrow, true, 500, 34);
    lv_obj_set_pos(s_ctx.job_ta_timeout, 0, 82);
    lv_textarea_set_max_length(s_ctx.job_ta_timeout, 20);
    mk_label(jrow, "Overlap", DEVOS_W_TEXT_DIM, 0, 124);
    s_ctx.job_dd_overlap = devos_w_dd(jrow, "skip\nqueue one", 200);
    lv_obj_set_pos(s_ctx.job_dd_overlap, 0, 144);
    mk_label(jrow, "Cooldown  (empty = none, e.g. 5m)", DEVOS_W_TEXT_DIM, 0, 186);
    s_ctx.job_ta_cooldown = devos_w_ta(jrow, true, 500, 34);
    lv_obj_set_pos(s_ctx.job_ta_cooldown, 0, 206);
    lv_textarea_set_max_length(s_ctx.job_ta_cooldown, 20);
    devos_w_set_text(s_ctx.dlg_job.msg, "Enter = save      Esc = cancel");
    lv_obj_t *jok = devos_w_btn_kind(s_ctx.dlg_job.box, DEVOS_W_BTN_PRIMARY, "Save", 120, job_ok_cb, NULL, NULL);
    lv_obj_set_size(jok, 120, 36);
    lv_obj_align(jok, LV_ALIGN_BOTTOM_RIGHT, -132, 0);
    lv_obj_t *jcl = devos_w_btn(s_ctx.dlg_job.box, "Cancel", 120, job_cancel_cb, NULL, NULL);
    lv_obj_set_size(jcl, 120, 36);
    lv_obj_align(jcl, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    devos_focus_init(&s_ctx.job_focus);
    devos_focus_add(&s_ctx.job_focus, s_ctx.job_ta_name);
    devos_focus_add(&s_ctx.job_focus, s_ctx.job_ta_timeout);
    devos_focus_add(&s_ctx.job_focus, s_ctx.job_dd_overlap);
    devos_focus_add(&s_ctx.job_focus, s_ctx.job_ta_cooldown);
    devos_focus_add(&s_ctx.job_focus, jok);
    devos_focus_add(&s_ctx.job_focus, jcl);

    lv_obj_t *keys = devos_w_keys(scr);
    devos_w_set_text(keys, "Up/Down jobs  Enter open  Space enable  R run  |  "
                           "Sym+B Builder  Sym+M Text  Sym+O Overview  Sym+Y Runs  Sym+A Apply  Sym+D Delete  Sym+S keys");

    devos_focus_init(&s_ctx.focus);
    for (int i = 0; i < TAB_COUNT; i++) devos_focus_add(&s_ctx.focus, s_ctx.tab_btn[i]);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_new);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_del);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_pause);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_kind);
    devos_focus_add(&s_ctx.focus, s_ctx.ta_trig);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_hour);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_min);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_topic);
    devos_focus_add(&s_ctx.focus, s_ctx.ta_where);
    devos_focus_add(&s_ctx.focus, s_ctx.step_list);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_add);
    devos_focus_add(&s_ctx.focus, s_ctx.ta_add_filter);
    devos_focus_add(&s_ctx.focus, s_ctx.ta_val);
    for (int i = 0; i < PARAM_MAX; i++) {
        devos_focus_add(&s_ctx.focus, s_ctx.form_ta[i]);
        devos_focus_add(&s_ctx.focus, s_ctx.form_dd[i]);
        devos_focus_add(&s_ctx.focus, s_ctx.form_expr[i]);
    }
    devos_focus_add(&s_ctx.focus, s_ctx.form_out_ta);
    devos_focus_add(&s_ctx.focus, s_ctx.ta_src);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_validate);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_apply);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_enable);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_run);
    devos_focus_add(&s_ctx.focus, btn_dry);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_revs);

    devos_theme_add_listener(steps_theme_cb, NULL);
    devos_cmdpal_add(&CMD_NEW);
    devos_cmdpal_add(&CMD_PAUSE);

    list_refresh();
    load_selected();
    s_ctx.tab = TAB_COUNT;          /* force set_tab to apply visibility */
    set_tab(TAB_OVERVIEW);
    s_in_list = true;               /* keys start in the job list */
    devos_focus_clear(&s_ctx.focus);
    refresh_problems();
    update_footer();
    say("Jobs: pick one on the left. Sym+B builds, Sym+M edits text, Sym+O shows the overview.");
    lv_timer_create(tick_cb, 250, NULL);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_HIDDEN);
}

static void jobs_show(void)
{
    if (!s_ctx.screen) return;
    lv_obj_remove_flag(s_ctx.screen, LV_OBJ_FLAG_HIDDEN);
    /* An intent may ask for a new job (the command palette does). */
    char action[24] = "", arg[64] = "";
    if (devos_core_take_intent("jobs", action, sizeof(action), arg, sizeof(arg)) &&
        strcmp(action, "new") == 0) {
        act_new();
    }
    list_refresh();
    load_selected();
    if (s_ctx.tab == TAB_BUILDER) set_mode(MODE_BUILDER);
    else if (s_ctx.tab == TAB_OVERVIEW) refresh_overview();
    else if (s_ctx.tab == TAB_RUNS) refresh_runs();
    refresh_problems();
    s_in_list = true;                               /* keys start in the job list */
    devos_focus_clear(&s_ctx.focus);
}

static void jobs_hide(void)
{
    draft_save();                                   /* unsaved edits survive the visit */
    if (s_dry_active) { devos_jobs_dry_cancel(); s_dry_active = false; }
    if (s_dry_open) dry_close();
    if (s_ctx.screen) lv_obj_add_flag(s_ctx.screen, LV_OBJ_FLAG_HIDDEN);
    jobs_build_free(&s_ctx.build);
    s_ctx.build.ast = NULL;
}

static bool jobs_key(uint32_t key, uint8_t mods)
{
    /* The delete confirmation owns the keyboard: Enter confirms, Esc cancels. */
    if (s_del_open) {
        if (key == '\r' || key == '\n') { delete_confirmed(); return true; }
        if (key == LV_KEY_ESC) { close_delete_dialog(); return true; }
        return true;
    }

    /* The Job settings dialog owns the keyboard: Enter saves, Esc cancels. */
    if (s_job_open) {
        if (key == '\r' || key == '\n') { commit_job_settings(); return true; }
        if (key == LV_KEY_ESC) { close_job_settings(); return true; }
        if (devos_focus_key(&s_ctx.job_focus, key, mods)) return true;
        return true;
    }

    /* The dry-run result owns the keyboard: Enter/Esc closes it. */
    if (s_dry_open) {
        if (key == '\r' || key == '\n' || key == LV_KEY_ESC) { dry_close(); return true; }
        return true;
    }

    /* The Revisions dialog owns the keyboard: arrows move, Enter rolls back. */
    if (s_rev_open) {
        lv_obj_t *rf = devos_focus_get(&s_rev_focus);
        bool on_dd = (rf == s_rev_dd || rf == s_rev_mode) && !lv_dropdown_is_open(s_rev_dd) &&
                     !lv_dropdown_is_open(s_rev_mode);
        if (on_dd && (key == LV_KEY_UP || key == LV_KEY_DOWN || key == LV_KEY_LEFT || key == LV_KEY_RIGHT)) {
            lv_obj_t *dd = rf;
            int n = (int)lv_dropdown_get_option_count(dd);
            int sel = (int)lv_dropdown_get_selected(dd);
            if (key == LV_KEY_DOWN || key == LV_KEY_RIGHT) sel++;
            else sel--;
            if (sel < 0) sel = 0;
            if (sel >= n) sel = n - 1;
            if (sel != (int)lv_dropdown_get_selected(dd)) {
                lv_dropdown_set_selected(dd, (uint32_t)sel);
                lv_obj_send_event(dd, LV_EVENT_VALUE_CHANGED, NULL);
            }
            return true;
        }
        bool lists_open = lv_dropdown_is_open(s_rev_dd) || lv_dropdown_is_open(s_rev_mode);
        if (!lists_open && (key == '\r' || key == '\n')) { rev_rollback(); return true; }
        if (devos_focus_key(&s_rev_focus, key, mods)) return true;
        if (key == LV_KEY_ESC) { rev_close(); return true; }
        return true;
    }

    /* The New-job picker owns the keyboard. Arrows pick the starter directly;
     * Enter creates (unless the option list is open - then it selects);
     * Tab reaches the buttons, Esc cancels. */
    if (s_new_open) {
        bool dd_open = lv_dropdown_is_open(s_new_dd);
        lv_obj_t *nf = devos_focus_get(&s_new_focus);
        if (!dd_open && nf == s_new_dd &&
            (key == LV_KEY_UP || key == LV_KEY_DOWN || key == LV_KEY_LEFT || key == LV_KEY_RIGHT)) {
            int n = (int)lv_dropdown_get_option_count(s_new_dd);
            int sel = (int)lv_dropdown_get_selected(s_new_dd);
            if (key == LV_KEY_DOWN || key == LV_KEY_RIGHT) sel++;
            else sel--;
            if (sel < 0) sel = 0;
            if (sel >= n) sel = n - 1;
            lv_dropdown_set_selected(s_new_dd, (uint32_t)sel);
            return true;
        }
        if (!dd_open && (key == '\r' || key == '\n')) { new_confirmed(); return true; }
        if (devos_focus_key(&s_new_focus, key, mods)) return true;
        if (key == LV_KEY_ESC) { close_new_dialog(); return true; }
        return true;
    }

    /* Step-tree keys while the Builder tab is shown. */
    lv_obj_t *cur = devos_focus_get(&s_ctx.focus);
    if (s_ctx.tab == TAB_BUILDER && cur == s_ctx.step_list &&
        (key == LV_KEY_DEL || key == LV_KEY_BACKSPACE)) {
        builder_delete_step();                      /* step delete, not job delete */
        return true;
    }
    if (s_ctx.tab == TAB_BUILDER && cur == s_ctx.step_list &&
        (key == LV_KEY_UP || key == LV_KEY_DOWN)) {
        int dir = key == LV_KEY_DOWN ? 1 : -1;
        int ni = s_ctx.bstep + dir;
        if (ni >= 0 && ni < s_ctx.rows_n) {
            s_ctx.bstep = ni;
            s_ctx.bparam = 0;
            refresh_steps();
            builder_inspector();
        }
        return true;
    }
    /* [ / ] in the step list outdents/indents (P2: edit bodies). Tab keeps
     * moving focus (invariant 9) - it must never be trapped here. */
    if (s_ctx.tab == TAB_BUILDER && cur == s_ctx.step_list && (key == '[' || key == ']')) {
        builder_reblock(key == ']' ? 1 : -1);
        return true;
    }

    /* Sym+<key> commands work anywhere, including inside a text field. */
    if (mods & DEVOS_MOD_FN) {
        uint32_t k = (key >= 'A' && key <= 'Z') ? key + 32 : key;
        switch (k) {
        case 'b': set_tab(TAB_BUILDER); return true;
        case 'm': set_tab(TAB_TEXT); return true;
        case 'o': set_tab(TAB_OVERVIEW); return true;
        case 'y': set_tab(TAB_RUNS); return true;
        case 'e': open_job_settings(); return true;        /* Sym+P is system sleep */
        case 'w': act_dryrun(); return true;
        case 'z': open_revisions(); return true;
        case 'l': toggle_list(); return true;
        case 'c': act_validate(); return true;
        case 'a': builder_commit_all(); act_apply(); return true;
        case 'g': act_enable(); return true;
        case 'r': act_run(); return true;
        case 'x': act_cancel(); return true;
        case 'n': act_new(); return true;
        case 'd': act_delete(); return true;        /* always the job (P0-2) */
        case 'u': builder_add_step(); return true;
        case 'k': builder_move(-1); return true;
        case 'j': builder_move(1); return true;
        default: return false;
        }
    }

    /* The job list has the keyboard: Up/Down pick, Enter opens, Space enables,
     * letters act. Plain letters only here, never on form controls (P1-4). */
    if (s_in_list) {
        switch (key) {
        case LV_KEY_UP: list_move(-1); return true;
        case LV_KEY_DOWN: list_move(1); return true;
        case '\r': case '\n':
            set_tab(TAB_OVERVIEW);
            s_in_list = false;
            devos_focus_first(&s_ctx.focus);
            return true;
        case '\t': case LV_KEY_RIGHT:
            s_in_list = false;
            devos_focus_first(&s_ctx.focus);
            return true;
        case ' ': list_toggle_enabled(); return true;
        case 'b': case 'B': set_tab(TAB_BUILDER); return true;
        case 'm': case 'M': set_tab(TAB_TEXT); return true;
        case 'o': case 'O': set_tab(TAB_OVERVIEW); return true;
        case 'y': case 'Y': set_tab(TAB_RUNS); return true;
        case 'r': case 'R': act_run(); return true;
        case 'n': case 'N': act_new(); return true;
        case 'd': case 'D': act_delete(); return true;
        case 'e': case 'E': open_job_settings(); return true;
        case 'z': case 'Z': open_revisions(); return true;
        case LV_KEY_ESC: return false;              /* Home */
        default: return false;
        }
    }

    if (devos_focus_key(&s_ctx.focus, key, mods)) return true;

    cur = devos_focus_get(&s_ctx.focus);
    bool in_field = cur && lv_obj_check_type(cur, &lv_textarea_class);
    if (in_field && cur == s_ctx.ta_val && (key == '\r' || key == '\n')) { builder_commit_value(); return true; }
    if (in_field && (cur == s_ctx.ta_trig || cur == s_ctx.ta_where) &&
        (key == '\r' || key == '\n')) { builder_commit_trigger(); return true; }

    if (key == LV_KEY_ESC) {
        if (in_field) { devos_focus_clear(&s_ctx.focus); return true; }
        s_in_list = true;                           /* back to the job list */
        devos_focus_clear(&s_ctx.focus);
        return true;
    }
    return false;
}

static int jobs_telemetry(char lines[3][64])
{
    int total = devos_jobs_count(), on = 0, running = 0, failed = 0;
    devos_job_summary_t sum, next;
    memset(&next, 0, sizeof(next));
    int64_t soonest = 0;
    for (int i = 0; i < total; i++)
        if (devos_jobs_summary_at(i, &sum)) {
            if (sum.state == DEVOS_JOB_ENABLED) on++;
            if (sum.running) running++;
            if (sum.last_run_wall_s > 0 && !sum.last_ok) failed++;
            if (sum.next_run_in_ms > 0 && (soonest == 0 || sum.next_run_in_ms < soonest)) {
                soonest = sum.next_run_in_ms;
                next = sum;
            }
        }
    snprintf(lines[0], sizeof(lines[0]), "* %d on%s", on,
             running ? "  * running" : failed ? "  ! failed" : "");
    if (soonest > 0) {
        char d[16];
        fmt_dur(soonest, d, sizeof(d));           /* local: minutes/seconds */
        snprintf(lines[1], sizeof(lines[1]), "* next: %.16s in %s", next.name, d);
    } else {
        snprintf(lines[1], sizeof(lines[1]), "* %d job%s, no next run", total, total == 1 ? "" : "s");
    }
    const char *state = devos_jobs_safe_paused() ? "paused (recovery)" :
                        devos_jobs_paused() ? "paused automatic" : "automatic on";
    snprintf(lines[2], sizeof(lines[2]), "* %s%s", state, failed ? "" : "");
    return 3;
}

static const char *jobs_shortcuts(void)
{
    return "Jobs\n"
           "Job list\n"
           "Up / Down\tPick a job\n"
           "Enter / Tab\tOpen it (focus the view)\n"
           "Space\tEnable / disable\n"
           "B / M / O / Y\tBuilder / Text / Overview / Runs\n"
           "R / N / D\tRun now / New / Delete (asks first)\n"
           "E\tJob settings (name, timeout, overlap, cooldown)\n"
           "Sym+L\tShow / hide the job list\n"
           "Anywhere\n"
           "Sym+C / Sym+A\tValidate / Apply (unsaved changes)\n"
           "Sym+G / Sym+R / Sym+X\tEnable / Run now / Cancel\n"
           "Sym+E\tJob settings (name, timeout, overlap, cooldown)\n"
           "Sym+W\tDry-run the draft (no apply, with trace)\n"
           "Sym+Z\tRevisions (view, diff, roll back)\n"
           "Sym+O / Sym+Y\tOverview / Runs\n"
           "Sym+B / Sym+M\tBuilder / Text\n"
           "Builder\n"
           "Up / Down\tPick a step\n"
           "Sym+U / Del\tAdd a step / delete the selected step\n"
           "Sym+K / Sym+J\tMove the step up / down (or drag it)\n"
           "[ / ]\tOutdent / indent (into if/repeat bodies)\n"
           "Tab / Shift+Tab\tMove between regions (never trapped)\n"
           "Trigger\tManual / Every / Daily / Weekdays / Event (topic from a list)\n"
           "Event\tpick the topic; then a where filter over event.*\n";
}

devos_app_descriptor_t *app_jobs_get_descriptor(void)
{
    s_desc.id = 0;
    s_desc.uid = "jobs";
    s_desc.name = "Jobs";
    s_desc.title = "Jobs";
    s_desc.subtitle = "Automation: triggers, actions, schedules";
    s_desc.icon = LV_SYMBOL_PLAY;
    s_desc.draw_icon = devos_icon_jobs;
    s_desc.category = "tools";
    s_desc.init = jobs_init;
    s_desc.show = jobs_show;
    s_desc.hide = jobs_hide;
    s_desc.handle_key = jobs_key;
    s_desc.get_telemetry_lines = jobs_telemetry;
    s_desc.get_shortcuts = jobs_shortcuts;
    return &s_desc;
}

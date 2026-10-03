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
#include "devos_config.h"
#include "devos_widgets.h"
#include "devos_codeview.h"
#include "devos_focus.h"
#include "devos_icons.h"
#include "devos_toast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_MAX 4096
#define SRCMAX  (JOBS_MAX_SOURCE + 64)
#define STEP_ROWS 32
#define STEP_H 26

typedef enum { MODE_TEXT = 0, MODE_BUILDER } ui_mode_t;
typedef enum { INSP_NONE = 0, INSP_ACTION, INSP_IF, INSP_SET, INSP_WAIT, INSP_CUSTOM } insp_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *dd_job;
    lv_obj_t *lbl_name, *lbl_state, *lbl_hint, *lbl_mode;
    lv_obj_t *ta_src;
    lv_obj_t *btn_new, *btn_validate, *btn_apply, *btn_enable, *btn_run, *btn_cancel, *btn_hist, *btn_del;
    /* Builder */
    lv_obj_t *bld;
    lv_obj_t *dd_kind, *ta_trig;
    lv_obj_t *step_list, *step_row[STEP_ROWS], *step_lbl[STEP_ROWS];
    lv_obj_t *lbl_settings;
    lv_obj_t *dd_add, *btn_add, *btn_bdel, *btn_up, *btn_dn;
    lv_obj_t *dd_param, *dd_expr, *ta_val, *dd_choice, *lbl_custom;
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
    char ids[DEVOS_JOBS_MAX][DEVOS_JOBS_ID_MAX];
    int n, sel;
    uint32_t base_rev;
    ui_mode_t mode;
    char out[OUT_MAX];
} jobs_ctx_t;

static devos_app_descriptor_t s_desc;
static jobs_ctx_t s_ctx;

static void set_mode(ui_mode_t m);

static const char *NEW_TEMPLATE =
    "version 1;\n"
    "job \"New job\" {\n"
    "    trigger manual;\n"
    "    system.log(message: \"hello from Jobs\");\n"
    "}\n";

static void say(const char *t) { devos_codeview_set(&s_ctx.cv, t ? t : ""); }
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

/* ---- list ---- */
static void refresh_list(void)
{
    char opts[DEVOS_JOBS_MAX * (DEVOS_JOBS_NAME_MAX + 2) + 8];
    size_t o = 0;
    opts[0] = '\0';
    s_ctx.n = 0;
    devos_job_summary_t sum;
    for (int i = 0; i < devos_jobs_count() && s_ctx.n < DEVOS_JOBS_MAX; i++) {
        if (!devos_jobs_summary_at(i, &sum)) continue;
        snprintf(s_ctx.ids[s_ctx.n], sizeof(s_ctx.ids[0]), "%s", sum.id);
        o += (size_t)snprintf(opts + o, sizeof(opts) - o, "%s%s%s",
                              s_ctx.n ? "\n" : "", sum.running ? "* " : "", sum.name);
        s_ctx.n++;
    }
    if (s_ctx.n == 0) snprintf(opts, sizeof(opts), "(no jobs)");
    if (s_ctx.sel >= s_ctx.n) s_ctx.sel = s_ctx.n > 0 ? s_ctx.n - 1 : 0;
    lv_dropdown_set_options(s_ctx.dd_job, opts);
    lv_dropdown_set_selected(s_ctx.dd_job, (uint32_t)s_ctx.sel);
}

static const char *cur_source(void) { return lv_textarea_get_text(s_ctx.ta_src); }

static void load_selected(void)
{
    if (s_ctx.n == 0) {
        lv_textarea_set_text(s_ctx.ta_src, "");
        devos_w_set_text(s_ctx.lbl_name, "");
        devos_w_set_text(s_ctx.lbl_state, "no jobs - press Sym+N for a new one");
        s_ctx.base_rev = 0;
        return;
    }
    const char *id = s_ctx.ids[s_ctx.sel];
    devos_job_summary_t sum;
    for (int i = 0; i < devos_jobs_count(); i++)
        if (devos_jobs_summary_at(i, &sum) && strcmp(sum.id, id) == 0) break;
    static char src[SRCMAX];
    size_t len = 0;
    if (devos_jobs_source(id, src, sizeof(src), &len) == DEVOS_OK) lv_textarea_set_text(s_ctx.ta_src, src);
    else lv_textarea_set_text(s_ctx.ta_src, NEW_TEMPLATE);
    s_ctx.base_rev = sum.revision;
    devos_w_set_text(s_ctx.lbl_name, sum.name);
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
    case JN_WAIT: { char d[24]; fmt_dur(n->u.i, d, sizeof(d)); snprintf(out, cap, "wait %s", d); break; }
    case JN_SET: snprintf(out, cap, "set %s = %s", n->u.str.s, jobs_build_expr_text(n->a)); break;
    default: snprintf(out, cap, "step");
    }
}

static void builder_sync(void)
{
    char src[SRCMAX];
    jobs_build_source(&s_ctx.build, src, sizeof(src));
    lv_textarea_set_text(s_ctx.ta_src, src);
    s_ctx.build_ok = jobs_build_revalidate(&s_ctx.build);
}

static void builder_inspector(void);

/* (Re)draw the step rows and rebuild the row cache. */
static void refresh_steps(void)
{
    const jobs_node_t *sel_node = (s_ctx.bstep >= 0 && s_ctx.bstep < s_ctx.rows_n) ? s_ctx.rows[s_ctx.bstep].node : NULL;
    s_ctx.rows_n = jobs_build_rows(&s_ctx.build, s_ctx.rows, JOBS_BUILD_ROWS);
    for (int i = 0; i < STEP_ROWS; i++) {
        if (i < s_ctx.rows_n) {
            char s[96], txt[120];
            step_summary(s_ctx.rows[i].node, s, sizeof(s));
            snprintf(txt, sizeof(txt), "%*s%s", s_ctx.rows[i].depth * 2, "", s);
            devos_w_set_text(s_ctx.step_lbl[i], txt);
            lv_obj_set_y(s_ctx.step_row[i], i * STEP_H);
            devos_w_track(s_ctx.step_row[i], i == s_ctx.bstep ? DEVOS_W_BTN_PRIMARY : DEVOS_W_PANEL_ALT);
            devos_w_track(s_ctx.step_lbl[i], i == s_ctx.bstep ? DEVOS_W_TEXT_ON_ACCENT : DEVOS_W_TEXT);
            set_visible(s_ctx.step_row[i], true);
        } else {
            set_visible(s_ctx.step_row[i], false);
        }
    }
    /* size the list to its content so the controls below don't leave a gap */
    int lh = s_ctx.rows_n * STEP_H + 6;
    if (lh < STEP_H + 6) lh = STEP_H + 6;
    if (lh > 130) lh = 130;
    lv_obj_set_height(s_ctx.step_list, lh);
    int add_y = 72 + lh + 8;
    lv_obj_set_y(s_ctx.dd_add, add_y);
    lv_obj_set_y(s_ctx.btn_add, add_y);
    lv_obj_set_y(s_ctx.btn_bdel, add_y);
    lv_obj_set_y(s_ctx.btn_up, add_y);
    lv_obj_set_y(s_ctx.btn_dn, add_y);
    int set_y = add_y + 34;
    lv_obj_set_y(s_ctx.lbl_settings, set_y);
    lv_obj_set_y(s_ctx.dd_param, set_y + 18);
    lv_obj_set_y(s_ctx.dd_expr, set_y + 18);
    lv_obj_set_y(s_ctx.dd_choice, set_y + 18);
    lv_obj_set_y(s_ctx.ta_val, set_y + 58);
    lv_obj_set_y(s_ctx.lbl_custom, set_y + 98);

    /* keep the same node selected across a reorder */
    if (sel_node) for (int i = 0; i < s_ctx.rows_n; i++) if (s_ctx.rows[i].node == sel_node) s_ctx.bstep = i;
    if (s_ctx.bstep >= s_ctx.rows_n) s_ctx.bstep = s_ctx.rows_n ? s_ctx.rows_n - 1 : 0;
}

static void builder_refresh(void)
{
    char aopts[1024];
    size_t ao = 0;
    aopts[0] = '\0';
    for (int i = 0; i < devos_actions_count(); i++) {
        const devos_action_descriptor_t *d = devos_actions_at(i);
        if (!d) continue;
        ao += (size_t)snprintf(aopts + ao, sizeof(aopts) - ao, "%s%s", i ? "\n" : "", d->id);
    }
    if (ao) lv_dropdown_set_options(s_ctx.dd_add, aopts);

    jobs_node_t *t = jobs_build_trigger(&s_ctx.build);
    if (t) {
        int kind = t->sub;
        lv_dropdown_set_selected(s_ctx.dd_kind, (uint32_t)(kind == JTRIG_EVERY ? 1 : kind == JTRIG_DAILY ? 2 : kind == JTRIG_WEEKDAYS ? 3 : 0));
        char v[32] = "";
        if (kind == JTRIG_EVERY) fmt_dur(t->u.i, v, sizeof(v));
        else if (kind == JTRIG_DAILY || kind == JTRIG_WEEKDAYS) snprintf(v, sizeof(v), "%s", t->u.str.s ? t->u.str.s : "08:00");
        lv_textarea_set_text(s_ctx.ta_trig, v);
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

    if (!node) { devos_w_set_text(s_ctx.lbl_custom, "No step selected."); set_visible(s_ctx.lbl_custom, true); return; }

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
    if (!d || d->param_count == 0) {
        s_ctx.insp = INSP_CUSTOM;
        devos_w_set_text(s_ctx.lbl_custom, "Advanced step - edit it in Text (Sym+M).");
        set_visible(s_ctx.lbl_custom, true);
        return;
    }

    /* action parameters */
    s_ctx.insp = INSP_ACTION;
    char popts[512];
    size_t o = 0;
    for (int i = 0; i < d->param_count; i++)
        o += (size_t)snprintf(popts + o, sizeof(popts) - o, "%s%s", i ? "\n" : "", d->params[i].name);
    if (s_ctx.bparam >= d->param_count) s_ctx.bparam = 0;
    lv_dropdown_set_options(s_ctx.dd_param, popts);
    lv_dropdown_set_selected(s_ctx.dd_param, (uint32_t)s_ctx.bparam);
    set_visible(s_ctx.dd_param, true);

    const devos_action_param_t *p = &d->params[s_ctx.bparam];
    bool choice = p->choices != NULL || p->type == DEVOS_VAL_BOOL;
    s_ctx.insp_expr = p->expression && jobs_build_arg_is_expr(node, p->name);

    set_visible(s_ctx.lbl_custom, true);
    if (p->credential) {
        devos_w_set_text(s_ctx.lbl_custom, "Credential: a secret name (e.g. health-token)");
        set_visible(s_ctx.ta_val, true);
        const char *n = jobs_build_arg_secret_name(node, p->name);
        lv_textarea_set_text(s_ctx.ta_val, n ? n : "");
        return;
    }
    if (choice && !s_ctx.insp_expr) {
        devos_w_set_text(s_ctx.lbl_custom, p->choices ? "Choose a value" : "Toggle");
        set_visible(s_ctx.dd_choice, true);
        if (p->choices) {
            static char c[128];
            snprintf(c, sizeof(c), "%s", p->choices);
            for (char *q = c; *q; q++) if (*q == '|') *q = '\n';
            lv_dropdown_set_options(s_ctx.dd_choice, c);
            const char *cur = jobs_build_arg_text(node, p->name);
            int sel = 0, i = 0;
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "%s", p->choices);
            for (char *tok = strtok(tmp, "|"); tok; tok = strtok(NULL, "|"), i++)
                if (cur && strcmp(tok, cur) == 0) sel = i;
            lv_dropdown_set_selected(s_ctx.dd_choice, (uint32_t)sel);
        } else {
            lv_dropdown_set_options(s_ctx.dd_choice, "false\ntrue");
            bool b = false;
            jobs_build_arg_bool(node, p->name, &b);
            lv_dropdown_set_selected(s_ctx.dd_choice, b ? 1 : 0);
        }
        return;
    }
    /* free value or expression */
    if (p->expression) {
        devos_w_set_text(s_ctx.lbl_custom, "Value - Literal or full expression");
        set_visible(s_ctx.dd_expr, true);
        lv_dropdown_set_options(s_ctx.dd_expr, "Literal\nExpression");
        lv_dropdown_set_selected(s_ctx.dd_expr, s_ctx.insp_expr ? 1 : 0);
    } else {
        devos_w_set_text(s_ctx.lbl_custom, "Value");
    }
    set_visible(s_ctx.ta_val, true);
    if (s_ctx.insp_expr) {
        lv_textarea_set_text(s_ctx.ta_val, jobs_build_expr_text(jobs_build_arg_expr(node, p->name)));
    } else if (p->type == DEVOS_VAL_DURATION) {
        int64_t ms = jobs_build_arg_duration(node, p->name);
        char dv[24];
        if (ms >= 0) { fmt_dur(ms, dv, sizeof(dv)); lv_textarea_set_text(s_ctx.ta_val, dv); }
        else lv_textarea_set_text(s_ctx.ta_val, "");
    } else {
        const char *cur = jobs_build_arg_text(node, p->name);
        lv_textarea_set_text(s_ctx.ta_val, cur ? cur : "");
    }
}

static void builder_commit_trigger(void)
{
    int kind = (int)lv_dropdown_get_selected(s_ctx.dd_kind);
    jobs_build_set_trigger_kind(&s_ctx.build, kind == 1 ? JTRIG_EVERY : kind == 2 ? JTRIG_DAILY :
                                                 kind == 3 ? JTRIG_WEEKDAYS : JTRIG_MANUAL);
    const char *v = lv_textarea_get_text(s_ctx.ta_trig);
    if (kind == 1) { int64_t ms = parse_dur(v); if (ms > 0) jobs_build_set_trigger_duration(&s_ctx.build, ms); }
    else if (kind == 2 || kind == 3) jobs_build_set_trigger_time(&s_ctx.build, v);
    builder_sync();
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
    } else if (s_ctx.insp == INSP_ACTION) {
        const devos_action_descriptor_t *d = cur_action();
        if (!d || s_ctx.bparam >= d->param_count) return;
        const devos_action_param_t *p = &d->params[s_ctx.bparam];
        if (p->credential) {
            jobs_build_set_arg_secret(&s_ctx.build, node, p->name, val);
        } else if (s_ctx.insp_expr) {
            if (!jobs_build_set_arg_expr(&s_ctx.build, node, p->name, val)) { say(s_ctx.build.diag); return; }
        } else if (p->choices) {
            char opts[128];
            snprintf(opts, sizeof(opts), "%s", p->choices);
            int sel = (int)lv_dropdown_get_selected(s_ctx.dd_choice), i = 0;
            for (char *tok = strtok(opts, "|"); tok; tok = strtok(NULL, "|"), i++)
                if (i == sel) { jobs_build_set_arg_str(&s_ctx.build, node, p->name, tok); break; }
        } else if (p->type == DEVOS_VAL_BOOL) {
            jobs_build_set_arg_bool(&s_ctx.build, node, p->name, lv_dropdown_get_selected(s_ctx.dd_choice) == 1);
        } else if (p->type == DEVOS_VAL_DURATION) {
            int64_t ms = parse_dur(val);
            if (ms >= 0) jobs_build_set_arg_duration(&s_ctx.build, node, p->name, ms);
        } else if (p->type == DEVOS_VAL_INT) {
            jobs_build_set_arg_int(&s_ctx.build, node, p->name, strtoll(val, NULL, 10));
        } else {
            jobs_build_set_arg_str(&s_ctx.build, node, p->name, val);
        }
    } else {
        return;
    }
    builder_sync();
}

static void builder_commit_all(void)
{
    if (s_ctx.mode != MODE_BUILDER) return;
    builder_commit_trigger();
    builder_commit_value();
}

static void builder_add_step(void)
{
    if (s_ctx.mode != MODE_BUILDER || !s_ctx.build.ast) return;
    const jobs_node_t *block = (s_ctx.bstep >= 0 && s_ctx.bstep < s_ctx.rows_n)
                                   ? s_ctx.rows[s_ctx.bstep].block : s_ctx.build.ast->root->c;
    const devos_action_descriptor_t *d = devos_actions_at((int)lv_dropdown_get_selected(s_ctx.dd_add));
    if (!d) return;
    if (jobs_build_add_action(&s_ctx.build, block, d->id)) {
        builder_sync();
        s_ctx.bstep = s_ctx.rows_n;
        builder_refresh();
    }
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

static void act_history(void)
{
    if (s_ctx.n == 0) { say("No job selected."); return; }
    int n = devos_jobs_history(s_ctx.ids[s_ctx.sel], s_ctx.out, OUT_MAX);
    if (n <= 0) say("No history yet."); else say(s_ctx.out);
}

static void act_new(void)
{
    char id[DEVOS_JOBS_ID_MAX];
    for (int k = 1; k < 1000; k++) {
        snprintf(id, sizeof(id), "job%d", k);
        bool taken = false;
        for (int i = 0; i < s_ctx.n; i++) if (strcmp(s_ctx.ids[i], id) == 0) taken = true;
        if (!taken) break;
    }
    if (devos_jobs_apply(id, NEW_TEMPLATE, strlen(NEW_TEMPLATE), NULL) != DEVOS_OK) { say("Couldn't create the job."); return; }
    s_ctx.sel = s_ctx.n;
    refresh_list();
    load_selected();
    if (s_ctx.mode == MODE_BUILDER) set_mode(MODE_BUILDER);
    devos_toast_show("New job (disabled)", DEVOS_TOAST_OK, 0);
}

static void act_delete(void)
{
    if (s_ctx.n == 0) return;
    if (devos_jobs_delete(s_ctx.ids[s_ctx.sel]) != DEVOS_OK) { say("Can't delete a running job - cancel it first."); return; }
    if (s_ctx.sel > 0) s_ctx.sel--;
    refresh_list();
    load_selected();
    if (s_ctx.mode == MODE_BUILDER) set_mode(MODE_BUILDER);
    devos_toast_show("Deleted", DEVOS_TOAST_WARN, 0);
}

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
static void job_cb(lv_event_t *e) { LV_UNUSED(e); s_ctx.sel = (int)lv_dropdown_get_selected(s_ctx.dd_job); load_selected(); if (s_ctx.mode == MODE_BUILDER) set_mode(MODE_BUILDER); }
static void validate_cb(lv_event_t *e) { LV_UNUSED(e); act_validate(); }
static void apply_cb(lv_event_t *e) { LV_UNUSED(e); builder_commit_all(); act_apply(); }
static void enable_cb(lv_event_t *e) { LV_UNUSED(e); act_enable(); }
static void run_cb(lv_event_t *e) { LV_UNUSED(e); act_run(); }
static void cancel_cb(lv_event_t *e) { LV_UNUSED(e); act_cancel(); }
static void hist_cb(lv_event_t *e) { LV_UNUSED(e); act_history(); }
static void new_cb(lv_event_t *e) { LV_UNUSED(e); act_new(); }
static void del_cb(lv_event_t *e) { LV_UNUSED(e); act_delete(); }
static void kind_cb(lv_event_t *e) { LV_UNUSED(e); builder_commit_trigger(); }
static void param_cb(lv_event_t *e) { LV_UNUSED(e); s_ctx.bparam = (int)lv_dropdown_get_selected(s_ctx.dd_param); builder_inspector(); }
static void expr_cb(lv_event_t *e) { LV_UNUSED(e); s_ctx.insp_expr = lv_dropdown_get_selected(s_ctx.dd_expr) == 1; builder_inspector(); }
static void val_cb(lv_event_t *e) { LV_UNUSED(e); builder_commit_value(); }
static void add_cb(lv_event_t *e) { LV_UNUSED(e); builder_add_step(); }
static void bdel_cb(lv_event_t *e) { LV_UNUSED(e); builder_delete_step(); }
static void up_cb(lv_event_t *e) { LV_UNUSED(e); builder_move(-1); }
static void dn_cb(lv_event_t *e) { LV_UNUSED(e); builder_move(1); }

static void tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!s_ctx.screen || lv_obj_has_flag(s_ctx.screen, LV_OBJ_FLAG_HIDDEN)) return;
    devos_jobs_run_t run;
    if (devos_jobs_run(&run)) devos_w_set_text(s_ctx.lbl_hint, run.cancelling ? "Cancelling..." : "Running");
    else if (devos_jobs_safe_paused()) devos_w_set_text(s_ctx.lbl_hint, "Automatic jobs paused after a recovery boot - Run now still works");
    else if (devos_jobs_paused()) devos_w_set_text(s_ctx.lbl_hint, "Automatic jobs paused");
    else devos_w_set_text(s_ctx.lbl_hint, "Ready");
}

/* ---- lifecycle ---- */
static lv_obj_t *mk_label(lv_obj_t *parent, const char *text, devos_w_kind_t kind, int x, int y)
{
    lv_obj_t *l = devos_w_label(parent, NULL, kind, text);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void jobs_init(void)
{
    lv_obj_t *scr = s_ctx.screen = devos_w_screen(&s_desc);
    devos_w_bar(scr, "Jobs", NULL);

    s_ctx.dd_job = devos_w_dd(scr, "(no jobs)", 340);
    lv_obj_set_pos(s_ctx.dd_job, 20, 54);
    lv_obj_add_event_cb(s_ctx.dd_job, job_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_ctx.btn_new = devos_w_btn(scr, "New  [Sym+N]", 130, new_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_new, 380, 54);
    s_ctx.btn_del = devos_w_btn(scr, "Delete  [Sym+D]", 140, del_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_del, 520, 54);
    s_ctx.lbl_mode = devos_w_label(scr, NULL, DEVOS_W_TEXT_ACCENT, "Builder");
    lv_obj_set_pos(s_ctx.lbl_mode, 680, 60);

    s_ctx.lbl_name = devos_w_label(scr, NULL, DEVOS_W_TEXT_ACCENT, "");
    lv_obj_set_pos(s_ctx.lbl_name, 20, 92);
    s_ctx.lbl_state = devos_w_label(scr, NULL, DEVOS_W_TEXT_DIM, "");
    lv_obj_set_pos(s_ctx.lbl_state, 20, 112);

    /* Text view */
    s_ctx.ta_src = devos_w_ta(scr, false, 760, 356);
    lv_obj_set_pos(s_ctx.ta_src, 20, 138);
    lv_textarea_set_max_length(s_ctx.ta_src, SRCMAX);
    lv_obj_add_flag(s_ctx.ta_src, LV_OBJ_FLAG_HIDDEN);

    /* Builder view */
    s_ctx.bld = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ctx.bld);
    lv_obj_set_pos(s_ctx.bld, 20, 138);
    lv_obj_set_size(s_ctx.bld, 760, 366);
    lv_obj_remove_flag(s_ctx.bld, LV_OBJ_FLAG_SCROLLABLE);

    mk_label(s_ctx.bld, "Trigger", DEVOS_W_TEXT_DIM, 0, 0);
    s_ctx.dd_kind = devos_w_dd(s_ctx.bld, "Manual\nEvery\nDaily\nWeekdays", 170);
    lv_obj_set_pos(s_ctx.dd_kind, 0, 18);
    lv_obj_add_event_cb(s_ctx.dd_kind, kind_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_ctx.ta_trig = devos_w_ta(s_ctx.bld, true, 220, 36);
    lv_obj_set_pos(s_ctx.ta_trig, 180, 18);
    lv_textarea_set_max_length(s_ctx.ta_trig, 24);
    lv_obj_add_event_cb(s_ctx.ta_trig, val_cb, LV_EVENT_VALUE_CHANGED, NULL);

    mk_label(s_ctx.bld, "Steps  (tap to select, drag to reorder)", DEVOS_W_TEXT_DIM, 0, 56);
    s_ctx.step_list = lv_obj_create(s_ctx.bld);
    lv_obj_remove_style_all(s_ctx.step_list);
    lv_obj_set_pos(s_ctx.step_list, 0, 72);
    lv_obj_set_size(s_ctx.step_list, 740, 130);
    lv_obj_set_style_bg_opa(s_ctx.step_list, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_ctx.step_list, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < STEP_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(s_ctx.step_list);
        lv_obj_set_size(row, 736, STEP_H - 2);
        lv_obj_set_pos(row, 0, i * STEP_H);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_pad_all(row, 2, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, step_row_cb, LV_EVENT_ALL, (void *)(intptr_t)i);
        s_ctx.step_row[i] = row;
        s_ctx.step_lbl[i] = devos_w_label(row, NULL, DEVOS_W_TEXT, "");
        lv_obj_set_pos(s_ctx.step_lbl[i], 4, 2);
        lv_obj_set_width(s_ctx.step_lbl[i], 720);
        devos_w_track(row, DEVOS_W_PANEL_ALT);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }

    s_ctx.dd_add = devos_w_dd(s_ctx.bld, "", 260);
    lv_obj_set_pos(s_ctx.dd_add, 0, 206);
    s_ctx.btn_add = devos_w_btn(s_ctx.bld, "Add  [Sym+U]", 110, add_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_add, 270, 206);
    s_ctx.btn_bdel = devos_w_btn(s_ctx.bld, "Delete  [Sym+D]", 110, bdel_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_bdel, 390, 206);
    s_ctx.btn_up = devos_w_btn(s_ctx.bld, "Up  [Sym+K]", 90, up_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_up, 510, 206);
    s_ctx.btn_dn = devos_w_btn(s_ctx.bld, "Down  [Sym+J]", 90, dn_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_dn, 610, 206);

    s_ctx.lbl_settings = mk_label(s_ctx.bld, "Step settings", DEVOS_W_TEXT_DIM, 0, 238);
    s_ctx.dd_param = devos_w_dd(s_ctx.bld, "", 180);
    lv_obj_set_pos(s_ctx.dd_param, 0, 256);
    lv_obj_add_event_cb(s_ctx.dd_param, param_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_ctx.dd_expr = devos_w_dd(s_ctx.bld, "Literal\nExpression", 140);
    lv_obj_set_pos(s_ctx.dd_expr, 190, 256);
    lv_obj_add_event_cb(s_ctx.dd_expr, expr_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_ctx.dd_choice = devos_w_dd(s_ctx.bld, "", 180);
    lv_obj_set_pos(s_ctx.dd_choice, 340, 256);
    lv_obj_add_event_cb(s_ctx.dd_choice, val_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_ctx.ta_val = devos_w_ta(s_ctx.bld, true, 740, 36);
    lv_obj_set_pos(s_ctx.ta_val, 0, 296);
    lv_textarea_set_max_length(s_ctx.ta_val, 240);
    lv_obj_add_event_cb(s_ctx.ta_val, val_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_ctx.lbl_custom = devos_w_label(s_ctx.bld, NULL, DEVOS_W_TEXT_MUTED, "");
    lv_obj_set_pos(s_ctx.lbl_custom, 0, 336);
    lv_obj_set_width(s_ctx.lbl_custom, 740);

    /* shared toolbar */
    s_ctx.btn_validate = devos_w_btn(scr, "Validate  [Sym+C]", 150, validate_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_validate, 20, 520);
    s_ctx.btn_apply = devos_w_btn_kind(scr, DEVOS_W_BTN_PRIMARY, LV_SYMBOL_OK " Apply  [Sym+A]", 150, apply_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_apply, 180, 520);
    s_ctx.btn_enable = devos_w_btn(scr, "Enable  [Sym+G]", 150, enable_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_enable, 340, 520);
    s_ctx.btn_run = devos_w_btn(scr, "Run now  [Sym+R]", 150, run_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_run, 500, 520);
    s_ctx.btn_cancel = devos_w_btn(scr, "Cancel  [Sym+X]", 150, cancel_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_cancel, 20, 558);
    s_ctx.btn_hist = devos_w_btn(scr, "History  [Sym+Y]", 150, hist_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_hist, 180, 558);

    lv_obj_t *l = devos_w_label(scr, NULL, DEVOS_W_TEXT_DIM, "Output");
    lv_obj_set_pos(l, 800, 92);
    lv_obj_t *panel = devos_w_panel(scr, 800, 112, DEVOS_SCREEN_WIDTH - 820, 544, DEVOS_W_CODE);
    s_ctx.out_scroll = lv_obj_create(panel);
    lv_obj_set_style_bg_opa(s_ctx.out_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ctx.out_scroll, 0, 0);
    lv_obj_set_style_pad_all(s_ctx.out_scroll, 6, 0);
    lv_obj_set_pos(s_ctx.out_scroll, 0, 0);
    lv_obj_set_size(s_ctx.out_scroll, DEVOS_SCREEN_WIDTH - 820, 544);
    devos_codeview_create(&s_ctx.cv, s_ctx.out_scroll);
    s_ctx.cv.plain = true;

    s_ctx.lbl_hint = devos_w_label(scr, NULL, DEVOS_W_TEXT_MUTED, "");
    lv_obj_set_pos(s_ctx.lbl_hint, 20, 596);
    lv_obj_set_width(s_ctx.lbl_hint, DEVOS_SCREEN_WIDTH - 40);

    lv_obj_t *keys = devos_w_keys(scr);
    devos_w_set_text(keys, "Sym+B builder  Sym+M text  Sym+C validate  Sym+A apply  Sym+G enable  "
                           "Sym+R run  Sym+X cancel  Sym+Y history  Sym+N new  Sym+D delete  |  "
                           "Builder: Sym+U add  Sym+K/Sym+J move  drag a step to reorder  |  Esc back");

    devos_focus_init(&s_ctx.focus);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_job);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_kind);
    devos_focus_add(&s_ctx.focus, s_ctx.ta_trig);
    devos_focus_add(&s_ctx.focus, s_ctx.step_list);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_add);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_param);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_expr);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_choice);
    devos_focus_add(&s_ctx.focus, s_ctx.ta_val);
    devos_focus_add(&s_ctx.focus, s_ctx.ta_src);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_validate);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_apply);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_enable);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_run);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_cancel);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_hist);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_new);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_del);

    refresh_list();
    load_selected();
    set_mode(MODE_BUILDER);                 /* Builder is the default */
    say("Builder: pick a step, edit its settings, Sym+U adds a step. Sym+M for Text.");
    lv_timer_create(tick_cb, 250, NULL);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_HIDDEN);
}

static void jobs_show(void)
{
    if (!s_ctx.screen) return;
    lv_obj_remove_flag(s_ctx.screen, LV_OBJ_FLAG_HIDDEN);
    refresh_list();
    load_selected();
    if (s_ctx.mode == MODE_BUILDER) set_mode(MODE_BUILDER);
    devos_focus_first(&s_ctx.focus);
}

static void jobs_hide(void)
{
    if (s_ctx.screen) lv_obj_add_flag(s_ctx.screen, LV_OBJ_FLAG_HIDDEN);
    jobs_build_free(&s_ctx.build);
    s_ctx.build.ast = NULL;
}

static bool jobs_key(uint32_t key, uint8_t mods)
{
    /* Arrow keys over the step tree move the selection, not the focus. */
    lv_obj_t *cur = devos_focus_get(&s_ctx.focus);
    if (s_ctx.mode == MODE_BUILDER && cur == s_ctx.step_list &&
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

    if (devos_focus_key(&s_ctx.focus, key, mods)) return true;

    cur = devos_focus_get(&s_ctx.focus);
    bool in_field = cur && lv_obj_check_type(cur, &lv_textarea_class);
    if (in_field && cur == s_ctx.ta_val && (key == '\r' || key == '\n')) { builder_commit_value(); return true; }
    if (in_field && cur == s_ctx.ta_trig && (key == '\r' || key == '\n')) { builder_commit_trigger(); return true; }

    /* Sym+<key> commands work anywhere, including inside a text field. */
    if (mods & DEVOS_MOD_FN) {
        uint32_t k = (key >= 'A' && key <= 'Z') ? key + 32 : key;
        switch (k) {
        case 'b': set_mode(MODE_BUILDER); return true;
        case 'm': set_mode(MODE_TEXT); if (s_ctx.mode == MODE_TEXT) devos_focus_set(&s_ctx.focus, s_ctx.ta_src); return true;
        case 'c': act_validate(); return true;
        case 'a': builder_commit_all(); act_apply(); return true;
        case 'g': act_enable(); return true;
        case 'r': act_run(); return true;
        case 'x': act_cancel(); return true;
        case 'y': act_history(); return true;
        case 'n': act_new(); return true;
        case 'd': if (s_ctx.mode == MODE_BUILDER) builder_delete_step(); else act_delete(); return true;
        case 'u': builder_add_step(); return true;
        case 'k': builder_move(-1); return true;
        case 'j': builder_move(1); return true;
        default: return false;
        }
    }

    if (key == LV_KEY_ESC) {
        if (in_field) { devos_focus_clear(&s_ctx.focus); return true; }
        return false;
    }
    if (mods & (DEVOS_MOD_CTRL | DEVOS_MOD_ALT)) return false;

    /* Plain-letter aliases outside a field (kept for convenience). */
    switch (key) {
    case 'v': case 'V': act_validate(); return true;
    case 'a': case 'A': builder_commit_all(); act_apply(); return true;
    case 'e': case 'E': act_enable(); return true;
    case 'r': case 'R': act_run(); return true;
    case 'x': case 'X': act_cancel(); return true;
    case 'h': case 'H': act_history(); return true;
    case 'n': case 'N': act_new(); return true;
    case 'd': case 'D': if (s_ctx.mode == MODE_BUILDER) builder_delete_step(); else act_delete(); return true;
    case 'i': case 'I': builder_add_step(); return true;
    case 'k': case 'K': builder_move(-1); return true;
    case 'j': case 'J': builder_move(1); return true;
    case 'b': case 'B': set_mode(MODE_BUILDER); return true;
    case 't': case 'T': set_mode(MODE_TEXT); if (s_ctx.mode == MODE_TEXT) devos_focus_set(&s_ctx.focus, s_ctx.ta_src); return true;
    default: return false;
    }
}

static int jobs_telemetry(char lines[3][64])
{
    int total = devos_jobs_count(), on = 0, running = 0;
    devos_job_summary_t sum;
    for (int i = 0; i < total; i++)
        if (devos_jobs_summary_at(i, &sum)) {
            if (sum.state == DEVOS_JOB_ENABLED) on++;
            if (sum.running) running++;
        }
    snprintf(lines[0], sizeof(lines[0]), "* %d of %d enabled", on, total);
    snprintf(lines[1], sizeof(lines[1]), "* %d running", running);
    snprintf(lines[2], sizeof(lines[2]), "* %s", devos_jobs_safe_paused() ? "paused (recovery)" :
                                                          devos_jobs_paused() ? "paused" : "ready");
    return 3;
}

static const char *jobs_shortcuts(void)
{
    return "Jobs\n"
           "Sym+B / Sym+M\tBuilder / Text\n"
           "Sym+C / Sym+A\tValidate / Apply\n"
           "Sym+G / Sym+R / Sym+X\tEnable / Run now / Cancel\n"
           "Sym+Y / Sym+N / Sym+D\tHistory / New / Delete\n"
           "Builder\n"
           "Up / Down\tPick a step (tap to select)\n"
           "Sym+U / Sym+D\tAdd / delete a step\n"
           "Sym+K / Sym+J\tMove the step up / down (or drag it)\n";
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

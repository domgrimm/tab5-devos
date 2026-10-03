/* app_jobs: the Jobs app (PLAN.md 11). Two modes over one definition:
 *   - Text: edit the source, Validate / Apply / Enable / Run now / Cancel /
 *     History.
 *   - Builder: a schema-driven editor - a trigger card, a step tree and an
 *     inspector generated from the action registry, plus a custom-node card for
 *     advanced expressions the Builder cannot render (edited in Text).
 * Both modes share one AST/source; every Builder edit re-serializes it, so the
 * Text view and the engine always agree. The engine keeps running while this
 * screen is hidden or the display is off.
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
#define BFIELDS 8

typedef enum { MODE_TEXT = 0, MODE_BUILDER } ui_mode_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *lbl_status;
    lv_obj_t *dd_job;
    lv_obj_t *lbl_name, *lbl_state, *lbl_hint;
    lv_obj_t *ta_src;
    lv_obj_t *btn_new, *btn_validate, *btn_apply, *btn_enable, *btn_run, *btn_cancel, *btn_hist, *btn_del;
    lv_obj_t *lbl_mode;
    /* Builder */
    lv_obj_t *bld;                       /* the Builder container */
    lv_obj_t *dd_kind, *ta_trig;
    lv_obj_t *dd_step, *dd_add, *btn_add, *btn_bdel, *btn_up, *btn_dn;
    lv_obj_t *dd_param, *ta_val, *dd_choice, *lbl_custom;
    jobs_build_t build;
    bool build_ok;
    int bstep, bparam;
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

static const char *NEW_TEMPLATE =
    "version 1;\n"
    "job \"New job\" {\n"
    "    trigger manual;\n"
    "    system.log(message: \"hello from Jobs\");\n"
    "}\n";

static void say(const char *text) { devos_codeview_set(&s_ctx.cv, text ? text : ""); }
static void set_visible(lv_obj_t *o, bool v)
{
    if (!o) return;
    if (v) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* ---- duration text ---- */
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
    if (*end == '\0') return v;                  /* bare number = ms */
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
        devos_w_set_text(s_ctx.lbl_state, "no jobs - press N for a new one");
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
static const jobs_node_t *bstep_node(jobs_build_row_t *rows, int n)
{
    return (s_ctx.bstep >= 0 && s_ctx.bstep < n) ? rows[s_ctx.bstep].node : NULL;
}

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
    case JN_IF: snprintf(out, cap, "if ... (custom expression)"); break;
    case JN_WAIT: { char d[24]; fmt_dur(n->u.i, d, sizeof(d)); snprintf(out, cap, "wait %s", d); break; }
    case JN_SET: snprintf(out, cap, "set %s = ...", n->u.str.s); break;
    default: snprintf(out, cap, "step");
    }
}

/* Re-serialize the draft into the Text area and refresh the Builder controls. */
static void builder_sync(void)
{
    char src[SRCMAX];
    jobs_build_source(&s_ctx.build, src, sizeof(src));
    lv_textarea_set_text(s_ctx.ta_src, src);
    s_ctx.build_ok = jobs_build_revalidate(&s_ctx.build);
}

static void builder_inspector(void);

static void builder_refresh(void)
{
    jobs_build_row_t rows[JOBS_BUILD_ROWS];
    int n = jobs_build_rows(&s_ctx.build, rows, JOBS_BUILD_ROWS);

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

    char opts[4096];
    size_t o = 0;
    opts[0] = '\0';
    for (int i = 0; i < n; i++) {
        char s[96];
        step_summary(rows[i].node, s, sizeof(s));
        o += (size_t)snprintf(opts + o, sizeof(opts) - o, "%s%*s%s", i ? "\n" : "", rows[i].depth * 2, "", s);
    }
    if (n == 0) snprintf(opts, sizeof(opts), "(no steps - press the Add button)");
    if (s_ctx.bstep >= n) s_ctx.bstep = n > 0 ? n - 1 : 0;
    lv_dropdown_set_options(s_ctx.dd_step, opts);
    lv_dropdown_set_selected(s_ctx.dd_step, (uint32_t)(s_ctx.bstep < 0 ? 0 : s_ctx.bstep));

    builder_inspector();
}

static void builder_inspector(void)
{
    jobs_build_row_t rows[JOBS_BUILD_ROWS];
    int n = jobs_build_rows(&s_ctx.build, rows, JOBS_BUILD_ROWS);
    const jobs_node_t *node = bstep_node(rows, n);

    bool is_action = node && node->kind == JN_ACTION;
    const devos_action_descriptor_t *d = is_action ? devos_actions_find(node->u.str.s) : NULL;

    set_visible(s_ctx.dd_param, is_action && d && d->param_count > 0);
    set_visible(s_ctx.lbl_custom, !(is_action && d && d->param_count > 0));

    if (!node) { devos_w_set_text(s_ctx.lbl_custom, ""); set_visible(s_ctx.ta_val, false); set_visible(s_ctx.dd_choice, false); return; }

    if (node->kind == JN_WAIT) {
        set_visible(s_ctx.dd_param, false);
        set_visible(s_ctx.lbl_custom, true);
        char d2[24];
        fmt_dur(node->u.i, d2, sizeof(d2));
        static char m[64];
        snprintf(m, sizeof(m), "Wait %s - edit it in Text (T).", d2);
        devos_w_set_text(s_ctx.lbl_custom, m);
        set_visible(s_ctx.ta_val, false);
        set_visible(s_ctx.dd_choice, false);
        return;
    }
    if (!is_action || !d || d->param_count == 0) {
        devos_w_set_text(s_ctx.lbl_custom, "Advanced step - edit it in Text (T).");
        set_visible(s_ctx.ta_val, false);
        set_visible(s_ctx.dd_choice, false);
        return;
    }

    /* action parameters */
    char popts[512];
    size_t o = 0;
    for (int i = 0; i < d->param_count; i++)
        o += (size_t)snprintf(popts + o, sizeof(popts) - o, "%s%s", i ? "\n" : "", d->params[i].name);
    if (s_ctx.bparam >= d->param_count) s_ctx.bparam = 0;
    lv_dropdown_set_options(s_ctx.dd_param, popts);
    lv_dropdown_set_selected(s_ctx.dd_param, (uint32_t)s_ctx.bparam);

    const devos_action_param_t *p = &d->params[s_ctx.bparam];
    devos_w_set_text(s_ctx.lbl_custom, p->credential ? "Credential: a secret name, e.g. health-token"
                                                     : "Value");
    bool choice = p->choices != NULL || p->type == DEVOS_VAL_BOOL;
    set_visible(s_ctx.dd_choice, choice);
    set_visible(s_ctx.ta_val, !choice);
    if (choice) {
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
    } else {
        const char *cur = p->credential ? jobs_build_arg_secret_name(node, p->name) : jobs_build_arg_text(node, p->name);
        char dv[24];
        if (p->type == DEVOS_VAL_DURATION) {
            int64_t ms = jobs_build_arg_duration(node, p->name);
            if (ms >= 0) { fmt_dur(ms, dv, sizeof(dv)); cur = dv; }
        }
        lv_textarea_set_text(s_ctx.ta_val, cur ? cur : "");
    }
}

/* Commit the trigger card. */
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

/* Commit the selected parameter's value. */
static void builder_commit_value(void)
{
    jobs_build_row_t rows[JOBS_BUILD_ROWS];
    int n = jobs_build_rows(&s_ctx.build, rows, JOBS_BUILD_ROWS);
    const jobs_node_t *node = bstep_node(rows, n);
    if (!node || node->kind != JN_ACTION) return;
    const devos_action_descriptor_t *d = devos_actions_find(node->u.str.s);
    if (!d || s_ctx.bparam >= d->param_count) return;
    const devos_action_param_t *p = &d->params[s_ctx.bparam];

    if (p->credential) {
        jobs_build_set_arg_secret(&s_ctx.build, node, p->name, lv_textarea_get_text(s_ctx.ta_val));
    } else if (p->choices) {
        char opts[128];
        snprintf(opts, sizeof(opts), "%s", p->choices);
        int sel = (int)lv_dropdown_get_selected(s_ctx.dd_choice), i = 0;
        for (char *tok = strtok(opts, "|"); tok; tok = strtok(NULL, "|"), i++)
            if (i == sel) { jobs_build_set_arg_str(&s_ctx.build, node, p->name, tok); break; }
    } else if (p->type == DEVOS_VAL_BOOL) {
        jobs_build_set_arg_bool(&s_ctx.build, node, p->name, lv_dropdown_get_selected(s_ctx.dd_choice) == 1);
    } else if (p->type == DEVOS_VAL_DURATION) {
        int64_t ms = parse_dur(lv_textarea_get_text(s_ctx.ta_val));
        if (ms >= 0) jobs_build_set_arg_duration(&s_ctx.build, node, p->name, ms);
    } else if (p->type == DEVOS_VAL_INT) {
        jobs_build_set_arg_int(&s_ctx.build, node, p->name, strtoll(lv_textarea_get_text(s_ctx.ta_val), NULL, 10));
    } else {
        jobs_build_set_arg_str(&s_ctx.build, node, p->name, lv_textarea_get_text(s_ctx.ta_val));
    }
    builder_sync();
}

static void builder_add_step(void)
{
    jobs_build_row_t rows[JOBS_BUILD_ROWS];
    int n = jobs_build_rows(&s_ctx.build, rows, JOBS_BUILD_ROWS);
    const jobs_node_t *node = bstep_node(rows, n);
    const jobs_node_t *block = node ? rows[s_ctx.bstep].block : s_ctx.build.ast->root->c;
    int sel = (int)lv_dropdown_get_selected(s_ctx.dd_add);
    const devos_action_descriptor_t *d = devos_actions_at(sel);
    if (!d) return;
    if (jobs_build_add_action(&s_ctx.build, block, d->id)) {
        builder_sync();
        s_ctx.bstep = n;                        /* the new step */
        builder_refresh();
    }
}

static void builder_delete_step(void)
{
    jobs_build_row_t rows[JOBS_BUILD_ROWS];
    int n = jobs_build_rows(&s_ctx.build, rows, JOBS_BUILD_ROWS);
    if (s_ctx.bstep < 0 || s_ctx.bstep >= n) return;
    if (jobs_build_delete(&s_ctx.build, rows[s_ctx.bstep].block, rows[s_ctx.bstep].node)) {
        if (s_ctx.bstep > 0) s_ctx.bstep--;
        builder_sync();
        builder_refresh();
    }
}

static void builder_move(int dir)
{
    jobs_build_row_t rows[JOBS_BUILD_ROWS];
    int n = jobs_build_rows(&s_ctx.build, rows, JOBS_BUILD_ROWS);
    if (s_ctx.bstep < 0 || s_ctx.bstep >= n) return;
    if (jobs_build_move(&s_ctx.build, rows[s_ctx.bstep].block, rows[s_ctx.bstep].node, dir)) {
        s_ctx.bstep += dir;
        builder_sync();
        builder_refresh();
    }
}

/* ---- actions (shared) ---- */
static void act_validate(void)
{
    const char *src = cur_source();
    char diag[160];
    if (devos_jobs_check(src, strlen(src), diag, sizeof(diag)) == DEVOS_OK) say("Valid - press A to Apply.");
    else { snprintf(s_ctx.out, OUT_MAX, "Invalid:\n  %s", diag); say(s_ctx.out); }
}

static void act_apply(void)
{
    if (s_ctx.n == 0) { say("Create a job first (N)."); return; }
    const char *id = s_ctx.ids[s_ctx.sel];
    const char *src = cur_source();
    uint32_t rev = 0;
    devos_err_t rc = devos_jobs_apply_base(id, src, strlen(src), s_ctx.base_rev, true, &rev);
    if (rc == DEVOS_OK) {
        s_ctx.base_rev = rev;
        refresh_list();
        load_selected();
        devos_toast_show("Applied", DEVOS_TOAST_OK, 0);
        say("Applied. Press E to enable, R to run now.");
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
    devos_toast_show("New job (disabled)", DEVOS_TOAST_OK, 0);
}

static void act_delete(void)
{
    if (s_ctx.n == 0) return;
    if (devos_jobs_delete(s_ctx.ids[s_ctx.sel]) != DEVOS_OK) { say("Can't delete a running job - cancel it first."); return; }
    if (s_ctx.sel > 0) s_ctx.sel--;
    refresh_list();
    load_selected();
    devos_toast_show("Deleted", DEVOS_TOAST_WARN, 0);
}

/* ---- mode ---- */
static void set_mode(ui_mode_t m)
{
    s_ctx.mode = m;
    bool b = m == MODE_BUILDER;
    set_visible(s_ctx.ta_src, !b);
    set_visible(s_ctx.bld, b);
    devos_w_set_text(s_ctx.lbl_mode, b ? "Builder" : "Text");
    if (b) {
        if (!jobs_build_load(&s_ctx.build, cur_source(), strlen(cur_source()))) {
            devos_toast_show("Fix the source in Text first", DEVOS_TOAST_WARN, 0);
            snprintf(s_ctx.out, OUT_MAX, "Builder needs a valid job:\n  %s", s_ctx.build.diag);
            say(s_ctx.out);
            set_mode(MODE_TEXT);
            return;
        }
        s_ctx.bstep = 0;
        s_ctx.bparam = 0;
        builder_refresh();
    } else {
        jobs_build_free(&s_ctx.build);
    }
}

/* ---- callbacks ---- */
static void job_cb(lv_event_t *e) { LV_UNUSED(e); s_ctx.sel = (int)lv_dropdown_get_selected(s_ctx.dd_job); load_selected(); if (s_ctx.mode == MODE_BUILDER) set_mode(MODE_BUILDER); }
static void validate_cb(lv_event_t *e) { LV_UNUSED(e); act_validate(); }
static void apply_cb(lv_event_t *e) { LV_UNUSED(e); if (s_ctx.mode == MODE_BUILDER) builder_commit_value(); act_apply(); }
static void enable_cb(lv_event_t *e) { LV_UNUSED(e); act_enable(); }
static void run_cb(lv_event_t *e) { LV_UNUSED(e); act_run(); }
static void cancel_cb(lv_event_t *e) { LV_UNUSED(e); act_cancel(); }
static void hist_cb(lv_event_t *e) { LV_UNUSED(e); act_history(); }
static void new_cb(lv_event_t *e) { LV_UNUSED(e); act_new(); }
static void del_cb(lv_event_t *e) { LV_UNUSED(e); act_delete(); }
static void kind_cb(lv_event_t *e) { LV_UNUSED(e); if (s_ctx.mode == MODE_BUILDER) builder_commit_trigger(); }
static void step_cb(lv_event_t *e) { LV_UNUSED(e); s_ctx.bstep = (int)lv_dropdown_get_selected(s_ctx.dd_step); s_ctx.bparam = 0; builder_inspector(); }
static void param_cb(lv_event_t *e) { LV_UNUSED(e); s_ctx.bparam = (int)lv_dropdown_get_selected(s_ctx.dd_param); builder_inspector(); }
static void val_cb(lv_event_t *e) { LV_UNUSED(e); if (s_ctx.mode == MODE_BUILDER) builder_commit_value(); }
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

    s_ctx.dd_job = devos_w_dd(scr, "(no jobs)", 360);
    lv_obj_set_pos(s_ctx.dd_job, 20, 54);
    lv_obj_add_event_cb(s_ctx.dd_job, job_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_ctx.btn_new = devos_w_btn(scr, "New  [N]", 100, new_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_new, 400, 54);
    s_ctx.btn_del = devos_w_btn(scr, "Delete  [D]", 110, del_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_del, 510, 54);
    s_ctx.lbl_mode = devos_w_label(scr, NULL, DEVOS_W_TEXT_ACCENT, "Text");
    lv_obj_set_pos(s_ctx.lbl_mode, 640, 60);

    s_ctx.lbl_name = devos_w_label(scr, NULL, DEVOS_W_TEXT_ACCENT, "");
    lv_obj_set_pos(s_ctx.lbl_name, 20, 92);
    s_ctx.lbl_state = devos_w_label(scr, NULL, DEVOS_W_TEXT_DIM, "");
    lv_obj_set_pos(s_ctx.lbl_state, 20, 112);

    /* Text view */
    s_ctx.ta_src = devos_w_ta(scr, false, 760, 356);
    lv_obj_set_pos(s_ctx.ta_src, 20, 138);
    lv_textarea_set_max_length(s_ctx.ta_src, SRCMAX);

    /* Builder view */
    s_ctx.bld = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ctx.bld);
    lv_obj_set_pos(s_ctx.bld, 20, 138);
    lv_obj_set_size(s_ctx.bld, 760, 356);
    lv_obj_remove_flag(s_ctx.bld, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ctx.bld, LV_OBJ_FLAG_HIDDEN);

    mk_label(s_ctx.bld, "Trigger", DEVOS_W_TEXT_DIM, 0, 0);
    s_ctx.dd_kind = devos_w_dd(s_ctx.bld, "Manual\nEvery\nDaily\nWeekdays", 170);
    lv_obj_set_pos(s_ctx.dd_kind, 0, 20);
    lv_obj_add_event_cb(s_ctx.dd_kind, kind_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_ctx.ta_trig = devos_w_ta(s_ctx.bld, true, 220, 36);
    lv_obj_set_pos(s_ctx.ta_trig, 180, 20);
    lv_textarea_set_max_length(s_ctx.ta_trig, 24);
    lv_obj_add_event_cb(s_ctx.ta_trig, val_cb, LV_EVENT_VALUE_CHANGED, NULL);

    mk_label(s_ctx.bld, "Steps", DEVOS_W_TEXT_DIM, 0, 62);
    s_ctx.dd_step = devos_w_dd(s_ctx.bld, "(no steps)", 520);
    lv_obj_set_pos(s_ctx.dd_step, 0, 82);
    lv_obj_add_event_cb(s_ctx.dd_step, step_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_ctx.dd_add = devos_w_dd(s_ctx.bld, "", 300);
    lv_obj_set_pos(s_ctx.dd_add, 0, 120);
    s_ctx.btn_add = devos_w_btn(s_ctx.bld, "Add  [I]", 90, add_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_add, 310, 120);
    s_ctx.btn_bdel = devos_w_btn(s_ctx.bld, "Del  [D]", 90, bdel_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_bdel, 410, 120);
    s_ctx.btn_up = devos_w_btn(s_ctx.bld, "Up  [K]", 80, up_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_up, 510, 120);
    s_ctx.btn_dn = devos_w_btn(s_ctx.bld, "Down  [J]", 100, dn_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_dn, 600, 120);

    mk_label(s_ctx.bld, "Step settings", DEVOS_W_TEXT_DIM, 0, 166);
    s_ctx.dd_param = devos_w_dd(s_ctx.bld, "", 200);
    lv_obj_set_pos(s_ctx.dd_param, 0, 186);
    lv_obj_add_event_cb(s_ctx.dd_param, param_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_ctx.dd_choice = devos_w_dd(s_ctx.bld, "", 180);
    lv_obj_set_pos(s_ctx.dd_choice, 210, 186);
    lv_obj_add_event_cb(s_ctx.dd_choice, val_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_ctx.ta_val = devos_w_ta(s_ctx.bld, true, 340, 36);
    lv_obj_set_pos(s_ctx.ta_val, 400, 186);
    lv_textarea_set_max_length(s_ctx.ta_val, 200);
    lv_obj_add_event_cb(s_ctx.ta_val, val_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_ctx.lbl_custom = devos_w_label(s_ctx.bld, NULL, DEVOS_W_TEXT_MUTED, "");
    lv_obj_set_pos(s_ctx.lbl_custom, 0, 232);
    lv_obj_set_width(s_ctx.lbl_custom, 740);

    /* shared toolbar */
    s_ctx.btn_validate = devos_w_btn(scr, "Validate  [V]", 130, validate_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_validate, 20, 520);
    s_ctx.btn_apply = devos_w_btn_kind(scr, DEVOS_W_BTN_PRIMARY, LV_SYMBOL_OK " Apply  [A]", 130, apply_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_apply, 160, 520);
    s_ctx.btn_enable = devos_w_btn(scr, "Enable  [E]", 130, enable_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_enable, 300, 520);
    s_ctx.btn_run = devos_w_btn(scr, "Run now  [R]", 140, run_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_run, 440, 520);
    s_ctx.btn_cancel = devos_w_btn(scr, "Cancel  [X]", 130, cancel_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_cancel, 590, 520);
    s_ctx.btn_hist = devos_w_btn(scr, "History  [H]", 130, hist_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_hist, 20, 558);

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
    devos_w_set_text(keys, "B builder / T text  |  V validate  A apply  E enable  R run  X cancel  H history  "
                           "N new  D delete  |  Builder: I add  K/J move  |  Esc back");

    devos_focus_init(&s_ctx.focus);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_job);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_kind);
    devos_focus_add(&s_ctx.focus, s_ctx.ta_trig);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_step);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_add);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_param);
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
    set_mode(MODE_TEXT);
    say("Text: edit and Validate (V) then Apply (A). Builder: press B.");
    lv_timer_create(tick_cb, 250, NULL);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_HIDDEN);
}

static void jobs_show(void)
{
    if (!s_ctx.screen) return;
    lv_obj_remove_flag(s_ctx.screen, LV_OBJ_FLAG_HIDDEN);
    refresh_list();
    load_selected();
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
    if (devos_focus_key(&s_ctx.focus, key, mods)) return true;

    lv_obj_t *cur = devos_focus_get(&s_ctx.focus);
    bool in_field = cur && lv_obj_check_type(cur, &lv_textarea_class);
    if (in_field && cur == s_ctx.ta_val && (key == '\r' || key == '\n')) { builder_commit_value(); return true; }
    if (in_field && cur == s_ctx.ta_trig && (key == '\r' || key == '\n')) { builder_commit_trigger(); return true; }
    if (key == LV_KEY_ESC) {
        if (in_field) { devos_focus_clear(&s_ctx.focus); return true; }
        return false;
    }
    if (mods & (DEVOS_MOD_CTRL | DEVOS_MOD_FN | DEVOS_MOD_ALT)) return false;

    switch (key) {
    case 'v': case 'V': act_validate(); return true;
    case 'a': case 'A': if (s_ctx.mode == MODE_BUILDER) builder_commit_value(); act_apply(); return true;
    case 'e': case 'E': act_enable(); return true;
    case 'r': case 'R': act_run(); return true;
    case 'x': case 'X': act_cancel(); return true;
    case 'h': case 'H': act_history(); return true;
    case 'n': case 'N': act_new(); return true;
    case 'd': case 'D': if (s_ctx.mode == MODE_BUILDER) builder_delete_step(); else act_delete(); return true;
    case 'i': case 'I': if (s_ctx.mode == MODE_BUILDER) builder_add_step(); return true;
    case 'k': case 'K': if (s_ctx.mode == MODE_BUILDER) builder_move(-1); return true;
    case 'j': case 'J': if (s_ctx.mode == MODE_BUILDER) builder_move(1); return true;
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
           "B / T\tBuilder / Text\n"
           "V / A\tValidate / Apply\n"
           "E / R / X\tEnable / Run now / Cancel\n"
           "Builder\n"
           "Up / Down\tPick a step\n"
           "Tab\tStep settings\n"
           "I / D\tAdd / delete a step\n"
           "K / J\tMove the step up / down\n";
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

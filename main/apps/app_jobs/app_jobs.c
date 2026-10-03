/* app_jobs: the Jobs app (PLAN.md 11). A keyboard-first Text release: pick a
 * job, edit its source, Validate, Apply, Enable, Run now / Cancel, and read
 * diagnostics and run history. The engine (devos_jobs) keeps executing while
 * this screen is hidden or the display is off; the app only projects snapshots.
 *
 * Rules (AGENTS.md): widgets/theme (#5), keyboard first (#9) via devos_focus
 * and letter shortcuts, modular app (#8) - no launcher edits. */
#include "app_jobs.h"
#include "devos_jobs.h"
#include "devos_config.h"
#include "devos_widgets.h"
#include "devos_codeview.h"
#include "devos_focus.h"
#include "devos_icons.h"
#include "devos_toast.h"
#include <stdio.h>
#include <string.h>

#define OUT_MAX 4096
#define SRCMAX  (JOBS_MAX_SOURCE + 64)

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *lbl_status;
    lv_obj_t *dd_job;
    lv_obj_t *lbl_name, *lbl_state, *lbl_hint;
    lv_obj_t *ta_src;
    lv_obj_t *btn_new, *btn_validate, *btn_apply, *btn_enable, *btn_run, *btn_cancel, *btn_hist, *btn_del;
    lv_obj_t *out_scroll;
    devos_codeview_t cv;
    devos_focus_t focus;
    char ids[DEVOS_JOBS_MAX][DEVOS_JOBS_ID_MAX];
    int n;
    int sel;
    uint32_t base_rev;
    char out[OUT_MAX];
} jobs_ctx_t;

static devos_app_descriptor_t s_desc;
static jobs_ctx_t s_ctx;

static void set_visible(lv_obj_t *o, bool vis)
{
    if (vis) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static const char *NEW_TEMPLATE =
    "version 1;\n"
    "job \"New job\" {\n"
    "    trigger manual;\n"
    "    system.log(message: \"hello from Jobs\");\n"
    "}\n";

static void say(const char *text)
{
    devos_codeview_set(&s_ctx.cv, text ? text : "");
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

/* ---- actions ---- */
static void act_validate(void)
{
    const char *src = lv_textarea_get_text(s_ctx.ta_src);
    char diag[160];
    if (devos_jobs_check(src, strlen(src), diag, sizeof(diag)) == DEVOS_OK)
        say("Valid - press A to Apply.");
    else
        snprintf(s_ctx.out, OUT_MAX, "Invalid:\n  %s", diag), say(s_ctx.out);
}

static void act_apply(void)
{
    if (s_ctx.n == 0) { say("Create a job first (N)."); return; }
    const char *id = s_ctx.ids[s_ctx.sel];
    const char *src = lv_textarea_get_text(s_ctx.ta_src);
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
        say("Conflict: this job changed since you loaded it. Reload to see the active revision.");
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
    if (n <= 0) say("No history yet.");
    else say(s_ctx.out);
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
    if (devos_jobs_apply(id, NEW_TEMPLATE, strlen(NEW_TEMPLATE), NULL) != DEVOS_OK) {
        say("Couldn't create the job (storage full?).");
        return;
    }
    s_ctx.sel = s_ctx.n;                     /* the new job is appended */
    refresh_list();
    load_selected();
    devos_toast_show("New job (disabled)", DEVOS_TOAST_OK, 0);
}

static void act_delete(void)
{
    if (s_ctx.n == 0) return;
    if (devos_jobs_delete(s_ctx.ids[s_ctx.sel]) != DEVOS_OK) {
        say("Can't delete a running job - cancel it first.");
        return;
    }
    if (s_ctx.sel > 0) s_ctx.sel--;
    refresh_list();
    load_selected();
    devos_toast_show("Deleted", DEVOS_TOAST_WARN, 0);
}

/* ---- callbacks ---- */
static void job_cb(lv_event_t *e) { LV_UNUSED(e); s_ctx.sel = (int)lv_dropdown_get_selected(s_ctx.dd_job); load_selected(); }
static void validate_cb(lv_event_t *e) { LV_UNUSED(e); act_validate(); }
static void apply_cb(lv_event_t *e) { LV_UNUSED(e); act_apply(); }
static void enable_cb(lv_event_t *e) { LV_UNUSED(e); act_enable(); }
static void run_cb(lv_event_t *e) { LV_UNUSED(e); act_run(); }
static void cancel_cb(lv_event_t *e) { LV_UNUSED(e); act_cancel(); }
static void hist_cb(lv_event_t *e) { LV_UNUSED(e); act_history(); }
static void new_cb(lv_event_t *e) { LV_UNUSED(e); act_new(); }
static void del_cb(lv_event_t *e) { LV_UNUSED(e); act_delete(); }

/* ~4 Hz: status line + a live trace of the active run. */
static void tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!s_ctx.screen || lv_obj_has_flag(s_ctx.screen, LV_OBJ_FLAG_HIDDEN)) return;
    devos_jobs_run_t run;
    if (devos_jobs_run(&run)) {
        devos_w_set_text(s_ctx.lbl_hint, run.cancelling ? "Cancelling..." : "Running - see Output");
    } else if (devos_jobs_safe_paused()) {
        devos_w_set_text(s_ctx.lbl_hint, "Automatic jobs paused after a recovery boot - Run now still works");
    } else if (devos_jobs_paused()) {
        devos_w_set_text(s_ctx.lbl_hint, "Automatic jobs paused");
    } else {
        devos_w_set_text(s_ctx.lbl_hint, "Ready");
    }
}

/* ---- lifecycle ---- */
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

    s_ctx.lbl_name = devos_w_label(scr, NULL, DEVOS_W_TEXT_ACCENT, "");
    lv_obj_set_pos(s_ctx.lbl_name, 20, 96);
    s_ctx.lbl_state = devos_w_label(scr, NULL, DEVOS_W_TEXT_DIM, "");
    lv_obj_set_pos(s_ctx.lbl_state, 20, 118);

    s_ctx.ta_src = devos_w_ta(scr, false, 760, 360);
    lv_obj_set_pos(s_ctx.ta_src, 20, 146);
    lv_textarea_set_max_length(s_ctx.ta_src, SRCMAX);

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
    lv_obj_set_pos(l, 800, 96);
    lv_obj_t *panel = devos_w_panel(scr, 800, 116, DEVOS_SCREEN_WIDTH - 820, 540, DEVOS_W_CODE);
    s_ctx.out_scroll = lv_obj_create(panel);
    lv_obj_set_style_bg_opa(s_ctx.out_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ctx.out_scroll, 0, 0);
    lv_obj_set_style_pad_all(s_ctx.out_scroll, 6, 0);
    lv_obj_set_pos(s_ctx.out_scroll, 0, 0);
    lv_obj_set_size(s_ctx.out_scroll, DEVOS_SCREEN_WIDTH - 820, 540);
    devos_codeview_create(&s_ctx.cv, s_ctx.out_scroll);
    s_ctx.cv.plain = true;

    s_ctx.lbl_hint = devos_w_label(scr, NULL, DEVOS_W_TEXT_MUTED, "");
    lv_obj_set_pos(s_ctx.lbl_hint, 20, 596);
    lv_obj_set_width(s_ctx.lbl_hint, DEVOS_SCREEN_WIDTH - 40);

    lv_obj_t *keys = devos_w_keys(scr);
    devos_w_set_text(keys, "Up / Down pick a job  |  Tab move  |  T text  V validate  A apply  E enable  "
                           "R run  X cancel  H history  N new  D delete  |  Esc back");

    devos_focus_init(&s_ctx.focus);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_job);
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
    say("Pick a job, edit its source, Validate (V) then Apply (A).");
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
}

static bool jobs_key(uint32_t key, uint8_t mods)
{
    if (devos_focus_key(&s_ctx.focus, key, mods)) return true;

    lv_obj_t *cur = devos_focus_get(&s_ctx.focus);
    bool in_field = cur && lv_obj_check_type(cur, &lv_textarea_class);
    if (key == LV_KEY_ESC) {
        if (in_field) { devos_focus_clear(&s_ctx.focus); return true; }
        return false;
    }
    if (mods & (DEVOS_MOD_CTRL | DEVOS_MOD_FN | DEVOS_MOD_ALT)) return false;

    switch (key) {
    case 'v': case 'V': act_validate(); return true;
    case 'a': case 'A': act_apply(); return true;
    case 'e': case 'E': act_enable(); return true;
    case 'r': case 'R': act_run(); return true;
    case 'x': case 'X': act_cancel(); return true;
    case 'h': case 'H': act_history(); return true;
    case 'n': case 'N': act_new(); return true;
    case 'd': case 'D': act_delete(); return true;
    case 't': case 'T': devos_focus_set(&s_ctx.focus, s_ctx.ta_src); return true;
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
           "Up / Down\tPick a job\n"
           "T\tEdit the source\n"
           "V / A\tValidate / Apply\n"
           "E / R / X\tEnable / Run now / Cancel\n"
           "H / N / D\tHistory / New / Delete\n";
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

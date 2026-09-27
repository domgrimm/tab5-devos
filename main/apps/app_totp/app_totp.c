/* Authenticator: offline TOTP codes (like Google Authenticator / Aegis) from
 * an encrypted vault in the Tab5's flash, unlocked with a passphrase typed on
 * the keyboard. Accounts come from the camera (otpauth:// QR codes, or Google
 * Authenticator's "Transfer accounts" export codes), a typed secret, or a
 * file on the SD card. Codes need the right time: the RTC keeps it, NTP sets
 * it whenever Wi-Fi is up; a warning shows if it was never set.
 *
 * The vault locks after 3 minutes without a key press, or a minute after you
 * leave the app. Keys: Up / Down pick, Enter shows the code big, A add,
 * E edit, Del delete, Aa+Up / Down move, B backup / passphrase, L lock.
 */
#include "app_totp.h"
#include "devos_config.h"
#include "devos_icons.h"
#include "devos_core.h"
#include "devos_focus.h"
#include "devos_theme.h"
#include "devos_widgets.h"
#include "devos_qrscan.h"
#include "devos_totp.h"
#include "devos_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ROW_H         66
#define IDLE_LOCK_MS  (3 * 60 * 1000)
#define AWAY_LOCK_MS  (60 * 1000)
#define IMPORT_FILE   TAB5_SD_MOUNT_POINT "/totp/import.txt"

static devos_app_descriptor_t s_desc;
static lv_obj_t *s_screen, *lbl_clock, *s_keys, *lbl_flash;
static lv_obj_t *btn_add, *btn_backup, *btn_lock;
/* setup + lock panels */
static lv_obj_t *p_setup, *ta_new1, *ta_new2, *lbl_setup_err, *btn_create;
static lv_obj_t *p_lock, *ta_unlock, *lbl_lock_msg, *btn_unlock, *btn_erase;
static devos_focus_t s_fsetup, s_flock;
/* main */
static lv_obj_t *p_main, *lbl_empty;
static lv_obj_t *p_busy, *lbl_busy, *bar_busy, *lbl_busy_pct;   /* while the key is derived */
static devos_vlist_t s_list;
/* big view */
static lv_obj_t *big, *lbl_big_name, *lbl_big_code, *lbl_big_next, *bar_big;
/* dialogs */
static devos_w_dialog_t d_add, d_form, d_del, d_backup, d_pass, d_erase, d_sdimp;
static devos_focus_t f_add, f_form, f_del, f_backup, f_pass, f_erase, f_sdimp;
static lv_obj_t *ta_iss, *ta_acc, *ta_sec, *dd_digits, *dd_period, *dd_algo, *lbl_form_hint;
static lv_obj_t *ta_pass1, *ta_pass2, *lbl_pass2_cap, *ta_erase;
static devos_qrscan_t s_scan;

static devos_totp_state_t s_last_state = (devos_totp_state_t)-1;
static int s_edit = -1;                         /* form: -1 add, else index */
static int s_pass_mode;                         /* 1 import backup, 2 change passphrase */
static uint32_t s_last_key, s_hidden_at, s_flash_until;
static bool s_visible, s_inited;

/* ------------------------------------------------------------------ helpers */
static void flash(const char *msg, bool err)
{
    devos_w_set_text(lbl_flash, msg);
    devos_w_track(lbl_flash, err ? DEVOS_W_TEXT_ERR : DEVOS_W_TEXT_OK);
    s_flash_until = lv_tick_get() + 4000;
}

static void fmt_code(uint32_t code, int digits, char *out, size_t cap)
{
    char d[12];
    snprintf(d, sizeof(d), "%0*u", digits, (unsigned)code);
    int half = digits / 2 + (digits == 7);     /* 123 456, 123 4567, 1234 5678 */
    snprintf(out, cap, "%.*s %s", half, d, d + half);
}

static bool any_dialog(void)
{
    return devos_w_dialog_open(&d_add) || devos_w_dialog_open(&d_form) || devos_w_dialog_open(&d_del) ||
           devos_w_dialog_open(&d_backup) || devos_w_dialog_open(&d_pass) || devos_w_dialog_open(&d_erase) ||
           devos_w_dialog_open(&d_sdimp) || devos_qrscan_is_open(&s_scan);
}

static void close_dialog(devos_w_dialog_t *d, devos_focus_t *f)
{
    devos_w_dialog_show(d, false);
    devos_focus_clear(f);
}

static void close_all(void)
{
    close_dialog(&d_add, &f_add);
    close_dialog(&d_form, &f_form);
    close_dialog(&d_del, &f_del);
    close_dialog(&d_backup, &f_backup);
    close_dialog(&d_pass, &f_pass);
    close_dialog(&d_erase, &f_erase);
    close_dialog(&d_sdimp, &f_sdimp);
    devos_qrscan_close(&s_scan);
    lv_obj_add_flag(big, LV_OBJ_FLAG_HIDDEN);
}

static void do_lock(void)
{
    close_all();
    devos_totp_lock();
    lv_textarea_set_text(ta_unlock, "");
}

/* ------------------------------------------------------------------ list */
static void draw_countdown(lv_layer_t *layer, int cx, int cy, int r, int remaining, int period)
{
    const devos_palette_t *p = devos_theme_get();
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.center.x = cx;
    ad.center.y = cy;
    ad.radius = (uint16_t)r;
    ad.width = 4;
    ad.start_angle = 0;
    ad.end_angle = 360;
    ad.color = p->surface_border;
    lv_draw_arc(layer, &ad);
    ad.start_angle = 270;
    ad.end_angle = (lv_value_precise_t)(270 + 360 * remaining / (period ? period : 30));
    ad.color = remaining <= 5 ? p->accent_warning : p->accent_primary;
    if (ad.end_angle > ad.start_angle) lv_draw_arc(layer, &ad);
    char s[8];
    snprintf(s, sizeof(s), "%d", remaining);
    devos_w_draw_text(layer, &lv_font_montserrat_12, cx - (remaining >= 10 ? 7 : 4), cy - 7, 0, s, p->text_secondary);
}

static void row_draw(devos_vlist_t *v, lv_layer_t *layer, const lv_area_t *row, int idx)
{
    LV_UNUSED(v);
    devos_totp_acct_t a;
    if (!devos_totp_get(idx, &a)) return;
    const devos_palette_t *p = devos_theme_get();
    int64_t now = (int64_t)time(NULL);
    int rem;
    uint32_t code = devos_totp_code(&a, now, &rem);
    char buf[24];
    int x = row->x1 + 20, y = row->y1;
    devos_w_draw_text(layer, &lv_font_montserrat_18, x, y + 12, 520, a.issuer[0] ? a.issuer : a.label, p->text_primary);
    if (a.issuer[0]) devos_w_draw_text(layer, &lv_font_montserrat_12, x, y + 38, 520, a.label, p->text_secondary);
    fmt_code(code, a.digits, buf, sizeof(buf));
    devos_w_draw_text(layer, &lv_font_montserrat_28, row->x2 - 330, y + 16, 0, buf, rem <= 5 ? p->accent_warning : p->accent_primary);
    if (rem <= 10) {
        char nx[32], nb[24];
        uint32_t next = devos_totp_code(&a, now + a.period, NULL);
        fmt_code(next, a.digits, nb, sizeof(nb));
        snprintf(nx, sizeof(nx), "next %s", nb);
        devos_w_draw_text(layer, &lv_font_montserrat_12, row->x2 - 330, y + 48, 0, nx, p->text_muted);
    }
    draw_countdown(layer, row->x2 - 44, y + ROW_H / 2, 16, rem, a.period);
    devos_w_draw_rect(layer, row->x1 + 12, row->y2, row->x2 - 12, row->y2, p->surface_border, LV_OPA_50, 0);
    devos_wipe(&a, sizeof(a));
}

static void show_big(bool on)
{
    if (on && s_list.sel < 0) return;
    if (on) {
        lv_obj_remove_flag(big, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(big);
    } else {
        lv_obj_add_flag(big, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_big(void)
{
    if (lv_obj_has_flag(big, LV_OBJ_FLAG_HIDDEN)) return;
    devos_totp_acct_t a;
    if (!devos_totp_get(s_list.sel, &a)) {
        show_big(false);
        return;
    }
    int64_t now = (int64_t)time(NULL);
    int rem;
    char buf[24], nb[24], nx[48], name[128];
    fmt_code(devos_totp_code(&a, now, &rem), a.digits, buf, sizeof(buf));
    fmt_code(devos_totp_code(&a, now + a.period, NULL), a.digits, nb, sizeof(nb));
    snprintf(name, sizeof(name), "%s%s%s", a.issuer, a.issuer[0] && a.label[0] ? "  -  " : "", a.label);
    snprintf(nx, sizeof(nx), "%d s left  -  next %s", rem, nb);
    devos_w_set_text(lbl_big_name, name);
    devos_w_set_text(lbl_big_code, buf);
    devos_w_track(lbl_big_code, rem <= 5 ? DEVOS_W_TEXT_WARN : DEVOS_W_TEXT_ACCENT);
    devos_w_set_text(lbl_big_next, nx);
    lv_bar_set_value(bar_big, rem * 1000 / (a.period ? a.period : 30), LV_ANIM_OFF);
    devos_wipe(&a, sizeof(a));
}

static void activate_cb(devos_vlist_t *v, int idx)
{
    LV_UNUSED(v);
    LV_UNUSED(idx);
    show_big(true);
    refresh_big();
}

/* ------------------------------------------------------------------ setup / unlock */
static void create_vault(void)
{
    const char *a = lv_textarea_get_text(ta_new1), *b = lv_textarea_get_text(ta_new2);
    if (strlen(a) < 6) {
        devos_w_set_text(lbl_setup_err, "Use at least 6 characters - longer is much safer");
        return;
    }
    if (strcmp(a, b)) {
        devos_w_set_text(lbl_setup_err, "The two passphrases don't match");
        devos_focus_set(&s_fsetup, ta_new2);
        return;
    }
    if (devos_totp_create(a) != 0) devos_w_set_text(lbl_setup_err, devos_totp_error());
    lv_textarea_set_text(ta_new1, "");
    lv_textarea_set_text(ta_new2, "");
}

static void unlock_vault(void)
{
    const char *pw = lv_textarea_get_text(ta_unlock);
    if (!pw[0]) return;
    if (devos_totp_unlock(pw) != 0) devos_w_set_text(lbl_lock_msg, devos_totp_error());
    lv_textarea_set_text(ta_unlock, "");
}

static void create_cb(lv_event_t *e) { LV_UNUSED(e); create_vault(); }
static void unlock_cb(lv_event_t *e) { LV_UNUSED(e); unlock_vault(); }

/* ------------------------------------------------------------------ add / edit */
static bool scan_result(const char *text, char *msg, size_t cap)
{
    int before = devos_totp_count();
    int r = devos_totp_import_uri(text);
    bool migration = !strncmp(text, "otpauth-migration://", 20);
    if (r > 0) {
        if (migration) {
            snprintf(msg, cap, "%s. More export codes? Show the next one, or press Esc.", devos_totp_error());
            devos_vlist_set_count(&s_list, devos_totp_count());
            return false;                               /* stay open for the next batch */
        }
        devos_totp_acct_t a;
        char m[128];
        if (devos_totp_get(before, &a)) {
            snprintf(m, sizeof(m), "Added %s%s%s", a.issuer, a.issuer[0] ? " - " : "", a.label);
            devos_wipe(&a, sizeof(a));
        } else {
            snprintf(m, sizeof(m), "Added");
        }
        flash(m, false);
        devos_vlist_set_count(&s_list, devos_totp_count());
        devos_vlist_select(&s_list, devos_totp_count() - 1);
        return true;
    }
    snprintf(msg, cap, "%s", r == 0 ? "Already in the vault" : devos_totp_error());
    return false;
}

static void form_open(int idx)
{
    s_edit = idx;
    devos_totp_acct_t a;
    memset(&a, 0, sizeof(a));
    a.digits = 6;
    a.period = 30;
    a.algo = 1;
    if (idx >= 0 && !devos_totp_get(idx, &a)) return;
    lv_textarea_set_text(ta_iss, a.issuer);
    lv_textarea_set_text(ta_acc, a.label);
    lv_textarea_set_text(ta_sec, "");
    lv_textarea_set_placeholder_text(ta_sec, idx >= 0 ? "(unchanged - type a new one to replace it)" : "JBSW Y3DP EHPK 3PXP ...");
    lv_dropdown_set_selected(dd_digits, (uint32_t)(a.digits - 6));
    lv_dropdown_set_selected(dd_period, a.period == 60 ? 1 : 0);
    lv_dropdown_set_selected(dd_algo, (uint32_t)(a.algo - 1));
    devos_w_set_text(d_form.title, idx >= 0 ? LV_SYMBOL_EDIT "  Edit account" : LV_SYMBOL_KEYBOARD "  Add an account by its key");
    devos_w_set_text(d_form.msg, "");
    devos_wipe(&a, sizeof(a));
    close_dialog(&d_add, &f_add);
    devos_w_dialog_show(&d_form, true);
    devos_focus_set(&f_form, ta_iss);
}

static void form_save(void)
{
    devos_totp_acct_t a;
    memset(&a, 0, sizeof(a));
    if (s_edit >= 0 && !devos_totp_get(s_edit, &a)) return;
    snprintf(a.issuer, sizeof(a.issuer), "%s", lv_textarea_get_text(ta_iss));
    snprintf(a.label, sizeof(a.label), "%s", lv_textarea_get_text(ta_acc));
    const char *sec = lv_textarea_get_text(ta_sec);
    if (sec[0] || s_edit < 0) {
        uint8_t raw[64];
        int n = devos_base32_decode(sec, raw, sizeof(raw));
        if (n < 10) {
            devos_w_set_text(d_form.msg, n < 0 ? "The key has characters base32 doesn't use (only A-Z, 2-7)"
                                               : "That key is too short (usually 16 or 32 characters)");
            devos_wipe(raw, sizeof(raw));
            devos_wipe(&a, sizeof(a));
            return;
        }
        memcpy(a.secret, raw, (size_t)n);
        a.secret_len = (uint8_t)n;
        devos_wipe(raw, sizeof(raw));
    }
    if (!a.issuer[0] && !a.label[0]) {
        devos_w_set_text(d_form.msg, "Give it a name (issuer or account)");
        devos_wipe(&a, sizeof(a));
        return;
    }
    a.digits = (uint8_t)(6 + lv_dropdown_get_selected(dd_digits));
    a.period = lv_dropdown_get_selected(dd_period) ? 60 : 30;
    a.algo = (uint8_t)(1 + lv_dropdown_get_selected(dd_algo));
    int rc = s_edit >= 0 ? devos_totp_update(s_edit, &a) : devos_totp_add(&a);
    devos_wipe(&a, sizeof(a));
    lv_textarea_set_text(ta_sec, "");
    if (rc != 0) {
        devos_w_set_text(d_form.msg, devos_totp_error());
        return;
    }
    close_dialog(&d_form, &f_form);
    devos_vlist_set_count(&s_list, devos_totp_count());
    if (s_edit < 0) devos_vlist_select(&s_list, devos_totp_count() - 1);
    flash(s_edit >= 0 ? "Saved" : "Added", false);
}

static void sd_import(void)
{
    close_dialog(&d_add, &f_add);
    FILE *f = fopen(IMPORT_FILE, "rb");
    if (!f) {
        flash("Put otpauth:// lines in /totp/import.txt on the SD card first", true);
        return;
    }
    static char line[4096];
    int added = 0, failed = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = '\0';
        if (!l || line[0] == '#') continue;
        int r = devos_totp_import_uri(line);
        if (r > 0) added += r;
        else if (r < 0) failed++;
    }
    devos_wipe(line, sizeof(line));
    fclose(f);
    devos_vlist_set_count(&s_list, devos_totp_count());
    char msg[160];
    snprintf(msg, sizeof(msg), "Imported %d account%s%s.\nimport.txt holds your secrets unencrypted: delete it now?",
             added, added == 1 ? "" : "s", failed ? " (some lines weren't valid)" : "");
    devos_w_set_text(d_sdimp.title, msg);
    devos_w_dialog_show(&d_sdimp, true);
    devos_focus_first(&f_sdimp);
}

static void sdimp_delete_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    /* overwrite before removing: FAT doesn't shred on delete */
    FILE *f = fopen(IMPORT_FILE, "r+b");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        for (long i = 0; i < n; i++) fputc(0, f);
        fclose(f);
    }
    bool ok = remove(IMPORT_FILE) == 0;
    close_dialog(&d_sdimp, &f_sdimp);
    flash(ok ? "import.txt wiped and deleted" : "Couldn't delete import.txt", !ok);
}

static void sdimp_keep_cb(lv_event_t *e) { LV_UNUSED(e); close_dialog(&d_sdimp, &f_sdimp); }

static void add_scan_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    close_dialog(&d_add, &f_add);
    devos_qrscan_open(&s_scan);
}
static void add_manual_cb(lv_event_t *e) { LV_UNUSED(e); form_open(-1); }
static void add_sd_cb(lv_event_t *e) { LV_UNUSED(e); sd_import(); }
static void add_cancel_cb(lv_event_t *e) { LV_UNUSED(e); close_dialog(&d_add, &f_add); }
static void form_ok_cb(lv_event_t *e) { LV_UNUSED(e); form_save(); }
static void form_cancel_cb(lv_event_t *e) { LV_UNUSED(e); lv_textarea_set_text(ta_sec, ""); close_dialog(&d_form, &f_form); }

static void add_open(void)
{
    devos_w_dialog_show(&d_add, true);
    devos_focus_first(&f_add);
}

/* ------------------------------------------------------------------ delete */
static void del_open(void)
{
    devos_totp_acct_t a;
    if (!devos_totp_get(s_list.sel, &a)) return;
    char buf[160];
    snprintf(buf, sizeof(buf), LV_SYMBOL_TRASH "  Delete %s%s%s?", a.issuer, a.issuer[0] ? " - " : "", a.label);
    devos_wipe(&a, sizeof(a));
    devos_w_set_text(d_del.title, buf);
    devos_w_dialog_show(&d_del, true);
    devos_focus_first(&f_del);
}

static void del_ok_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int i = s_list.sel;
    close_dialog(&d_del, &f_del);
    if (devos_totp_delete(i) == 0) {
        devos_vlist_set_count(&s_list, devos_totp_count());
        flash("Deleted", false);
    } else {
        flash(devos_totp_error(), true);
    }
}
static void del_cancel_cb(lv_event_t *e) { LV_UNUSED(e); close_dialog(&d_del, &f_del); }

/* ------------------------------------------------------------------ backup / passphrase */
static void backup_open(void)
{
    devos_w_dialog_show(&d_backup, true);
    devos_focus_first(&f_backup);
}

static void pass_open(int mode)
{
    s_pass_mode = mode;
    close_dialog(&d_backup, &f_backup);
    lv_textarea_set_text(ta_pass1, "");
    lv_textarea_set_text(ta_pass2, "");
    if (mode == 1) {
        devos_w_set_text(d_pass.title, LV_SYMBOL_DOWNLOAD "  Import the backup from the SD card");
        lv_obj_add_flag(ta_pass2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_pass2_cap, LV_OBJ_FLAG_HIDDEN);
    } else {
        devos_w_set_text(d_pass.title, LV_SYMBOL_EDIT "  New passphrase");
        lv_obj_remove_flag(ta_pass2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(lbl_pass2_cap, LV_OBJ_FLAG_HIDDEN);
    }
    devos_w_set_text(d_pass.msg, "");
    devos_w_dialog_show(&d_pass, true);
    devos_focus_set(&f_pass, ta_pass1);
}

static void pass_ok(void)
{
    const char *a = lv_textarea_get_text(ta_pass1), *b = lv_textarea_get_text(ta_pass2);
    int rc;
    if (s_pass_mode == 2) {
        if (strcmp(a, b)) {
            devos_w_set_text(d_pass.msg, "The two passphrases don't match");
            return;
        }
        rc = devos_totp_change_passphrase(a);
    } else {
        rc = devos_totp_import_backup(a);
    }
    lv_textarea_set_text(ta_pass1, "");
    lv_textarea_set_text(ta_pass2, "");
    if (rc != 0) {
        devos_w_set_text(d_pass.msg, devos_totp_error());
        return;
    }
    close_dialog(&d_pass, &f_pass);
    flash(s_pass_mode == 2 ? "Re-encrypting under the new passphrase..." : "Reading the backup...", false);
}

static void export_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    close_dialog(&d_backup, &f_backup);
    int rc = devos_totp_export_backup();
    flash(devos_totp_error(), rc != 0);
}
static void import_bk_cb(lv_event_t *e) { LV_UNUSED(e); pass_open(1); }
static void change_cb(lv_event_t *e) { LV_UNUSED(e); pass_open(2); }
static void backup_cancel_cb(lv_event_t *e) { LV_UNUSED(e); close_dialog(&d_backup, &f_backup); }
static void pass_ok_cb(lv_event_t *e) { LV_UNUSED(e); pass_ok(); }
static void pass_cancel_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_textarea_set_text(ta_pass1, "");
    lv_textarea_set_text(ta_pass2, "");
    close_dialog(&d_pass, &f_pass);
}

/* ------------------------------------------------------------------ erase */
static void erase_open(void)
{
    lv_textarea_set_text(ta_erase, "");
    devos_w_set_text(d_erase.msg, "");
    devos_w_dialog_show(&d_erase, true);
    devos_focus_set(&f_erase, ta_erase);
}

static void erase_ok(void)
{
    if (strcmp(lv_textarea_get_text(ta_erase), "ERASE")) {
        devos_w_set_text(d_erase.msg, "Type ERASE (capitals) to confirm");
        return;
    }
    close_dialog(&d_erase, &f_erase);
    devos_totp_erase();
    flash("Vault erased", false);
}
static void erase_open_cb(lv_event_t *e) { LV_UNUSED(e); erase_open(); }
static void erase_ok_cb(lv_event_t *e) { LV_UNUSED(e); erase_ok(); }
static void erase_cancel_cb(lv_event_t *e) { LV_UNUSED(e); close_dialog(&d_erase, &f_erase); }

static void add_cb(lv_event_t *e) { LV_UNUSED(e); add_open(); }
static void backup_cb(lv_event_t *e) { LV_UNUSED(e); backup_open(); }
static void lock_cb(lv_event_t *e) { LV_UNUSED(e); do_lock(); }

/* ------------------------------------------------------------------ state + tick */
static void apply_state(devos_totp_state_t st)
{
    bool unlocked = st == DEVOS_TOTP_UNLOCKED;
    if (st == DEVOS_TOTP_NO_VAULT) lv_obj_remove_flag(p_setup, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(p_setup, LV_OBJ_FLAG_HIDDEN);
    if (st == DEVOS_TOTP_LOCKED) lv_obj_remove_flag(p_lock, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(p_lock, LV_OBJ_FLAG_HIDDEN);
    if (st == DEVOS_TOTP_BUSY) {
        char t[64];
        snprintf(t, sizeof(t), LV_SYMBOL_REFRESH "  %s...", devos_totp_busy_text());
        devos_w_set_text(lbl_busy, t);
        lv_bar_set_value(bar_busy, 0, LV_ANIM_OFF);
        lv_obj_remove_flag(p_busy, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(p_busy, LV_OBJ_FLAG_HIDDEN);
    }
    if (unlocked) lv_obj_remove_flag(p_main, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(p_main, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *btns[] = { btn_add, btn_backup, btn_lock };
    for (int i = 0; i < 3; i++) {
        if (unlocked) lv_obj_remove_flag(btns[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(btns[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (st == DEVOS_TOTP_NO_VAULT) devos_focus_set(&s_fsetup, ta_new1);
    else if (st == DEVOS_TOTP_LOCKED) {
        devos_focus_set(&s_flock, ta_unlock);
        if (s_last_state == DEVOS_TOTP_BUSY || s_last_state == DEVOS_TOTP_UNLOCKED)
            devos_w_set_text(lbl_lock_msg, s_last_state == DEVOS_TOTP_BUSY ? devos_totp_error() : "Locked.");
    } else if (unlocked) {
        devos_focus_clear(&s_flock);
        devos_focus_clear(&s_fsetup);
        devos_vlist_set_count(&s_list, devos_totp_count());
        if (s_list.sel < 0 && devos_totp_count()) devos_vlist_select(&s_list, 0);
        if (s_last_state == DEVOS_TOTP_BUSY && devos_totp_error()[0]) flash(devos_totp_error(), !strstr(devos_totp_error(), "Imported"));
    } else {
        close_all();
    }
    if (st == DEVOS_TOTP_NO_VAULT) devos_w_set_text(lbl_setup_err, s_last_state == DEVOS_TOTP_BUSY ? devos_totp_error() : "");
    s_last_state = st;
}

static const char *keys_text(void)
{
    if (devos_qrscan_is_open(&s_scan)) return "Show the QR code to the camera    Esc cancels";
    if (any_dialog()) return "Tab / arrows move    Enter confirms    Esc cancels";
    if (!lv_obj_has_flag(big, LV_OBJ_FLAG_HIDDEN)) return "Up / Down next account    Enter or Esc back";
    switch (devos_totp_state()) {
    case DEVOS_TOTP_NO_VAULT: return "Type a passphrase, Tab, type it again, Enter    Esc home";
    case DEVOS_TOTP_LOCKED: return "Type your passphrase, Enter unlocks    Tab reaches Erase    Esc home";
    case DEVOS_TOTP_BUSY: return "Working - a few seconds    Esc home (it carries on)";
    default: return "Up / Down pick    Enter big code    A add    E edit    Del delete    Aa+Up / Down move    B backup    L lock    Esc home";
    }
}

static void tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    devos_totp_state_t st = devos_totp_state();
    if (st != s_last_state) apply_state(st);
    if (!s_visible) {
        if (st == DEVOS_TOTP_UNLOCKED && s_hidden_at && lv_tick_elaps(s_hidden_at) > AWAY_LOCK_MS) do_lock();
        return;
    }
    if (st == DEVOS_TOTP_UNLOCKED && lv_tick_elaps(s_last_key) > IDLE_LOCK_MS && !devos_qrscan_is_open(&s_scan)) {
        do_lock();
        devos_w_set_text(lbl_lock_msg, "Locked after 3 minutes idle.");
    }
    /* clock */
    const devos_telemetry_t *tel = devos_telemetry_get();
    time_t now = time(NULL);
    char buf[160];
    if (!tel->time_valid) snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING "  The clock was never set - codes will be wrong. Connect Wi-Fi (NTP) or set it in Settings.");
    else {
        struct tm tm;
        gmtime_r(&now, &tm);
        strftime(buf, sizeof(buf), "UTC %H:%M:%S", &tm);
    }
    devos_w_set_text(lbl_clock, buf);
    devos_w_track(lbl_clock, tel->time_valid ? DEVOS_W_TEXT_DIM : DEVOS_W_TEXT_ERR);
    if (s_flash_until && (int32_t)(lv_tick_get() - s_flash_until) >= 0) {
        s_flash_until = 0;
        devos_w_set_text(lbl_flash, "");
    }
    if (st == DEVOS_TOTP_LOCKED) {
        int w = devos_totp_lockout_s();
        if (w) {
            snprintf(buf, sizeof(buf), "Too many wrong tries: wait %d s", w);
            devos_w_set_text(lbl_lock_msg, buf);
        }
    } else if (st == DEVOS_TOTP_BUSY) {
        int pct = devos_totp_progress();
        lv_bar_set_value(bar_busy, pct, LV_ANIM_OFF);
        snprintf(buf, sizeof(buf), "%d%%", pct);
        devos_w_set_text(lbl_busy_pct, buf);
    }
    if (st == DEVOS_TOTP_UNLOCKED) {
        int n = devos_totp_count();
        if (n != s_list.count) devos_vlist_set_count(&s_list, n);
        if (n && s_list.sel < 0) devos_vlist_select(&s_list, 0);
        if (n) lv_obj_add_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(lbl_empty, LV_OBJ_FLAG_HIDDEN);
        devos_vlist_redraw(&s_list);            /* codes and countdowns move every second */
        refresh_big();
    }
    devos_w_set_text(s_keys, keys_text());
}

/* ------------------------------------------------------------------ keys */
static bool dlg_key(devos_w_dialog_t *d, devos_focus_t *f, uint32_t key, uint8_t mods, void (*enter)(void))
{
    if (!devos_w_dialog_open(d)) return false;
    if (key == LV_KEY_ESC && !f->dd_open) {
        if (d == &d_form) lv_textarea_set_text(ta_sec, "");
        if (d == &d_pass) { lv_textarea_set_text(ta_pass1, ""); lv_textarea_set_text(ta_pass2, ""); }
        close_dialog(d, f);
    } else if (devos_focus_key(f, key, mods)) {
    } else if ((key == '\r' || key == '\n') && enter) {
        enter();
    }
    return true;
}

static bool totp_key(uint32_t key, uint8_t mods)
{
    s_last_key = lv_tick_get();
    if (devos_qrscan_key(&s_scan, key)) return true;
    if (dlg_key(&d_form, &f_form, key, mods, form_save)) return true;
    if (dlg_key(&d_add, &f_add, key, mods, NULL)) return true;
    if (dlg_key(&d_del, &f_del, key, mods, NULL)) return true;
    if (dlg_key(&d_backup, &f_backup, key, mods, NULL)) return true;
    if (dlg_key(&d_pass, &f_pass, key, mods, pass_ok)) return true;
    if (dlg_key(&d_erase, &f_erase, key, mods, erase_ok)) return true;
    if (dlg_key(&d_sdimp, &f_sdimp, key, mods, NULL)) return true;

    devos_totp_state_t st = devos_totp_state();
    if (st == DEVOS_TOTP_NO_VAULT) {
        if (key == LV_KEY_ESC) return false;
        if (devos_focus_key(&s_fsetup, key, mods)) return true;
        if (key == '\r' || key == '\n') {
            if (devos_focus_get(&s_fsetup) == ta_new1) devos_focus_set(&s_fsetup, ta_new2);
            else create_vault();
        }
        return true;
    }
    if (st == DEVOS_TOTP_LOCKED || st == DEVOS_TOTP_BUSY) {
        if (key == LV_KEY_ESC) return false;
        if (st == DEVOS_TOTP_BUSY) return true;
        if (devos_focus_key(&s_flock, key, mods)) return true;
        if (key == '\r' || key == '\n') unlock_vault();
        return true;
    }
    /* unlocked */
    if (!lv_obj_has_flag(big, LV_OBJ_FLAG_HIDDEN)) {
        if (key == LV_KEY_ESC || key == '\r' || key == '\n' || key == ' ') show_big(false);
        else if (key == LV_KEY_UP || key == LV_KEY_DOWN) { devos_vlist_key(&s_list, key); refresh_big(); }
        return true;
    }
    if (mods & (DEVOS_MOD_CTRL | DEVOS_MOD_ALT | DEVOS_MOD_FN)) return false;
    if ((mods & DEVOS_MOD_SHIFT) && (key == LV_KEY_UP || key == LV_KEY_DOWN)) {
        int d = key == LV_KEY_UP ? -1 : 1;
        if (devos_totp_move(s_list.sel, d) == 0) devos_vlist_select(&s_list, s_list.sel + d);
        return true;
    }
    if (devos_vlist_key(&s_list, key)) return true;
    switch (key) {
    case 'a': case 'A': add_open(); return true;
    case 'e': case 'E': if (s_list.sel >= 0) form_open(s_list.sel); return true;
    case 127: case 'd': case 'D': if (s_list.sel >= 0) del_open(); return true;
    case 'b': case 'B': backup_open(); return true;
    case 'l': case 'L': do_lock(); return true;
    case LV_KEY_ESC: return false;
    default: return key >= 32 && key < 127;
    }
}

/* ------------------------------------------------------------------ init */
static lv_obj_t *centre_panel(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *p = devos_w_panel(parent, (DEVOS_SCREEN_WIDTH - w) / 2, 70, w, h, DEVOS_W_PANEL);
    lv_obj_set_style_radius(p, 10, 0);
    lv_obj_set_style_pad_all(p, 24, 0);
    return p;
}

static lv_obj_t *dlg_btn(devos_w_dialog_t *d, devos_w_kind_t kind, const char *text, int w, int x_from_right,
                         lv_event_cb_t cb)
{
    lv_obj_t *b = devos_w_btn_kind(d->box, kind, text, w, cb, NULL, NULL);
    lv_obj_set_size(b, w, 36);
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -x_from_right, 0);
    return b;
}

static lv_obj_t *menu_btn(lv_obj_t *parent, const char *text, int y, lv_event_cb_t cb)
{
    lv_obj_t *b = devos_w_btn(parent, text, 480, cb, NULL, NULL);
    lv_obj_set_size(b, 480, 44);
    lv_obj_set_pos(b, 0, y);
    lv_obj_set_style_text_font(b, &lv_font_montserrat_14, 0);
    return b;
}

static void totp_init(void)
{
    if (s_inited) return;
    s_inited = true;
    devos_totp_init();

    s_screen = devos_w_screen(&s_desc);
    lv_obj_t *bar = devos_w_bar(s_screen, LV_SYMBOL_EYE_CLOSE "  Authenticator", NULL);
    lbl_clock = devos_w_label(bar, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "");
    lv_obj_align(lbl_clock, LV_ALIGN_LEFT_MID, 190, 0);
    btn_lock = devos_w_btn(bar, LV_SYMBOL_EYE_CLOSE " Lock", 70, lock_cb, NULL, NULL);
    lv_obj_align(btn_lock, LV_ALIGN_RIGHT_MID, -8, 0);
    btn_backup = devos_w_btn(bar, LV_SYMBOL_SAVE " Backup", 84, backup_cb, NULL, NULL);
    lv_obj_align_to(btn_backup, btn_lock, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    btn_add = devos_w_btn(bar, LV_SYMBOL_PLUS " Add", 64, add_cb, NULL, NULL);
    lv_obj_align_to(btn_add, btn_backup, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    lbl_flash = devos_w_label(bar, &lv_font_montserrat_12, DEVOS_W_TEXT_OK, "");
    lv_label_set_long_mode(lbl_flash, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_flash, 420);
    lv_obj_set_style_text_align(lbl_flash, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(lbl_flash, LV_ALIGN_RIGHT_MID, -250, 0);

    /* setup */
    p_setup = centre_panel(s_screen, 620, 400);
    lv_obj_t *t = devos_w_label(p_setup, &lv_font_montserrat_20, DEVOS_W_TEXT_ACCENT, LV_SYMBOL_EYE_CLOSE "  Create your vault");
    lv_obj_t *e = devos_w_label(p_setup, &lv_font_montserrat_14, DEVOS_W_TEXT_DIM,
                                "Your 2FA secrets are encrypted with this passphrase and kept in the\n"
                                "Tab5's flash. There is no way to recover it: pick one you'll remember,\n"
                                "and use Backup to keep an encrypted copy on the SD card.");
    lv_obj_set_pos(e, 0, 36);
    ta_new1 = devos_w_field(p_setup, "Passphrase (6+ characters; a few words is best)", 0, 112, 570);
    lv_textarea_set_password_mode(ta_new1, true);
    lv_textarea_set_max_length(ta_new1, 120);
    ta_new2 = devos_w_field(p_setup, "Again", 0, 174, 570);
    lv_textarea_set_password_mode(ta_new2, true);
    lv_textarea_set_max_length(ta_new2, 120);
    lbl_setup_err = devos_w_label(p_setup, &lv_font_montserrat_12, DEVOS_W_TEXT_ERR, "");
    lv_obj_set_pos(lbl_setup_err, 0, 244);
    btn_create = devos_w_btn_kind(p_setup, DEVOS_W_BTN_PRIMARY, LV_SYMBOL_OK "  Create vault", 170, create_cb, NULL, NULL);
    lv_obj_set_size(btn_create, 170, 40);
    lv_obj_align(btn_create, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    (void)t;
    devos_focus_init(&s_fsetup);
    devos_focus_add(&s_fsetup, ta_new1);
    devos_focus_add(&s_fsetup, ta_new2);
    devos_focus_add(&s_fsetup, btn_create);

    /* lock */
    p_lock = centre_panel(s_screen, 560, 300);
    devos_w_label(p_lock, &lv_font_montserrat_20, DEVOS_W_TEXT_ACCENT, LV_SYMBOL_EYE_CLOSE "  Authenticator locked");
    ta_unlock = devos_w_field(p_lock, "Passphrase", 0, 48, 510);
    lv_textarea_set_password_mode(ta_unlock, true);
    lv_textarea_set_max_length(ta_unlock, 120);
    lbl_lock_msg = devos_w_label(p_lock, &lv_font_montserrat_14, DEVOS_W_TEXT_WARN, "");
    lv_obj_set_width(lbl_lock_msg, 510);
    lv_label_set_long_mode(lbl_lock_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(lbl_lock_msg, 0, 118);
    btn_unlock = devos_w_btn_kind(p_lock, DEVOS_W_BTN_PRIMARY, LV_SYMBOL_OK "  Unlock", 140, unlock_cb, NULL, NULL);
    lv_obj_set_size(btn_unlock, 140, 40);
    lv_obj_align(btn_unlock, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    btn_erase = devos_w_btn(p_lock, "Forgot it? Erase the vault...", 230, erase_open_cb, NULL, NULL);
    lv_obj_set_size(btn_erase, 230, 32);
    lv_obj_align(btn_erase, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    devos_focus_init(&s_flock);
    devos_focus_add(&s_flock, ta_unlock);
    devos_focus_add(&s_flock, btn_unlock);
    devos_focus_add(&s_flock, btn_erase);

    /* busy: the passphrase is being stretched (seconds on the device) */
    p_busy = centre_panel(s_screen, 560, 164);
    lbl_busy = devos_w_label(p_busy, &lv_font_montserrat_20, DEVOS_W_TEXT_ACCENT, "");
    bar_busy = lv_bar_create(p_busy);
    lv_obj_set_size(bar_busy, 440, 10);
    lv_obj_set_pos(bar_busy, 0, 56);
    lv_bar_set_range(bar_busy, 0, 100);
    devos_w_track(bar_busy, DEVOS_W_PROGRESS);
    lbl_busy_pct = devos_w_label(p_busy, &lv_font_montserrat_14, DEVOS_W_TEXT, "0%");
    lv_obj_set_pos(lbl_busy_pct, 456, 50);
    lv_obj_t *bt = devos_w_label(p_busy, &lv_font_montserrat_14, DEVOS_W_TEXT_DIM,
                                 "Your passphrase is stretched 100 000 times so that guessing it is slow. "
                                 "This takes a few seconds.");
    lv_obj_set_width(bt, 510);
    lv_label_set_long_mode(bt, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(bt, 0, 86);
    lv_obj_add_flag(p_busy, LV_OBJ_FLAG_HIDDEN);

    /* main list */
    int h = DEVOS_CONTENT_HEIGHT - DEVOS_W_BAR_H - DEVOS_W_KEYS_H;
    p_main = lv_obj_create(s_screen);
    lv_obj_remove_style_all(p_main);
    lv_obj_set_pos(p_main, 0, DEVOS_W_BAR_H);
    lv_obj_set_size(p_main, DEVOS_SCREEN_WIDTH, h);
    lv_obj_remove_flag(p_main, LV_OBJ_FLAG_SCROLLABLE);
    devos_vlist_create(&s_list, p_main, ROW_H, row_draw);
    s_list.on_activate = activate_cb;
    s_list.active = true;
    lv_obj_set_pos(s_list.scroll, 120, 8);
    lv_obj_set_size(s_list.scroll, DEVOS_SCREEN_WIDTH - 240, h - 16);
    lbl_empty = devos_w_label(p_main, &lv_font_montserrat_16, DEVOS_W_TEXT_DIM,
                              "No accounts yet. Press A to add one:\nscan its QR code, type its key, or import from the SD card.");
    lv_obj_set_style_text_align(lbl_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_empty, LV_ALIGN_CENTER, 0, 0);

    /* big view */
    big = devos_w_panel(s_screen, 0, DEVOS_W_BAR_H, DEVOS_SCREEN_WIDTH, h, DEVOS_W_SCREEN);
    lbl_big_name = devos_w_label(big, &lv_font_montserrat_22, DEVOS_W_TEXT, "");
    lv_obj_align(lbl_big_name, LV_ALIGN_TOP_MID, 0, 120);
    lbl_big_code = devos_w_label(big, &lv_font_montserrat_48, DEVOS_W_TEXT_ACCENT, "");
    lv_obj_set_style_text_letter_space(lbl_big_code, 6, 0);
    lv_obj_align(lbl_big_code, LV_ALIGN_TOP_MID, 0, 190);
    bar_big = lv_bar_create(big);
    lv_obj_set_size(bar_big, 420, 8);
    lv_obj_align(bar_big, LV_ALIGN_TOP_MID, 0, 280);
    lv_bar_set_range(bar_big, 0, 1000);
    devos_w_track(bar_big, DEVOS_W_PROGRESS);
    lbl_big_next = devos_w_label(big, &lv_font_montserrat_14, DEVOS_W_TEXT_DIM, "");
    lv_obj_align(lbl_big_next, LV_ALIGN_TOP_MID, 0, 304);
    lv_obj_add_flag(big, LV_OBJ_FLAG_HIDDEN);

    s_keys = devos_w_keys(s_screen);

    /* add menu */
    devos_w_dialog(&d_add, s_screen, 540, 330, LV_SYMBOL_PLUS "  Add an account");
    lv_obj_t *b1 = menu_btn(d_add.box, LV_SYMBOL_IMAGE "   Scan its QR code with the camera", 40, add_scan_cb);
    lv_obj_t *b2 = menu_btn(d_add.box, LV_SYMBOL_KEYBOARD "   Type its setup key", 94, add_manual_cb);
    lv_obj_t *b3 = menu_btn(d_add.box, LV_SYMBOL_SD_CARD "   Import /totp/import.txt from the SD card", 148, add_sd_cb);
    lv_obj_t *b4 = menu_btn(d_add.box, "Cancel", 214, add_cancel_cb);
    devos_focus_init(&f_add);
    devos_focus_add(&f_add, b1);
    devos_focus_add(&f_add, b2);
    devos_focus_add(&f_add, b3);
    devos_focus_add(&f_add, b4);

    /* form */
    devos_w_dialog(&d_form, s_screen, 700, 380, "");
    ta_iss = devos_w_field(d_form.box, "Issuer (service)", 0, 34, 320);
    lv_textarea_set_max_length(ta_iss, 47);
    ta_acc = devos_w_field(d_form.box, "Account", 336, 34, 320);
    lv_textarea_set_max_length(ta_acc, 63);
    ta_sec = devos_w_field(d_form.box, "Setup key (base32)", 0, 96, 656);
    lv_textarea_set_max_length(ta_sec, 120);
    lv_obj_t *l1 = devos_w_label(d_form.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "Digits");
    lv_obj_set_pos(l1, 0, 158);
    dd_digits = devos_w_dd(d_form.box, "6\n7\n8", 110);
    lv_obj_set_pos(dd_digits, 0, 176);
    lv_obj_t *l2 = devos_w_label(d_form.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "Period");
    lv_obj_set_pos(l2, 126, 158);
    dd_period = devos_w_dd(d_form.box, "30 s\n60 s", 110);
    lv_obj_set_pos(dd_period, 126, 176);
    lv_obj_t *l3 = devos_w_label(d_form.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "Algorithm");
    lv_obj_set_pos(l3, 252, 158);
    dd_algo = devos_w_dd(d_form.box, "SHA-1\nSHA-256\nSHA-512", 130);
    lv_obj_set_pos(dd_algo, 252, 176);
    lbl_form_hint = devos_w_label(d_form.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM,
                                  "Most sites use 6 digits, 30 s and SHA-1 - change them only if the site says so.");
    lv_obj_set_pos(lbl_form_hint, 0, 226);
    lv_obj_t *fo = dlg_btn(&d_form, DEVOS_W_BTN_PRIMARY, LV_SYMBOL_OK "  Save", 120, 132, form_ok_cb);
    lv_obj_t *fc = dlg_btn(&d_form, DEVOS_W_BTN, "Cancel", 120, 0, form_cancel_cb);
    devos_focus_init(&f_form);
    lv_obj_t *fo_order[] = { ta_iss, ta_acc, ta_sec, dd_digits, dd_period, dd_algo, fo, fc };
    for (unsigned i = 0; i < sizeof(fo_order) / sizeof(fo_order[0]); i++) devos_focus_add(&f_form, fo_order[i]);

    /* delete */
    devos_w_dialog(&d_del, s_screen, 600, 150, "");
    lv_obj_t *dn = devos_w_label(d_del.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "Its codes are gone for good unless you have a backup.");
    lv_obj_set_pos(dn, 0, 32);
    lv_obj_t *dok = dlg_btn(&d_del, DEVOS_W_BTN_DANGER, LV_SYMBOL_TRASH "  Delete", 120, 132, del_ok_cb);
    lv_obj_t *dcl = dlg_btn(&d_del, DEVOS_W_BTN, "Cancel", 120, 0, del_cancel_cb);
    devos_focus_init(&f_del);
    devos_focus_add(&f_del, dcl);                   /* the safe choice first */
    devos_focus_add(&f_del, dok);

    /* backup menu */
    devos_w_dialog(&d_backup, s_screen, 540, 330, LV_SYMBOL_SAVE "  Backup and passphrase");
    lv_obj_t *k1 = menu_btn(d_backup.box, LV_SYMBOL_UPLOAD "   Export an encrypted backup to the SD card", 40, export_cb);
    lv_obj_t *k2 = menu_btn(d_backup.box, LV_SYMBOL_DOWNLOAD "   Import a backup from the SD card", 94, import_bk_cb);
    lv_obj_t *k3 = menu_btn(d_backup.box, LV_SYMBOL_EDIT "   Change the passphrase", 148, change_cb);
    lv_obj_t *k4 = menu_btn(d_backup.box, "Cancel", 214, backup_cancel_cb);
    devos_focus_init(&f_backup);
    devos_focus_add(&f_backup, k1);
    devos_focus_add(&f_backup, k2);
    devos_focus_add(&f_backup, k3);
    devos_focus_add(&f_backup, k4);

    /* passphrase */
    devos_w_dialog(&d_pass, s_screen, 600, 260, "");
    ta_pass1 = devos_w_field(d_pass.box, "Passphrase", 0, 34, 556);
    lv_textarea_set_password_mode(ta_pass1, true);
    lv_textarea_set_max_length(ta_pass1, 120);
    ta_pass2 = devos_w_field(d_pass.box, "Again", 0, 96, 556);
    lv_textarea_set_password_mode(ta_pass2, true);
    lv_textarea_set_max_length(ta_pass2, 120);
    lbl_pass2_cap = lv_obj_get_child(d_pass.box, lv_obj_get_index(ta_pass2) - 1);
    lv_obj_t *po = dlg_btn(&d_pass, DEVOS_W_BTN_PRIMARY, LV_SYMBOL_OK "  OK", 120, 132, pass_ok_cb);
    lv_obj_t *pc = dlg_btn(&d_pass, DEVOS_W_BTN, "Cancel", 120, 0, pass_cancel_cb);
    devos_focus_init(&f_pass);
    devos_focus_add(&f_pass, ta_pass1);
    devos_focus_add(&f_pass, ta_pass2);
    devos_focus_add(&f_pass, po);
    devos_focus_add(&f_pass, pc);

    /* erase */
    devos_w_dialog(&d_erase, s_screen, 620, 250, LV_SYMBOL_WARNING "  Erase the vault?");
    lv_obj_t *en = devos_w_label(d_erase.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM,
                                 "Every account in it is lost - you'll need each site's recovery codes\n"
                                 "(or a backup and its passphrase). Type ERASE to confirm.");
    lv_obj_set_pos(en, 0, 32);
    ta_erase = devos_w_ta(d_erase.box, true, 300, 36);
    lv_obj_set_pos(ta_erase, 0, 84);
    lv_obj_t *eo = dlg_btn(&d_erase, DEVOS_W_BTN_DANGER, LV_SYMBOL_TRASH "  Erase", 120, 132, erase_ok_cb);
    lv_obj_t *ec = dlg_btn(&d_erase, DEVOS_W_BTN, "Cancel", 120, 0, erase_cancel_cb);
    devos_focus_init(&f_erase);
    devos_focus_add(&f_erase, ta_erase);
    devos_focus_add(&f_erase, ec);
    devos_focus_add(&f_erase, eo);

    /* after an SD import */
    devos_w_dialog(&d_sdimp, s_screen, 620, 170, "");
    lv_obj_t *so = dlg_btn(&d_sdimp, DEVOS_W_BTN_DANGER, LV_SYMBOL_TRASH "  Wipe it", 130, 132, sdimp_delete_cb);
    lv_obj_t *sk = dlg_btn(&d_sdimp, DEVOS_W_BTN, "Keep it", 120, 0, sdimp_keep_cb);
    devos_focus_init(&f_sdimp);
    devos_focus_add(&f_sdimp, so);
    devos_focus_add(&f_sdimp, sk);

    devos_qrscan_create(&s_scan, s_screen, LV_SYMBOL_IMAGE "  Scan an authenticator QR code",
                        "Show the site's 2FA setup QR code (or Google Authenticator's Transfer code) to the camera.",
                        scan_result);

    apply_state(devos_totp_state());
    lv_timer_create(tick_cb, 250, NULL);
}

static void totp_show(void)
{
    s_visible = true;
    s_last_key = lv_tick_get();
    if (devos_totp_state() == DEVOS_TOTP_UNLOCKED && s_hidden_at && lv_tick_elaps(s_hidden_at) > AWAY_LOCK_MS) do_lock();
    s_hidden_at = 0;
    s_last_state = (devos_totp_state_t)-1;          /* re-apply focus */
}

static void totp_hide(void)
{
    s_visible = false;
    s_hidden_at = lv_tick_get();
    if (!s_hidden_at) s_hidden_at = 1;
    devos_qrscan_close(&s_scan);
    show_big(false);
}

static int totp_telemetry(char lines[3][64])
{
    switch (devos_totp_state()) {
    case DEVOS_TOTP_NO_VAULT: snprintf(lines[0], 64, "* No vault yet"); snprintf(lines[1], 64, "* Offline 2FA codes"); return 2;
    case DEVOS_TOTP_UNLOCKED: snprintf(lines[0], 64, "* Unlocked: %d account%s", devos_totp_count(), devos_totp_count() == 1 ? "" : "s"); return 1;
    default: snprintf(lines[0], 64, "* Locked"); snprintf(lines[1], 64, "* Offline 2FA codes"); return 2;
    }
}

devos_app_descriptor_t *app_totp_get_descriptor(void)
{
    s_desc.id = DEVOS_APP_LAUNCHER;                 /* auto-assigned */
    s_desc.uid = "totp";
    s_desc.icon = LV_SYMBOL_EYE_CLOSE;
    s_desc.draw_icon = devos_icon_totp;
    s_desc.category = "security";
    s_desc.name = "2FA";
    s_desc.title = "Authenticator";
    s_desc.subtitle = "Offline 2FA codes (TOTP)";
    s_desc.init = totp_init;
    s_desc.show = totp_show;
    s_desc.hide = totp_hide;
    s_desc.handle_key = totp_key;
    s_desc.get_telemetry_lines = totp_telemetry;
    return &s_desc;
}

/* Editor voice memos: see ed_voice.h. */
#include "ed_voice.h"
#include "devos_config.h"
#include "devos_core.h"
#include "devos_focus.h"
#include "devos_theme.h"
#include "devos_widgets.h"
#include "devos_audio.h"
#include "devos_http.h"
#include "devos_json.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#else
#define VOICE_SIM_CFG TAB5_SD_MOUNT_POINT "/.devos/voice.cfg"
#endif

#define MAX_MEMOS     200
#define TX_MAX_BYTES  (DEVOS_AUDIO_RATE * 2 * 180 + 44)    /* 3 minutes */
#define BOUNDARY      "devosVoiceMemo7MA4YWxkTrZu0gW"

typedef struct {
    uint32_t magic;
    char url[200];
    char key[160];
    char model[48];
    uint8_t auto_tx;
    uint8_t gain_db;
    uint8_t volume;
} voice_cfg_t;

typedef struct {
    char name[64];
    uint32_t ms;
    uint32_t bytes;
} memo_t;

static ed_voice_insert_fn s_insert;
static voice_cfg_t s_cfg;
static char s_msg[160];
/* recording */
static char s_rec_dir[200], s_rec_name[64];
static bool s_rec_active, s_rec_closing;
/* playback */
static char s_play_name[64];
/* transcription: one at a time, a few queued */
static int s_tx_job;
static char s_tx_name[64], s_tx_dir[200];
static char s_txq_name[4][64], s_txq_dir[4][200];
static int s_txq_n;
/* memo list */
static devos_w_dialog_t d_list, d_cfg;
static devos_vlist_t s_vl;
static lv_obj_t *lbl_list_empty, *lbl_list_hint;
static memo_t *s_memos;
static int s_nmemo;
static char s_list_dir[200];
static bool s_del_armed;
/* settings */
static lv_obj_t *ta_url, *ta_key, *ta_model, *dd_gain, *dd_vol, *cb_auto;
static devos_focus_t f_cfg;
static const int GAINS[] = { 18, 24, 30, 36 };
static const int VOLS[] = { 25, 50, 70, 85, 100 };

/* ------------------------------------------------------------------ settings */
static void cfg_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.magic = 0x564f4331;
    snprintf(s_cfg.model, sizeof(s_cfg.model), "whisper-1");
    s_cfg.gain_db = 30;
    s_cfg.volume = 70;
}

static void cfg_load(void)
{
    cfg_defaults();
    voice_cfg_t c;
    bool ok = false;
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("voice", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(c);
        ok = nvs_get_blob(h, "cfg", &c, &n) == ESP_OK && n == sizeof(c);
        nvs_close(h);
    }
#else
    FILE *f = fopen(VOICE_SIM_CFG, "rb");
    if (f) {
        ok = fread(&c, 1, sizeof(c), f) == sizeof(c);
        fclose(f);
    }
#endif
    if (ok && c.magic == s_cfg.magic) s_cfg = c;
    devos_audio_set_gain((float)s_cfg.gain_db);
    devos_audio_set_volume(s_cfg.volume);
}

static void cfg_save(void)
{
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("voice", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "cfg", &s_cfg, sizeof(s_cfg));
        nvs_commit(h);
        nvs_close(h);
    }
#else
    FILE *f = fopen(VOICE_SIM_CFG, "wb");
    if (f) {
        fwrite(&s_cfg, 1, sizeof(s_cfg), f);
        fclose(f);
    }
#endif
}

/* ------------------------------------------------------------------ helpers */
static void fmt_len(uint32_t ms, char *out, size_t cap)
{
    uint32_t s = (ms + 500) / 1000;
    if (s >= 3600) snprintf(out, cap, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    else snprintf(out, cap, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

static void set_msg(const char *m)
{
    snprintf(s_msg, sizeof(s_msg), "%s", m);
}

const char *ed_voice_take_message(void)
{
    static char out[160];
    snprintf(out, sizeof(out), "%s", s_msg);
    s_msg[0] = '\0';
    return out;
}

/* ------------------------------------------------------------------ transcription */
static bool tx_submit(const char *dir, const char *name)
{
    if (!s_cfg.url[0]) {
        set_msg("Set a transcription URL first (Ctrl+M, then S)");
        return false;
    }
    if (s_tx_job) {                                 /* busy: queue it */
        if (s_txq_n < 4) {
            snprintf(s_txq_name[s_txq_n], sizeof(s_txq_name[0]), "%s", name);
            snprintf(s_txq_dir[s_txq_n], sizeof(s_txq_dir[0]), "%s", dir);
            s_txq_n++;
        }
        return true;
    }
    char path[280];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_msg("Couldn't read the memo");
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 44 || n > TX_MAX_BYTES) {
        fclose(f);
        set_msg(n > TX_MAX_BYTES ? "Memos over 3 minutes are too long to transcribe from here" : "That memo is empty");
        return false;
    }
    char pre[512], post[64];
    int pl = snprintf(pre, sizeof(pre),
                      "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n"
                      "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n"
                      "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                      "Content-Type: audio/wav\r\n\r\n",
                      s_cfg.model[0] ? s_cfg.model : "whisper-1", name);
    int ql = snprintf(post, sizeof(post), "\r\n--" BOUNDARY "--\r\n");
    size_t total = (size_t)pl + (size_t)n + (size_t)ql;
    char *body = malloc(total);
    if (!body) {
        fclose(f);
        set_msg("Out of memory for the upload");
        return false;
    }
    memcpy(body, pre, (size_t)pl);
    size_t got = fread(body + pl, 1, (size_t)n, f);
    fclose(f);
    memcpy(body + pl + got, post, (size_t)ql);
    char hdrs[320];
    int hl = snprintf(hdrs, sizeof(hdrs), "Content-Type: multipart/form-data; boundary=" BOUNDARY);
    if (s_cfg.key[0]) snprintf(hdrs + hl, sizeof(hdrs) - (size_t)hl, "\nAuthorization: Bearer %s", s_cfg.key);
    devos_http_req_t q = { .method = "POST", .url = s_cfg.url, .headers = hdrs, .body = body,
                           .body_len = (size_t)pl + got + (size_t)ql, .timeout_ms = 60000, .max_body = 64 * 1024,
                           .max_redirects = 2 };
    s_tx_job = devos_http_submit(&q);
    free(body);
    if (s_tx_job <= 0) {
        s_tx_job = 0;
        set_msg("Couldn't queue the upload");
        return false;
    }
    snprintf(s_tx_name, sizeof(s_tx_name), "%s", name);
    snprintf(s_tx_dir, sizeof(s_tx_dir), "%s", dir);
    return true;
}

static void tx_poll(void)
{
    if (!s_tx_job) {
        if (s_txq_n) {
            char name[64], dir[200];
            snprintf(name, sizeof(name), "%s", s_txq_name[0]);
            snprintf(dir, sizeof(dir), "%s", s_txq_dir[0]);
            memmove(s_txq_name, s_txq_name + 1, sizeof(s_txq_name[0]) * (size_t)(s_txq_n - 1));
            memmove(s_txq_dir, s_txq_dir + 1, sizeof(s_txq_dir[0]) * (size_t)(s_txq_n - 1));
            s_txq_n--;
            tx_submit(dir, name);
        }
        return;
    }
    devos_http_resp_t r;
    int st = devos_http_poll(s_tx_job, &r);
    if (st == 0) return;
    s_tx_job = 0;
    if (st < 0) return;
    char *text = malloc(8192);
    if (r.status == 200 && r.body && text && devos_json_get_str(r.body, r.body_len, "text", text, 8192) == 0) {
        for (char *p = text; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
        char *t = text;
        while (*t == ' ') t++;
        size_t l = strlen(t);
        char *line = malloc(l + 8);
        if (line && s_insert) {
            snprintf(line, l + 8, "  > %s", l ? t : "(no speech found)");
            s_insert(s_tx_name, line);
        }
        free(line);
        set_msg("Transcript added");
    } else {
        char m[96] = "";
        if (r.body) devos_json_get_str(r.body, r.body_len, "message", m, sizeof(m));
        char out[160];
        if (!r.status) snprintf(out, sizeof(out), "Transcription failed: %s", r.error);
        else snprintf(out, sizeof(out), "Transcription failed: HTTP %d %s", r.status, m[0] ? m : r.reason);
        set_msg(out);
    }
    free(text);
    devos_http_resp_free(&r);
}

/* ------------------------------------------------------------------ record / play */
bool ed_voice_record_start(const char *memo_dir, char *msg, size_t cap)
{
    if (s_rec_active || s_rec_closing) return false;
    if (devos_audio_playing()) devos_audio_play_stop();
    mkdir(memo_dir, 0755);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char name[64];
    strftime(name, sizeof(name), "memo-%Y%m%d-%H%M%S.wav", &tm);
    char path[280];
    snprintf(path, sizeof(path), "%s/%s", memo_dir, name);
    if (devos_audio_record_start(path) != 0) {
        snprintf(msg, cap, "Can't record: %s", devos_audio_error()[0] ? devos_audio_error() : "busy");
        return false;
    }
    snprintf(s_rec_dir, sizeof(s_rec_dir), "%s", memo_dir);
    snprintf(s_rec_name, sizeof(s_rec_name), "%s", name);
    s_rec_active = true;
    snprintf(msg, cap, "Recording - Ctrl+R stops");
    return true;
}

void ed_voice_record_stop(void)
{
    if (!s_rec_active) return;
    devos_audio_record_stop();
    s_rec_active = false;
    s_rec_closing = true;                           /* the link goes in once the file is closed */
}

bool ed_voice_recording(void) { return s_rec_active || s_rec_closing; }

bool ed_voice_play(const char *abs_path, char *msg, size_t cap)
{
    if (s_rec_active) {
        snprintf(msg, cap, "Stop recording first (Ctrl+R)");
        return false;
    }
    if (devos_audio_playing()) devos_audio_play_stop();
    for (int i = 0; i < 50 && devos_audio_playing(); i++) {
#ifdef ESP_PLATFORM
        vTaskDelay(pdMS_TO_TICKS(10));
#else
        struct timespec ts = { 0, 10000000 };
        nanosleep(&ts, NULL);
#endif
    }
    if (devos_audio_play(abs_path) != 0) {
        snprintf(msg, cap, "Can't play it: %s", devos_audio_error()[0] ? devos_audio_error() : "busy");
        return false;
    }
    const char *b = strrchr(abs_path, '/');
    snprintf(s_play_name, sizeof(s_play_name), "%s", b ? b + 1 : abs_path);
    snprintf(msg, cap, "Playing %s", s_play_name);
    return true;
}

void ed_voice_stop(void) { devos_audio_play_stop(); }
bool ed_voice_playing(void) { return devos_audio_playing(); }

void ed_voice_status(char *out, size_t cap)
{
    out[0] = '\0';
    char a[16], b[16];
    if (s_rec_active) {
        fmt_len(devos_audio_record_ms(), a, sizeof(a));
        int bars = (int)(devos_audio_level() * 10 + 0.5f);
        char meter[12];
        for (int i = 0; i < 10; i++) meter[i] = i < bars ? '|' : '.';
        meter[10] = '\0';
        snprintf(out, cap, LV_SYMBOL_AUDIO " REC %s  %s", a, meter);
    } else if (s_rec_closing) {
        snprintf(out, cap, LV_SYMBOL_AUDIO " saving memo...");
    } else if (devos_audio_playing()) {
        uint32_t pos, len;
        devos_audio_play_pos(&pos, &len);
        fmt_len(pos, a, sizeof(a));
        fmt_len(len, b, sizeof(b));
        snprintf(out, cap, LV_SYMBOL_PLAY " %s / %s", a, b);
    } else if (s_tx_job || s_txq_n) {
        snprintf(out, cap, LV_SYMBOL_UPLOAD " transcribing%s", s_txq_n ? "..." : "");
    }
}

/* ------------------------------------------------------------------ memo list */
static int memo_cmp(const void *a, const void *b) { return strcmp(((const memo_t *)b)->name, ((const memo_t *)a)->name); }

static void list_load(void)
{
    s_nmemo = 0;
    DIR *d = opendir(s_list_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && s_nmemo < MAX_MEMOS) {
            size_t l = strlen(e->d_name);
            if (l < 5 || strcasecmp(e->d_name + l - 4, ".wav") || l >= sizeof(s_memos[0].name)) continue;
            memo_t *m = &s_memos[s_nmemo++];
            snprintf(m->name, sizeof(m->name), "%s", e->d_name);
            char p[280];
            snprintf(p, sizeof(p), "%s/%s", s_list_dir, e->d_name);
            struct stat st;
            m->bytes = stat(p, &st) == 0 ? (uint32_t)st.st_size : 0;
            m->ms = devos_audio_wav_ms(p);
        }
        closedir(d);
    }
    qsort(s_memos, (size_t)s_nmemo, sizeof(memo_t), memo_cmp);
    devos_vlist_set_count(&s_vl, s_nmemo);
    if (s_vl.sel < 0 && s_nmemo) devos_vlist_select(&s_vl, 0);
    if (s_nmemo) lv_obj_add_flag(lbl_list_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(lbl_list_empty, LV_OBJ_FLAG_HIDDEN);
}

static void row_draw(devos_vlist_t *v, lv_layer_t *layer, const lv_area_t *row, int idx)
{
    LV_UNUSED(v);
    if (idx >= s_nmemo) return;
    const devos_palette_t *p = devos_theme_get();
    const memo_t *m = &s_memos[idx];
    char when[40], len[16];
    int y, mo, d, h, mi, s;
    if (sscanf(m->name, "memo-%4d%2d%2d-%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) == 6)
        snprintf(when, sizeof(when), "%04d-%02d-%02d  %02d:%02d:%02d", y, mo, d, h, mi, s);
    else snprintf(when, sizeof(when), "%s", m->name);
    fmt_len(m->ms, len, sizeof(len));
    bool playing = devos_audio_playing() && !strcmp(s_play_name, m->name);
    if (playing) devos_w_draw_text(layer, &lv_font_montserrat_12, row->x1 + 12, row->y1 + 4, 0, LV_SYMBOL_PLAY, p->accent_secondary);
    devos_w_draw_text(layer, NULL, row->x1 + 34, row->y1 + 3, 260, when, p->text_primary);
    devos_w_draw_text(layer, NULL, row->x1 + 290, row->y1 + 3, 80, len, p->accent_primary);
    devos_w_draw_text(layer, NULL, row->x1 + 370, row->y1 + 3, row->x2 - row->x1 - 380, m->name, p->text_muted);
}

void ed_voice_open_list(const char *memo_dir)
{
    snprintf(s_list_dir, sizeof(s_list_dir), "%s", memo_dir);
    s_vl.sel = -1;
    s_del_armed = false;
    char t[240];
    const char *root = strstr(memo_dir, TAB5_SD_MOUNT_POINT);
    snprintf(t, sizeof(t), LV_SYMBOL_AUDIO "  Voice memos  -  %s", root ? memo_dir + strlen(TAB5_SD_MOUNT_POINT) : memo_dir);
    devos_w_set_text(d_list.title, t);
    devos_w_set_text(d_list.msg, "");
    list_load();
    devos_w_dialog_show(&d_list, true);
}

bool ed_voice_list_open(void) { return devos_w_dialog_open(&d_list) || devos_w_dialog_open(&d_cfg); }

static void cfg_open(void)
{
    lv_textarea_set_text(ta_url, s_cfg.url);
    lv_textarea_set_text(ta_key, s_cfg.key);
    lv_textarea_set_text(ta_model, s_cfg.model);
    int gi = 2, vi = 2;
    for (int i = 0; i < 4; i++) if (GAINS[i] == s_cfg.gain_db) gi = i;
    for (int i = 0; i < 5; i++) if (VOLS[i] == s_cfg.volume) vi = i;
    lv_dropdown_set_selected(dd_gain, (uint32_t)gi);
    lv_dropdown_set_selected(dd_vol, (uint32_t)vi);
    if (s_cfg.auto_tx) lv_obj_add_state(cb_auto, LV_STATE_CHECKED);
    else lv_obj_remove_state(cb_auto, LV_STATE_CHECKED);
    devos_w_set_text(d_cfg.msg, "");
    devos_w_dialog_show(&d_cfg, true);
    devos_focus_set(&f_cfg, dd_gain);
}

static void cfg_ok(void)
{
    snprintf(s_cfg.url, sizeof(s_cfg.url), "%s", lv_textarea_get_text(ta_url));
    snprintf(s_cfg.key, sizeof(s_cfg.key), "%s", lv_textarea_get_text(ta_key));
    snprintf(s_cfg.model, sizeof(s_cfg.model), "%s", lv_textarea_get_text(ta_model));
    s_cfg.gain_db = (uint8_t)GAINS[lv_dropdown_get_selected(dd_gain)];
    s_cfg.volume = (uint8_t)VOLS[lv_dropdown_get_selected(dd_vol)];
    s_cfg.auto_tx = lv_obj_has_state(cb_auto, LV_STATE_CHECKED);
    if (s_cfg.auto_tx && !s_cfg.url[0]) {
        devos_w_set_text(d_cfg.msg, "Automatic transcription needs a URL");
        return;
    }
    devos_audio_set_gain((float)s_cfg.gain_db);
    devos_audio_set_volume(s_cfg.volume);
    cfg_save();
    devos_w_dialog_show(&d_cfg, false);
    devos_focus_clear(&f_cfg);
    devos_w_set_text(d_list.msg, "Settings saved");
}

static void cfg_ok_cb(lv_event_t *e) { LV_UNUSED(e); cfg_ok(); }
static void cfg_cancel_cb(lv_event_t *e) { LV_UNUSED(e); devos_w_dialog_show(&d_cfg, false); devos_focus_clear(&f_cfg); }

static void list_play(int i)
{
    if (i < 0 || i >= s_nmemo) return;
    char msg[160], p[280];
    if (devos_audio_playing() && !strcmp(s_play_name, s_memos[i].name)) {
        devos_audio_play_stop();
        devos_w_set_text(d_list.msg, "Stopped");
        return;
    }
    snprintf(p, sizeof(p), "%s/%s", s_list_dir, s_memos[i].name);
    ed_voice_play(p, msg, sizeof(msg));
    devos_w_set_text(d_list.msg, msg);
}

static void list_activate(devos_vlist_t *v, int idx)
{
    LV_UNUSED(v);
    list_play(idx);
}

bool ed_voice_key(uint32_t key, uint8_t mods)
{
    if (devos_w_dialog_open(&d_cfg)) {
        if (key == LV_KEY_ESC && !f_cfg.dd_open) cfg_cancel_cb(NULL);
        else if (devos_focus_key(&f_cfg, key, mods)) {}
        else if (key == '\r' || key == '\n') cfg_ok();
        return true;
    }
    if (!devos_w_dialog_open(&d_list)) return false;
    int i = s_vl.sel;
    if (key != LV_KEY_DEL) s_del_armed = false;
    if (key == LV_KEY_ESC) {
        devos_w_dialog_show(&d_list, false);
        return true;
    }
    if (devos_vlist_key(&s_vl, key)) return true;
    char msg[160];
    switch (key) {
    case ' ': list_play(i); break;
    case 't': case 'T':
        if (i >= 0 && i < s_nmemo) {
            if (tx_submit(s_list_dir, s_memos[i].name)) devos_w_set_text(d_list.msg, "Transcribing - the text goes under its link");
            else devos_w_set_text(d_list.msg, ed_voice_take_message());
        }
        break;
    case 'i': case 'I':
        if (i >= 0 && i < s_nmemo && s_insert) {
            char len[16], line[160];
            fmt_len(s_memos[i].ms, len, sizeof(len));
            snprintf(line, sizeof(line), "[voice memo %s](memos/%s)", len, s_memos[i].name);
            s_insert(NULL, line);
            devos_w_dialog_show(&d_list, false);
        }
        break;
    case LV_KEY_DEL:                                    /* 127 */
        if (i < 0 || i >= s_nmemo) break;
        if (!s_del_armed) {
            s_del_armed = true;
            snprintf(msg, sizeof(msg), "Press Del again to delete %s", s_memos[i].name);
            devos_w_set_text(d_list.msg, msg);
        } else {
            char p[280];
            snprintf(p, sizeof(p), "%s/%s", s_list_dir, s_memos[i].name);
            if (devos_audio_playing() && !strcmp(s_play_name, s_memos[i].name)) devos_audio_play_stop();
            bool ok = remove(p) == 0;
            s_del_armed = false;
            devos_w_set_text(d_list.msg, ok ? "Deleted (its link in the note stays)" : "Couldn't delete it");
            list_load();
        }
        break;
    case 's': case 'S': cfg_open(); break;
    default: break;
    }
    return true;
}

const char *ed_voice_keys(void)
{
    if (devos_w_dialog_open(&d_cfg)) return "Tab / arrows move    Left / Right change    Space ticks    Enter saves    Esc cancels";
    return "Up / Down pick    Enter / Space play or stop    T transcribe    I insert link    Del delete    S settings    Esc close";
}

/* ------------------------------------------------------------------ tick + init */
void ed_voice_tick(void)
{
    if (s_rec_closing && !devos_audio_recording()) {
        s_rec_closing = false;
        uint32_t ms = devos_audio_record_ms();
        char len[16], line[160];
        fmt_len(ms, len, sizeof(len));
        if (ms < 300) {
            char p[280];
            snprintf(p, sizeof(p), "%s/%s", s_rec_dir, s_rec_name);
            remove(p);                              /* a stray tap: drop it */
            set_msg(devos_audio_error()[0] ? devos_audio_error() : "Too short - not kept");
        } else {
            snprintf(line, sizeof(line), "[voice memo %s](memos/%s)", len, s_rec_name);
            if (s_insert) s_insert(NULL, line);
            if (s_cfg.auto_tx && s_cfg.url[0]) {
                if (tx_submit(s_rec_dir, s_rec_name)) set_msg("Memo saved - transcribing");
            } else {
                set_msg("Memo saved - Ctrl+L plays it");
            }
        }
        if (devos_w_dialog_open(&d_list)) list_load();
    }
    tx_poll();
    if (devos_w_dialog_open(&d_list)) devos_vlist_redraw(&s_vl);
}

void ed_voice_init(lv_obj_t *screen, ed_voice_insert_fn insert)
{
    s_insert = insert;
    cfg_load();
    s_memos = calloc(MAX_MEMOS, sizeof(memo_t));

    devos_w_dialog(&d_list, screen, 820, 520, LV_SYMBOL_AUDIO "  Voice memos");
    devos_vlist_create(&s_vl, d_list.box, 24, row_draw);
    s_vl.on_activate = list_activate;
    s_vl.active = true;
    lv_obj_set_pos(s_vl.scroll, 0, 34);
    lv_obj_set_size(s_vl.scroll, 776, 370);
    lbl_list_empty = devos_w_label(d_list.box, &lv_font_montserrat_14, DEVOS_W_TEXT_DIM,
                                   "No memos here yet. Ctrl+R in a note records one.");
    lv_obj_align(lbl_list_empty, LV_ALIGN_TOP_MID, 0, 140);
    lbl_list_hint = devos_w_label(d_list.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM,
                                  "Enter plays    T transcribe    I insert its link    Del delete    S settings");
    lv_obj_align(lbl_list_hint, LV_ALIGN_BOTTOM_RIGHT, 0, -4);
    lv_obj_set_width(d_list.msg, 400);

    devos_w_dialog(&d_cfg, screen, 740, 460, LV_SYMBOL_SETTINGS "  Voice memo settings");
    lv_obj_t *l = devos_w_label(d_cfg.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "Microphone gain");
    lv_obj_set_pos(l, 0, 34);
    dd_gain = devos_w_dd(d_cfg.box, "18 dB (loud room)\n24 dB\n30 dB (normal)\n36 dB (quiet voice)", 220);
    lv_obj_set_pos(dd_gain, 0, 52);
    l = devos_w_label(d_cfg.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "Speaker volume");
    lv_obj_set_pos(l, 236, 34);
    dd_vol = devos_w_dd(d_cfg.box, "25 %\n50 %\n70 %\n85 %\n100 %", 130);
    lv_obj_set_pos(dd_vol, 236, 52);
    ta_url = devos_w_field(d_cfg.box, "Transcription URL (OpenAI-compatible; blank = off)", 0, 102, 696);
    lv_textarea_set_placeholder_text(ta_url, "https://api.openai.com/v1/audio/transcriptions");
    lv_textarea_set_max_length(ta_url, sizeof(s_cfg.url) - 1);
    ta_key = devos_w_field(d_cfg.box, "API key (blank if your server needs none)", 0, 164, 460);
    lv_textarea_set_password_mode(ta_key, true);
    lv_textarea_set_max_length(ta_key, sizeof(s_cfg.key) - 1);
    ta_model = devos_w_field(d_cfg.box, "Model", 476, 164, 220);
    lv_textarea_set_max_length(ta_model, sizeof(s_cfg.model) - 1);
    cb_auto = devos_w_cb(d_cfg.box, "Transcribe new memos automatically");
    lv_obj_set_pos(cb_auto, 0, 232);
    lv_obj_t *hint = devos_w_label(d_cfg.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM,
                                   "Memos are sent there only when transcribed - e.g. OpenAI (whisper-1), or your own\n"
                                   "faster-whisper / whisper.cpp server on the LAN or tailnet (http://host:8000/v1/audio/transcriptions).");
    lv_obj_set_pos(hint, 0, 272);
    lv_obj_t *bok = devos_w_btn_kind(d_cfg.box, DEVOS_W_BTN_PRIMARY, LV_SYMBOL_OK "  Save", 120, cfg_ok_cb, NULL, NULL);
    lv_obj_set_size(bok, 120, 36);
    lv_obj_align(bok, LV_ALIGN_BOTTOM_RIGHT, -132, 0);
    lv_obj_t *bc = devos_w_btn(d_cfg.box, "Cancel", 120, cfg_cancel_cb, NULL, NULL);
    lv_obj_set_size(bc, 120, 36);
    lv_obj_align(bc, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    devos_focus_init(&f_cfg);
    lv_obj_t *order[] = { dd_gain, dd_vol, ta_url, ta_key, ta_model, cb_auto, bok, bc };
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) devos_focus_add(&f_cfg, order[i]);
}

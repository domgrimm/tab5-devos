/* REST: a small Postman for the keyboard - craft HTTP(S) requests, fire
 * webhooks (Home Assistant, CI pipelines, ntfy ...), read the response.
 *
 * Top: method, URL, Send. Then the headers ("Name: value" per line) and the
 * body, with options (follow redirects, skip the certificate check, body
 * type - which sets Content-Type unless you do). Below: the response status,
 * timings, and the body pretty-printed (JSON highlighted) or the headers.
 *
 * Saved requests live in /rest/requests.json (Sym+L shows them; Enter loads,
 * Del deletes). {{NAME}} anywhere is replaced from the variables (Variables
 * button, kept in NVS - put tokens there, not in the SD file) or the
 * built-ins {{ts}} {{time}} {{uuid}} {{battery}}.
 *
 * Keys: Ctrl+Enter sends from anywhere, Ctrl+S saves, Ctrl+N starts a new
 * request, Ctrl+K edits the variables, Alt+H flips the response between body and headers, Esc cancels a
 * request in flight, then leaves the panel, then goes Home.
 */
#include "app_rest.h"
#include "devos_config.h"
#include "devos_icons.h"
#include "devos_core.h"
#include "devos_focus.h"
#include "devos_theme.h"
#include "devos_widgets.h"
#include "devos_codeview.h"
#include "devos_http.h"
#include "devos_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_random.h"
#include "nvs.h"
#endif

#define REQ_DIR    TAB5_SD_MOUNT_POINT "/rest"
#define REQ_FILE   REQ_DIR "/requests.json"
#define VARS_SIM   TAB5_SD_MOUNT_POINT "/.devos/rest_vars.txt"
#define MAX_REQ    32
#define URL_MAX    1024
#define HDR_MAX    2048
#define BODY_MAX   8192
#define VARS_MAX   4096
#define VIEW_MAX   (512 * 1024)
#define SIDE_W     DEVOS_PANE_LEFT_WIDTH

typedef struct {
    char name[48];
    char method[8];
    char url[URL_MAX];
    char headers[HDR_MAX];
    char body[BODY_MAX];
    int btype;                          /* index into BTYPES */
    bool redirects, insecure;
} req_t;

static const char *METHODS[] = { "GET", "POST", "PUT", "PATCH", "DELETE", "HEAD" };
static const char *BTYPES[] = { "JSON", "Text", "Form", "Raw" };
static const char *CTYPES[] = { "application/json", "text/plain; charset=utf-8", "application/x-www-form-urlencoded", NULL };

static devos_app_descriptor_t s_desc;
static lv_obj_t *s_screen, *s_bar, *lbl_status, *btn_saved, *btn_vars, *btn_save, *btn_new;
static lv_obj_t *side, *lbl_side_hdr, *lbl_side_empty;
static devos_vlist_t s_side;
static lv_obj_t *dd_method, *ta_url, *btn_send, *lbl_send, *cap_hdr, *cap_body, *ta_headers, *ta_body;
static lv_obj_t *cb_redir, *cb_insecure, *cap_btype, *dd_btype;
static lv_obj_t *resp, *lbl_code, *lbl_meta, *btn_rbody, *btn_rhdrs, *resp_scroll, *lbl_resp_empty;
static lv_obj_t *s_keys;
static devos_codeview_t s_cv;
static devos_focus_t s_f;
static devos_w_dialog_t s_dlg_save, s_dlg_vars, s_dlg_del;
static lv_obj_t *ta_save_name, *ta_vars;
static devos_focus_t s_fsave, s_fvars, s_fdel;

static EXT_RAM_BSS_ATTR req_t s_reqs[MAX_REQ];
static int s_nreq;
static int s_loaded = -1;                   /* s_reqs index of the loaded request */
static bool s_side_open = true, s_side_focus;
static EXT_RAM_BSS_ATTR char s_vars[VARS_MAX];
static int s_job;
static uint32_t s_sent_at;
static devos_http_resp_t s_resp;
static bool s_have_resp, s_show_headers;
static char *s_view;                        /* what the code view shows (PSRAM) */
static uint32_t s_flash_until;
static bool s_inited;

static void layout(void);
static void refresh_status(void);

/* ------------------------------------------------------------------ helpers */
static void flash(const char *msg)
{
    devos_w_set_text(lbl_status, msg);
    s_flash_until = lv_tick_get() + 3000;
}

static int method_index(const char *m)
{
    for (int i = 0; i < (int)(sizeof(METHODS) / sizeof(METHODS[0])); i++) if (!strcasecmp(m, METHODS[i])) return i;
    return 0;
}

static lv_color_t method_color(const char *m)
{
    const devos_palette_t *p = devos_theme_get();
    if (!strcasecmp(m, "GET")) return p->accent_secondary;
    if (!strcasecmp(m, "POST")) return p->accent_warning;
    if (!strcasecmp(m, "DELETE")) return p->accent_danger;
    return p->accent_primary;
}

/* ------------------------------------------------------------------ variables */
static void vars_load(void)
{
    s_vars[0] = '\0';
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("rest", NVS_READONLY, &h) == ESP_OK) {
        size_t l = sizeof(s_vars);
        if (nvs_get_str(h, "vars", s_vars, &l) != ESP_OK) s_vars[0] = '\0';
        nvs_close(h);
    }
#else
    FILE *f = fopen(VARS_SIM, "rb");
    if (f) {
        size_t n = fread(s_vars, 1, sizeof(s_vars) - 1, f);
        s_vars[n] = '\0';
        fclose(f);
    }
#endif
}

static bool vars_save(const char *text)
{
    snprintf(s_vars, sizeof(s_vars), "%s", text);
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open("rest", NVS_READWRITE, &h) == ESP_OK) {
        ok = nvs_set_str(h, "vars", s_vars) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    return ok;
#else
    FILE *f = fopen(VARS_SIM, "wb");
    if (!f) return false;
    fputs(s_vars, f);
    fclose(f);
    return true;
#endif
}

static bool var_lookup(const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    for (const char *p = s_vars; *p;) {
        const char *eol = strchr(p, '\n');
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        while (ll && *p == ' ') { p++; ll--; }
        if (ll > nl && !strncmp(p, name, nl) && (p[nl] == '=' || p[nl] == ' ')) {
            const char *v = p + nl;
            while (*v == ' ' || *v == '=') { if (*v == '=') { v++; break; } v++; }
            while (*v == ' ') v++;
            size_t vl = (size_t)(p + ll - v);
            while (vl && (v[vl - 1] == '\r' || v[vl - 1] == ' ')) vl--;
            if (vl >= cap) vl = cap - 1;
            memcpy(out, v, vl);
            out[vl] = '\0';
            return true;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return false;
}

/* Expand {{NAME}}; unknown names are left as they are and reported. */
static size_t expand(const char *in, char *out, size_t cap, char *missing, size_t mcap)
{
    size_t o = 0;
    while (*in && o + 1 < cap) {
        if (in[0] == '{' && in[1] == '{') {
            const char *end = strstr(in + 2, "}}");
            if (end && end - in - 2 < 48) {
                char key[48], val[512] = "";
                size_t kl = (size_t)(end - in - 2);
                memcpy(key, in + 2, kl);
                key[kl] = '\0';
                char *k = key;
                while (*k == ' ') k++;
                size_t l = strlen(k);
                while (l && k[l - 1] == ' ') k[--l] = '\0';
                bool ok = true;
                time_t now = time(NULL);
                if (!strcmp(k, "ts")) snprintf(val, sizeof(val), "%lld", (long long)now);
                else if (!strcmp(k, "time")) {
                    struct tm tm;
                    gmtime_r(&now, &tm);
                    strftime(val, sizeof(val), "%Y-%m-%dT%H:%M:%SZ", &tm);
                } else if (!strcmp(k, "uuid")) {
                    uint8_t b[16];
                    for (int i = 0; i < 16; i++) {
#ifdef ESP_PLATFORM
                        b[i] = (uint8_t)esp_random();
#else
                        b[i] = (uint8_t)rand();
#endif
                    }
                    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
                    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
                    snprintf(val, sizeof(val), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
                } else if (!strcmp(k, "battery")) {
                    snprintf(val, sizeof(val), "%d", devos_telemetry_get()->battery_percent);
                } else ok = var_lookup(k, val, sizeof(val));
                if (ok) {
                    for (const char *v = val; *v && o + 1 < cap; v++) out[o++] = *v;
                    in = end + 2;
                    continue;
                }
                if (missing && !missing[0]) snprintf(missing, mcap, "%s", k);
            }
        }
        out[o++] = *in++;
    }
    out[o] = '\0';
    return o;
}

/* ------------------------------------------------------------------ collection */
static void req_defaults(void)
{
    static const struct { const char *name, *method, *url, *headers, *body; int btype; } d[] = {
        { "httpbin: GET", "GET", "https://httpbin.org/get", "", "", 0 },
        { "httpbin: POST JSON", "POST", "https://httpbin.org/post", "", "{\n  \"hello\": \"from the Tab5\",\n  \"ts\": {{ts}}\n}", 0 },
        { "Home Assistant: toggle a light", "POST", "http://homeassistant.local:8123/api/services/light/toggle",
          "Authorization: Bearer {{HA_TOKEN}}", "{\"entity_id\": \"light.desk\"}", 0 },
        { "Home Assistant: webhook", "POST", "http://homeassistant.local:8123/api/webhook/{{HA_WEBHOOK_ID}}", "",
          "{\"source\": \"tab5\", \"battery\": {{battery}}}", 0 },
        { "GitHub: run a workflow", "POST",
          "https://api.github.com/repos/{{GH_REPO}}/actions/workflows/{{GH_WORKFLOW}}/dispatches",
          "Authorization: Bearer {{GH_TOKEN}}\nAccept: application/vnd.github+json\nX-GitHub-Api-Version: 2022-11-28",
          "{\"ref\": \"main\"}", 0 },
        { "GitLab: trigger a pipeline", "POST", "https://gitlab.com/api/v4/projects/{{GL_PROJECT_ID}}/trigger/pipeline", "",
          "token={{GL_TRIGGER_TOKEN}}&ref=main", 2 },
        { "ntfy: push a notification", "POST", "https://ntfy.sh/{{NTFY_TOPIC}}", "Title: devOS", "Hello from the Tab5 ({{time}})", 1 },
    };
    s_nreq = 0;
    for (unsigned i = 0; i < sizeof(d) / sizeof(d[0]) && s_nreq < MAX_REQ; i++) {
        req_t *r = &s_reqs[s_nreq++];
        memset(r, 0, sizeof(*r));
        snprintf(r->name, sizeof(r->name), "%s", d[i].name);
        snprintf(r->method, sizeof(r->method), "%s", d[i].method);
        snprintf(r->url, sizeof(r->url), "%s", d[i].url);
        snprintf(r->headers, sizeof(r->headers), "%s", d[i].headers);
        snprintf(r->body, sizeof(r->body), "%s", d[i].body);
        r->btype = d[i].btype;
        r->redirects = true;
    }
}

static void req_parse_one(const char *e, size_t len, void *ud)
{
    (void)ud;
    if (s_nreq >= MAX_REQ || !len || *e != '{') return;
    req_t *r = &s_reqs[s_nreq];
    memset(r, 0, sizeof(*r));
    int v;
    devos_json_get_str(e, len, "name", r->name, sizeof(r->name));
    devos_json_get_str(e, len, "method", r->method, sizeof(r->method));
    devos_json_get_str(e, len, "url", r->url, sizeof(r->url));
    devos_json_get_str(e, len, "headers", r->headers, sizeof(r->headers));
    devos_json_get_str(e, len, "body", r->body, sizeof(r->body));
    char bt[16] = "";
    devos_json_get_str(e, len, "body_type", bt, sizeof(bt));
    for (int i = 0; i < 4; i++) if (!strcasecmp(bt, BTYPES[i])) r->btype = i;
    r->redirects = devos_json_get_int(e, len, "follow_redirects", &v) != 0 || v;
    r->insecure = devos_json_get_int(e, len, "insecure", &v) == 0 && v;
    if (!r->url[0]) return;
    if (!r->method[0]) snprintf(r->method, sizeof(r->method), "GET");
    if (!r->name[0]) {                          /* same struct: copy, don't snprintf */
        size_t n = strlen(r->url);
        if (n > sizeof(r->name) - 1) n = sizeof(r->name) - 1;
        memcpy(r->name, r->url, n);
        r->name[n] = '\0';
    }
    s_nreq++;
}

static void req_load_all(void)
{
    s_nreq = 0;
    FILE *f = fopen(REQ_FILE, "rb");
    if (!f) {
        req_defaults();
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t cap = sz > 0 && sz < 1024 * 1024 ? (size_t)sz + 1 : 1;
    char *buf = malloc(cap);
    if (buf) {
        size_t n = fread(buf, 1, cap - 1, f);
        buf[n] = '\0';
        const char *a = strchr(buf, '[');
        if (a) devos_json_array_each(a, n - (size_t)(a - buf), req_parse_one, NULL);
        free(buf);
    }
    fclose(f);
}

static bool req_save_all(void)
{
    mkdir(REQ_DIR, 0755);
    FILE *f = fopen(REQ_FILE, "wb");
    if (!f) return false;
    char *esc = malloc(BODY_MAX * 6 + 16);
    if (!esc) {
        fclose(f);
        return false;
    }
    fprintf(f, "[\n");
    for (int i = 0; i < s_nreq; i++) {
        const req_t *r = &s_reqs[i];
        devos_json_escape(r->name, esc, BODY_MAX * 6);
        fprintf(f, "  {\"name\": \"%s\", \"method\": \"%s\", ", esc, r->method);
        devos_json_escape(r->url, esc, BODY_MAX * 6);
        fprintf(f, "\"url\": \"%s\",\n   ", esc);
        devos_json_escape(r->headers, esc, BODY_MAX * 6);
        fprintf(f, "\"headers\": \"%s\",\n   ", esc);
        devos_json_escape(r->body, esc, BODY_MAX * 6);
        fprintf(f, "\"body\": \"%s\", \"body_type\": \"%s\", \"follow_redirects\": %d, \"insecure\": %d}%s\n", esc,
                BTYPES[r->btype & 3], r->redirects ? 1 : 0, r->insecure ? 1 : 0, i + 1 < s_nreq ? "," : "");
    }
    fprintf(f, "]\n");
    free(esc);
    return fclose(f) == 0;
}

/* form <-> request */
static void form_from(const req_t *r)
{
    lv_dropdown_set_selected(dd_method, (uint32_t)method_index(r->method));
    lv_textarea_set_text(ta_url, r->url);
    lv_textarea_set_text(ta_headers, r->headers);
    lv_textarea_set_text(ta_body, r->body);
    lv_dropdown_set_selected(dd_btype, (uint32_t)(r->btype & 3));
    if (r->redirects) lv_obj_add_state(cb_redir, LV_STATE_CHECKED);
    else lv_obj_remove_state(cb_redir, LV_STATE_CHECKED);
    if (r->insecure) lv_obj_add_state(cb_insecure, LV_STATE_CHECKED);
    else lv_obj_remove_state(cb_insecure, LV_STATE_CHECKED);
}

static void form_to(req_t *r)
{
    snprintf(r->method, sizeof(r->method), "%s", METHODS[lv_dropdown_get_selected(dd_method)]);
    snprintf(r->url, sizeof(r->url), "%s", lv_textarea_get_text(ta_url));
    snprintf(r->headers, sizeof(r->headers), "%s", lv_textarea_get_text(ta_headers));
    snprintf(r->body, sizeof(r->body), "%s", lv_textarea_get_text(ta_body));
    r->btype = (int)lv_dropdown_get_selected(dd_btype);
    r->redirects = lv_obj_has_state(cb_redir, LV_STATE_CHECKED);
    r->insecure = lv_obj_has_state(cb_insecure, LV_STATE_CHECKED);
}

static void refresh_side(void)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "SAVED REQUESTS (%d)", s_nreq);
    devos_w_set_text(lbl_side_hdr, buf);
    devos_vlist_set_count(&s_side, s_nreq);
    if (s_nreq) lv_obj_add_flag(lbl_side_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(lbl_side_empty, LV_OBJ_FLAG_HIDDEN);
}

static void load_req(int i)
{
    if (i < 0 || i >= s_nreq) return;
    s_loaded = i;
    form_from(&s_reqs[i]);
    s_side.sel = i;
    devos_vlist_redraw(&s_side);
    char buf[80];
    snprintf(buf, sizeof(buf), "Loaded \"%.48s\" - Ctrl+Enter sends it", s_reqs[i].name);
    flash(buf);
}

static void new_req(void)
{
    req_t r;
    memset(&r, 0, sizeof(r));
    snprintf(r.method, sizeof(r.method), "GET");
    r.redirects = true;
    form_from(&r);
    s_loaded = -1;
    s_side.sel = -1;
    devos_vlist_redraw(&s_side);
    devos_focus_set(&s_f, ta_url);
    flash("New request");
}

/* ------------------------------------------------------------------ sending */
static void show_response(void);

static void send_now(void)
{
    if (s_job) {
        flash("Still waiting for the last response (Esc cancels it)");
        return;
    }
    static EXT_RAM_BSS_ATTR char url[URL_MAX * 2], hdrs[HDR_MAX * 2 + 128], body[BODY_MAX * 2];
    char missing[48] = "";
    expand(lv_textarea_get_text(ta_url), url, sizeof(url), missing, sizeof(missing));
    char *u = url;
    while (*u == ' ') u++;
    if (!*u) {
        flash("Enter a URL first");
        devos_focus_set(&s_f, ta_url);
        return;
    }
    if (!strstr(u, "://")) {                    /* "example.com/x" -> http:// */
        memmove(u + 7, u, strlen(u) + 1 < sizeof(url) - 7 ? strlen(u) + 1 : sizeof(url) - 8);
        memcpy(u, "http://", 7);
    }
    size_t hl = expand(lv_textarea_get_text(ta_headers), hdrs, sizeof(hdrs) - 128, missing, sizeof(missing));
    const char *m = METHODS[lv_dropdown_get_selected(dd_method)];
    int bt = (int)lv_dropdown_get_selected(dd_btype);
    size_t bl = 0;
    bool has_body = strcasecmp(m, "GET") && strcasecmp(m, "HEAD");
    if (has_body || lv_textarea_get_text(ta_body)[0]) bl = expand(lv_textarea_get_text(ta_body), body, sizeof(body), missing, sizeof(missing));
    if (bl && CTYPES[bt]) {
        bool has_ct = false;
        for (const char *p = hdrs; *p;) {
            while (*p == ' ' || *p == '\n' || *p == '\r') p++;
            if (!strncasecmp(p, "Content-Type:", 13)) has_ct = true;
            p += strcspn(p, "\n");
        }
        if (!has_ct) snprintf(hdrs + hl, sizeof(hdrs) - hl, "%sContent-Type: %s", hl ? "\n" : "", CTYPES[bt]);
    }
    if (missing[0]) {
        char msg[96];
        snprintf(msg, sizeof(msg), "{{%s}} isn't set - add it under Variables", missing);
        flash(msg);
        return;
    }
    devos_http_req_t q = {
        .method = m, .url = u, .headers = hdrs, .body = bl ? body : NULL, .body_len = bl,
        .insecure = lv_obj_has_state(cb_insecure, LV_STATE_CHECKED), .timeout_ms = 15000,
        .max_redirects = lv_obj_has_state(cb_redir, LV_STATE_CHECKED) ? 5 : 0, .max_body = VIEW_MAX / 2,
    };
    s_job = devos_http_submit(&q);
    if (s_job <= 0) {
        s_job = 0;
        flash("Couldn't queue the request");
        return;
    }
    s_sent_at = lv_tick_get();
    refresh_status();
}

static bool is_text(const char *s, size_t n)
{
    for (size_t i = 0; i < n && i < 4096; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0 || (c < 32 && c != '\t' && c != '\n' && c != '\r')) return false;
    }
    return true;
}

static void show_response(void)
{
    const devos_palette_t *p = devos_theme_get();
    (void)p;
    if (!s_have_resp) {
        s_cv.json = false;
        devos_codeview_set(&s_cv, "");
        devos_w_set_text(lbl_code, "");
        devos_w_set_text(lbl_meta, "");
        lv_obj_remove_flag(lbl_resp_empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(lbl_resp_empty, LV_OBJ_FLAG_HIDDEN);
    devos_http_resp_t *r = &s_resp;
    char buf[300];
    if (r->status) snprintf(buf, sizeof(buf), "%d %s", r->status, r->reason);
    else snprintf(buf, sizeof(buf), "No response");
    devos_w_set_text(lbl_code, buf);
    devos_w_track(lbl_code, !r->status ? DEVOS_W_TEXT_ERR : r->status < 300 ? DEVOS_W_TEXT_OK
                            : r->status < 400 ? DEVOS_W_TEXT_ACCENT : r->status < 500 ? DEVOS_W_TEXT_WARN : DEVOS_W_TEXT_ERR);
    char size[32];
    if (r->body_total >= 1024 * 1024) snprintf(size, sizeof(size), "%.1f MB", r->body_total / 1048576.0);
    else if (r->body_total >= 1024) snprintf(size, sizeof(size), "%.1f KB", r->body_total / 1024.0);
    else snprintf(size, sizeof(size), "%u B", (unsigned)r->body_total);
    if (r->status)
        snprintf(buf, sizeof(buf), "%d ms  (DNS %d, connect %d, TLS %d, wait %d)  -  %s%s  -  %s%s%s", r->ms_total, r->ms_dns,
                 r->ms_connect, r->ms_tls, r->ms_first_byte, size, r->truncated ? " (cut)" : "",
                 r->tls ? (r->tls_verified ? "TLS verified" : "TLS NOT verified") : "plain HTTP",
                 r->redirects ? ", redirected" : "", r->error[0] ? "  -  incomplete" : "");
    else snprintf(buf, sizeof(buf), "%s  (%d ms)", r->error, r->ms_total);
    devos_w_set_text(lbl_meta, buf);
    devos_w_track(lbl_meta, r->status ? DEVOS_W_TEXT_DIM : DEVOS_W_TEXT_ERR);
    devos_w_track(btn_rbody, s_show_headers ? DEVOS_W_BTN : DEVOS_W_BTN_PRIMARY);
    devos_w_track(btn_rhdrs, s_show_headers ? DEVOS_W_BTN_PRIMARY : DEVOS_W_BTN);

    if (!s_view) s_view = malloc(VIEW_MAX);
    if (!s_view) return;
    s_cv.plain = true;
    s_cv.json = false;
    if (s_show_headers) {
        size_t o = (size_t)snprintf(s_view, VIEW_MAX, "%s\n\n", r->final_url);
        if (r->tls_info[0]) o += (size_t)snprintf(s_view + o, VIEW_MAX - o, "TLS: %s (%s)\n\n", r->tls_info,
                                                   r->tls_verified ? "certificate verified" : "certificate NOT verified");
        snprintf(s_view + o, VIEW_MAX - o, "%s", r->headers ? r->headers : "");
    } else if (!r->body_len) {
        snprintf(s_view, VIEW_MAX, "%s", r->status ? "(empty body)" : "");
    } else if (devos_json_pretty(r->body, r->body_len, s_view, VIEW_MAX)) {
        s_cv.json = true;
    } else if (is_text(r->body, r->body_len)) {
        snprintf(s_view, VIEW_MAX, "%s", r->body);
    } else {
        size_t o = (size_t)snprintf(s_view, VIEW_MAX, "(binary, %u bytes - first 2 KB)\n\n", (unsigned)r->body_len);
        const unsigned char *d = (const unsigned char *)r->body;
        for (size_t i = 0; i < r->body_len && i < 2048 && o + 80 < VIEW_MAX; i += 16) {
            o += (size_t)snprintf(s_view + o, VIEW_MAX - o, "%04x  ", (unsigned)i);
            for (size_t k = 0; k < 16; k++) {
                if (i + k < r->body_len) o += (size_t)snprintf(s_view + o, VIEW_MAX - o, "%02x ", d[i + k]);
                else o += (size_t)snprintf(s_view + o, VIEW_MAX - o, "   ");
            }
            s_view[o++] = '\n';
            s_view[o] = '\0';
        }
    }
    devos_codeview_set(&s_cv, s_view);
}

static void poll_job(void)
{
    if (!s_job) return;
    devos_http_resp_t r;
    int st = devos_http_poll(s_job, &r);
    if (st == 0) return;
    s_job = 0;
    if (st < 0) return;
    if (s_have_resp) devos_http_resp_free(&s_resp);
    s_resp = r;
    s_have_resp = true;
    show_response();
}

static void cancel_job(void)
{
    if (!s_job) return;
    devos_http_cancel(s_job);
    s_job = 0;
    flash("Cancelled");
}

/* ------------------------------------------------------------------ dialogs */
static void save_as_open(void)
{
    char name[48];
    const char *url = lv_textarea_get_text(ta_url);
    const char *host = strstr(url, "://");
    snprintf(name, sizeof(name), "%.8s %.38s", METHODS[lv_dropdown_get_selected(dd_method)], host ? host + 3 : url);
    lv_textarea_set_text(ta_save_name, s_loaded >= 0 ? s_reqs[s_loaded].name : name);
    devos_w_set_text(s_dlg_save.msg, "");
    devos_w_dialog_show(&s_dlg_save, true);
    devos_focus_set(&s_fsave, ta_save_name);
}

static void save_as_ok(void)
{
    const char *n = lv_textarea_get_text(ta_save_name);
    while (*n == ' ') n++;
    if (!*n) {
        devos_w_set_text(s_dlg_save.msg, "Give it a name");
        return;
    }
    int i;
    for (i = 0; i < s_nreq && strcmp(s_reqs[i].name, n); i++) {}
    if (i == s_nreq) {
        if (s_nreq >= MAX_REQ) {
            devos_w_set_text(s_dlg_save.msg, "The list is full (32): delete one first");
            return;
        }
        s_nreq++;
    }
    req_t *r = &s_reqs[i];
    char nm[48];
    snprintf(nm, sizeof(nm), "%s", n);
    form_to(r);
    snprintf(r->name, sizeof(r->name), "%s", nm);
    s_loaded = i;
    devos_w_dialog_show(&s_dlg_save, false);
    devos_focus_clear(&s_fsave);
    refresh_side();
    s_side.sel = i;
    flash(req_save_all() ? "Saved to /rest/requests.json" : "Couldn't write /rest/requests.json (SD card?)");
}

static void save_quick(void)
{
    if (s_loaded < 0) {
        save_as_open();
        return;
    }
    form_to(&s_reqs[s_loaded]);
    flash(req_save_all() ? "Saved" : "Couldn't write /rest/requests.json (SD card?)");
}

static void vars_open(void)
{
    lv_textarea_set_text(ta_vars, s_vars);
    devos_w_set_text(s_dlg_vars.msg, "");
    devos_w_dialog_show(&s_dlg_vars, true);
    devos_focus_set(&s_fvars, ta_vars);
}

static void vars_ok(void)
{
    if (vars_save(lv_textarea_get_text(ta_vars))) {
        devos_w_dialog_show(&s_dlg_vars, false);
        devos_focus_clear(&s_fvars);
        flash("Variables saved");
    } else {
        devos_w_set_text(s_dlg_vars.msg, "Couldn't save them");
    }
}

static void del_open(void)
{
    if (s_side.sel < 0 || s_side.sel >= s_nreq) return;
    char buf[96];
    snprintf(buf, sizeof(buf), LV_SYMBOL_TRASH "  Delete \"%.48s\"?", s_reqs[s_side.sel].name);
    devos_w_set_text(s_dlg_del.title, buf);
    devos_w_dialog_show(&s_dlg_del, true);
    devos_focus_first(&s_fdel);
}

static void del_ok(void)
{
    int i = s_side.sel;
    devos_w_dialog_show(&s_dlg_del, false);
    devos_focus_clear(&s_fdel);
    if (i < 0 || i >= s_nreq) return;
    memmove(&s_reqs[i], &s_reqs[i + 1], (size_t)(s_nreq - i - 1) * sizeof(req_t));
    s_nreq--;
    if (s_loaded == i) s_loaded = -1;
    else if (s_loaded > i) s_loaded--;
    refresh_side();
    if (s_side.sel >= s_nreq) s_side.sel = s_nreq - 1;
    flash(req_save_all() ? "Deleted" : "Couldn't write /rest/requests.json");
}

static void save_ok_cb(lv_event_t *e) { LV_UNUSED(e); save_as_ok(); }
static void save_cancel_cb(lv_event_t *e) { LV_UNUSED(e); devos_w_dialog_show(&s_dlg_save, false); devos_focus_clear(&s_fsave); }
static void vars_ok_cb(lv_event_t *e) { LV_UNUSED(e); vars_ok(); }
static void vars_cancel_cb(lv_event_t *e) { LV_UNUSED(e); devos_w_dialog_show(&s_dlg_vars, false); devos_focus_clear(&s_fvars); }
static void del_ok_cb(lv_event_t *e) { LV_UNUSED(e); del_ok(); }
static void del_cancel_cb(lv_event_t *e) { LV_UNUSED(e); devos_w_dialog_show(&s_dlg_del, false); devos_focus_clear(&s_fdel); }

/* ------------------------------------------------------------------ buttons */
static void send_cb(lv_event_t *e) { LV_UNUSED(e); if (s_job) cancel_job(); else send_now(); }
static void side_toggle(void);
static void saved_cb(lv_event_t *e) { LV_UNUSED(e); side_toggle(); }
static void vars_cb(lv_event_t *e) { LV_UNUSED(e); vars_open(); }
static void save_cb(lv_event_t *e) { LV_UNUSED(e); save_quick(); }
static void new_cb(lv_event_t *e) { LV_UNUSED(e); new_req(); }
static void rbody_cb(lv_event_t *e) { LV_UNUSED(e); s_show_headers = false; show_response(); }
static void rhdrs_cb(lv_event_t *e) { LV_UNUSED(e); s_show_headers = true; show_response(); }

/* ------------------------------------------------------------------ side panel */
static void side_draw(devos_vlist_t *v, lv_layer_t *layer, const lv_area_t *row, int idx)
{
    LV_UNUSED(v);
    if (idx >= s_nreq) return;
    const devos_palette_t *p = devos_theme_get();
    const req_t *r = &s_reqs[idx];
    devos_w_draw_text(layer, &lv_font_montserrat_12, row->x1 + 10, row->y1 + 4, 52, r->method, method_color(r->method));
    devos_w_draw_text(layer, &lv_font_montserrat_14, row->x1 + 64, row->y1 + 3, row->x2 - row->x1 - 72, r->name,
                      idx == s_loaded ? p->accent_primary : p->text_primary);
}

static void side_activate(devos_vlist_t *v, int idx)
{
    LV_UNUSED(v);
    load_req(idx);
    s_side_focus = false;
    s_side.active = false;
    devos_focus_set(&s_f, ta_url);
    layout();
}

static void side_toggle(void)
{
    if (!s_side_open) {
        s_side_open = true;
        s_side_focus = true;
    } else if (!s_side_focus) {
        s_side_focus = true;                /* open: first press goes there */
    } else {
        s_side_open = false;
        s_side_focus = false;
    }
    s_side.active = s_side_focus;
    if (s_side_focus) {
        devos_focus_clear(&s_f);
        if (s_side.sel < 0 && s_nreq) devos_vlist_select(&s_side, s_loaded >= 0 ? s_loaded : 0);
    } else if (!devos_focus_get(&s_f)) {
        devos_focus_set(&s_f, ta_url);
    }
    devos_vlist_redraw(&s_side);
    layout();
}

/* ------------------------------------------------------------------ layout */
static void layout(void)
{
    int x0 = s_side_open ? SIDE_W : 0, w = DEVOS_SCREEN_WIDTH - x0;
    if (s_side_open) lv_obj_remove_flag(side, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(side, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_color(side, s_side_focus ? devos_theme_get()->accent_primary : devos_theme_get()->surface_border, 0);
    lv_obj_set_pos(dd_method, x0 + 12, 50);
    lv_obj_set_pos(ta_url, x0 + 130, 50);
    lv_obj_set_width(ta_url, w - 130 - 12 - 120);
    lv_obj_set_pos(btn_send, x0 + w - 12 - 110, 50);
    int hw = (w - 36) * 2 / 5;
    lv_obj_set_pos(cap_hdr, x0 + 12, 94);
    lv_obj_set_pos(ta_headers, x0 + 12, 112);
    lv_obj_set_width(ta_headers, hw);
    lv_obj_set_pos(cap_body, x0 + 24 + hw, 94);
    lv_obj_set_pos(ta_body, x0 + 24 + hw, 112);
    lv_obj_set_width(ta_body, w - 36 - hw);
    lv_obj_set_pos(cb_redir, x0 + 14, 272);
    lv_obj_set_pos(cb_insecure, x0 + 190, 272);
    lv_obj_set_pos(cap_btype, x0 + w - 12 - 110 - 72, 276);
    lv_obj_set_pos(dd_btype, x0 + w - 12 - 110, 266);
    lv_obj_set_pos(resp, x0 + 12, 312);
    lv_obj_set_width(resp, w - 24);
    lv_obj_set_width(resp_scroll, w - 32);
    lv_obj_set_width(lbl_meta, w - 24 - 200 - 190);
    lv_obj_set_pos(s_keys, x0 + 12, DEVOS_CONTENT_HEIGHT - DEVOS_W_KEYS_H + 3);
    lv_obj_set_width(s_keys, w - 24);
}

static void refresh_status(void)
{
    if ((int32_t)(lv_tick_get() - s_flash_until) < 0) return;
    char buf[160];
    if (s_job) snprintf(buf, sizeof(buf), "Sending...  %.1f s  (Esc cancels)", (lv_tick_get() - s_sent_at) / 1000.0);
    else if (s_loaded >= 0) snprintf(buf, sizeof(buf), "%.60s", s_reqs[s_loaded].name);
    else snprintf(buf, sizeof(buf), "Unsaved request");
    devos_w_set_text(lbl_status, buf);
}

static const char *keys_text(void)
{
    if (devos_w_dialog_open(&s_dlg_save)) return "Enter saves    Esc cancels";
    if (devos_w_dialog_open(&s_dlg_vars)) return "One per line: NAME=value    Tab to the buttons    Ctrl+Enter saves    Esc cancels";
    if (devos_w_dialog_open(&s_dlg_del)) return "Enter deletes    Esc keeps it";
    if (s_side_focus) return "Up / Down pick    Enter loads    Del deletes    Ctrl+N new    Esc or Tab back to the request    Sym+L hides";
    if (devos_focus_get(&s_f) == resp_scroll)
        return "Up / Down / PgUp / PgDn scroll    Alt+H body / headers    Ctrl+Enter sends    Tab moves    Esc home";
    return "Ctrl+Enter sends    Tab moves    Ctrl+S saves    Ctrl+N new    Ctrl+K variables    Alt+H body / headers    Sym+L saved    Esc home";
}

static void tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!s_screen || lv_obj_has_flag(s_screen, LV_OBJ_FLAG_HIDDEN)) return;
    poll_job();
    refresh_status();
    devos_w_set_text(lbl_send, s_job ? LV_SYMBOL_CLOSE "  Cancel" : LV_SYMBOL_UPLOAD "  Send");
    devos_w_track(btn_send, s_job ? DEVOS_W_BTN_DANGER : DEVOS_W_BTN_PRIMARY);
    devos_w_set_text(s_keys, keys_text());
}

/* ------------------------------------------------------------------ keys */
static bool dialog_key(devos_w_dialog_t *d, devos_focus_t *f, uint32_t key, uint8_t mods, void (*ok)(void),
                       bool multiline)
{
    if (!devos_w_dialog_open(d)) return false;
    if (key == LV_KEY_ESC && !f->dd_open) {
        devos_w_dialog_show(d, false);
        devos_focus_clear(f);
    } else if ((mods & DEVOS_MOD_CTRL) && (key == '\r' || key == '\n')) {
        ok();
    } else if (devos_focus_key(f, key, mods)) {
    } else if ((key == '\r' || key == '\n') && !multiline) {
        ok();
    }
    return true;
}

static bool rest_key(uint32_t key, uint8_t mods)
{
    if ((mods & DEVOS_MOD_CTRL) && key >= 1 && key <= 26 && key != '\b' && key != '\t' && key != '\n' && key != '\r')
        key += 'a' - 1;
    if (dialog_key(&s_dlg_save, &s_fsave, key, mods, save_as_ok, false)) return true;
    if (dialog_key(&s_dlg_vars, &s_fvars, key, mods, vars_ok, true)) return true;
    if (dialog_key(&s_dlg_del, &s_fdel, key, mods, del_ok, false)) return true;

    if ((mods & DEVOS_MOD_FN) && (key == 'l' || key == 'L')) { side_toggle(); return true; }
    if ((mods & DEVOS_MOD_CTRL) && (key == '\r' || key == '\n')) { if (s_job) cancel_job(); else send_now(); return true; }
    if (mods & DEVOS_MOD_CTRL) {
        if (key == 's' || key == 'S') { save_quick(); return true; }
        if (key == 'n' || key == 'N') { new_req(); return true; }
        if (key == 'k' || key == 'K') { vars_open(); return true; }
        return false;
    }
    if ((mods & DEVOS_MOD_ALT) && (key == 'h' || key == 'H')) {
        s_show_headers = !s_show_headers;
        show_response();
        return true;
    }
    if (key == LV_KEY_ESC && s_job && !s_f.dd_open) { cancel_job(); return true; }

    if (s_side_focus) {
        if (key == LV_KEY_ESC || key == '\t') {
            s_side_focus = false;
            s_side.active = false;
            devos_vlist_redraw(&s_side);
            devos_focus_set(&s_f, ta_url);
            layout();
            return true;
        }
        if (key == LV_KEY_DEL || key == 127 || key == 'd' || key == 'D') { del_open(); return true; }
        if (devos_vlist_key(&s_side, key)) return true;
        return !(mods & DEVOS_MOD_FN);
    }

    if (devos_focus_get(&s_f) == resp_scroll && s_f.ring) {
        int page = lv_obj_get_height(resp_scroll) - 40;
        switch (key) {
        case LV_KEY_UP: lv_obj_scroll_by_bounded(resp_scroll, 0, DEVOS_CODEVIEW_LINE_H * 2, LV_ANIM_OFF); return true;
        case LV_KEY_DOWN: lv_obj_scroll_by_bounded(resp_scroll, 0, -DEVOS_CODEVIEW_LINE_H * 2, LV_ANIM_OFF); return true;
        case DEVOS_KEY_PGUP: lv_obj_scroll_by_bounded(resp_scroll, 0, page, LV_ANIM_OFF); return true;
        case DEVOS_KEY_PGDN: lv_obj_scroll_by_bounded(resp_scroll, 0, -page, LV_ANIM_OFF); return true;
        case LV_KEY_HOME: lv_obj_scroll_to_y(resp_scroll, 0, LV_ANIM_OFF); return true;
        case LV_KEY_END: lv_obj_scroll_to_y(resp_scroll, LV_COORD_MAX, LV_ANIM_OFF); return true;
        default: break;
        }
    }
    if (key == LV_KEY_ESC && !s_f.dd_open) return false;           /* Home */
    if (devos_focus_key(&s_f, key, mods)) return true;
    if (key == '\r' || key == '\n') {                               /* Enter in the URL field */
        send_now();
        return true;
    }
    return !(mods & (DEVOS_MOD_FN | DEVOS_MOD_ALT)) && key >= 32 && key < 127;
}

/* ------------------------------------------------------------------ init */
static lv_obj_t *dialog_buttons(devos_w_dialog_t *d, const char *ok_text, lv_event_cb_t ok, lv_event_cb_t cancel,
                                devos_w_kind_t kind, lv_obj_t **cancel_out)
{
    lv_obj_t *bok = devos_w_btn_kind(d->box, kind, ok_text, 120, ok, NULL, NULL);
    lv_obj_set_size(bok, 120, 36);
    lv_obj_align(bok, LV_ALIGN_BOTTOM_RIGHT, -132, 0);
    lv_obj_t *bc = devos_w_btn(d->box, "Cancel", 120, cancel, NULL, NULL);
    lv_obj_set_size(bc, 120, 36);
    lv_obj_align(bc, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    if (cancel_out) *cancel_out = bc;
    return bok;
}

static void rest_init(void)
{
    if (s_inited) return;
    s_inited = true;
    vars_load();
    req_load_all();

    s_screen = devos_w_screen(&s_desc);
    s_bar = devos_w_bar(s_screen, LV_SYMBOL_UPLOAD "  REST", NULL);
    lbl_status = devos_w_label(s_bar, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "");
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_status, 560);
    lv_obj_align(lbl_status, LV_ALIGN_LEFT_MID, 110, 0);
    btn_new = devos_w_btn(s_bar, LV_SYMBOL_PLUS " New", 70, new_cb, NULL, NULL);
    lv_obj_align(btn_new, LV_ALIGN_RIGHT_MID, -8, 0);
    btn_save = devos_w_btn(s_bar, LV_SYMBOL_SAVE " Save", 74, save_cb, NULL, NULL);
    lv_obj_align_to(btn_save, btn_new, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    btn_vars = devos_w_btn(s_bar, "{{ }}  Variables", 110, vars_cb, NULL, NULL);
    lv_obj_align_to(btn_vars, btn_save, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    btn_saved = devos_w_btn(s_bar, LV_SYMBOL_LIST " Saved", 76, saved_cb, NULL, NULL);
    lv_obj_align_to(btn_saved, btn_vars, LV_ALIGN_OUT_LEFT_MID, -6, 0);

    side = devos_w_panel(s_screen, 0, DEVOS_W_BAR_H, SIDE_W, DEVOS_CONTENT_HEIGHT - DEVOS_W_BAR_H, DEVOS_W_PANEL);
    lbl_side_hdr = devos_w_label(side, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "SAVED REQUESTS");
    lv_obj_set_pos(lbl_side_hdr, 10, 8);
    devos_vlist_create(&s_side, side, 26, side_draw);
    s_side.on_activate = side_activate;
    lv_obj_set_pos(s_side.scroll, 0, 30);
    lv_obj_set_size(s_side.scroll, SIDE_W - 2, DEVOS_CONTENT_HEIGHT - DEVOS_W_BAR_H - 34);
    lbl_side_empty = devos_w_label(side, &lv_font_montserrat_12, DEVOS_W_TEXT_MUTED, "Nothing saved yet.\nCtrl+S saves the request.");
    lv_obj_set_pos(lbl_side_empty, 10, 36);

    dd_method = devos_w_dd(s_screen, "GET\nPOST\nPUT\nPATCH\nDELETE\nHEAD", 110);
    ta_url = devos_w_ta(s_screen, true, 600, 36);
    lv_textarea_set_max_length(ta_url, URL_MAX - 1);
    lv_textarea_set_placeholder_text(ta_url, "https://api.example.com/v1/things  -  {{NAME}} is filled in from Variables");
    btn_send = devos_w_btn_kind(s_screen, DEVOS_W_BTN_PRIMARY, LV_SYMBOL_UPLOAD "  Send", 110, send_cb, NULL, &lbl_send);
    lv_obj_set_size(btn_send, 110, 36);
    cap_hdr = devos_w_label(s_screen, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "Headers  (Name: value, one per line)");
    ta_headers = devos_w_ta(s_screen, false, 400, 150);
    lv_textarea_set_max_length(ta_headers, HDR_MAX - 1);
    lv_textarea_set_placeholder_text(ta_headers, "Authorization: Bearer {{TOKEN}}\nAccept: application/json");
    cap_body = devos_w_label(s_screen, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "Body");
    ta_body = devos_w_ta(s_screen, false, 600, 150);
    lv_textarea_set_max_length(ta_body, BODY_MAX - 1);
    lv_textarea_set_placeholder_text(ta_body, "{\"key\": \"value\"}");
    cb_redir = devos_w_cb(s_screen, "Follow redirects");
    lv_obj_add_state(cb_redir, LV_STATE_CHECKED);
    cb_insecure = devos_w_cb(s_screen, "Skip certificate check");
    cap_btype = devos_w_label(s_screen, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "Body type");
    dd_btype = devos_w_dd(s_screen, "JSON\nText\nForm\nRaw", 110);

    resp = devos_w_panel(s_screen, 12, 312, 1256, DEVOS_CONTENT_HEIGHT - DEVOS_W_KEYS_H - 318, DEVOS_W_CODE);
    lv_obj_set_style_radius(resp, 6, 0);
    lbl_code = devos_w_label(resp, &lv_font_montserrat_20, DEVOS_W_TEXT_OK, "");
    lv_obj_set_pos(lbl_code, 12, 8);
    lbl_meta = devos_w_label(resp, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM, "");
    lv_label_set_long_mode(lbl_meta, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(lbl_meta, 200, 14);
    btn_rbody = devos_w_btn_kind(resp, DEVOS_W_BTN_PRIMARY, "Body", 80, rbody_cb, NULL, NULL);
    lv_obj_align(btn_rbody, LV_ALIGN_TOP_RIGHT, -96, 8);
    btn_rhdrs = devos_w_btn(resp, "Headers", 80, rhdrs_cb, NULL, NULL);
    lv_obj_align(btn_rhdrs, LV_ALIGN_TOP_RIGHT, -8, 8);
    resp_scroll = lv_obj_create(resp);
    lv_obj_set_style_bg_opa(resp_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(resp_scroll, 0, 0);
    lv_obj_set_style_radius(resp_scroll, 4, 0);
    lv_obj_set_pos(resp_scroll, 4, 42);
    lv_obj_set_size(resp_scroll, 1248, DEVOS_CONTENT_HEIGHT - DEVOS_W_KEYS_H - 318 - 48);
    devos_codeview_create(&s_cv, resp_scroll);
    lv_obj_set_scrollbar_mode(resp_scroll, LV_SCROLLBAR_MODE_ACTIVE);
    s_cv.plain = true;
    lbl_resp_empty = devos_w_label(resp, &lv_font_montserrat_14, DEVOS_W_TEXT_MUTED,
                                   "The response shows up here.  Ctrl+Enter sends the request.");
    lv_obj_align(lbl_resp_empty, LV_ALIGN_CENTER, 0, 0);

    s_keys = devos_w_keys(s_screen);

    devos_focus_init(&s_f);
    lv_obj_t *order[] = { dd_method, ta_url, btn_send, ta_headers, ta_body, cb_redir, cb_insecure, dd_btype,
                          btn_rbody, btn_rhdrs, resp_scroll, btn_saved, btn_vars, btn_save, btn_new };
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) devos_focus_add(&s_f, order[i]);

    /* dialogs */
    devos_w_dialog(&s_dlg_save, s_screen, 560, 190, LV_SYMBOL_SAVE "  Save request");
    ta_save_name = devos_w_field(s_dlg_save.box, "Name", 0, 34, 516);
    lv_textarea_set_max_length(ta_save_name, 47);
    lv_obj_t *c1;
    lv_obj_t *b1 = dialog_buttons(&s_dlg_save, LV_SYMBOL_OK "  Save", save_ok_cb, save_cancel_cb, DEVOS_W_BTN_PRIMARY, &c1);
    devos_focus_init(&s_fsave);
    devos_focus_add(&s_fsave, ta_save_name);
    devos_focus_add(&s_fsave, b1);
    devos_focus_add(&s_fsave, c1);

    devos_w_dialog(&s_dlg_vars, s_screen, 760, 520, "{{ }}  Variables");
    lv_obj_t *vh = devos_w_label(s_dlg_vars.box, &lv_font_montserrat_12, DEVOS_W_TEXT_DIM,
                                 "One per line as NAME=value. Use them as {{NAME}} in the URL, headers or body.\n"
                                 "Kept in the Tab5's flash (not on the SD card) - the place for tokens.\n"
                                 "Built in: {{ts}} Unix time, {{time}} ISO time (UTC), {{uuid}}, {{battery}}.");
    lv_obj_set_pos(vh, 0, 30);
    ta_vars = devos_w_ta(s_dlg_vars.box, false, 716, 330);
    lv_textarea_set_max_length(ta_vars, VARS_MAX - 1);
    lv_textarea_set_placeholder_text(ta_vars, "HA_TOKEN=eyJhbGciOi...\nGH_REPO=me/my-repo");
    lv_obj_set_pos(ta_vars, 0, 90);
    lv_obj_t *c2;
    lv_obj_t *b2 = dialog_buttons(&s_dlg_vars, LV_SYMBOL_OK "  Save", vars_ok_cb, vars_cancel_cb, DEVOS_W_BTN_PRIMARY, &c2);
    devos_focus_init(&s_fvars);
    devos_focus_add(&s_fvars, ta_vars);
    devos_focus_add(&s_fvars, b2);
    devos_focus_add(&s_fvars, c2);

    devos_w_dialog(&s_dlg_del, s_screen, 560, 130, LV_SYMBOL_TRASH "  Delete?");
    lv_obj_t *c3;
    lv_obj_t *b3 = dialog_buttons(&s_dlg_del, LV_SYMBOL_TRASH "  Delete", del_ok_cb, del_cancel_cb, DEVOS_W_BTN_DANGER, &c3);
    devos_focus_init(&s_fdel);
    devos_focus_add(&s_fdel, b3);
    devos_focus_add(&s_fdel, c3);

    refresh_side();
    if (s_nreq) load_req(0);
    s_flash_until = 0;
    show_response();
    layout();
    lv_timer_create(tick_cb, 100, NULL);
}

static void rest_show(void)
{
    char action[24], arg[512];
    if (devos_core_take_intent("rest", action, sizeof(action), arg, sizeof(arg))) {
        req_t r;
        memset(&r, 0, sizeof(r));
        snprintf(r.method, sizeof(r.method), "%s", !strcasecmp(action, "post") ? "POST" : "GET");
        snprintf(r.url, sizeof(r.url), "%s", arg);
        r.redirects = true;
        form_from(&r);
        s_loaded = -1;
        s_side_focus = false;
        s_side.active = false;
        devos_focus_set(&s_f, ta_url);
        send_now();
    } else if (!devos_focus_get(&s_f) && !s_side_focus) {
        devos_focus_set(&s_f, ta_url);
    }
    layout();
}

static void rest_hide(void) {}

static int rest_telemetry(char lines[3][64])
{
    snprintf(lines[0], 64, "* %d saved request%s", s_nreq, s_nreq == 1 ? "" : "s");
    if (s_have_resp && s_resp.status) snprintf(lines[1], 64, "* Last: %d %.40s", s_resp.status, s_resp.reason);
    else if (s_have_resp) snprintf(lines[1], 64, "* Last: failed");
    else snprintf(lines[1], 64, "* Webhooks, APIs, CI triggers");
    snprintf(lines[2], 64, "* HTTP and HTTPS");
    return 3;
}

devos_app_descriptor_t *app_rest_get_descriptor(void)
{
    s_desc.id = DEVOS_APP_LAUNCHER;             /* auto-assigned */
    s_desc.uid = "rest";
    s_desc.icon = LV_SYMBOL_UPLOAD;
    s_desc.draw_icon = devos_icon_rest;
    s_desc.category = "network";
    s_desc.name = "REST";
    s_desc.title = "REST";
    s_desc.subtitle = "HTTP requests and webhooks";
    s_desc.init = rest_init;
    s_desc.show = rest_show;
    s_desc.hide = rest_hide;
    s_desc.handle_key = rest_key;
    s_desc.get_telemetry_lines = rest_telemetry;
    return &s_desc;
}

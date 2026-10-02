/* Coder's Toolkit: offline developer helpers.
 *
 *   Base64 / Base64 URL / Hex / URL / Base58   encode & decode
 *   Hash          SHA-1 / SHA-256 / SHA-512, or HMAC with a key
 *   CRC-32        IEEE 802.3 checksum
 *   Hash file     stream a file off the SD card (Core 0 worker, devos_hashfile)
 *   JWT           split + pretty-print header & payload
 *   UUID          random v4
 *   Unix time     epoch <-> UTC / local date
 *
 * The pure logic lives in coder_core.c (no LVGL), unit-tested in
 * tools/coder_test.c; this file builds the screen and drives it. The one slow
 * path - hashing a file - is not computed here but handed to the Core 0
 * devos_hashfile worker (AGENTS.md #1), whose state is polled while running.
 *
 * Rules (AGENTS.md): #5 widgets/theme, #7 no forked crypto or JSON, #9
 * keyboard first - devos_focus drives the controls and letter shortcuts pick
 * tools and run actions, with the keys listed in the footer.
 */
#include "app_coder.h"
#include "coder_core.h"
#include "devos_config.h"
#include "devos_widgets.h"
#include "devos_codeview.h"
#include "devos_focus.h"
#include "devos_icons.h"
#include "devos_toast.h"
#include "devos_hashfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

/* Input cap: hex is the widest output (2x input) and the code view scrolls, so
 * a bigger buffer is fine now that it lives in PSRAM. */
#define IN_MAX   8192
#define OUT_MAX  (IN_MAX * 2 + 2048)

typedef enum {
    TOOL_B64 = 0,
    TOOL_B64URL,
    TOOL_HEX,
    TOOL_URL,
    TOOL_BASE58,
    TOOL_CRC32,
    TOOL_HASH,
    TOOL_HASHFILE,
    TOOL_JWT,
    TOOL_UUID,
    TOOL_EPOCH,
    TOOL_COUNT
} tool_t;

static coder_tool_t core_tool(tool_t t)
{
    switch (t) {
    case TOOL_B64:    return CODER_B64;
    case TOOL_B64URL: return CODER_B64URL;
    case TOOL_HEX:    return CODER_HEX;
    case TOOL_URL:    return CODER_URL;
    case TOOL_BASE58: return CODER_BASE58;
    default:          return CODER_B64;
    }
}

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *dd_mode;
    lv_obj_t *lbl_dir, *dd_dir;        /* Encode / Decode (text tools) */
    lv_obj_t *lbl_algo, *dd_algo;      /* SHA-1/256/512 (hash / hash file) */
    lv_obj_t *lbl_key, *ta_key;        /* HMAC key (hash) */
    lv_obj_t *lbl_in, *ta_in;
    lv_obj_t *out_panel, *out_scroll;  /* output: a devos_codeview in a panel */
    devos_codeview_t cv;
    lv_obj_t *lbl_hint;
    lv_obj_t *btn_compute, *btn_copy, *btn_swap, *btn_clear;
    devos_focus_t focus;
    char *out;                         /* OUT_MAX, PSRAM on device */
    tool_t tool;
    bool dir_auto;
} coder_ctx_t;

static devos_app_descriptor_t s_desc;
static coder_ctx_t s_ctx;

/* PSRAM for the big buffers (AGENTS.md #2); falls back to the heap. */
static void *big_alloc(size_t n)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
#else
    return malloc(n);
#endif
}

static void set_visible(lv_obj_t *o, bool vis)
{
    if (vis) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static const char *tool_name(tool_t t)
{
    switch (t) {
    case TOOL_B64:      return "Base64";
    case TOOL_B64URL:   return "Base64 URL";
    case TOOL_HEX:      return "Hex";
    case TOOL_URL:      return "URL";
    case TOOL_BASE58:   return "Base58";
    case TOOL_CRC32:    return "CRC-32";
    case TOOL_HASH:     return "Hash";
    case TOOL_HASHFILE: return "Hash file";
    case TOOL_JWT:      return "JWT";
    case TOOL_UUID:     return "UUID";
    case TOOL_EPOCH:    return "Unix time";
    default:            return "";
    }
}

static const char *tool_hint(tool_t t)
{
    switch (t) {
    case TOOL_B64:      return "Base64 (standard, padded) - Encode / Decode.";
    case TOOL_B64URL:   return "Base64 URL-safe (no padding, - and _) - Encode / Decode.";
    case TOOL_HEX:      return "Hexadecimal - Encode / Decode (skips spaces, : and -).";
    case TOOL_URL:      return "Percent-encoding (RFC 3986) - Encode / Decode.";
    case TOOL_BASE58:   return "Base58 (Bitcoin alphabet) - Encode / Decode.";
    case TOOL_CRC32:    return "CRC-32 (IEEE 802.3) of the text.";
    case TOOL_HASH:     return "SHA-1 / SHA-256 / SHA-512; type a key to get the HMAC instead.";
    case TOOL_HASHFILE: return "Hash a file on the SD card (e.g. /sdcard/notes/welcome.md).";
    case TOOL_JWT:      return "Decode a JSON Web Token's header and payload (signature shown, not verified).";
    case TOOL_UUID:     return "Random UUID v4 - press C for another.";
    case TOOL_EPOCH:    return "Unix seconds to UTC and local time; leave the box empty for now.";
    default:            return "";
    }
}

static const char *input_caption(tool_t t)
{
    switch (t) {
    case TOOL_B64: case TOOL_B64URL: case TOOL_HEX: case TOOL_URL: case TOOL_BASE58:
        return "Text";
    case TOOL_CRC32:    return "Text";
    case TOOL_HASH:     return "Message";
    case TOOL_HASHFILE: return "File path";
    case TOOL_JWT:      return "JWT  (header.payload.signature)";
    case TOOL_EPOCH:    return "Unix seconds  (empty = now)";
    default:            return "";
    }
}

/* =========================================================================
 * Compute
 * ========================================================================= */

static int algo_index(void) { return (int)lv_dropdown_get_selected(s_ctx.dd_algo); }

static void set_out(const char *text)
{
    devos_codeview_set(&s_ctx.cv, text ? text : "");
}

static void compute(void)
{
    tool_t t = s_ctx.tool;
    const char *in = lv_textarea_get_text(s_ctx.ta_in);
    size_t n = strlen(in);

    switch (t) {
    case TOOL_UUID:
        coder_uuid(s_ctx.out, OUT_MAX);
        set_out(s_ctx.out);
        break;
    case TOOL_EPOCH:
        coder_epoch(in[0] ? strtoll(in, NULL, 10) : 0, in[0] ? 0 : 1, s_ctx.out, OUT_MAX);
        set_out(s_ctx.out);
        break;
    case TOOL_JWT:
        coder_jwt(in, n, s_ctx.out, OUT_MAX);
        set_out(s_ctx.out);
        break;
    case TOOL_CRC32:
        coder_crc32(in, n, s_ctx.out, OUT_MAX);
        set_out(s_ctx.out);
        break;
    case TOOL_HASH: {
        const char *key = lv_textarea_get_text(s_ctx.ta_key);
        coder_hash((coder_hash_t)(CODER_SHA1 + algo_index()),
                   key, strlen(key), in, n, s_ctx.out, OUT_MAX);
        set_out(s_ctx.out);
        break;
    }
    case TOOL_HASHFILE:
        if (!in[0]) { set_out("Type a file path first"); break; }
        if (devos_hashfile_state() == DEVOS_HASHFILE_RUNNING) break;
        if (devos_hashfile_start(in, CODER_SHA1 + algo_index()) != 0) {
            const char *e = devos_hashfile_error();
            snprintf(s_ctx.out, OUT_MAX, "%s", e[0] ? e : "Could not start hashing");
        } else {
            snprintf(s_ctx.out, OUT_MAX, "Hashing...");
        }
        set_out(s_ctx.out);
        break;
    default:
        coder_transform(core_tool(t), lv_dropdown_get_selected(s_ctx.dd_dir) == 1,
                        in, n, s_ctx.out, OUT_MAX);
        set_out(s_ctx.out);
        break;
    }
}

/* Poll ~4 Hz while shown: when a running file hash finishes, place the digest.
 * Keeps the UI task off the read loop (the worker does the reading). */
static void tick_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!s_ctx.screen || lv_obj_has_flag(s_ctx.screen, LV_OBJ_FLAG_HIDDEN)) return;
    if (s_ctx.tool != TOOL_HASHFILE) return;
    devos_hashfile_state_t st = devos_hashfile_state();
    if (st == DEVOS_HASHFILE_RUNNING) {
        snprintf(s_ctx.out, OUT_MAX, "Hashing %d%% ...", devos_hashfile_progress());
        set_out(s_ctx.out);
    } else if (st == DEVOS_HASHFILE_DONE) {
        snprintf(s_ctx.out, OUT_MAX, "%s\n\n%llu bytes", devos_hashfile_result(),
                 (unsigned long long)devos_hashfile_total());
        set_out(s_ctx.out);
    } else if (st == DEVOS_HASHFILE_FAILED) {
        set_out(devos_hashfile_error());
    }
}

/* =========================================================================
 * Mode switching (with auto-direction: an already-encoded input decodes)
 * ========================================================================= */

static bool text_tool(tool_t t)
{
    return t == TOOL_B64 || t == TOOL_B64URL || t == TOOL_HEX || t == TOOL_URL || t == TOOL_BASE58;
}

static void apply_mode(void)
{
    s_ctx.tool = (tool_t)lv_dropdown_get_selected(s_ctx.dd_mode);
    bool is_text = text_tool(s_ctx.tool);
    bool is_hash = (s_ctx.tool == TOOL_HASH);
    bool has_algo = is_hash || s_ctx.tool == TOOL_HASHFILE;
    bool is_uuid = (s_ctx.tool == TOOL_UUID);

    set_visible(s_ctx.lbl_dir, is_text);
    set_visible(s_ctx.dd_dir, is_text);
    set_visible(s_ctx.lbl_algo, has_algo);
    set_visible(s_ctx.dd_algo, has_algo);
    set_visible(s_ctx.lbl_key, is_hash);
    set_visible(s_ctx.ta_key, is_hash);

    /* every tool except UUID has an input (hash file: a path) */
    set_visible(s_ctx.lbl_in, !is_uuid);
    set_visible(s_ctx.ta_in, !is_uuid);

    devos_w_set_text(s_ctx.lbl_in, input_caption(s_ctx.tool));
    devos_w_set_text(s_ctx.lbl_hint, tool_hint(s_ctx.tool));

    if (is_text) {
        const char *in = lv_textarea_get_text(s_ctx.ta_in);
        lv_dropdown_set_selected(s_ctx.dd_dir, coder_looks_encoded(in, strlen(in)) ? 1 : 0);
        s_ctx.dir_auto = true;
    }

    lv_obj_t *cur = devos_focus_get(&s_ctx.focus);
    if (cur && lv_obj_has_flag(cur, LV_OBJ_FLAG_HIDDEN)) devos_focus_set(&s_ctx.focus, s_ctx.dd_mode);

    compute();
}

static void set_mode(tool_t t)
{
    lv_dropdown_set_selected(s_ctx.dd_mode, (uint32_t)t);
    apply_mode();
}

static void mode_cb(lv_event_t *e) { LV_UNUSED(e); apply_mode(); }
static void dir_cb(lv_event_t *e) { LV_UNUSED(e); s_ctx.dir_auto = false; }

/* =========================================================================
 * Buttons
 * ========================================================================= */

static void compute_cb(lv_event_t *e) { LV_UNUSED(e); compute(); }

static void copy_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    /* a finished file hash lives in the engine, everything else in s_ctx.out */
    const char *src = (s_ctx.tool == TOOL_HASHFILE && devos_hashfile_state() == DEVOS_HASHFILE_DONE)
                          ? devos_hashfile_result()
                          : s_ctx.out;
    if (!src[0]) return;
    devos_clipboard_set(src, strlen(src));
    devos_toast_show("Copied to clipboard", DEVOS_TOAST_OK, 0);
}

static void swap_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_ctx.tool == TOOL_HASHFILE) return;
    lv_textarea_set_text(s_ctx.ta_in, s_ctx.out);
    compute();
}

static void clear_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_textarea_set_text(s_ctx.ta_in, "");
    s_ctx.out[0] = '\0';
    set_out("");
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

static void coder_init(void)
{
    if (!s_ctx.out) s_ctx.out = big_alloc(OUT_MAX);
    lv_obj_t *scr = s_ctx.screen = devos_w_screen(&s_desc);
    devos_w_bar(scr, "Coder's Toolkit", NULL);

    lv_obj_t *l = devos_w_label(scr, NULL, DEVOS_W_TEXT_DIM, "Tool");
    lv_obj_set_pos(l, 20, 54);
    s_ctx.dd_mode = devos_w_dd(scr,
        "Base64\nBase64 URL\nHex\nURL\nBase58\nCRC-32\nHash\nHash file\nJWT\nUUID\nUnix time", 230);
    lv_obj_set_pos(s_ctx.dd_mode, 20, 72);
    lv_obj_add_event_cb(s_ctx.dd_mode, mode_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_ctx.lbl_dir = devos_w_label(scr, NULL, DEVOS_W_TEXT_DIM, "Direction");
    lv_obj_set_pos(s_ctx.lbl_dir, 270, 54);
    s_ctx.dd_dir = devos_w_dd(scr, "Encode\nDecode", 140);
    lv_obj_set_pos(s_ctx.dd_dir, 270, 72);
    lv_obj_add_event_cb(s_ctx.dd_dir, dir_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_ctx.lbl_algo = devos_w_label(scr, NULL, DEVOS_W_TEXT_DIM, "Algorithm");
    lv_obj_set_pos(s_ctx.lbl_algo, 270, 54);
    s_ctx.dd_algo = devos_w_dd(scr, "SHA-1\nSHA-256\nSHA-512", 150);
    lv_obj_set_pos(s_ctx.dd_algo, 270, 72);
    lv_dropdown_set_selected(s_ctx.dd_algo, 1);          /* SHA-256 */

    s_ctx.lbl_key = devos_w_label(scr, NULL, DEVOS_W_TEXT_DIM, "HMAC key  (empty = plain hash)");
    lv_obj_set_pos(s_ctx.lbl_key, 440, 54);
    s_ctx.ta_key = devos_w_ta(scr, true, 340, 36);
    lv_obj_set_pos(s_ctx.ta_key, 440, 72);
    lv_textarea_set_max_length(s_ctx.ta_key, 128);

    s_ctx.lbl_hint = devos_w_label(scr, NULL, DEVOS_W_TEXT_MUTED, "");
    lv_obj_set_pos(s_ctx.lbl_hint, 20, 118);
    lv_obj_set_width(s_ctx.lbl_hint, DEVOS_SCREEN_WIDTH - 40);

    s_ctx.lbl_in = devos_w_label(scr, NULL, DEVOS_W_TEXT_DIM, "");
    lv_obj_set_pos(s_ctx.lbl_in, 20, 148);
    s_ctx.ta_in = devos_w_ta(scr, false, DEVOS_SCREEN_WIDTH - 40, 150);
    lv_obj_set_pos(s_ctx.ta_in, 20, 166);
    lv_textarea_set_max_length(s_ctx.ta_in, IN_MAX);

    /* output: a code view (scrolls, monospace, colours JSON) in a panel */
    l = devos_w_label(scr, NULL, DEVOS_W_TEXT_DIM, "Output");
    lv_obj_set_pos(l, 20, 330);
    s_ctx.out_panel = devos_w_panel(scr, 20, 348, DEVOS_SCREEN_WIDTH - 40, 236, DEVOS_W_CODE);
    s_ctx.out_scroll = lv_obj_create(s_ctx.out_panel);
    lv_obj_set_style_bg_opa(s_ctx.out_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ctx.out_scroll, 0, 0);
    lv_obj_set_style_pad_all(s_ctx.out_scroll, 6, 0);
    lv_obj_set_pos(s_ctx.out_scroll, 0, 0);
    lv_obj_set_size(s_ctx.out_scroll, DEVOS_SCREEN_WIDTH - 40, 236);
    devos_codeview_create(&s_ctx.cv, s_ctx.out_scroll);
    s_ctx.cv.plain = true;

    s_ctx.btn_compute = devos_w_btn_kind(scr, DEVOS_W_BTN_PRIMARY, LV_SYMBOL_OK " Compute  [C]", 160, compute_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_compute, 20, 598);
    s_ctx.btn_copy = devos_w_btn(scr, "Copy  [Y]", 120, copy_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_copy, 200, 598);
    s_ctx.btn_swap = devos_w_btn(scr, "Swap  [S]", 120, swap_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_swap, 340, 598);
    s_ctx.btn_clear = devos_w_btn(scr, "Clear  [W]", 120, clear_cb, NULL, NULL);
    lv_obj_set_pos(s_ctx.btn_clear, 480, 598);

    lv_obj_t *keys = devos_w_keys(scr);
    devos_w_set_text(keys, "Tab / Up / Down move  |  Left / Right change a value  |  "
                           "B U X R 5 K H F J I T pick the tool  |  C compute  Y copy  S swap  W clear  |  Esc back");

    devos_focus_init(&s_ctx.focus);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_mode);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_dir);
    devos_focus_add(&s_ctx.focus, s_ctx.dd_algo);
    devos_focus_add(&s_ctx.focus, s_ctx.ta_key);
    devos_focus_add(&s_ctx.focus, s_ctx.ta_in);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_compute);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_copy);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_swap);
    devos_focus_add(&s_ctx.focus, s_ctx.btn_clear);

    set_mode(TOOL_B64);
    lv_timer_create(tick_cb, 250, NULL);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_HIDDEN);
}

static void coder_show(void)
{
    if (!s_ctx.screen) return;
    lv_obj_remove_flag(s_ctx.screen, LV_OBJ_FLAG_HIDDEN);
    devos_focus_first(&s_ctx.focus);
}

static void coder_hide(void)
{
    if (s_ctx.screen) lv_obj_add_flag(s_ctx.screen, LV_OBJ_FLAG_HIDDEN);
}

static bool coder_handle_key(uint32_t key, uint8_t mods)
{
    if (devos_focus_key(&s_ctx.focus, key, mods)) return true;

    lv_obj_t *cur = devos_focus_get(&s_ctx.focus);
    bool in_field = cur && lv_obj_check_type(cur, &lv_textarea_class);

    if (in_field && cur == s_ctx.ta_key && (key == '\r' || key == '\n')) {
        compute();
        return true;
    }
    if (key == LV_KEY_ESC) {
        if (in_field) { devos_focus_clear(&s_ctx.focus); return true; }
        return false;
    }
    if (mods & (DEVOS_MOD_CTRL | DEVOS_MOD_FN | DEVOS_MOD_ALT)) return false;

    switch (key) {
    case 'c': case 'C': compute(); return true;
    case 'y': case 'Y': copy_cb(NULL); return true;
    case 's': case 'S': swap_cb(NULL); return true;
    case 'w': case 'W': clear_cb(NULL); return true;
    case 'b': case 'B': set_mode(TOOL_B64);      return true;
    case 'u': case 'U': set_mode(TOOL_B64URL);   return true;
    case 'x': case 'X': set_mode(TOOL_HEX);      return true;
    case 'r': case 'R': set_mode(TOOL_URL);      return true;
    case '5':           set_mode(TOOL_BASE58);   return true;
    case 'k': case 'K': set_mode(TOOL_CRC32);    return true;
    case 'h': case 'H': set_mode(TOOL_HASH);     return true;
    case 'f': case 'F': set_mode(TOOL_HASHFILE); return true;
    case 'j': case 'J': set_mode(TOOL_JWT);      return true;
    case 'i': case 'I': set_mode(TOOL_UUID);     return true;
    case 't': case 'T': set_mode(TOOL_EPOCH);    return true;
    default: return false;
    }
}

static int coder_telemetry(char lines[3][64])
{
    snprintf(lines[0], sizeof(lines[0]), "* %s", tool_name(s_ctx.tool));
    snprintf(lines[1], sizeof(lines[1]), "* Base64 / Hex / Base58");
    snprintf(lines[2], sizeof(lines[2]), "* Hash / CRC / JWT / UUID");
    return 3;
}

static const char *coder_shortcuts(void)
{
    return "Coder's Toolkit\n"
           "B / U / X / R / 5\tBase64 / b64url / Hex / URL / Base58\n"
           "K / H / F\tCRC-32 / Hash / Hash file\n"
           "J / I / T\tJWT / UUID / Unix time\n"
           "C\tCompute (or start the file hash)\n"
           "Y\tCopy the result\n"
           "S\tSwap the result into the input\n"
           "W\tClear\n"
           "Left / Right\tChange the focused dropdown\n";
}

devos_app_descriptor_t *app_coder_get_descriptor(void)
{
    s_desc.id = 0;
    s_desc.uid = "coder";
    s_desc.name = "Coder";
    s_desc.title = "Coder's Toolkit";
    s_desc.subtitle = "Base64, hashes, JWT, UUID";
    s_desc.icon = LV_SYMBOL_EDIT;
    s_desc.draw_icon = devos_icon_coder;
    s_desc.category = "tools";
    s_desc.init = coder_init;
    s_desc.show = coder_show;
    s_desc.hide = coder_hide;
    s_desc.handle_key = coder_handle_key;
    s_desc.get_telemetry_lines = coder_telemetry;
    s_desc.get_shortcuts = coder_shortcuts;
    return &s_desc;
}

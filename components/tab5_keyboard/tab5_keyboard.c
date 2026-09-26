#include "tab5_keyboard.h"
#include "devos_core.h"
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "nvs.h"
#include "lvgl.h"
#include "devos_config.h"

/* =========================================================================
 * Tab5 Keyboard (A164): STM32F030 register device on Ext.Port1 I2C @ 0x6D.
 *
 * The keys are Esc, number row, Tab, letters, arrows and four modifiers:
 * Sym, Aa, Ctrl, Alt. There is NO Fn key. In the firmware's HID mode the Sym
 * key is invisible (Sym+H arrives as a plain 'h'), so devOS runs the keyboard
 * in Normal (raw matrix) mode and does the mapping itself. That lets Sym act
 * as the system modifier: Sym + a key whose Sym layer is unused (letters,
 * digits, arrows, Space, Esc, Tab, Enter, Backspace, -, +) is reported with
 * DEVOS_MOD_FN; Sym + punctuation still types the Sym-layer symbol.
 *
 * Register map and matrix layout: M5Stack M5Unit-Keyboard (unit_Tab5Keyboard),
 * MIT licensed. Normal-mode event byte: [7]=pressed, [6:4]=row, [3:0]=col,
 * 0xFF = queue empty.
 * ========================================================================= */
#define KBD_REG_EVENT_NUM      0x02   /* write 0 to clear the event queue */
#define KBD_REG_BRIGHTNESS     0x03   /* RGB lights brightness 0-100 (default 20) */
#define KBD_REG_MODE_KEYBOARD  0x10   /* 0=Normal 1=HID 2=Character */
#define KBD_REG_MODE_RGB       0x11   /* 0=firmware drives the lights 1=custom colours */
#define KBD_REG_RGB1_B         0x60   /* left light B,G,R at 0x60..0x62 */
#define KBD_REG_RGB2_B         0x64   /* right light B,G,R at 0x64..0x66 */
#define KBD_REG_KEY_EVENT      0x20   /* Normal mode: 1 byte per event */
#define KBD_REG_FW_VERSION     0xFE
#define KBD_MODE_NORMAL        0
#define KBD_EVENT_EMPTY        0xFF

#define KBD_ROWS 5
#define KBD_COLS 14

#define KM_SYM   0x7F000001u
#define KM_AA    0x7F000002u
#define KM_CTRL  0x7F000003u
#define KM_ALT   0x7F000004u

#define KBD_REPEAT_DELAY_US  400000
#define KBD_REPEAT_RATE_US    45000

/* Base layer (US ANSI, as printed on the keycaps). */
static const uint32_t s_map_base[KBD_ROWS][KBD_COLS] = {
    { LV_KEY_ESC, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '+', LV_KEY_DEL },
    { '`', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '[', ']', '\\' },
    { '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', ';', '\'', '\b' },
    { KM_SYM, KM_AA, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', LV_KEY_UP, '_', '\r' },
    { KM_CTRL, KM_ALT, 'z', 'x', 'c', 'v', 'b', 'n', 'm', '.', LV_KEY_LEFT, LV_KEY_DOWN, LV_KEY_RIGHT, ' ' },
};

/* Sym layer: only these punctuation keys change. */
static const uint32_t s_map_sym[KBD_ROWS][KBD_COLS] = {
    { LV_KEY_ESC, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '+', LV_KEY_DEL },
    { '~', '?', '@', '#', '$', '%', '^', '&', '/', '<', '>', '{', '}', '|' },
    { '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', ':', '"', '\b' },
    { KM_SYM, KM_AA, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', LV_KEY_UP, '=', '\r' },
    { KM_CTRL, KM_ALT, 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', LV_KEY_LEFT, LV_KEY_DOWN, LV_KEY_RIGHT, ' ' },
};

static esp_err_t kbd_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TAB5_KBD_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, buf, 2, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(TAB5_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static void kbd_write_rgb(uint8_t base, uint32_t rgb)
{
    kbd_write_reg(base, (uint8_t)rgb);                   /* B */
    kbd_write_reg((uint8_t)(base + 1), (uint8_t)(rgb >> 8));   /* G */
    kbd_write_reg((uint8_t)(base + 2), (uint8_t)(rgb >> 16));  /* R */
}

static esp_err_t kbd_read_reg(uint8_t reg, uint8_t *buf, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TAB5_KBD_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TAB5_KBD_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, buf + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(TAB5_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return ret;
}
#endif

static uint8_t current_modifiers = DEVOS_MOD_NONE;

/* ---- indicator lights ----
 * Written on the GUI task only; the keyboard task applies them whenever
 * s_led_gen moves (and after a re-attach), so the UI never waits on I2C. */
static volatile uint8_t s_led_mode = TAB5_KBD_LIGHTS_STATUS;
static volatile uint8_t s_led_bright = 20;
static volatile uint32_t s_led_accent = 0x00E5FF;
static volatile bool s_led_off;
static volatile uint32_t s_led_gen = 1;

#define KBD_LIGHTS_NVS_NS  "devos"
#define KBD_LIGHTS_NVS_KEY "kbd_lights"   /* u16: mode | brightness << 8 */

void tab5_keyboard_set_lights(tab5_kbd_lights_t mode, uint8_t brightness, bool save)
{
    if (mode >= TAB5_KBD_LIGHTS_COUNT) mode = TAB5_KBD_LIGHTS_STATUS;
    if (brightness > 100) brightness = 100;
    if (mode != s_led_mode || brightness != s_led_bright) {
        s_led_mode = (uint8_t)mode;
        s_led_bright = brightness;
        s_led_gen++;
    }
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    uint16_t v = (uint16_t)(mode | (brightness << 8)), old = 0;
    if (save && nvs_open(KBD_LIGHTS_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_get_u16(h, KBD_LIGHTS_NVS_KEY, &old) != ESP_OK || old != v) {
            if (nvs_set_u16(h, KBD_LIGHTS_NVS_KEY, v) == ESP_OK) nvs_commit(h);
        }
        nvs_close(h);
    }
#else
    (void)save;
#endif
}

void tab5_keyboard_get_lights(tab5_kbd_lights_t *mode, uint8_t *brightness)
{
    if (mode) *mode = (tab5_kbd_lights_t)s_led_mode;
    if (brightness) *brightness = s_led_bright;
}

void tab5_keyboard_set_accent(uint32_t rgb)
{
    rgb &= 0xFFFFFF;
    if (rgb != s_led_accent) {
        s_led_accent = rgb;
        s_led_gen++;
    }
}

void tab5_keyboard_lights_suspend(bool off)
{
    if (off != s_led_off) {
        s_led_off = off;
        s_led_gen++;
    }
}

#ifdef ESP_PLATFORM
/* Keyboard events are produced on the keyboard task but must be *dispatched* on
 * the GUI task: app handle_key()s (and the app-switching they trigger) run LVGL,
 * which is single-threaded, and they need the GUI task's larger stack. Running
 * them on the 4 KB keyboard task overflowed it (stack-protection panic). So the
 * keyboard task only enqueues; the GUI task drains via tab5_keyboard_get_key(). */
typedef struct {
    uint32_t key;
    uint8_t  mods;
} kbd_event_t;

static QueueHandle_t s_kbd_queue = NULL;
static volatile bool s_connected = false;
static volatile bool s_caps = false;

bool tab5_keyboard_get_key(uint32_t *key, uint8_t *modifiers)
{
    if (s_kbd_queue == NULL) {
        return false;
    }
    kbd_event_t ev;
    if (xQueueReceive(s_kbd_queue, &ev, 0) != pdTRUE) {
        return false;
    }
    if (key)       *key = ev.key;
    if (modifiers) *modifiers = ev.mods;
    return true;
}

bool tab5_keyboard_is_connected(void) { return s_connected; }
bool tab5_keyboard_caps_lock(void) { return s_caps; }
#else
bool tab5_keyboard_get_key(uint32_t *key, uint8_t *modifiers)
{
    (void)key;
    (void)modifiers;
    return false;  /* simulator dispatches keys directly from the SDL watcher */
}

bool tab5_keyboard_is_connected(void) { return true; }
bool tab5_keyboard_caps_lock(void) { return false; }
#endif

void tab5_keyboard_inject_key(uint32_t key, uint8_t modifiers, bool pressed)
{
    current_modifiers = modifiers;

    if (!pressed) {
        return;
    }

#ifdef ESP_PLATFORM
    /* Hand off to the GUI task; never dispatch app/LVGL code here (see above). */
    if (s_kbd_queue != NULL) {
        kbd_event_t ev = { .key = key, .mods = modifiers };
        xQueueSend(s_kbd_queue, &ev, 0);
    }
#else
    /* Pass directly to devos_core hotkey & app dispatcher */
    devos_core_dispatch_key(key, modifiers);
#endif
}

uint8_t tab5_keyboard_get_modifiers(void)
{
    return current_modifiers;
}

#ifdef ESP_PLATFORM
static bool s_sym, s_aa, s_ctrl, s_alt;
static bool s_aa_alone;           /* Aa pressed with no other key: tap = caps lock */

static int      s_rep_row = -1, s_rep_col = -1;
static uint32_t s_rep_key;
static uint8_t  s_rep_mods;
static int64_t  s_rep_t0, s_rep_last;

static uint32_t us_shift(uint32_t c)
{
    switch (c) {
    case '1': return '!'; case '2': return '@'; case '3': return '#'; case '4': return '$';
    case '5': return '%'; case '6': return '^'; case '7': return '&'; case '8': return '*';
    case '9': return '('; case '0': return ')'; case '-': return '_'; case '=': return '+';
    case '[': return '{'; case ']': return '}'; case '\\': return '|'; case ';': return ':';
    case '\'': return '"'; case '`': return '~'; case ',': return '<'; case '.': return '>';
    case '/': return '?';
    default:  return c;
    }
}

/* Map a non-modifier key press to (key, modifiers). Returns whether it may
 * auto-repeat while held. */
static bool translate(int r, int c, uint32_t *key_out, uint8_t *mods_out)
{
    uint8_t mods = (s_ctrl ? DEVOS_MOD_CTRL : 0) | (s_alt ? DEVOS_MOD_ALT : 0) |
                   (s_aa ? DEVOS_MOD_SHIFT : 0);
    uint32_t base = s_map_base[r][c];
    uint32_t key;
    bool repeat = true;

    if (s_sym) {
        bool alnum = (base >= 'a' && base <= 'z') || (base >= '0' && base <= '9');
        if (alnum) {
            key = base;                 /* system shortcut, e.g. Sym+H = Home */
            mods |= DEVOS_MOD_FN;
            repeat = false;
        } else if (base == LV_KEY_UP) {
            key = DEVOS_KEY_PGUP;
        } else if (base == LV_KEY_DOWN) {
            key = DEVOS_KEY_PGDN;
        } else if (base == LV_KEY_LEFT || base == LV_KEY_RIGHT || base == '-' || base == '+') {
            key = base;                 /* page flip / brightness */
            mods |= DEVOS_MOD_FN;
        } else if (base == ' ' || base == '\t' || base == '\r' || base == '\b' ||
                   base == LV_KEY_ESC || base == LV_KEY_DEL) {
            key = base;
            mods |= DEVOS_MOD_FN;
            repeat = false;
        } else {
            key = s_map_sym[r][c];          /* Sym-layer symbol: ~ ? / < > { } | : " = , */
        }
    } else {
        key = base;
        if (key >= 'a' && key <= 'z') {
            /* Ctrl chords stay lowercase so handlers can match 'c', 's', ... */
            if ((s_aa != s_caps) && !s_ctrl) key -= 32;
        } else if (s_aa) {
            key = us_shift(key);
        }
    }
    if (key == LV_KEY_ESC) repeat = false;
    *key_out = key;
    *mods_out = mods;
    return repeat;
}

static void emit(uint32_t key, uint8_t mods)
{
    tab5_keyboard_inject_key(key, mods, true);
}

static void process_event(uint8_t raw, int *diag)
{
    bool pressed = (raw & 0x80) != 0;
    int r = (raw >> 4) & 0x07;
    int c = raw & 0x0F;
    if (r >= KBD_ROWS || c >= KBD_COLS) return;
    uint32_t base = s_map_base[r][c];

    if (*diag > 0) {
        printf("[kbd] event: %s row=%d col=%d\n", pressed ? "down" : "up", r, c);
        (*diag)--;
    }

    switch (base) {
    case KM_SYM:  s_sym = pressed; return;
    case KM_CTRL: s_ctrl = pressed; return;
    case KM_ALT:  s_alt = pressed; return;
    case KM_AA:
        s_aa = pressed;
        if (pressed) {
            s_aa_alone = true;
        } else if (s_aa_alone) {
            s_caps = !s_caps;           /* tap Aa = caps lock */
            printf("[kbd] caps lock %s\n", s_caps ? "on" : "off");
        }
        return;
    default:
        break;
    }

    if (!pressed) {
        if (r == s_rep_row && c == s_rep_col) s_rep_row = s_rep_col = -1;
        return;
    }
    s_aa_alone = false;

    uint32_t key;
    uint8_t mods;
    bool repeat = translate(r, c, &key, &mods);
    emit(key, mods);
    if (repeat) {
        s_rep_row = r;
        s_rep_col = c;
        s_rep_key = key;
        s_rep_mods = mods;
        s_rep_t0 = s_rep_last = esp_timer_get_time();
    } else {
        s_rep_row = s_rep_col = -1;
    }
}

static void reset_state(void)
{
    s_sym = s_aa = s_ctrl = s_alt = false;
    s_aa_alone = false;
    s_rep_row = s_rep_col = -1;
}

static uint32_t lights_colour(tab5_kbd_lights_t m)
{
    switch (m) {
    case TAB5_KBD_LIGHTS_ACCENT: return s_led_accent;
    case TAB5_KBD_LIGHTS_CYAN:   return 0x00E5FF;
    case TAB5_KBD_LIGHTS_GREEN:  return 0x00E676;
    case TAB5_KBD_LIGHTS_AMBER:  return 0xFFB300;
    case TAB5_KBD_LIGHTS_RED:    return 0xFF3030;
    case TAB5_KBD_LIGHTS_PURPLE: return 0xB060FF;
    case TAB5_KBD_LIGHTS_WHITE:  return 0xFFFFFF;
    default:                     return 0;
    }
}

static void lights_apply(bool caps)
{
    tab5_kbd_lights_t m = (tab5_kbd_lights_t)s_led_mode;
    if (s_led_off || m == TAB5_KBD_LIGHTS_OFF) {
        kbd_write_reg(KBD_REG_MODE_RGB, 1);
        kbd_write_rgb(KBD_REG_RGB1_B, 0);
        kbd_write_rgb(KBD_REG_RGB2_B, 0);
        return;
    }
    kbd_write_reg(KBD_REG_BRIGHTNESS, s_led_bright);
    if (m == TAB5_KBD_LIGHTS_STATUS) {
        kbd_write_reg(KBD_REG_MODE_RGB, 0);
        return;
    }
    uint32_t c = lights_colour(m);
    uint32_t caps_c = m == TAB5_KBD_LIGHTS_AMBER ? 0xFF3030 : 0xFFB300;
    kbd_write_reg(KBD_REG_MODE_RGB, 1);
    kbd_write_rgb(KBD_REG_RGB1_B, caps ? caps_c : c);
    kbd_write_rgb(KBD_REG_RGB2_B, c);
}

static void lights_load(void)
{
    nvs_handle_t h;
    uint16_t v = 0;
    if (nvs_open(KBD_LIGHTS_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u16(h, KBD_LIGHTS_NVS_KEY, &v) == ESP_OK) {
            tab5_keyboard_set_lights((tab5_kbd_lights_t)(v & 0xFF), (uint8_t)(v >> 8), false);
        }
        nvs_close(h);
    }
}

static void keyboard_task(void *pvParameters)
{
    uint32_t led_applied = 0;
    bool led_caps = false;
    (void)pvParameters;
    int failures = 0;
    int diag = 0;
    vTaskDelay(pdMS_TO_TICKS(50));  /* let the STM32 finish booting */

    while (1) {
        if (!s_connected) {
            /* (Re)attach: also covers a keyboard plugged in after boot. */
            if (kbd_write_reg(KBD_REG_MODE_KEYBOARD, KBD_MODE_NORMAL) == ESP_OK) {
                kbd_write_reg(KBD_REG_EVENT_NUM, 0x00);
                uint8_t fw = 0;
                kbd_read_reg(KBD_REG_FW_VERSION, &fw, 1);
                printf("[kbd] Tab5 keyboard attached (Normal mode), fw=0x%02x\n", fw);
                reset_state();
                led_applied = s_led_gen - 1;      /* re-apply the lights */
                failures = 0;
                diag = 10;
                s_connected = true;
            } else {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        for (int n = 0; n < 16; n++) {
            uint8_t raw = KBD_EVENT_EMPTY;
            if (kbd_read_reg(KBD_REG_KEY_EVENT, &raw, 1) != ESP_OK) {
                failures++;
                break;
            }
            failures = 0;
            if (raw == KBD_EVENT_EMPTY) break;
            process_event(raw, &diag);
        }
        if (failures >= 50) {       /* ~0.5 s of NACKs: keyboard detached */
            printf("[kbd] Tab5 keyboard detached\n");
            s_connected = false;
            reset_state();
            continue;
        }

        if (s_rep_row >= 0) {
            int64_t now = esp_timer_get_time();
            if (now - s_rep_t0 >= KBD_REPEAT_DELAY_US && now - s_rep_last >= KBD_REPEAT_RATE_US) {
                s_rep_last = now;
                emit(s_rep_key, s_rep_mods);
            }
        }
        current_modifiers = (s_ctrl ? DEVOS_MOD_CTRL : 0) | (s_alt ? DEVOS_MOD_ALT : 0) |
                            (s_aa ? DEVOS_MOD_SHIFT : 0) | (s_sym ? DEVOS_MOD_FN : 0);

        uint32_t gen = s_led_gen;
        bool caps = s_caps;
        bool colour = s_led_mode >= TAB5_KBD_LIGHTS_ACCENT && !s_led_off;
        if (gen != led_applied || (colour && caps != led_caps)) {
            lights_apply(caps);
            led_applied = gen;
            led_caps = caps;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
#endif

bool tab5_keyboard_init(void)
{
#ifdef ESP_PLATFORM
    /* GPIO 50 is the keyboard INT line; we poll the event register instead
     * (1 byte / 10 ms), which also detects hot-plug. */
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = (1ULL << TAB5_PIN_KBD_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE
    };
    gpio_config(&io_conf);

    /* Queue drained by the GUI task; keyboard task only produces into it. */
    s_kbd_queue = xQueueCreate(32, sizeof(kbd_event_t));
    if (s_kbd_queue == NULL) {
        printf("[kbd] failed to create key queue\n");
        return false;
    }

    lights_load();

    /* Keyboard polling task pinned to Core 1 (UI & Input core) */
    xTaskCreatePinnedToCore(keyboard_task, "tab5_kbd", 4096, NULL, 10, NULL, DEVOS_CORE_UI_INPUT);
#endif
    return true;
}

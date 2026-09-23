#include "tab5_keyboard.h"
#include "devos_core.h"
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "devos_config.h"

/* Translate a USB-HID boot-keyboard usage code to the ASCII / LV_KEY value the
 * devos_core hotkey table and app handle_key()s expect (they compare against
 * ASCII, e.g. 'h', '1', 'e'). Without this, raw HID usages (0x0B for 'h', etc.)
 * never match anything. If the A164 turns out not to emit standard HID usages,
 * the raw-report log in keyboard_task shows the real codes to remap here. */
static uint32_t hid_usage_to_key(uint8_t u, bool shift)
{
    if (u >= 0x04 && u <= 0x1D) {          /* a-z */
        char c = 'a' + (u - 0x04);
        return (uint32_t)(shift ? c - 32 : c);
    }
    if (u >= 0x1E && u <= 0x27) {           /* 1-9,0 row */
        static const char base[] = "1234567890";
        static const char shft[] = "!@#$%^&*()";
        return (uint32_t)(shift ? shft[u - 0x1E] : base[u - 0x1E]);
    }
    switch (u) {
        case 0x28: return '\r';             /* Enter */
        case 0x29: return LV_KEY_ESC;       /* Esc */
        case 0x2A: return '\b';             /* Backspace */
        case 0x2B: return '\t';             /* Tab */
        case 0x2C: return ' ';              /* Space */
        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        case 0x4F: return LV_KEY_RIGHT;
        case 0x50: return LV_KEY_LEFT;
        case 0x51: return LV_KEY_DOWN;
        case 0x52: return LV_KEY_UP;
        case 0x4B: return DEVOS_KEY_PGUP;
        case 0x4E: return DEVOS_KEY_PGDN;
        default:   return 0;                /* unmapped */
    }
}

/* Tab5 keyboard (STM32) register map — from M5's unit_Tab5Keyboard driver.
 * It is a register device: address a register, then read/write. A raw read
 * (no register) returns zeros, which is why the old code saw nothing. */
#define KBD_REG_INT_STAT       0x01
#define KBD_REG_EVENT_NUM      0x02   /* write 0 to clear the event queue */
#define KBD_REG_MODE_KEYBOARD  0x10   /* 0=Normal 1=HID 2=Character */
#define KBD_REG_HID_EVENT      0x30   /* read 2 bytes: [modifier, HID keycode] */
#define KBD_REG_FW_VERSION     0xFE
#define KBD_MODE_HID           1
#define KBD_EVENT_EMPTY        0xFF

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
#else
bool tab5_keyboard_get_key(uint32_t *key, uint8_t *modifiers)
{
    (void)key;
    (void)modifiers;
    return false;  /* simulator dispatches keys directly from the SDL watcher */
}
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
static void keyboard_task(void *pvParameters)
{
    /* The Tab5 keyboard STM32 is a register device (M5 unit_Tab5Keyboard):
     * put it in HID mode, then drain REG_HID_EVENT (2 bytes: modifier + HID
     * usage code) until it reports empty (0xFF 0xFF). */
    vTaskDelay(pdMS_TO_TICKS(50));  /* let the STM32 finish booting */
    esp_err_t mret = kbd_write_reg(KBD_REG_MODE_KEYBOARD, KBD_MODE_HID);
    kbd_write_reg(KBD_REG_INT_STAT, 0x00);
    kbd_write_reg(KBD_REG_EVENT_NUM, 0x00);
    uint8_t fw = 0;
    kbd_read_reg(KBD_REG_FW_VERSION, &fw, 1);
    printf("[kbd] Tab5 keyboard: HID mode %s, fw=0x%02x\n",
           mret == ESP_OK ? "set" : "FAILED", fw);

    int diag = 20;
    while (1) {
        /* Drain queued key events (bounded per tick). */
        for (int n = 0; n < 16; n++) {
            uint8_t buf[2] = {KBD_EVENT_EMPTY, KBD_EVENT_EMPTY};
            if (kbd_read_reg(KBD_REG_HID_EVENT, buf, sizeof(buf)) != ESP_OK) break;
            if (buf[0] == KBD_EVENT_EMPTY && buf[1] == KBD_EVENT_EMPTY) break; /* empty */

            uint8_t hidmod = buf[0];
            uint8_t keycode = buf[1];

            if (diag > 0) {
                printf("[kbd] hid event: mod=0x%02x key=0x%02x\n", hidmod, keycode);
                diag--;
            }

            uint8_t mods = 0;
            if (hidmod & 0x01) mods |= DEVOS_MOD_CTRL;
            if (hidmod & 0x02) mods |= DEVOS_MOD_SHIFT;
            if (hidmod & 0x04) mods |= DEVOS_MOD_ALT;
            if (hidmod & 0x08) mods |= DEVOS_MOD_FN;

            if (keycode != 0) {
                uint32_t key = hid_usage_to_key(keycode, (mods & DEVOS_MOD_SHIFT) != 0);
                if (key != 0) {
                    tab5_keyboard_inject_key(key, mods, true);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}
#endif

bool tab5_keyboard_init(void)
{
#ifdef ESP_PLATFORM
    /* Configure GPIO 50 for INT */
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .pin_bit_mask = (1ULL << TAB5_PIN_KBD_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE
    };
    gpio_config(&io_conf);

    /* Queue drained by the GUI task; keyboard task only produces into it. */
    s_kbd_queue = xQueueCreate(16, sizeof(kbd_event_t));
    if (s_kbd_queue == NULL) {
        printf("[kbd] failed to create key queue\n");
        return false;
    }

    /* Spawn keyboard polling task pinned to Core 1 (UI & Input core) */
    xTaskCreatePinnedToCore(keyboard_task, "tab5_kbd", 4096, NULL, 10, NULL, DEVOS_CORE_UI_INPUT);
#endif
    return true;
}

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
#endif

static uint8_t current_modifiers = DEVOS_MOD_NONE;

void tab5_keyboard_inject_key(uint32_t key, uint8_t modifiers, bool pressed)
{
    current_modifiers = modifiers;

    if (pressed) {
        /* Pass directly to devos_core hotkey & app dispatcher */
        devos_core_dispatch_key(key, modifiers);
    }
}

uint8_t tab5_keyboard_get_modifiers(void)
{
    return current_modifiers;
}

#ifdef ESP_PLATFORM
static void keyboard_task(void *pvParameters)
{
    /* I2C Read loop for STM32F030 HID reports */
    while (1) {
        /* Wait for INT pin (GPIO 50) low signal */
        if (gpio_get_level(TAB5_PIN_KBD_INT) == 0) {
            uint8_t report[8];
            i2c_cmd_handle_t cmd = i2c_cmd_link_create();
            i2c_master_start(cmd);
            i2c_master_write_byte(cmd, (TAB5_KBD_I2C_ADDR << 1) | I2C_MASTER_READ, true);
            i2c_master_read(cmd, report, sizeof(report), I2C_MASTER_LAST_NACK);
            i2c_master_stop(cmd);
            esp_err_t ret = i2c_master_cmd_begin(TAB5_I2C_PORT, cmd, pdMS_TO_TICKS(50));
            i2c_cmd_link_delete(cmd);

            uint8_t any = report[0];
            for (int i = 2; i < 8; i++) any |= report[i];

            if (ret == ESP_OK && any) {
                /* report[0] = modifiers, report[2..7] = HID key usages */
                uint8_t mods = 0;
                if (report[0] & 0x01) mods |= DEVOS_MOD_CTRL;
                if (report[0] & 0x02) mods |= DEVOS_MOD_SHIFT;
                if (report[0] & 0x04) mods |= DEVOS_MOD_ALT;
                if (report[0] & 0x08) mods |= DEVOS_MOD_FN;

                /* Diagnostic: show the raw report so the A164's real key codes
                 * can be confirmed against hid_usage_to_key() above. */
                printf("[kbd] raw: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                       report[0], report[1], report[2], report[3],
                       report[4], report[5], report[6], report[7]);

                bool shift = (mods & DEVOS_MOD_SHIFT) != 0;
                for (int i = 2; i < 8; i++) {
                    if (report[i] != 0) {
                        uint32_t key = hid_usage_to_key(report[i], shift);
                        if (key != 0) {
                            tab5_keyboard_inject_key(key, mods, true);
                        }
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
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

    /* Spawn keyboard polling task pinned to Core 1 (UI & Input core) */
    xTaskCreatePinnedToCore(keyboard_task, "tab5_kbd", 4096, NULL, 10, NULL, DEVOS_CORE_UI_INPUT);
#endif
    return true;
}

#include "tab5_keyboard.h"
#include "devos_core.h"
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
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

            if (ret == ESP_OK) {
                /* report[0] = modifiers, report[2..7] = keys */
                uint8_t mods = 0;
                if (report[0] & 0x01) mods |= DEVOS_MOD_CTRL;
                if (report[0] & 0x02) mods |= DEVOS_MOD_SHIFT;
                if (report[0] & 0x04) mods |= DEVOS_MOD_ALT;
                if (report[0] & 0x08) mods |= DEVOS_MOD_FN;

                for (int i = 2; i < 8; i++) {
                    if (report[i] != 0) {
                        tab5_keyboard_inject_key(report[i], mods, true);
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

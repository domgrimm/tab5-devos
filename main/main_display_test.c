/* Display-only diagnostic firmware (THROWAWAY).
 *
 * Selected instead of main.c when DEVOS_DISPLAY_TEST is set in main/CMakeLists.txt.
 * Links ONLY the display stack (bsp_tab5 + esp_lcd + lvgl) -- no Wi-Fi/ESP-Hosted,
 * SSH, storage, keyboard, or apps -- which frees enough internal RAM to enable the
 * 256 KB L2 cache. Brings up the MIPI-DSI panel, paints static colour bars, then
 * idles. Purpose: determine whether the 256 KB cache (M5GFX's one remaining config
 * difference) removes the scanout flicker.
 */
#include "bsp_tab5.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

void app_main(void)
{
    printf("\n[DISPLAY-TEST] Display-only + 256KB L2 cache build (Wi-Fi/SSH stripped).\n");

    /* LVGL must be initialised before bsp_tab5_init(), which creates the LVGL
     * display object during panel bring-up. */
    lv_init();

    bsp_tab5_init();               /* LDO, MIPI-DSI, ST7121 panel, DPI, touch */
    bsp_tab5_fill_test_pattern();  /* static colour bars straight into the FB */

    printf("[DISPLAY-TEST] Static colour bars shown. Observe whether they flicker.\n");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

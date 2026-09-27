idf_component_register(SRCS "bsp_tab5.c" "esp_lcd_st7123.c" "esp_lcd_ili9881c.c"
                       INCLUDE_DIRS "."
                       REQUIRES "lvgl" "devos_config" "driver" "esp_driver_i2c" "esp_lcd" "esp_mm")

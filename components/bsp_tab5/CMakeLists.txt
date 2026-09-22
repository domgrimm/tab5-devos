idf_component_register(SRCS "bsp_tab5.c" "bsp_tab5_camera.c" "esp_lcd_st7123.c"
                       INCLUDE_DIRS "."
                       REQUIRES "lvgl" "quirc" "devos_config" "driver" "esp_lcd")

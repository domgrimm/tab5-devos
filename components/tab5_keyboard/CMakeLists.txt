idf_component_register(
    SRCS "tab5_keyboard.c"
    INCLUDE_DIRS "."
    REQUIRES devos_core devos_config driver esp_driver_i2c bsp_tab5 lvgl esp_timer nvs_flash
)

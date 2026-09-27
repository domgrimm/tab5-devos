idf_component_register(
    SRCS "devos_core.c" "devos_apps.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config devos_power lvgl nvs_flash heap
)

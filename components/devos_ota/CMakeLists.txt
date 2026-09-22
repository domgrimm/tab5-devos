idf_component_register(
    SRCS "devos_ota.c"
    INCLUDE_DIRS "."
    REQUIRES devos_core devos_net devos_json nvs_flash esp_https_ota
)

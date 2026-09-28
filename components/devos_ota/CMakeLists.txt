idf_component_register(
    SRCS "devos_ota.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config
    PRIV_REQUIRES devos_json devos_http devos_crypto nvs_flash app_update esp_app_format
)

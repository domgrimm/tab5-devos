idf_component_register(
    SRCS "devos_ota.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config
    PRIV_REQUIRES devos_json nvs_flash app_update esp_http_client mbedtls
)

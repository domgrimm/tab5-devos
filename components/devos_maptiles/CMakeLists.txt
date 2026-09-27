idf_component_register(
    SRCS "devos_maptiles.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config
    PRIV_REQUIRES devos_http esp_timer
)

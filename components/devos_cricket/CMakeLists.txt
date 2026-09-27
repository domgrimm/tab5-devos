idf_component_register(
    SRCS "devos_cricket.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config
    PRIV_REQUIRES devos_http devos_json esp_timer
)

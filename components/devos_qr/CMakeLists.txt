idf_component_register(
    SRCS "devos_qr.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config
    PRIV_REQUIRES bsp_tab5 quirc esp_timer
)

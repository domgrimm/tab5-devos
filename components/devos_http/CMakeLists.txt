idf_component_register(
    SRCS "devos_http.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config
    PRIV_REQUIRES devos_net mbedtls esp_timer lwip
)

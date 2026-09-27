idf_component_register(
    SRCS "devos_http.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config
    PRIV_REQUIRES devos_net mbedtls esp_timer lwip
)

# UI / log strings are cut to fit on purpose
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)

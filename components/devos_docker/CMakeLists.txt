idf_component_register(
    SRCS "devos_docker.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config
    PRIV_REQUIRES devos_http devos_json esp_timer nvs_flash
)

# UI / log strings are cut to fit on purpose
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)

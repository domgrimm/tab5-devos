idf_component_register(
    SRCS "devos_secrets.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config devos_err
    PRIV_REQUIRES devos_crypto nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)

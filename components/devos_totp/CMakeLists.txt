idf_component_register(
    SRCS "devos_totp.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config
    PRIV_REQUIRES devos_crypto esp_timer nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)

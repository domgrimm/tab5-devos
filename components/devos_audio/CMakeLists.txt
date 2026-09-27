idf_component_register(
    SRCS "devos_audio.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config
    PRIV_REQUIRES bsp_tab5 esp_driver_i2s esp_driver_i2c
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)

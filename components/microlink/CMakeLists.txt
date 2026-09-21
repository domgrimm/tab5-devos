idf_component_register(
    SRCS "microlink.c"
    INCLUDE_DIRS "."
    REQUIRES devos_core devos_config nvs_flash
)

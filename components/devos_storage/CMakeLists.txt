idf_component_register(
    SRCS "devos_storage.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config fatfs sdmmc driver
)

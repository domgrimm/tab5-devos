idf_component_register(
    SRCS "opendev_client.c"
    INCLUDE_DIRS "."
    REQUIRES devos_core devos_net devos_conn devos_storage devos_json devos_config nvs_flash lwip
)

idf_component_register(
    SRCS "agy_client.c"
    INCLUDE_DIRS "."
    REQUIRES devos_core devos_net devos_storage devos_json
)

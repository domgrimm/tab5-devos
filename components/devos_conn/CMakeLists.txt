idf_component_register(
    SRCS "devos_conn.c"
    INCLUDE_DIRS "."
    REQUIRES devos_net devos_core
    PRIV_REQUIRES mbedtls lwip
)

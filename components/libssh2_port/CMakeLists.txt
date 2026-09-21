idf_component_register(
    SRCS "libssh2_port.c"
    INCLUDE_DIRS "."
    REQUIRES devos_core devos_net devos_storage mbedtls
)

idf_component_register(
    SRCS "devos_hashfile.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config
    PRIV_REQUIRES devos_crypto
)

# UI / log strings are cut to fit on purpose
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)

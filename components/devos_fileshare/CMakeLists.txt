idf_component_register(
    SRCS "devos_fileshare.c" "fileshare_page.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config
    PRIV_REQUIRES devos_net devos_storage devos_crypto devos_json lwip
)

# status strings are cut to fit on purpose
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)

idf_component_register(
    SRCS "nd_common.c" "nd_ping.c" "nd_dns.c" "nd_scan.c" "nd_wol.c" "nd_probe.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config
    PRIV_REQUIRES devos_net esp_timer lwip
)

# UI / log strings are cut to fit on purpose
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)

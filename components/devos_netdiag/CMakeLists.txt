idf_component_register(
    SRCS "nd_common.c" "nd_ping.c" "nd_dns.c" "nd_scan.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config
    PRIV_REQUIRES devos_net esp_timer lwip
)

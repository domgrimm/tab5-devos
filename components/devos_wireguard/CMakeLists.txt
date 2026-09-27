idf_component_register(
    SRCS "devos_wireguard.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config devos_net
    PRIV_REQUIRES devos_tailnet wireguard_lwip lwip esp_netif nvs_flash
)

idf_component_register(
    SRCS "devos_sysmon.c"
    INCLUDE_DIRS "."
    REQUIRES devos_core devos_config devos_net devos_storage devos_tailnet devos_wireguard bsp_tab5
    PRIV_REQUIRES esp_timer esp_netif lwip nvs_flash
)

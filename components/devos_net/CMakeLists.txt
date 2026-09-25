idf_component_register(
    SRCS "devos_net.c"
    INCLUDE_DIRS "."
    REQUIRES devos_core devos_config devos_tailnet esp_wifi esp_event esp_netif esp_timer lwip nvs_flash
)

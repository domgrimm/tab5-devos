idf_component_register(
    SRCS "devos_tailnet.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config
    PRIV_REQUIRES microlink nvs_flash esp_netif lwip
)

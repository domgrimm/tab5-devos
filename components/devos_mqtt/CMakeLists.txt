idf_component_register(
    SRCS "devos_mqtt.c"
    INCLUDE_DIRS "."
    REQUIRES devos_config devos_json devos_net
    PRIV_REQUIRES nvs_flash esp_timer
)

idf_component_register(
    SRCS "main.c"
    REQUIRES esp_driver_spi esp_driver_gpio esp_event esp_netif esp_wifi nvs_flash esp_http_client
)

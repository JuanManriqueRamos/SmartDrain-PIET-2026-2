idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES console esp_timer esp_driver_gpio esp_event esp_netif esp_wifi nvs_flash nopnop2002__sx126x
)
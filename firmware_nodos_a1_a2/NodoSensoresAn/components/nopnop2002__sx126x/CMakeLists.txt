idf_component_register(
    SRCS "ra01s.c"
    REQUIRES esp_driver_spi
    PRIV_REQUIRES driver esp_driver_gpio
    INCLUDE_DIRS "."
)
idf_component_register(SRCS "sdlog.cpp"
                       INCLUDE_DIRS include
                       REQUIRES racebox
                       PRIV_REQUIRES fatfs sdmmc esp_driver_spi esp_driver_gpio esp_hal_gpspi esp_rom nvs_flash esp_timer)

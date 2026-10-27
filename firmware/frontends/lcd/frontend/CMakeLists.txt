# Status display on the Waveshare ESP32-C6-GEEK's ST7789 LCD (RACEBOX_FRONTEND=lcd).
idf_component_register(SRCS "frontend_lcd.cpp"
                       PRIV_REQUIRES bridge_status esp_lcd esp_driver_gpio esp_driver_spi esp_timer)

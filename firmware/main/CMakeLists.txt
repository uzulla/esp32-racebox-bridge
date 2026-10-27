idf_component_register(
    SRCS "main.cpp"
    PRIV_REQUIRES racebox bridge_status frontend bt esp_wifi esp_netif esp_event esp_timer esp_pm nvs_flash lwip
                  esp_driver_usb_serial_jtag)
# C++20 designated initializers leave unnamed members zeroed, which is intended
# for the NimBLE/esp_timer config structs.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)

idf_component_register(
    SRCS "main.cpp"
    PRIV_REQUIRES racebox sdlog bridge_status frontend bt esp_wifi esp_netif esp_event esp_timer esp_pm nvs_flash lwip
                  esp_driver_usb_serial_jtag)
# C++20 designated initializers leave unnamed members zeroed, which is intended
# for the NimBLE/esp_timer config structs.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)

# Soft-AP credentials: new random values on every build, written to the build directory
# (ap_credentials.h) so they are never committed. `mise run fw:ap` shows the last build's.
idf_build_get_property(python PYTHON)
set(ap_credentials ${CMAKE_CURRENT_BINARY_DIR}/ap_credentials.h)
add_custom_target(racebox_ap_credentials
    COMMAND ${python} ${CMAKE_CURRENT_LIST_DIR}/gen_ap_credentials.py ${ap_credentials}
    BYPRODUCTS ${ap_credentials}
    VERBATIM)
add_dependencies(${COMPONENT_LIB} racebox_ap_credentials)
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

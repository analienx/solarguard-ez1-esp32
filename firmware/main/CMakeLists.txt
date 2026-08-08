idf_component_register(
    SRCS
        "app_main.c"
        "config_store.c"
        "ez1_ble.c"
        "ez1_http.c"
        "lease_server.c"
        "safety_state.c"
        "wifi_manager.c"
    INCLUDE_DIRS "."
    REQUIRES bt esp_driver_gpio esp_driver_usb_serial_jtag esp_event esp_http_client esp_http_server esp_netif esp_timer esp_wifi mbedtls nvs_flash spi_flash
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE WATCHDOG_VERSION="0.1.0")

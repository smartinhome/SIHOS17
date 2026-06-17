idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        wifi_manager
        webserver
        cc1101
        wmbus_decoder
        history
        led_rx
        meter_total
        ota_manager
        nvs_config
        log_buffer
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        driver
        spi_flash
        log
)

idf_component_register(
    SRCS "main.c" "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        wifi_manager
        webserver
        cc1101
        wmbus_decoder
        ota_manager
        nvs_config
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        driver
        spi_flash
        log
)

idf_component_register(
    SRCS "ota_manager.c"
    INCLUDE_DIRS "."
    REQUIRES esp_https_ota esp_http_client cc1101 log nvs_flash app_update
             mbedtls esp_app_format
)

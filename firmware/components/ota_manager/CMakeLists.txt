idf_component_register(
    SRCS "ota_manager.c"
    INCLUDE_DIRS "."
    REQUIRES esp_https_ota mbedtls esp_http_client log nvs_flash app_update
)

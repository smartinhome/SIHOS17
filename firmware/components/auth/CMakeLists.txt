idf_component_register(
    SRCS "auth.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server nvs_flash nvs_config mbedtls esp_hw_support esp_timer json lwip log
)

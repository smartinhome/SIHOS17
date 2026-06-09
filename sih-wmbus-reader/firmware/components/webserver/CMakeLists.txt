idf_component_register(
    SRCS "webserver.c" "api_handlers.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server wifi_manager wmbus_decoder nvs_config ota_manager log esp_timer
)

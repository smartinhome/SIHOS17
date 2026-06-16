idf_component_register(
    SRCS "wmbus_decoder.c"
    INCLUDE_DIRS "."
    REQUIRES cc1101 nvs_config log freertos meter_total history esp_timer
)

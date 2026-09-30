idf_component_register(
    SRCS "mqtt_pub.c"
    INCLUDE_DIRS "."
    REQUIRES mqtt nvs_config history esp_event log esp_timer
)

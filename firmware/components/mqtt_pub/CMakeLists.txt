idf_component_register(
    SRCS "mqtt_pub.c"
    INCLUDE_DIRS "."
    REQUIRES mqtt nvs_config esp_event log
)

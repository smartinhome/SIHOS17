idf_component_register(
    SRCS "history.c"
    INCLUDE_DIRS "."
    REQUIRES spiffs esp_timer nvs_flash
)

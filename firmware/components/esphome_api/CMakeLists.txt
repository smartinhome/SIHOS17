idf_component_register(
    SRCS "esphome_api.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_config history nvs_flash lwip esp_hw_support esp_timer log esp_app_format
)

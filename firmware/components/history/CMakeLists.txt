idf_component_register(
    SRCS "history.c" "hist_store.c" "hist_migrate.c"
    INCLUDE_DIRS "."
    REQUIRES spiffs joltwallet__littlefs esp_partition app_update esp_rom esp_timer nvs_flash
)

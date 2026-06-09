idf_component_register(
    SRCS "cc1101.c"
    INCLUDE_DIRS "."
    REQUIRES driver log esp_timer freertos
)

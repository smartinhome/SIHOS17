idf_component_register(
    SRCS "nvs_config.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash log
)

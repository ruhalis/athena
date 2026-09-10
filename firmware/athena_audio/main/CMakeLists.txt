idf_component_register(SRCS "main.c" "audio.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES athena_common esp_timer esp_driver_i2s esp_driver_gpio lwip)

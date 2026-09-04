idf_component_register(SRCS "main.c" "face.c" "serial.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES hub75 esp_timer json esp_driver_uart)

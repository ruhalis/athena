idf_component_register(SRCS "main.c" "face.c" "aura.c" "command.c" "serial.c" "net.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES hub75 athena_common esp_timer json esp_driver_uart lwip)

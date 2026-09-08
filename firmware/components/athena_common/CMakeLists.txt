# athena_common: code shared by every Athena board (Wi-Fi station with
# reconnect, mDNS name and service). Board projects pull it in through
# EXTRA_COMPONENT_DIRS in their top-level CMakeLists.txt.
idf_component_register(SRCS "athena_wifi.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_timer)

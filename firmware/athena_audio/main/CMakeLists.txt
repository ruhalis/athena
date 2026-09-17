# sr.c (the wake word and the VAD) is in the build only with
# CONFIG_ATHENA_AUDIO_SR, see Kconfig.projbuild.
set(srcs "main.c" "audio.c")
if(CONFIG_ATHENA_AUDIO_SR)
    list(APPEND srcs "sr.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES athena_common esp_timer esp_driver_i2s esp_driver_gpio lwip)

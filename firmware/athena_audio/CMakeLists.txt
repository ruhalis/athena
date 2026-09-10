cmake_minimum_required(VERSION 3.16)

# Athena audio: the microphone and amplifier board of AUDIO-BOARD.md on an
# ESP32-S3-DevKitC-1. Wi-Fi and mDNS come from the shared
# firmware/components/athena_common.
set(EXTRA_COMPONENT_DIRS ../components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(athena_audio)

cmake_minimum_required(VERSION 3.16)

# Athena face: Waveshare RGB-Matrix-P3-64x64 (HUB75E) on an ESP32 (WROOM-32 or S3).
# The HUB75 driver lives in components/hub75, which ESP-IDF picks up automatically;
# Wi-Fi and mDNS come from the shared firmware/components/athena_common.
set(EXTRA_COMPONENT_DIRS ../components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(athena_matrix)

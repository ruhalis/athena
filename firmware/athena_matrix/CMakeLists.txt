cmake_minimum_required(VERSION 3.16)

# Athena face: Waveshare RGB-Matrix-P3-64x64 (HUB75E) on a classic ESP32-WROOM-32.
# The HUB75 driver lives in components/hub75, which ESP-IDF picks up automatically.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(athena_matrix)

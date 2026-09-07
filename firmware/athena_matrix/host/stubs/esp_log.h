/* esp_log.h - host stub. Prints straight to stderr instead of the IDF log
 * subsystem; format matches the real "I (tag) fmt" shape closely enough for a
 * host run to be readable. */
#pragma once

#include <stdio.h>

#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E (%s) " fmt "\n", tag, ##__VA_ARGS__)

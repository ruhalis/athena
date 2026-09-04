/* face.h - the `render` task: owns the panel contents, driven by face_cmd_t from one queue. */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"

/* Start the 40 fps render task on core 0. It boots in `test` (sticky) and
 * applies whatever arrives on `queue` (items are face_cmd_t) at the next frame. */
esp_err_t face_start(QueueHandle_t queue);

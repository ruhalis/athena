/* serial.h - the `serial` task: UART0 lines in, face_cmd_t out, ok/err back. */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"

/* Install the UART0 driver (console pins and baud rate unchanged), route the
 * console's stdout through it, and start the reader task on core 0. Parsed
 * commands go to `queue`, whose items are face_cmd_t. */
esp_err_t serial_start(QueueHandle_t queue);

/* serial.h - the `serial` task: UART0 lines in, ok/err back (command.h does the rest). */
#pragma once

#include "esp_err.h"

/* Install the UART0 driver (console pins and baud rate unchanged), route the
 * console's stdout through it, and start the reader task on core 0.
 * cmd_init() must have run first. */
esp_err_t serial_start(void);

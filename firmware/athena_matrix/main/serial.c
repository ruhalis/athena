/* serial.c - the `serial` task. Reads UART0 byte by byte, assembles lines
 * with command.c and answers on the same port.
 *
 * UART0 is also the console. Once the driver is installed, stdout is switched
 * to go through it and esp_log is pointed at log_vprintf(), which writes each
 * log line with one uart_write_bytes() call. That call holds the driver's TX
 * mutex for the whole line, as a reply does, so a log line and a reply can
 * never be spliced into each other.
 */
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_check.h"
#include "esp_log.h"

#include "command.h"
#include "serial.h"

static const char *TAG = "serial";

#define SERIAL_UART         UART_NUM_0
#define SERIAL_BAUD         115200
#define SERIAL_RX_BUF       1024

/* esp_log hook: one log line, one write. Lines longer than the buffer are cut
 * but still end with LF so the Mac side keeps its line framing. */
static int log_vprintf(const char *fmt, va_list ap)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) return n;
    if (n >= (int)sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
        buf[n - 1] = '\n';
    }
    uart_write_bytes(SERIAL_UART, buf, (size_t)n);
    return n;
}

static void reply(void *ctx, const char *text, size_t len)
{
    (void)ctx;
    uart_write_bytes(SERIAL_UART, text, len);
}

static void serial_task(void *arg)
{
    static cmd_line_t line;
    (void)arg;
    cmd_line_reset(&line);

    for (;;) {
        uint8_t c;
        if (uart_read_bytes(SERIAL_UART, &c, 1, portMAX_DELAY) != 1) continue;
        if (cmd_line_feed(&line, (char)c)) cmd_handle(&line, reply, NULL);
    }
}

esp_err_t serial_start(void)
{
    const uart_config_t uc = {
        .baud_rate = SERIAL_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* Let the boot log leave through the pre-driver console path before switching. */
    fflush(stdout);
    fsync(fileno(stdout));
    ESP_RETURN_ON_ERROR(uart_driver_install(SERIAL_UART, SERIAL_RX_BUF, 0, 0, NULL, 0), TAG, "uart_driver_install");
    ESP_RETURN_ON_ERROR(uart_param_config(SERIAL_UART, &uc), TAG, "uart_param_config");
    uart_vfs_dev_use_driver(SERIAL_UART);
    esp_log_set_vprintf(log_vprintf);

    BaseType_t ok = xTaskCreatePinnedToCore(serial_task, "serial", 4096, NULL, 4, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "serial task");

    ESP_LOGI(TAG, "UART%d %d 8N1, lines up to %d bytes", (int)SERIAL_UART, SERIAL_BAUD, FACE_LINE_MAX);
    return ESP_OK;
}

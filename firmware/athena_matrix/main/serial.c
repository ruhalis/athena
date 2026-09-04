/* serial.c - the `serial` task. Assembles LF-terminated lines from UART0,
 * parses each as one JSON object, validates it against protocol.h, posts the
 * resulting face_cmd_t to the render task's queue and answers `ok` or
 * `err <reason>` on the same port.
 *
 * UART0 is also the console. Once the driver is installed, stdout is switched
 * to go through it and esp_log is pointed at log_vprintf(), which writes each
 * log line with one uart_write_bytes() call. That call holds the driver's TX
 * mutex for the whole line, as reply_ok()/reject() do for a reply, so a log
 * line and a reply can never be spliced into each other.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_check.h"
#include "esp_log.h"
#include "cJSON.h"

#include "protocol.h"
#include "serial.h"

static const char *TAG = "serial";

#define SERIAL_UART         UART_NUM_0
#define SERIAL_BAUD         115200
#define SERIAL_RX_BUF       1024
#define REPLY_MAX           48
#define LOG_PREVIEW         64          /* how much of a rejected line goes to the log */

static QueueHandle_t s_queue;

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

static void reply_ok(void)
{
    static const char ok[] = "ok\n";
    uart_write_bytes(SERIAL_UART, ok, sizeof(ok) - 1);
}

static void reject(const char *reason, const char *line)
{
    char out[REPLY_MAX];
    int n = snprintf(out, sizeof(out), "err %s\n", reason);
    if (n >= (int)sizeof(out)) n = (int)sizeof(out) - 1;
    ESP_LOGW(TAG, "rejected (%s): %.*s", reason, LOG_PREVIEW, line);
    uart_write_bytes(SERIAL_UART, out, (size_t)n);
}

/* An integer in lo..hi, refusing fractions and anything that is not a number. */
static bool json_int(const cJSON *item, int32_t lo, int32_t hi, int32_t *out)
{
    if (!cJSON_IsNumber(item)) return false;
    double d = item->valuedouble;
    if (d < (double)lo || d > (double)hi) return false;
    int32_t v = (int32_t)d;
    if ((double)v != d) return false;
    *out = v;
    return true;
}

/* Fill `cmd` from the recognised keys of `root`. Returns NULL when every
 * present key is valid, else the reason for `err <reason>`. Unknown keys are
 * ignored, so `{}` and objects with only unknown keys come back clean and empty. */
static const char *parse_object(const cJSON *root, face_cmd_t *cmd)
{
    const cJSON *item;
    int32_t v;

    item = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (item) {
        if (!cJSON_IsString(item) || !face_mode_from_string(item->valuestring, &cmd->mode)) {
            return "bad mode";
        }
        cmd->has_mode = true;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (item) {
        if (!cJSON_IsString(item)) return "bad t";
        size_t len = strlen(item->valuestring);
        if (len > FACE_TEXT_MAX) return "bad t";
        memcpy(cmd->text, item->valuestring, len + 1);
        cmd->has_text = true;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "ttl");
    if (item) {
        if (!json_int(item, 0, INT32_MAX, &v)) return "bad ttl";
        cmd->ttl_s = v;
        cmd->has_ttl = true;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "brightness");
    if (item) {
        if (!json_int(item, 0, 255, &v)) return "bad brightness";
        cmd->brightness = (uint8_t)v;
        cmd->has_brightness = true;
    }

    return NULL;
}

static void handle_line(const char *line, size_t len)
{
    face_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));

    cJSON *root = cJSON_ParseWithLength(line, len);
    const char *reason = "bad json";
    if (root && cJSON_IsObject(root)) {
        reason = parse_object(root, &cmd);
    }
    cJSON_Delete(root);

    if (reason) {
        reject(reason, line);
        return;
    }
    if (cmd.has_mode || cmd.has_text || cmd.has_ttl || cmd.has_brightness) {
        if (xQueueSend(s_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
            reject("busy", line);
            return;
        }
    }
    reply_ok();                     /* a ping, or a command the render task now owns */
}

static void serial_task(void *arg)
{
    /* One byte over the cap so a 256-byte line followed by CR LF still counts as 256. */
    static char line[FACE_LINE_MAX + 2];
    size_t len = 0;
    bool too_long = false;
    (void)arg;

    for (;;) {
        uint8_t c;
        if (uart_read_bytes(SERIAL_UART, &c, 1, portMAX_DELAY) != 1) continue;

        if (c != '\n') {
            if (len <= FACE_LINE_MAX) {
                line[len++] = (char)c;
            } else {
                too_long = true;            /* keep draining until the LF */
            }
            continue;
        }

        if (len && line[len - 1] == '\r') len--;
        line[len] = '\0';
        if (too_long || len > FACE_LINE_MAX) {
            reject("too long", line);
        } else if (len) {
            handle_line(line, len);         /* an empty line is not a command: ignored */
        }
        len = 0;
        too_long = false;
    }
}

esp_err_t serial_start(QueueHandle_t queue)
{
    ESP_RETURN_ON_FALSE(queue, ESP_ERR_INVALID_ARG, TAG, "no queue");
    s_queue = queue;

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

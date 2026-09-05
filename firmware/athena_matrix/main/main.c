/* Athena matrix: the face. app_main brings up the panel, then two tasks on
 * core 0 do the work: `serial` turns UART0 lines into commands (protocol.h),
 * `render` draws the current mode at 40 fps. Core 1 belongs to the hub75
 * refresh loop and nothing else is ever pinned there.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "board_pins.h"
#include "face.h"
#include "hub75.h"
#include "protocol.h"
#include "serial.h"

static const char *TAG = "athena_matrix";

#define CMD_QUEUE_DEPTH   8

/* Append a space-separated word to a bounded buffer. */
static void append_word(char *buf, size_t size, const char *word)
{
    size_t len = strlen(buf);
    if (len && len + 1 < size) buf[len++] = ' ';
    while (*word && len + 1 < size) buf[len++] = *word++;
    buf[len] = '\0';
}

void app_main(void)
{
    hub75_config_t cfg = HUB75_CONFIG_DEFAULT();
    cfg.pins = (hub75_pins_t)BOARD_HUB75_PINS;
    cfg.brightness = 255;           /* full; the panel needs its own 5 V supply at this level, USB alone browns out */
    /* cfg.driver = HUB75_DRIVER_FM6126A;  -- only if the panel stays dark with correct wiring */

    ESP_ERROR_CHECK(hub75_init(&cfg));

    QueueHandle_t queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(face_cmd_t));
    if (!queue) {
        ESP_LOGE(TAG, "no memory for the command queue");
        return;
    }
    ESP_ERROR_CHECK(face_start(queue));
    ESP_ERROR_CHECK(serial_start(queue));

    char modes[96] = "";
    for (int m = 0; m < FACE_MODE_COUNT; m++) {
        append_word(modes, sizeof(modes), face_mode_name((face_mode_t)m));
    }
    ESP_LOGI(TAG, "ready: boot mode %s, modes: %s", face_mode_name(FACE_MODE_TEST), modes);
}

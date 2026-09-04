/* Athena hello: the smallest possible sign of life. Prints what chip it is
 * running on, then one "alive" line per second, forever. Flash it to a fresh
 * board and open a serial monitor; if the lines scroll, the board, the cable,
 * the port and the toolchain are all fine.
 */
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "athena_hello";

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_bytes = 0;
    esp_flash_get_size(NULL, &flash_bytes);

    ESP_LOGI(TAG, "hello from Athena");
    ESP_LOGI(TAG, "chip: %s rev v%d.%d, %d core(s), ESP-IDF %s",
             CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100,
             chip.cores, esp_get_idf_version());
    ESP_LOGI(TAG, "flash: %" PRIu32 " MB, free heap: %" PRIu32 " bytes",
             flash_bytes / (1024 * 1024), esp_get_free_heap_size());

    uint32_t seconds = 0;
    for (;;) {
        ESP_LOGI(TAG, "alive for %" PRIu32 " s", seconds++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

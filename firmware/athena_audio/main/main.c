/* Athena audio: the ears and the mouth. app_main joins the network, then
 * audio_start() opens the I2S port and starts the four `audio_*` tasks that
 * bridge the microphone and the amplifier to TCP port AUDIO_TCP_PORT
 * (audio.h; the Mac end is scripts/audio.py). This is the raw bridge that
 * brings the hardware up, stages 2-4 of AUDIO-BOARD.md with the Mac as the
 * meter: no wake word, no echo cancellation and no hub protocol yet, the
 * board moves sound and the Mac is the brain.
 */
#include "esp_log.h"

#include "athena_wifi.h"
#include "audio.h"
#include "board_pins.h"

static const char *TAG = "athena_audio";

#define HOSTNAME          "athena-audio"      /* athena-audio.local */

void app_main(void)
{
    /* The network is this board's only transport: without it there is
     * nothing to serve, so say so on the console and stay up for a look. */
    static const athena_wifi_config_t wifi = {
        .hostname = HOSTNAME,
        .service = "_athena-audio",
        .service_port = AUDIO_TCP_PORT,
    };
    esp_err_t err = athena_wifi_start(&wifi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi unavailable: %s, nothing to serve", esp_err_to_name(err));
        return;
    }

    /* athena_wifi_start has brought up the TCP/IP stack the listener needs;
     * the join itself may still be under way, the socket does not mind. */
    err = audio_start(&(audio_pins_t)BOARD_AUDIO_PINS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio unavailable: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "ready: %s.local, mic out and speaker in on tcp port %d", HOSTNAME, AUDIO_TCP_PORT);
}

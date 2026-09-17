/* Athena audio: the ears and the mouth. app_main joins the network, then
 * audio_start() opens the I2S port and starts the four `audio_*` tasks that
 * bridge the microphone and the amplifier to TCP port AUDIO_TCP_PORT
 * (audio.h; the Mac end is scripts/audio.py), then sr_start() puts ESP-SR's
 * audio front end on the same microphone: the wake word and the VAD, their
 * detections on this console (sr.h; stage 5 of AUDIO-BOARD.md). No echo
 * cancellation and no hub protocol yet: the board moves sound and hears
 * the wake word, the Mac is still the brain. CONFIG_ATHENA_AUDIO_SR off
 * (Kconfig.projbuild) builds the bridge without sr.c.
 */
#include "esp_log.h"

#include "athena_wifi.h"
#include "audio.h"
#include "board_pins.h"
#if CONFIG_ATHENA_AUDIO_SR
#include "sr.h"
#endif

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

#if CONFIG_ATHENA_AUDIO_SR
    /* The wake word and the VAD listen to the same microphone. Without the
     * models in the `model` partition they are absent, not fatal: the raw
     * bridge is still the Mac's bench. */
    err = sr_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "speech front end unavailable: %s; raw bridge only", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "ready: %s.local, mic out and speaker in on tcp port %d, wake word and vad on this console",
             HOSTNAME, AUDIO_TCP_PORT);
#else
    /* Built without CONFIG_ATHENA_AUDIO_SR (Kconfig.projbuild): the raw
     * bridge alone, nothing else listens to the microphone. */
    ESP_LOGI(TAG, "ready: %s.local, mic out and speaker in on tcp port %d, raw bridge only (no wake word, no vad in this build)",
             HOSTNAME, AUDIO_TCP_PORT);
#endif
}

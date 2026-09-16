/* sr.c - the `sr` module: the AFE with WakeNet and VADNet on the microphone,
 * detections on the console. See sr.h for the contract.
 *
 * Pipeline: audio_rx -> sr_feed() -> s_in (half a second of int16) -> sr_feed
 * task assembles one AFE chunk and calls afe->feed -> the AFE's own task
 * (core 1, priority SR_AFE_PRIORITY) runs the models -> sr_fetch task reads
 * afe->fetch and logs every wake word (with a count since boot) and every
 * VAD edge, plus one summary line every 5 s. The summary carries the wake
 * count since boot, which is the number to read after an evening with the
 * TV on (AUDIO-BOARD.md, stage 5: count the false accepts).
 *
 * The cleaned mono chunk the AFE returns (res->data) is what goes uplink from
 * stage 6 on; here it is dropped, the raw bridge in audio.c still streams the
 * raw microphone so scripts/audio.py and station.py see what they saw before.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vadn_models.h"
#include "esp_wn_models.h"
#include "model_path.h"

#include "audio.h"
#include "sr.h"

static const char *TAG = "sr";

#define SR_MODEL_PARTITION      "model"     /* the partition label in partitions.csv; esp-sr's CMake flashes srmodels.bin there */
#define SR_IN_BUF_MS            500         /* microphone waiting for the feed task; beyond it whole blocks are dropped */
#define SR_IN_BUF_BYTES         (AUDIO_RATE_HZ * 2 * SR_IN_BUF_MS / 1000)
#define SR_VAD_MIN_SPEECH_MS    128         /* a burst shorter than this is not speech (the AFE's default) */
#define SR_VAD_MIN_NOISE_MS     600         /* the hangover: silence this long ends an utterance, AUDIO-BOARD.md's ~600 ms */
#define SR_LOG_PERIOD_US        (5 * 1000 * 1000)
#define SR_CORE                 1           /* the AFE and both tasks, next to audio_rx and audio_tx */
#define SR_AFE_PRIORITY         5           /* the AFE's own task: it has to keep up with the microphone */
#define SR_TASK_PRIORITY        4           /* sr_feed and sr_fetch: a copy and a log line each, below audio_rx */

static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static srmodel_list_t *s_models;
static StreamBufferHandle_t s_in;           /* int16 mono, audio_rx -> sr_feed task */
static int s_feed_chunk;                    /* samples per channel in one afe->feed */
static int s_feed_nch;                      /* channels the AFE expects interleaved in one feed */
static char s_wake_word[48] = "none";        /* the phrase, for the log */
static uint32_t s_in_dropped;               /* samples sr_feed() could not queue since the last log line */
static uint32_t s_wakes;                    /* wake words since boot */

void sr_feed(const int16_t *pcm, size_t samples)
{
    if (!s_in) return;
    size_t bytes = samples * sizeof(int16_t);
    /* Whole blocks only: a partial write would leave the feed task with half
     * a sample, and a block lost to a stalled AFE is counted, not waited for. */
    if (xStreamBufferSpacesAvailable(s_in) < bytes || xStreamBufferSend(s_in, pcm, bytes, 0) != bytes) {
        s_in_dropped += samples;
    }
}

/* Assemble the AFE's chunk from the microphone blocks and feed it. feed()
 * copies into the AFE's ring buffer; the models run in the AFE's own task. */
static void feed_task(void *arg)
{
    (void)arg;
    size_t bytes = (size_t)s_feed_chunk * s_feed_nch * sizeof(int16_t);
    int16_t *chunk = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!chunk) {
        ESP_LOGE(TAG, "no memory for the feed chunk (%u bytes)", (unsigned)bytes);
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        size_t have = 0;
        while (have < bytes) {
            have += xStreamBufferReceive(s_in, (uint8_t *)chunk + have, bytes - have, portMAX_DELAY);
        }
        s_afe->feed(s_afe_data, chunk);
    }
}

/* Read the AFE's results: log the wake words and the VAD edges as they
 * happen, and a summary every 5 s. */
static void fetch_task(void *arg)
{
    (void)arg;
    vad_state_t vad = VAD_SILENCE;
    int64_t speech_since = 0;
    uint32_t frames = 0, speech_frames = 0, wakes = 0, errors = 0;
    float volume_sum = 0.0f, free_min = 1.0f;
    int64_t next_log = esp_timer_get_time() + SR_LOG_PERIOD_US;

    for (;;) {
        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);    /* waits up to 2 s for the next chunk */
        int64_t now = esp_timer_get_time();
        if (!res || res->ret_value == ESP_FAIL) {
            errors++;
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            frames++;
            volume_sum += res->data_volume;
            if (res->ringbuff_free_pct < free_min) free_min = res->ringbuff_free_pct;
            if (res->vad_state == VAD_SPEECH) speech_frames++;

            if (res->wakeup_state == WAKENET_DETECTED) {
                s_wakes++;
                wakes++;
                /* wake_word_length is 0 in this mode (one channel, no
                 * channel verification), so it is printed only when set. */
                char length[24] = "";
                if (res->wake_word_length > 0) {
                    snprintf(length, sizeof(length), ", %d ms", res->wake_word_length * 1000 / AUDIO_RATE_HZ);
                }
                ESP_LOGI(TAG, "wake #%u: \"%s\" (word %d of model %d%s, %.0f dBFS)",
                         (unsigned)s_wakes, s_wake_word, res->wake_word_index, res->wakenet_model_index,
                         length, res->data_volume);
            }
            if (res->vad_state != vad) {
                vad = res->vad_state;
                if (vad == VAD_SPEECH) {
                    speech_since = now;
                    /* vad_cache is the head the detector's delay cut off; stage 6
                     * sends it uplink first so the utterance starts whole. */
                    ESP_LOGI(TAG, "vad: speech at %.0f dBFS, %d ms cached",
                             res->data_volume, res->vad_cache_size / (int)sizeof(int16_t) * 1000 / AUDIO_RATE_HZ);
                } else {
                    ESP_LOGI(TAG, "vad: silence after %.1f s of speech", (float)(now - speech_since) / 1e6f);
                }
            }
        }

        if (now >= next_log) {
            char dropped[40] = "", failed[32] = "";
            if (s_in_dropped) snprintf(dropped, sizeof(dropped), ", %u samples dropped", (unsigned)s_in_dropped);
            if (errors) snprintf(failed, sizeof(failed), ", %u fetch errors", (unsigned)errors);
            ESP_LOGI(TAG, "%u wakes (%u since boot), speech %.0f%% of %u frames, %.0f dBFS, ring %.0f%% free%s%s",
                     (unsigned)wakes, (unsigned)s_wakes, frames ? 100.0f * speech_frames / frames : 0.0f,
                     (unsigned)frames, frames ? volume_sum / frames : -96.0f, 100.0f * free_min, dropped, failed);
            s_in_dropped = 0;
            frames = speech_frames = wakes = errors = 0;
            volume_sum = 0.0f;
            free_min = 1.0f;
            next_log = now + SR_LOG_PERIOD_US;
        }
    }
}

esp_err_t sr_start(void)
{
    s_models = esp_srmodel_init(SR_MODEL_PARTITION);
    ESP_RETURN_ON_FALSE(s_models && s_models->num > 0, ESP_ERR_NOT_FOUND, TAG,
                        "no models in the `%s` partition: `idf.py flash` writes srmodels.bin there", SR_MODEL_PARTITION);
    for (int i = 0; i < s_models->num; i++) {
        ESP_LOGI(TAG, "model %d: %s (%s)", i, s_models->model_name[i],
                 s_models->model_info && s_models->model_info[i] ? s_models->model_info[i] : "no info");
    }

    /* The defaults enable everything the format and the models allow; pin
     * what stage 5 is about (a wake word and a VAD with the design note's
     * hangover) and where it runs. */
    afe_config_t *cfg = afe_config_init(SR_INPUT_FORMAT, s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    ESP_RETURN_ON_FALSE(cfg, ESP_FAIL, TAG, "afe config for \"%s\"", SR_INPUT_FORMAT);
    if (!cfg->wakenet_model_name) cfg->wakenet_model_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
    cfg->wakenet_init = cfg->wakenet_model_name != NULL;
    if (!cfg->vad_model_name) cfg->vad_model_name = esp_srmodel_filter(s_models, ESP_VADN_PREFIX, NULL);
    cfg->vad_init = true;
    cfg->vad_min_speech_ms = SR_VAD_MIN_SPEECH_MS;
    cfg->vad_min_noise_ms = SR_VAD_MIN_NOISE_MS;
    cfg->afe_perferred_core = SR_CORE;
    cfg->afe_perferred_priority = SR_AFE_PRIORITY;
    if (cfg->wakenet_model_name) {
        const char *words = esp_srmodel_get_wake_words(s_models, cfg->wakenet_model_name);
        snprintf(s_wake_word, sizeof(s_wake_word), "%s", words ? words : cfg->wakenet_model_name);
    } else {
        ESP_LOGW(TAG, "no wakenet model in the partition: CONFIG_SR_WN_* unset? VAD only");
    }
    const char *vad_model = cfg->vad_model_name ? cfg->vad_model_name : "webrtc";
    float vad_floor = cfg->vad_energy_threshold;

    s_afe = esp_afe_handle_from_config(cfg);
    if (!s_afe) {
        afe_config_free(cfg);
        ESP_LOGE(TAG, "no afe for this config");
        return ESP_FAIL;
    }
    s_afe_data = s_afe->create_from_config(cfg);
    afe_config_free(cfg);
    ESP_RETURN_ON_FALSE(s_afe_data, ESP_FAIL, TAG, "afe create failed");

    s_feed_chunk = s_afe->get_feed_chunksize(s_afe_data);
    s_feed_nch = s_afe->get_feed_channel_num(s_afe_data);
    /* sr_feed() carries one channel; a format with more needs the feed task
     * to interleave them before this check can go. */
    ESP_RETURN_ON_FALSE(s_feed_nch == 1, ESP_ERR_NOT_SUPPORTED, TAG,
                        "afe for \"%s\" wants %d channels, sr_feed() carries one", SR_INPUT_FORMAT, s_feed_nch);

    /* The trigger level is one chunk, so the feed task wakes once per chunk.
     * audio_rx starts queuing the moment s_in is set, before the feed task
     * exists; half a second of room covers that, and the task finds its
     * handle in place when it starts. */
    s_in = xStreamBufferCreate(SR_IN_BUF_BYTES, (size_t)s_feed_chunk * sizeof(int16_t));
    ESP_RETURN_ON_FALSE(s_in, ESP_ERR_NO_MEM, TAG, "no memory for the microphone queue");

    BaseType_t ok = xTaskCreatePinnedToCore(feed_task, "sr_feed", 4096, NULL, SR_TASK_PRIORITY, NULL, SR_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "sr_feed task");
    ok = xTaskCreatePinnedToCore(fetch_task, "sr_fetch", 4096, NULL, SR_TASK_PRIORITY, NULL, SR_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "sr_fetch task");

    s_afe->print_pipeline(s_afe_data);
    ESP_LOGI(TAG, "wake word \"%s\", vad %s (speech >= %d ms, silence >= %d ms, floor %.0f dBFS), feed %d samples, free heap %u",
             s_wake_word, vad_model, SR_VAD_MIN_SPEECH_MS, SR_VAD_MIN_NOISE_MS, vad_floor, s_feed_chunk,
             (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}

/* sr.h - the `sr` module: ESP-SR's audio front end (AFE) on the microphone,
 * WakeNet for the wake word and VADNet for the speech boundaries. Stage 5 of
 * AUDIO-BOARD.md: every detection goes to the console, nothing acts on it yet;
 * the hub protocol that will carry `wake` and `speech_end` is stage 6.
 *
 * audio_rx hands every microphone block to sr_feed(); two tasks on core 1
 * next to the AFE's own (sr_feed, sr_fetch) assemble the AFE chunks and read
 * the results. The models live in the `model` partition: the esp-sr component
 * packs the ones selected in sdkconfig.defaults into srmodels.bin and
 * `idf.py flash` writes it. Without them sr_start() fails and says so, and
 * the raw bridge in audio.c keeps working on its own.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* The AFE input format: M a microphone, R the playback reference, N unused,
 * in the order of the channels sr_feed() would carry. One mic and no
 * reference today, so the AFE runs WakeNet, VAD, noise suppression and gain
 * control but no echo cancellation. "MR" comes with the TX ring as the
 * reference (stage 7), "MMR" with the second mic; sr_feed() then has to carry
 * that many interleaved channels. */
#define SR_INPUT_FORMAT     "M"

/* Load the models from the `model` partition, build the AFE for
 * SR_INPUT_FORMAT and start the two tasks. Call it after audio_start(): the
 * microphone is what feeds it. A failure is returned, not fatal, so the caller
 * can log it and keep the raw bridge. */
esp_err_t sr_start(void);

/* Hand one block of int16 mono 16 kHz microphone samples to the AFE, from the
 * one task that reads the microphone. Never blocks: a block the queue cannot
 * take whole is dropped and counted on the 5 s log line. A no-op before
 * sr_start() has succeeded. */
void sr_feed(const int16_t *pcm, size_t samples);

/* Audio board pin map: the table in AUDIO-BOARD.md, for the ESP32-S3-DevKitC-1
 * (N16R8). Skipped on purpose: 26-32 (flash), 35-37 (octal PSRAM), 19/20 (USB),
 * 0/3/45/46 (strapping), 43/44 (UART console).
 */
#pragma once

#include "audio.h"

/* One I2S port in full duplex: the mic's SCK and the amp's BCLK share GPIO5,
 * the mic's WS and the amp's LRC share GPIO6. A second mic joins the same
 * three wires with its L/R on 3V3 (right slot). SD_MODE on GPIO16 turns the
 * amp on. AUDIO-BOARD.md keeps GPIO4 for a push-to-talk button and the
 * on-board RGB LED (48, or 38 on v1.1 boards) for state; neither is used yet. */
#define BOARD_AUDIO_PINS { .bclk = 5, .ws = 6, .dout = 15, .din = 7, .sd_mode = 16 }

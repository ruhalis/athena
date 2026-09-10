/* audio.h - the `audio` module: the microphone and the speaker as one raw PCM
 * stream on TCP port AUDIO_TCP_PORT, the audio board's one port for now.
 *
 * Format both ways: signed 16-bit little-endian, AUDIO_RATE_HZ, mono, no
 * framing. The board sends the microphone for as long as a client is
 * connected and plays whatever the client writes, silence when nothing
 * arrives. One client at a time; a second connection is closed at once. The
 * microphone never blocks or drops the client: whatever the client cannot
 * take right now, because the link stalls or it is busy playing, is lost and
 * counted on the 5 s log line. The client is closed only when it goes away.
 * scripts/audio.py is the Mac end for a bench test.
 */
#pragma once

#include "esp_err.h"

#define AUDIO_TCP_PORT      7076        /* the `audio_net` task listens here; scripts/audio.py's default */
#define AUDIO_RATE_HZ       16000

typedef struct {
    int bclk;       /* I2S bit clock: to the mic's SCK and the amp's BCLK */
    int ws;         /* I2S word select: to the mic's WS and the amp's LRC */
    int dout;       /* I2S data out: to the amp's DIN */
    int din;        /* I2S data in: from the mic's SD (an input-only pin is fine) */
    int sd_mode;    /* amp SD_MODE, driven high at start (on, left slot); -1 when the pin is tied to 3V3 instead */
} audio_pins_t;

/* Open one I2S port in full duplex (32-bit slots, stereo frame, 16 kHz) on
 * these pins, turn the amp on through sd_mode if it has one, and start the
 * four audio tasks (I2S on core 1, network on core 0). Call it after
 * athena_wifi_start() has returned ESP_OK: the listener needs the TCP/IP
 * stack that brings up. A failure is returned, not fatal, so the caller can
 * log it and leave the console readable. */
esp_err_t audio_start(const audio_pins_t *pins);

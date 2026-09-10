/* HUB75 pin map, chosen by the build target. The driver writes the whole
 * GPIO_OUT register per pixel clock, so on either chip every signal must sit
 * on GPIO 0..31.
 */
#pragma once

#include "sdkconfig.h"
#include "audio.h"
#include "hub75.h"

#if CONFIG_IDF_TARGET_ESP32S3

/* ESP32-S3-DevKitC-1 (N16R8). Identical to the map in RGB-MATRIX.md: every
 * signal lands on the J1 header in header order, and it carries over
 * unchanged when this build moves to the DMA driver. Skipped on purpose:
 * 0/3/45/46 (strapping), 19/20 (USB), 26-32 (flash), 35-37 (octal PSRAM),
 * 43/44 (UART console). GPIO14 stays free next to the block.
 */
#define BOARD_HUB75_PINS {          \
    .r1 = 4,  .g1 = 5,  .b1 = 6,    \
    .r2 = 7,  .g2 = 15, .b2 = 16,   \
    .a = 17, .b = 18, .c = 8, .d = 9, .e = 10, \
    .clk = 11, .lat = 12, .oe = 13, \
}

/* I2S for the microphone and the amplifier, on pins the panel does not use.
 * GPIO 1, 2, 14 and 21 are free on the headers; untested on this board, and
 * AUDIO-BOARD.md's separate S3 audio board uses 5/6/7/15 instead. */
#define BOARD_AUDIO_PINS { .bclk = 1, .ws = 2, .dout = 14, .din = 21 }

#else

/* ESP32-WROOM-32 DevKit (38-pin DevKitC or 30-pin "DevKit V1").
 *
 * Pins avoided on purpose: 6-11 (flash), 34-39 (input only), 0/12 (strapping,
 * a wrong level at reset stops the boot), 2/15 (strapping, left free while
 * cleaner pins exist), 1/3 (UART console), 32/33 (bank 1). That leaves exactly
 * these fourteen.
 *
 * Right header of the 38-pin DevKitC, top to bottom, carries the data side in
 * order: 23 22 [TX RX] 21 [GND] 19 18 5 17 16 4. The left header carries the
 * five address lines: 25 26 27 14 [12] [GND] 13, so E sits two positions
 * below D, not one.
 *
 * A WROVER module uses GPIO16/17 for its PSRAM. No clean bank-0 pins remain on
 * such a board, so CLK and LAT would have to move to GPIO2 and GPIO15: they are
 * sampled only at reset and the panel's inputs do not pull them, but this pin
 * map is for a WROOM and is untested on a WROVER.
 */
#define BOARD_HUB75_PINS {          \
    .r1 = 23, .g1 = 22, .b1 = 21,   \
    .r2 = 19, .g2 = 18, .b2 = 5,    \
    .a = 25, .b = 26, .c = 27, .d = 14, .e = 13, \
    .clk = 17, .lat = 16, .oe = 4,  \
}

/* I2S for the microphone and the amplifier, on the four pins the panel
 * leaves: 32 and 33 are the bank-1 outputs, 34 is input-only and only ever
 * reads the mic, and 15 is a strapping pin that nothing on the amp pulls at
 * reset. The refresh loop rewrites the whole bank-0 output register, which
 * does not touch a peripheral-routed pin such as 15 but would clobber a
 * software-driven output there, so the amp's SD (mute) pin is tied to 3V3
 * rather than to a GPIO. Wiring in README.md. */
#define BOARD_AUDIO_PINS { .bclk = 32, .ws = 33, .dout = 15, .din = 34 }

#endif

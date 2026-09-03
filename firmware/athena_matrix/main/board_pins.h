/* HUB75 pin map for an ESP32-WROOM-32 DevKit (38-pin DevKitC or 30-pin "DevKit V1").
 *
 * Every signal is on GPIO 0..31 because the driver writes the whole GPIO_OUT
 * register per pixel clock. Pins avoided on purpose: 6-11 (flash), 34-39 (input
 * only), 0/12 (strapping, a wrong level at reset stops the boot), 2/15
 * (strapping, left free while cleaner pins exist), 1/3 (UART console),
 * 32/33 (bank 1). That leaves exactly these fourteen.
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
#pragma once

#include "hub75.h"

#define BOARD_HUB75_PINS {          \
    .r1 = 23, .g1 = 22, .b1 = 21,   \
    .r2 = 19, .g2 = 18, .b2 = 5,    \
    .a = 25, .b = 26, .c = 27, .d = 14, .e = 13, \
    .clk = 17, .lat = 16, .oe = 4,  \
}

#pragma once
#include <stdint.h>

/* Classic 5x7 ASCII font, characters 32..126, column-major: five bytes per
 * glyph, bit 0 is the top row, bit 6 the bottom row. */
#define FONT5X7_FIRST 32
#define FONT5X7_LAST  126
extern const uint8_t font5x7[FONT5X7_LAST - FONT5X7_FIRST + 1][5];

/* hub75_gfx.c - text on top of the hub75 drawing primitives. */
#include <string.h>

#include "font5x7.h"
#include "hub75.h"

#define GLYPH_W   5
#define GLYPH_H   7
#define ADVANCE   6     /* glyph plus one column of spacing */

int hub75_draw_char(int x, int y, char ch, int scale, uint8_t r, uint8_t g, uint8_t b)
{
    if (scale < 1) scale = 1;
    unsigned c = (unsigned char)ch;
    if (c < FONT5X7_FIRST || c > FONT5X7_LAST) c = '?';
    const uint8_t *glyph = font5x7[c - FONT5X7_FIRST];

    for (int col = 0; col < GLYPH_W; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < GLYPH_H; row++) {
            if (bits & (1u << row)) {
                hub75_fill_rect(x + col * scale, y + row * scale, scale, scale, r, g, b);
            }
        }
    }
    return ADVANCE * scale;
}

int hub75_draw_text(int x, int y, const char *text, int scale, uint8_t r, uint8_t g, uint8_t b)
{
    if (!text) return 0;
    int cx = x;
    for (; *text; text++) {
        cx += hub75_draw_char(cx, y, *text, scale, r, g, b);
    }
    return cx - x;
}

int hub75_text_width(const char *text, int scale)
{
    if (!text || !*text) return 0;
    if (scale < 1) scale = 1;
    return (int)strlen(text) * ADVANCE * scale - scale;   /* no trailing gap */
}

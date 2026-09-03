/* hub75.h - bit-banged driver for a 64x64, 1/32-scan HUB75E LED matrix.
 *
 * Written from scratch for ESP-IDF. Works on any Xtensa ESP32 whose pins are
 * all in GPIO bank 0 (GPIO 0..31): classic ESP32, ESP32-S2, ESP32-S3.
 *
 * Model
 *   - One FreeRTOS task, pinned to a core, owns the panel and never blocks.
 *   - Binary code modulation: colour bit plane p is lit for (64 << p) pixel
 *     clocks, so its weight is exactly 2^p with no timer involved.
 *   - Brightness is the fraction of each plane window during which OE is low.
 *   - Drawing goes to an RGB888 back buffer; hub75_present() packs it into
 *     bit planes and hands it over at the next frame boundary.
 *
 * Rule: while the panel runs, nothing else on this firmware may drive a GPIO
 * in 0..31, because the refresh loop writes the whole GPIO_OUT register.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HUB75_WIDTH   64
#define HUB75_HEIGHT  64
#define HUB75_ROWS    (HUB75_HEIGHT / 2)   /* 1/32 scan: A..E select one of 32 row pairs */

typedef enum {
    HUB75_DRIVER_GENERIC = 0,   /* 74HC595 / ICN2012 / ICN2038 class: no init needed */
    HUB75_DRIVER_FM6126A,       /* FM6126A / FM6124 panels stay dark without a register init */
} hub75_driver_t;

typedef struct {
    int r1, g1, b1;             /* colour lines, top half (rows 0..31) */
    int r2, g2, b2;             /* colour lines, bottom half (rows 32..63) */
    int a, b, c, d, e;          /* row address, bit 0..4 */
    int clk, lat, oe;           /* pixel clock, latch, output enable (active low) */
} hub75_pins_t;

typedef struct {
    hub75_pins_t pins;
    uint8_t color_depth;        /* bit planes per channel, 1..8. 5 gives ~150 Hz on a 240 MHz ESP32 */
    uint8_t brightness;         /* initial brightness, 0..255 */
    float gamma;                /* 0 means 2.2 */
    hub75_driver_t driver;
    int core;                   /* CPU that runs the refresh loop */
    uint8_t task_priority;
} hub75_config_t;

#define HUB75_CONFIG_DEFAULT() {        \
    .color_depth = 5,                    \
    .brightness = 40,                    \
    .gamma = 2.2f,                       \
    .driver = HUB75_DRIVER_GENERIC,      \
    .core = 1,                           \
    .task_priority = 20,                 \
}

/* Configure the pins, allocate the buffers and start the refresh task.
 * The panel shows black until the first hub75_present(). */
esp_err_t hub75_init(const hub75_config_t *cfg);

/* Global brightness, 0..255. Takes effect within one frame. */
void hub75_set_brightness(uint8_t level);
uint8_t hub75_get_brightness(void);

/* Blank the panel without losing the frame or the brightness setting. */
void hub75_set_output(bool enabled);

/* Refresh rate measured by the refresh loop, in frames per second. 0 until measured. */
float hub75_refresh_hz(void);

/* ---- Drawing: all calls touch the back buffer only ---- */

void hub75_clear(void);
void hub75_fill(uint8_t r, uint8_t g, uint8_t b);
void hub75_draw_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);
void hub75_fill_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);
void hub75_draw_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);
void hub75_draw_hline(int x, int y, int w, uint8_t r, uint8_t g, uint8_t b);
void hub75_draw_vline(int x, int y, int h, uint8_t r, uint8_t g, uint8_t b);

/* 5x7 font, scaled by an integer factor. Returns the horizontal advance in pixels. */
int hub75_draw_char(int x, int y, char ch, int scale, uint8_t r, uint8_t g, uint8_t b);
int hub75_draw_text(int x, int y, const char *text, int scale, uint8_t r, uint8_t g, uint8_t b);
int hub75_text_width(const char *text, int scale);

/* Pack the back buffer into bit planes and show it. Returns once the refresh
 * loop has switched to the new frame, so the back buffer is free again. */
void hub75_present(void);

#ifdef __cplusplus
}
#endif

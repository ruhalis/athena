/* hub75.c - bit-banged HUB75E driver, see include/hub75.h for the model.
 *
 * Timing per (row, plane) window:
 *   1. OE high (blank), address lines = row, LAT pulse: the data that the
 *      previous window shifted in is now on the LEDs of this row pair.
 *   2. Shift the NEXT window's 64 columns, (1 << plane) times over, with OE
 *      low for the first `brightness` fraction of those clocks and high for
 *      the rest. Re-shifting the same 64 columns is harmless: the shift
 *      register is exactly one row wide, so only the last pass matters.
 * Every clock costs two writes to GPIO_OUT_REG (data with CLK low, then the
 * same word with CLK high), so a window is a fixed number of identical steps
 * and plane weights are exact powers of two.
 */
#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"

#include "hub75.h"

static const char *TAG = "hub75";

#define PLANE_BYTES         (HUB75_ROWS * HUB75_WIDTH)              /* one plane: 32 rows x 64 columns */
#define PLANE_OFFSET(p, r)  ((((p) * HUB75_ROWS) + (r)) * HUB75_WIDTH)
#define COLOUR_WORDS        64                                       /* 6 colour bits -> 64 GPIO words */
#define FPS_SAMPLE_FRAMES   64
#define PIN_COUNT           14

typedef struct {
    hub75_config_t cfg;
    uint32_t mask_all;                      /* every panel pin */
    uint32_t mask_colour;                   /* the six colour pins */
    uint32_t bit_clk, bit_lat, bit_oe;
    uint32_t addr[HUB75_ROWS];              /* A..E bits for each row pair */
    uint32_t lut[COLOUR_WORDS];             /* packed colour byte -> GPIO bits */
    uint8_t gamma[256];                     /* 8-bit -> color_depth-bit */
    uint8_t *packed[2];                     /* [plane][row][x], one byte per column: r1 g1 b1 r2 g2 b2 */
    uint8_t *front;                         /* buffer the refresh loop shows */
    uint8_t *volatile pending;              /* buffer waiting for the next frame boundary */
    uint8_t (*draw)[HUB75_WIDTH][3];        /* RGB888 back buffer, [y][x][c] */
    SemaphoreHandle_t presented;
    volatile uint8_t brightness;
    volatile bool output_on;
    volatile int64_t sample_us;             /* time the last FPS_SAMPLE_FRAMES frames took */
    bool running;
} hub75_state_t;

static hub75_state_t s;

/* ------------------------------------------------------------------------ */
/* Refresh loop                                                              */
/* ------------------------------------------------------------------------ */

#define PIXEL(i)                                        \
    do {                                                \
        uint32_t w_ = base | lut[data[(i)]];            \
        REG_WRITE(GPIO_OUT_REG, w_);                    \
        REG_WRITE(GPIO_OUT_REG, w_ | clk);              \
    } while (0)

/* Clock `count` columns out of `data`, starting at column `idx` and wrapping
 * at the panel width. `base` carries the address, OE and LAT levels for the
 * whole run. Returns the column the next run continues from. */
static inline int IRAM_ATTR shift_run(const uint8_t *data, int idx, uint32_t count, uint32_t base)
{
    const uint32_t *lut = s.lut;
    const uint32_t clk = s.bit_clk;

    while (count) {
        if (count >= 8 && idx <= HUB75_WIDTH - 8) {
            PIXEL(idx);     PIXEL(idx + 1); PIXEL(idx + 2); PIXEL(idx + 3);
            PIXEL(idx + 4); PIXEL(idx + 5); PIXEL(idx + 6); PIXEL(idx + 7);
            idx = (idx + 8) & (HUB75_WIDTH - 1);
            count -= 8;
        } else {
            PIXEL(idx);
            idx = (idx + 1) & (HUB75_WIDTH - 1);
            count--;
        }
    }
    return idx;
}

static void IRAM_ATTR refresh_task(void *arg)
{
    const int depth = s.cfg.color_depth;
    const uint32_t lat = s.bit_lat;
    const uint32_t oe = s.bit_oe;
    const uint8_t *frame = s.front;
    int64_t t_sample = esp_timer_get_time();
    uint32_t frames = 0;

    /* Prime the shift register with (row 0, plane 0) so the first window has data to latch. */
    {
        uint32_t base = (REG_READ(GPIO_OUT_REG) & ~s.mask_all) | s.addr[0] | oe;
        shift_run(frame + PLANE_OFFSET(0, 0), 0, HUB75_WIDTH, base);
    }

    for (;;) {
        for (int row = 0; row < HUB75_ROWS; row++) {
            const uint32_t bright = s.output_on ? (uint32_t)s.brightness + 1 : 0;   /* 0..256 */

            for (int plane = 0; plane < depth; plane++) {
                /* The window after this one, and the frame it comes from. */
                int nrow = row;
                int nplane = plane + 1;
                if (nplane == depth) {
                    nplane = 0;
                    nrow = row + 1;
                    if (nrow == HUB75_ROWS) {
                        nrow = 0;
                        uint8_t *p = s.pending;
                        if (p) {
                            s.pending = NULL;
                            s.front = p;
                            frame = p;
                            xSemaphoreGive(s.presented);
                        }
                        if (++frames == FPS_SAMPLE_FRAMES) {
                            int64_t now = esp_timer_get_time();
                            s.sample_us = now - t_sample;     /* no float math here: it would call into flash */
                            t_sample = now;
                            frames = 0;
                        }
                    }
                }
                const uint8_t *next = frame + PLANE_OFFSET(nplane, nrow);

                /* Show what the previous window shifted in: blank first (the address
                 * must not change while a row is lit), then select the row and latch. */
                const uint32_t cur = REG_READ(GPIO_OUT_REG);
                REG_WRITE(GPIO_OUT_REG, cur | oe);
                uint32_t base = (cur & ~s.mask_all) | s.addr[row];
                REG_WRITE(GPIO_OUT_REG, base | oe);
                REG_WRITE(GPIO_OUT_REG, base | oe | lat);
                REG_WRITE(GPIO_OUT_REG, base | oe);
                esp_rom_delay_us(1);        /* let the row drivers settle before lighting: no ghost row */

                /* Window: (64 << plane) clocks, OE low for the brightness fraction. */
                const uint32_t total = HUB75_WIDTH << plane;
                const uint32_t on = (total * bright) >> 8;
                int idx = shift_run(next, 0, on, base);
                shift_run(next, idx, total - on, base | oe);
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* FM6126A register init (only when cfg.driver says so)                     */
/* ------------------------------------------------------------------------ */

/* Clock one 64-column word onto all six colour lines, LSB of `pattern` first,
 * repeating every 16 columns, with LAT high for the last `lat_clocks` clocks.
 * 11 clocks addresses register 1, 12 clocks register 2. */
static void fm6126a_word(uint16_t pattern, int lat_clocks)
{
    const uint32_t base = (REG_READ(GPIO_OUT_REG) & ~s.mask_all) | s.bit_oe;
    for (int i = 0; i < HUB75_WIDTH; i++) {
        uint32_t w = base;
        if ((pattern >> (i % 16)) & 1) w |= s.mask_colour;
        if (i >= HUB75_WIDTH - lat_clocks) w |= s.bit_lat;
        REG_WRITE(GPIO_OUT_REG, w);
        esp_rom_delay_us(1);
        REG_WRITE(GPIO_OUT_REG, w | s.bit_clk);
        esp_rom_delay_us(1);
    }
    REG_WRITE(GPIO_OUT_REG, base);
    esp_rom_delay_us(1);
}

static void fm6126a_init(void)
{
    fm6126a_word(0xFFFE, 11);   /* register 1: 0b0111111111111111, full current */
    fm6126a_word(0x0200, 12);   /* register 2: 0b0000000001000000 */
    ESP_LOGI(TAG, "FM6126A registers written");
}

/* ------------------------------------------------------------------------ */
/* Setup                                                                     */
/* ------------------------------------------------------------------------ */

esp_err_t hub75_init(const hub75_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "no config");
    ESP_RETURN_ON_FALSE(!s.running, ESP_ERR_INVALID_STATE, TAG, "already running");
    ESP_RETURN_ON_FALSE(cfg->color_depth >= 1 && cfg->color_depth <= 8, ESP_ERR_INVALID_ARG,
                        TAG, "color_depth must be 1..8");
    ESP_RETURN_ON_FALSE(cfg->core == 0 || cfg->core == 1, ESP_ERR_INVALID_ARG, TAG, "core must be 0 or 1");

    const hub75_pins_t *p = &cfg->pins;
    const int pins[PIN_COUNT] = { p->r1, p->g1, p->b1, p->r2, p->g2, p->b2,
                                  p->a, p->b, p->c, p->d, p->e, p->clk, p->lat, p->oe };
    static const char *const names[PIN_COUNT] = { "R1", "G1", "B1", "R2", "G2", "B2",
                                                  "A", "B", "C", "D", "E", "CLK", "LAT", "OE" };
    uint32_t mask = 0;
    for (int i = 0; i < PIN_COUNT; i++) {
        ESP_RETURN_ON_FALSE(pins[i] >= 0 && pins[i] < 32, ESP_ERR_INVALID_ARG, TAG,
                            "%s: GPIO %d is not in 0..31 (set cfg.pins)", names[i], pins[i]);
        ESP_RETURN_ON_FALSE(!(mask & (1u << pins[i])), ESP_ERR_INVALID_ARG, TAG,
                            "%s: GPIO %d is used twice (set cfg.pins)", names[i], pins[i]);
        mask |= 1u << pins[i];
    }

    memset(&s, 0, sizeof(s));
    s.cfg = *cfg;
    s.mask_all = mask;
    s.mask_colour = (1u << p->r1) | (1u << p->g1) | (1u << p->b1) |
                    (1u << p->r2) | (1u << p->g2) | (1u << p->b2);
    s.bit_clk = 1u << p->clk;
    s.bit_lat = 1u << p->lat;
    s.bit_oe = 1u << p->oe;

    for (uint32_t w = 0; w < COLOUR_WORDS; w++) {
        s.lut[w] = ((w & 0x01) ? (1u << p->r1) : 0) | ((w & 0x02) ? (1u << p->g1) : 0) |
                   ((w & 0x04) ? (1u << p->b1) : 0) | ((w & 0x08) ? (1u << p->r2) : 0) |
                   ((w & 0x10) ? (1u << p->g2) : 0) | ((w & 0x20) ? (1u << p->b2) : 0);
    }
    for (int row = 0; row < HUB75_ROWS; row++) {
        s.addr[row] = ((row & 0x01) ? (1u << p->a) : 0) | ((row & 0x02) ? (1u << p->b) : 0) |
                      ((row & 0x04) ? (1u << p->c) : 0) | ((row & 0x08) ? (1u << p->d) : 0) |
                      ((row & 0x10) ? (1u << p->e) : 0);
    }

    const float gamma = cfg->gamma > 0.0f ? cfg->gamma : 2.2f;
    const int max_level = (1 << cfg->color_depth) - 1;
    for (int v = 0; v < 256; v++) {
        s.gamma[v] = (uint8_t)lroundf(powf((float)v / 255.0f, gamma) * (float)max_level);
    }

    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t packed_bytes = (size_t)cfg->color_depth * PLANE_BYTES;
    s.packed[0] = heap_caps_calloc(1, packed_bytes, caps);
    s.packed[1] = heap_caps_calloc(1, packed_bytes, caps);
    s.draw = heap_caps_calloc(HUB75_HEIGHT * HUB75_WIDTH, 3, caps);
    s.presented = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s.packed[0] && s.packed[1] && s.draw && s.presented, ESP_ERR_NO_MEM,
                        TAG, "out of internal RAM");
    s.front = s.packed[0];
    s.pending = NULL;
    s.brightness = cfg->brightness;
    s.output_on = true;

    /* Levels first (OE high = dark), then enable the outputs. */
    REG_WRITE(GPIO_OUT_W1TC_REG, mask & ~s.bit_oe);
    REG_WRITE(GPIO_OUT_W1TS_REG, s.bit_oe);
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config");
    for (int i = 0; i < PIN_COUNT; i++) {
        gpio_set_drive_capability(pins[i], GPIO_DRIVE_CAP_3);   /* strongest edges into the 5 V logic */
    }

    if (cfg->driver == HUB75_DRIVER_FM6126A) {
        fm6126a_init();
    }

    BaseType_t ok = xTaskCreatePinnedToCore(refresh_task, "hub75", 3072, NULL,
                                            cfg->task_priority, NULL, cfg->core);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "refresh task");
    s.running = true;

    ESP_LOGI(TAG, "64x64 1/32 scan, %d bit planes, brightness %u, refresh on core %d",
             cfg->color_depth, cfg->brightness, cfg->core);
    ESP_LOGI(TAG, "R1=%d G1=%d B1=%d R2=%d G2=%d B2=%d A=%d B=%d C=%d D=%d E=%d CLK=%d LAT=%d OE=%d",
             p->r1, p->g1, p->b1, p->r2, p->g2, p->b2, p->a, p->b, p->c, p->d, p->e,
             p->clk, p->lat, p->oe);
    return ESP_OK;
}

void hub75_set_brightness(uint8_t level)
{
    s.brightness = level;
}

uint8_t hub75_get_brightness(void)
{
    return s.brightness;
}

void hub75_set_output(bool enabled)
{
    s.output_on = enabled;
}

float hub75_refresh_hz(void)
{
    int64_t us = s.sample_us;
    return us > 0 ? (float)FPS_SAMPLE_FRAMES * 1e6f / (float)us : 0.0f;
}

/* ------------------------------------------------------------------------ */
/* Back buffer                                                               */
/* ------------------------------------------------------------------------ */

void hub75_clear(void)
{
    if (s.draw) memset(s.draw, 0, HUB75_HEIGHT * HUB75_WIDTH * 3);
}

void hub75_fill(uint8_t r, uint8_t g, uint8_t b)
{
    hub75_fill_rect(0, 0, HUB75_WIDTH, HUB75_HEIGHT, r, g, b);
}

void hub75_draw_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s.draw || x < 0 || y < 0 || x >= HUB75_WIDTH || y >= HUB75_HEIGHT) return;
    uint8_t *px = s.draw[y][x];
    px[0] = r;
    px[1] = g;
    px[2] = b;
}

void hub75_fill_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s.draw) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > HUB75_WIDTH ? HUB75_WIDTH : x + w;
    int y1 = y + h > HUB75_HEIGHT ? HUB75_HEIGHT : y + h;
    for (int yy = y0; yy < y1; yy++) {
        for (int xx = x0; xx < x1; xx++) {
            uint8_t *px = s.draw[yy][xx];
            px[0] = r;
            px[1] = g;
            px[2] = b;
        }
    }
}

void hub75_draw_hline(int x, int y, int w, uint8_t r, uint8_t g, uint8_t b)
{
    hub75_fill_rect(x, y, w, 1, r, g, b);
}

void hub75_draw_vline(int x, int y, int h, uint8_t r, uint8_t g, uint8_t b)
{
    hub75_fill_rect(x, y, 1, h, r, g, b);
}

void hub75_draw_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    if (w <= 0 || h <= 0) return;
    hub75_draw_hline(x, y, w, r, g, b);
    hub75_draw_hline(x, y + h - 1, w, r, g, b);
    hub75_draw_vline(x, y, h, r, g, b);
    hub75_draw_vline(x + w - 1, y, h, r, g, b);
}

/* RGB888 -> bit planes. Byte layout per column: bit0 R1, bit1 G1, bit2 B1,
 * bit3 R2, bit4 G2, bit5 B2, matching s.lut. */
static void pack_frame(uint8_t *dst)
{
    const int depth = s.cfg.color_depth;
    const uint8_t *gamma = s.gamma;

    for (int row = 0; row < HUB75_ROWS; row++) {
        for (int x = 0; x < HUB75_WIDTH; x++) {
            const uint8_t *top = s.draw[row][x];
            const uint8_t *bot = s.draw[row + HUB75_ROWS][x];
            const uint8_t v[6] = { gamma[top[0]], gamma[top[1]], gamma[top[2]],
                                   gamma[bot[0]], gamma[bot[1]], gamma[bot[2]] };
            for (int plane = 0; plane < depth; plane++) {
                uint8_t word = 0;
                for (int c = 0; c < 6; c++) {
                    word |= ((v[c] >> plane) & 1) << c;
                }
                dst[PLANE_OFFSET(plane, row) + x] = word;
            }
        }
    }
}

void hub75_present(void)
{
    if (!s.running) return;
    uint8_t *back = (s.front == s.packed[0]) ? s.packed[1] : s.packed[0];
    pack_frame(back);
    s.pending = back;
    xSemaphoreTake(s.presented, portMAX_DELAY);
}

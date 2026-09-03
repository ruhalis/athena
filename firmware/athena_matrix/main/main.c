/* Athena matrix demo: cycles through scenes that prove the wiring, the colour
 * depth and the frame rate of the panel. Replace app_main's loop with the real
 * serial protocol once the hardware checks out.
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "board_pins.h"
#include "hub75.h"

static const char *TAG = "athena_matrix";

#define SCENE_MS   6000
#define FRAME_MS   25

/* Wiring check: red top-left, green top-right, blue bottom-left, white
 * bottom-right, one-pixel white border. A swapped colour line shows as the
 * wrong colour in a quadrant; a missing E line shows as a wrong bottom half;
 * a wrong A..D line shows as scrambled rows. */
static void scene_wiring_test(void)
{
    hub75_clear();
    hub75_fill_rect(0, 0, 32, 32, 255, 0, 0);
    hub75_fill_rect(32, 0, 32, 32, 0, 255, 0);
    hub75_fill_rect(0, 32, 32, 32, 0, 0, 255);
    hub75_fill_rect(32, 32, 32, 32, 255, 255, 255);
    hub75_draw_rect(0, 0, HUB75_WIDTH, HUB75_HEIGHT, 255, 255, 255);
}

/* Eight colour bars on top, red / green / blue / grey ramps below. */
static void scene_bars(void)
{
    static const uint8_t bars[8][3] = {
        {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
        {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0},
    };
    hub75_clear();
    for (int i = 0; i < 8; i++) {
        hub75_fill_rect(i * 8, 0, 8, 28, bars[i][0], bars[i][1], bars[i][2]);
    }
    for (int x = 0; x < HUB75_WIDTH; x++) {
        uint8_t v = (uint8_t)(x * 255 / (HUB75_WIDTH - 1));
        hub75_fill_rect(x, 30, 1, 8, v, 0, 0);
        hub75_fill_rect(x, 39, 1, 8, 0, v, 0);
        hub75_fill_rect(x, 48, 1, 8, 0, 0, v);
        hub75_fill_rect(x, 57, 1, 7, v, v, v);
    }
}

/* A clock face the way the Athena protocol will draw it, plus a scrolling label. */
static void scene_text(uint32_t frame)
{
    static const char *clock_text = "12:34";
    hub75_clear();

    int w = hub75_text_width(clock_text, 2);
    hub75_draw_text((HUB75_WIDTH - w) / 2, 22, clock_text, 2, 255, 255, 255);
    if ((frame / 20) & 1) {                      /* blink the colon: hide it every other half second */
        int cx = (HUB75_WIDTH - w) / 2 + 2 * 6 * 2;
        hub75_fill_rect(cx, 22, 10, 14, 0, 0, 0);
    }

    const char *label = "ATHENA";
    int lw = hub75_text_width(label, 1);
    int x = HUB75_WIDTH - (int)(frame % (uint32_t)(HUB75_WIDTH + lw));
    hub75_draw_text(x, 4, label, 1, 0, 200, 255);

    hub75_draw_hline(8, 50, 48, 40, 40, 40);
    hub75_fill_rect(8 + (int)(frame % 48), 49, 4, 3, 0, 255, 120);
}

/* A ball bouncing inside a frame: motion shows tearing or a low refresh rate. */
static void scene_bounce(uint32_t frame)
{
    static int x = 10, y = 20, dx = 1, dy = 1;
    (void)frame;
    hub75_clear();
    hub75_draw_rect(0, 0, HUB75_WIDTH, HUB75_HEIGHT, 60, 60, 60);
    x += dx;
    y += dy;
    if (x <= 1 || x >= HUB75_WIDTH - 5) dx = -dx;
    if (y <= 1 || y >= HUB75_HEIGHT - 5) dy = -dy;
    hub75_fill_rect(x, y, 4, 4, 255, 120, 0);
    hub75_fill_rect(x + 1, y + 1, 2, 2, 255, 255, 200);
}

static void run_scene(const char *name, void (*draw)(uint32_t), uint32_t duration_ms)
{
    ESP_LOGI(TAG, "scene: %s", name);
    int64_t end = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    uint32_t frame = 0;
    while (esp_timer_get_time() < end) {
        draw(frame++);
        hub75_present();
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
    ESP_LOGI(TAG, "panel refresh %.0f Hz, free heap %lu",
             (double)hub75_refresh_hz(), (unsigned long)esp_get_free_heap_size());
}

static void draw_wiring(uint32_t f) { (void)f; scene_wiring_test(); }
static void draw_bars(uint32_t f)   { (void)f; scene_bars(); }

void app_main(void)
{
    hub75_config_t cfg = HUB75_CONFIG_DEFAULT();
    cfg.pins = (hub75_pins_t)BOARD_HUB75_PINS;
    cfg.brightness = 40;            /* wiring-safe; raise once the 5 V supply is proven */
    /* cfg.driver = HUB75_DRIVER_FM6126A;  -- only if the panel stays dark with correct wiring */

    ESP_ERROR_CHECK(hub75_init(&cfg));

    for (;;) {
        run_scene("wiring test", draw_wiring, SCENE_MS);
        run_scene("colour bars", draw_bars, SCENE_MS);
        run_scene("clock text", scene_text, SCENE_MS);
        run_scene("bounce", scene_bounce, SCENE_MS);
    }
}

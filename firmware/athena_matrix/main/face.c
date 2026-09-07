/* face.c - the `render` task. Owns the current mode, its expiry, the
 * remembered `t` text and the animation clock; draws one frame every
 * FRAME_MS and presents it.
 *
 * The eight agent states are one picture, the aura (aura.c): a circle outline
 * seen through a turbulence warp, in the state's colour and rhythm, easing
 * from one state to the next. `t` sits in the centre of the ring in idle and
 * alert. The two maintenance modes (test, off) are drawn here.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "aura.h"
#include "face.h"
#include "hub75.h"
#include "protocol.h"

static const char *TAG = "face";

#define FRAME_MS        25          /* 40 fps */
#define BLINK_MIN_MS    3000        /* re-armed by set_mode; the aura has no blink */
#define BLINK_MAX_MS    5000
#define TEXT_Y          29          /* `t` in the centre of the ring, 5x7 font */

typedef struct {
    QueueHandle_t queue;
    face_mode_t mode;
    int64_t expiry_us;              /* when the mode falls back to idle, 0 = sticky */
    char text[FACE_TEXT_MAX + 1];   /* `t`, kept across modes */
    uint32_t frame;                 /* frames since the mode was entered */
    int64_t now_us;                 /* time of the frame being drawn */
    int64_t next_blink_us;          /* set by set_mode, not read by the aura */
    int64_t blink_until_us;
    int64_t mouth_step_us;
    uint32_t prng;
} face_state_t;

static face_state_t s = {
    .mode = FACE_MODE_TEST,         /* boot: the wiring check, sticky, until the host speaks */
    .prng = 0x2545F491u,
};

/* ------------------------------------------------------------------------ */
/* Helpers shared by the modes                                               */
/* ------------------------------------------------------------------------ */

static uint32_t prng_next(void)
{
    uint32_t x = s.prng;            /* xorshift32 */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s.prng = x;
    return x;
}

static uint32_t prng_range(uint32_t lo, uint32_t hi)    /* lo..hi inclusive */
{
    return lo + prng_next() % (hi - lo + 1);
}

/* Which agent states show `t`; the aura fades it in and out with the state. */
static const bool aura_text[FACE_MODE_COUNT] = {
    [FACE_MODE_IDLE]  = true,
    [FACE_MODE_ALERT] = true,
};

/* One agent state as the aura; the renderer tweens from whatever it drew last. */
static void draw_aura(uint32_t frame)
{
    (void)frame;
    aura_draw(s.mode, aura_text[s.mode], s.now_us, s.text, TEXT_Y);
}

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

/* ------------------------------------------------------------------------ */
/* The maintenance modes. The back buffer is already black.                  */
/* ------------------------------------------------------------------------ */

static void draw_test(uint32_t frame)
{
    (void)frame;
    scene_wiring_test();
}

static void draw_off(uint32_t frame)
{
    (void)frame;                    /* output is disabled; the black buffer is what comes back on */
}

typedef void (*draw_fn_t)(uint32_t frame);

static const draw_fn_t draw_fns[FACE_MODE_COUNT] = {
    [FACE_MODE_IDLE]   = draw_aura,
    [FACE_MODE_LISTEN] = draw_aura,
    [FACE_MODE_THINK]  = draw_aura,
    [FACE_MODE_WORK]   = draw_aura,
    [FACE_MODE_SPEAK]  = draw_aura,
    [FACE_MODE_ALERT]  = draw_aura,
    [FACE_MODE_ERROR]  = draw_aura,
    [FACE_MODE_SLEEP]  = draw_aura,
    [FACE_MODE_TEST]   = draw_test,
    [FACE_MODE_OFF]    = draw_off,
};

/* ------------------------------------------------------------------------ */
/* State changes and the frame loop                                          */
/* ------------------------------------------------------------------------ */

static void set_mode(face_mode_t mode, int32_t ttl_s)
{
    s.mode = mode;
    s.frame = 0;
    s.expiry_us = ttl_s > 0 ? s.now_us + (int64_t)ttl_s * 1000000 : 0;
    s.blink_until_us = 0;
    s.next_blink_us = s.now_us + (int64_t)prng_range(BLINK_MIN_MS, BLINK_MAX_MS) * 1000;
    s.mouth_step_us = 0;
    if (draw_fns[mode] != draw_aura) aura_reset();   /* the next aura fades in from dark */
    hub75_set_output(mode != FACE_MODE_OFF);
    ESP_LOGI(TAG, "mode %s ttl %ld", face_mode_name(mode), (long)ttl_s);
}

static void apply_cmd(const face_cmd_t *cmd)
{
    if (cmd->has_brightness) {
        hub75_set_brightness(cmd->brightness);
        ESP_LOGI(TAG, "brightness %u", (unsigned)cmd->brightness);
    }
    if (cmd->has_text) {
        memcpy(s.text, cmd->text, sizeof(s.text));   /* the serial task guarantees the terminator */
        ESP_LOGI(TAG, "text \"%s\"", s.text);
    }
    if (cmd->has_mode) {
        set_mode(cmd->mode, cmd->has_ttl ? cmd->ttl_s : face_mode_default_ttl(cmd->mode));
    } else if (cmd->has_ttl) {
        /* A ttl on its own re-arms the current mode from now. */
        s.expiry_us = cmd->ttl_s > 0 ? s.now_us + (int64_t)cmd->ttl_s * 1000000 : 0;
        ESP_LOGI(TAG, "mode %s ttl %ld", face_mode_name(s.mode), (long)cmd->ttl_s);
    }
}

static void render_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    (void)arg;

    for (;;) {
        s.now_us = esp_timer_get_time();

        face_cmd_t cmd;
        while (xQueueReceive(s.queue, &cmd, 0) == pdTRUE) {
            apply_cmd(&cmd);
        }
        if (s.expiry_us && s.now_us >= s.expiry_us) {
            ESP_LOGI(TAG, "mode %s expired", face_mode_name(s.mode));
            set_mode(FACE_MODE_IDLE, 0);                /* `t` survives the fallback */
        }

        hub75_clear();
        draw_fns[s.mode](s.frame);
        hub75_present();
        s.frame++;

        vTaskDelayUntil(&wake, pdMS_TO_TICKS(FRAME_MS));
    }
}

esp_err_t face_start(QueueHandle_t queue)
{
    if (!queue) return ESP_ERR_INVALID_ARG;
    s.queue = queue;
    BaseType_t ok = xTaskCreatePinnedToCore(render_task, "render", 4096, NULL, 5, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

/* face.c - the `render` task. Owns the current mode, its expiry, the
 * remembered `t` text and the animation clock; draws one frame every 25 ms
 * and presents it.
 *
 * Every state shares one pair of eyes (draw_eyes) so the face stays
 * recognisable; a mode changes their size, position and colour and what sits
 * around them. Geometry is fixed for the 64x64 panel: eyes in the upper two
 * thirds, mouth / text / progress in the lower third.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "face.h"
#include "hub75.h"
#include "protocol.h"

static const char *TAG = "face";

#define FRAME_MS        25          /* 40 fps */
#define BLINK_MS        120         /* how long the eyes stay shut */
#define BLINK_MIN_MS    3000        /* gap between blinks, lower bound */
#define BLINK_MAX_MS    5000        /* gap between blinks, upper bound */
#define MOUTH_STEP_MS   80          /* speak: new bar heights this often */

#define EYE_W           14
#define EYE_H           14
#define EYE_CLOSED_H    2
#define EYE_LEFT_CX     19          /* eye centres */
#define EYE_RIGHT_CX    45
#define EYE_CY          26
#define TEXT_Y          46          /* `t` under the eyes, 5x7 font */
#define MOUTH_BARS      5
#define MOUTH_BOTTOM    61          /* speak: bars grow upwards from here */

typedef struct { uint8_t r, g, b; } rgb_t;

static const rgb_t col_idle   = { 180, 220, 255 };
static const rgb_t col_listen = { 60, 220, 120 };
static const rgb_t col_think  = { 170, 110, 255 };
static const rgb_t col_work   = { 255, 170, 0 };
static const rgb_t col_alert  = { 255, 190, 0 };
static const rgb_t col_error  = { 255, 40, 40 };
static const rgb_t col_text   = { 90, 90, 90 };

typedef struct {
    QueueHandle_t queue;
    face_mode_t mode;
    int64_t expiry_us;              /* when the mode falls back to idle, 0 = sticky */
    char text[FACE_TEXT_MAX + 1];   /* `t`, kept across modes */
    uint32_t frame;                 /* frames since the mode was entered */
    int64_t now_us;                 /* time of the frame being drawn */
    int64_t next_blink_us;
    int64_t blink_until_us;
    int64_t mouth_step_us;
    uint8_t mouth[MOUTH_BARS];
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

static rgb_t dim(rgb_t c, int divisor)
{
    rgb_t d = { (uint8_t)(c.r / divisor), (uint8_t)(c.g / divisor), (uint8_t)(c.b / divisor) };
    return d;
}

/* One eye: a filled box with the corners knocked out so it reads as round. */
static void draw_eye(int x, int y, int w, int h, rgb_t c)
{
    hub75_fill_rect(x, y, w, h, c.r, c.g, c.b);
    if (w >= 4 && h >= 4) {
        hub75_draw_pixel(x, y, 0, 0, 0);
        hub75_draw_pixel(x + w - 1, y, 0, 0, 0);
        hub75_draw_pixel(x, y + h - 1, 0, 0, 0);
        hub75_draw_pixel(x + w - 1, y + h - 1, 0, 0, 0);
    }
}

/* The pair of eyes every mode is built on, shifted by an offset from the
 * idle position and sized w x h around the same centres. */
static void draw_eyes(int x_offset, int y_offset, int w, int h, rgb_t c)
{
    int y = EYE_CY + y_offset - h / 2;
    draw_eye(EYE_LEFT_CX + x_offset - w / 2, y, w, h, c);
    draw_eye(EYE_RIGHT_CX + x_offset - w / 2, y, w, h, c);
}

/* True while a blink is in progress. Schedules the next one 3 to 5 s out. */
static bool blink_closed(void)
{
    if (s.now_us >= s.next_blink_us) {
        s.blink_until_us = s.now_us + BLINK_MS * 1000;
        s.next_blink_us = s.blink_until_us + (int64_t)prng_range(BLINK_MIN_MS, BLINK_MAX_MS) * 1000;
    }
    return s.now_us < s.blink_until_us;
}

/* Idle's eyes, blinking: the base of idle, speak and alert. */
static void draw_calm_eyes(rgb_t c)
{
    draw_eyes(0, 0, EYE_W, blink_closed() ? EYE_CLOSED_H : EYE_H, c);
}

/* `t`, centred under the eyes, dim grey. Nothing if it is empty. */
static void draw_text_centred(void)
{
    if (!s.text[0]) return;
    int w = hub75_text_width(s.text, 1);
    hub75_draw_text((HUB75_WIDTH - w) / 2, TEXT_Y, s.text, 1, col_text.r, col_text.g, col_text.b);
}

/* An X two pixels thick, filling a size x size box. */
static void draw_cross(int x, int y, int size, rgb_t c)
{
    for (int i = 0; i < size; i++) {
        int xl = x + i;
        int xr = x + size - 1 - i;
        hub75_draw_pixel(xl, y + i, c.r, c.g, c.b);
        hub75_draw_pixel(xr, y + i, c.r, c.g, c.b);
        if (i + 1 < size) {
            hub75_draw_pixel(xl + 1, y + i, c.r, c.g, c.b);
            hub75_draw_pixel(xr - 1, y + i, c.r, c.g, c.b);
        }
    }
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
/* One draw function per mode. The back buffer is already black.            */
/* ------------------------------------------------------------------------ */

static void draw_idle(uint32_t frame)
{
    (void)frame;
    draw_calm_eyes(col_idle);
    draw_text_centred();
}

static void draw_listen(uint32_t frame)
{
    draw_eyes(0, 0, EYE_W + 4, blink_closed() ? EYE_CLOSED_H : EYE_H + 4, col_listen);   /* attentive */

    /* A bar under the eyes whose width breathes once a second. */
    int phase = (int)(frame % 40);
    int amp = phase < 20 ? phase : 40 - phase;          /* 0..20..0 */
    int w = 12 + amp * 32 / 20;                         /* 12..44 px */
    hub75_fill_rect(HUB75_WIDTH / 2 - w / 2, 41, w, 2, col_listen.r, col_listen.g, col_listen.b);
}

static void draw_think(uint32_t frame)
{
    draw_eyes(5, -6, EYE_W, EYE_H, col_think);         /* looking up and away */

    /* Three dots above the right eye, lighting up one after another. */
    int lit = (int)((frame / 10) % 4);                  /* 0..3 dots on, a step every 250 ms */
    rgb_t off = dim(col_think, 6);
    int x = EYE_RIGHT_CX + 5 - EYE_W / 2 + 1;
    for (int i = 0; i < 3; i++) {
        rgb_t c = i < lit ? col_think : off;
        hub75_fill_rect(x + i * 5, 8, 2, 2, c.r, c.g, c.b);
    }
}

static void draw_work(uint32_t frame)
{
    draw_eyes(0, 0, EYE_W, EYE_H / 2, col_work);       /* narrowed */

    /* A bright segment sweeping left to right along a dim track at the bottom. */
    const int track_x = 8, track_w = 48, seg_w = 8;
    rgb_t track = dim(col_work, 5);
    hub75_fill_rect(track_x, 58, track_w, 2, track.r, track.g, track.b);

    int pos = (int)(frame % (uint32_t)(track_w + seg_w)) - seg_w;    /* enters from the left, leaves on the right */
    int x0 = pos < 0 ? 0 : pos;
    int x1 = pos + seg_w > track_w ? track_w : pos + seg_w;
    if (x1 > x0) {
        hub75_fill_rect(track_x + x0, 58, x1 - x0, 2, col_work.r, col_work.g, col_work.b);
    }
}

static void draw_speak(uint32_t frame)
{
    (void)frame;
    draw_calm_eyes(col_idle);

    /* A mouth of five bars in the lower third, new heights every ~80 ms. */
    if (s.now_us - s.mouth_step_us >= MOUTH_STEP_MS * 1000) {
        s.mouth_step_us = s.now_us;
        for (int i = 0; i < MOUTH_BARS; i++) {
            s.mouth[i] = (uint8_t)prng_range(2, 16);
        }
    }
    for (int i = 0; i < MOUTH_BARS; i++) {
        int h = s.mouth[i];
        hub75_fill_rect(17 + i * 6, MOUTH_BOTTOM - h, 5, h, col_idle.r, col_idle.g, col_idle.b);
    }
}

static void draw_alert(uint32_t frame)
{
    draw_calm_eyes(col_idle);
    draw_text_centred();

    /* An exclamation mark on the right, half a second on, half a second off. */
    if ((frame / 20) % 2 == 0) {
        hub75_fill_rect(57, 5, 3, 13, col_alert.r, col_alert.g, col_alert.b);
        hub75_fill_rect(57, 20, 3, 3, col_alert.r, col_alert.g, col_alert.b);
    }
}

static void draw_error(uint32_t frame)
{
    (void)frame;
    draw_cross(EYE_LEFT_CX - EYE_W / 2, EYE_CY - EYE_H / 2, EYE_W, col_error);
    draw_cross(EYE_RIGHT_CX - EYE_W / 2, EYE_CY - EYE_H / 2, EYE_W, col_error);
    hub75_draw_rect(0, 0, HUB75_WIDTH, HUB75_HEIGHT, col_error.r, col_error.g, col_error.b);
}

static void draw_sleep(uint32_t frame)
{
    (void)frame;
    draw_eyes(0, 0, EYE_W, EYE_CLOSED_H, dim(col_idle, 4));    /* closed, and nothing else */
}

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
    [FACE_MODE_IDLE]   = draw_idle,
    [FACE_MODE_LISTEN] = draw_listen,
    [FACE_MODE_THINK]  = draw_think,
    [FACE_MODE_WORK]   = draw_work,
    [FACE_MODE_SPEAK]  = draw_speak,
    [FACE_MODE_ALERT]  = draw_alert,
    [FACE_MODE_ERROR]  = draw_error,
    [FACE_MODE_SLEEP]  = draw_sleep,
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

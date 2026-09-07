/* aura.c - the "aura" face renderer for the 64x64 panel.
 *
 * This is a port of the aura shader from LiveKit's components-js
 * (packages/shadcn/components/agents-ui/agent-audio-visualizer-aura.tsx and
 * hooks/agents-ui/use-agent-audio-visualizer-aura.ts). The shader was
 * developed for Unicorn Studio and is licensed under the Polyform Non-Resale
 * License 1.0.0, (c) 2026 UNCRN LLC:
 * https://polyformproject.org/licenses/non-resale/1.0.0/
 * Keep this notice with the code; the licence requires it.
 *
 * Changes from the original:
 *   - a C port instead of GLSL, run on the CPU one pixel at a time;
 *   - fixed function: no uniforms, the per-state numbers are the table below;
 *   - lower resolution: the shader runs on a 32x32 grid with AURA_COPIES
 *     copies and AURA_LAYERS turbulence layers (36 and 4 upstream), then is
 *     upscaled to the 64x64 panel;
 *   - per-state colours instead of one colour;
 *   - added states: work, alert, error and sleep next to LiveKit's idle,
 *     listening, thinking and speaking;
 *   - the `t` text in the centre of the ring;
 *   - a haze of the state's colour over the whole panel;
 *   - every state is the idle picture with different numbers, and a state
 *     change tweens those numbers instead of cutting.
 *
 * How it works: a circle outline is drawn AURA_COPIES times, each time seen
 * through a turbulence warp at a slightly different phase, and the copies are
 * averaged. Where the copies agree the band is solid; where they fan out it
 * dissolves. The reference for every number here is reference/aurora.js
 * (sceneAura); where this file and that one differ, that one is right.
 *
 * One vocabulary for all eight states (aura_params_t): the ring's size, the
 * turbulence's pace, amplitude and frequency, a brightness with a pulse on
 * top (depth, rate, sharpness), and four things that are 0 in idle and fade
 * in where a state uses them: the voice (speak: the ring swells and glows
 * with a level that comes in syllables and phrases), a tremor of the centre
 * (error), the red frame (error), and `t`. The palette stays close: listen,
 * think and work keep idle's cyan within a step of hue, so motion is what
 * tells them apart; only alert and error change colour outright. A state
 * change snapshots the numbers on screen and eases them to the new state's
 * over AURA_TWEEN_S, except the colour, which fades more slowly over
 * AURA_FADE_S as a mix of the two colours' light rather than a sweep round
 * the hue wheel; the turbulence and pulse phases are integrated from the
 * current pace, so nothing on the panel ever jumps.
 *
 * Per frame: shader at 32x32 -> display colour -> linear light -> bilinear
 * 2x -> haze, frame, `t` -> 5-bit level with a 4x4 Bayer dither -> the byte
 * the driver's gamma table maps back to exactly that level.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "aura.h"
#include "hub75.h"
#include "../components/hub75/font5x7.h"

static const char *TAG = "aura";

/* The three knobs the frame time hangs on. Overridable from the compiler line
 * so a host build can check the port against the JS at full size. */
#ifndef AURA_RES
#define AURA_RES            32          /* the shader's grid; the panel is 64 */
#endif
#ifndef AURA_COPIES
#define AURA_COPIES         12          /* N: circle copies averaged (ITERATIONS upstream, 36) */
#endif
#ifndef AURA_LAYERS
#define AURA_LAYERS         3           /* turbulence layers per copy (4 upstream) */
#endif
#define AURA_STEP           (HUB75_WIDTH / AURA_RES)   /* 1 or 2 */
#define AURA_BLUR           0.2f
#define AURA_SPACING        0.5f
#define AURA_VARIANCE       0.1f
#define AURA_SMOOTHING      1.0f
#define AURA_COLOR_SHIFT    0.05f
#define AURA_HAZE           0.03f       /* the haze is this much of the state's colour, snapped to whole driver levels per channel */
#define AURA_VOICE          0.03f       /* speak: the ring radius grows up to this with the voice level */
#define AURA_VOICE_GLOW     0.4f        /* speak: the brightness grows up to this with the voice level */
#define AURA_SYLLABLE_S     0.14f       /* speak: a new voice sample this often (500 per TIME_WRAP_S) */
#define AURA_PHRASE_S       0.7f        /* speak: a new phrase loudness this often (100 per TIME_WRAP_S) */
#define AURA_TREMOR         0.025f      /* error: the centre wanders this far, shader units */
#define AURA_TWEEN_S        0.8f        /* a state change eases over this long */
#define AURA_FADE_S         2.0f        /* the colour fades over this long, slower than the shape */
#define AURA_FADE_DIP       0.25f       /* the colour dims this much halfway through a fade */
#define AURA_DT_MAX         0.1f        /* a stall longer than this counts as this */
#define AURA_TOE            0.006f      /* linear light below this is level 0 */
#define AURA_LEVELS         31          /* the driver's 5 bit planes */
#define LEVEL_LIN           ((1.0f - AURA_TOE) / (float)AURA_LEVELS)   /* linear light per level above the toe */
#define AURA_LOG_US         5000000     /* frame-time log period */

#define TAU_F               6.28318530717958647692f
#define TIME_WRAP_S         70.0        /* the voice and tremor rhythms repeat within 70 s */
#define SIN_N               256         /* sine table entries per turn */
#define IDX_PER_RAD         ((float)SIN_N / TAU_F)

/* Every number that makes one state look like itself. All floats, so a tween
 * is one loop over the struct; the colour is linear RGB so a fade between two
 * colours is a mix of their light, softening through a pale blend instead of
 * sweeping through every hue in between. */
typedef struct {
    float speed;                        /* turbulence pace: the phase advances 0.05 * speed rad/s */
    float scale;                        /* ring radius, shader units (the panel is 1 across) */
    float amp, freq;                    /* turbulence amplitude and frequency knob */
    float bright;                       /* brightness at the bottom of the pulse (the tonemap multiplier) */
    float depth;                        /* the pulse adds up to this on top */
    float rate;                         /* pulses per second */
    float sharp;                        /* pulse shape: 1 a sine breath, 2 a flash, 3 a beat */
    float voice;                        /* 0..1: the size and brightness follow a voice level */
    float tremor;                       /* 0..1: the centre wanders quickly */
    float dy;                           /* the ring sits this far down (+) in the shader's frame */
    float haze;                         /* 0..1 of the state's haze */
    float border;                       /* 0..1: the pulsing red frame */
    float text;                         /* 0..1: `t` in the centre */
    float r, g, b;                      /* the base colour, linear light */
    float hr, hg, hb;                   /* the haze, driver levels per channel: whole numbers in a steady
                                         * state (a flat background), dithered between them while it fades */
} aura_params_t;

#define AURA_NPARAMS        (sizeof(aura_params_t) / sizeof(float))

typedef struct {
    float speed, scale, amp, freq, bright, depth, rate, sharp, voice, tremor, dy, haze, border;
    uint8_t col[3];                     /* sRGB */
} aura_state_t;

/* AURA and AURA_COL from aurora.js. Idle is the reference; every other state
 * is idle with some of these numbers moved. The idle/listen/think/speak
 * geometry is what LiveKit's hook animates to; work, alert, error and sleep
 * are added in the same vocabulary. */
static const aura_state_t states[FACE_MODE_SLEEP + 1] = {
    /*                     speed scale  amp   freq  bright depth rate  sharp voice tremor dy    haze border   colour */
    [FACE_MODE_IDLE]   = { 10,   0.24f, 0.9f, 0.4f, 1.0f,  0.0f, 0.15f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, { 0x1F, 0xD5, 0xF9 } },   /* cyan, steady, breathing through the turbulence */
    [FACE_MODE_LISTEN] = { 20,   0.30f, 1.0f, 0.7f, 1.5f,  0.5f, 1.43f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, { 0x24, 0xF2, 0xBF } },   /* mint: idle's hue a step toward green, larger, a quick shallow pulse */
    [FACE_MODE_THINK]  = { 30,   0.30f, 0.7f, 1.0f, 0.5f,  1.7f, 0.7f,  1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, { 0x25, 0x7E, 0xFA } },   /* azure: idle's hue a step toward blue, swirling faster, a slow deep swell */
    [FACE_MODE_WORK]   = { 40,   0.28f, 0.6f, 1.0f, 1.2f,  0.0f, 0.5f,  1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, { 0x5C, 0xE4, 0xFF } },   /* ice: idle's cyan lifted toward white, turning fast, steady */
    [FACE_MODE_SPEAK]  = { 25,   0.26f, 0.9f, 0.8f, 1.2f,  0.0f, 0.5f,  1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, { 0x1F, 0xD5, 0xF9 } },   /* cyan: idle's ring a little quicker, swelling and glowing with the voice */
    [FACE_MODE_ALERT]  = { 10,   0.24f, 0.9f, 0.4f, 1.0f,  1.5f, 1.0f,  2.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, { 0xFF, 0xC8, 0x14 } },   /* gold: idle's ring flashing once a second */
    [FACE_MODE_ERROR]  = { 40,   0.25f, 2.0f, 0.8f, 0.6f,  1.6f, 2.0f,  3.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, { 0xFF, 0x3C, 0x3C } },   /* red, torn by turbulence, beating twice a second, trembling, framed */
    [FACE_MODE_SLEEP]  = { 6,    0.20f, 0.8f, 0.4f, 0.6f,  0.25f,0.2f,  1.0f, 0.0f, 0.0f, 0.10f,0.0f, 0.0f, { 0x2B, 0x57, 0xD9 } },   /* deep blue: idle's ring smaller, dimmer, slower, settled low, no haze */
};

static const uint8_t bayer4[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

/* Byte to write for each 5-bit level: the driver maps it back through
 * gamma[v] = round((v/255)^2.2 * 31), and gamma[inv_gamma[l]] == l for every
 * l (checked on the host). hub75_set_brightness scales on top of this. */
static const uint8_t inv_gamma[AURA_LEVELS + 1] = {
    0, 54, 73, 88, 101, 111, 121, 130, 138, 145, 152, 159, 166, 172, 178, 183,
    189, 194, 199, 204, 209, 214, 218, 223, 227, 231, 235, 239, 243, 247, 251, 255
};

/* Tables and buffers, static so the render task's stack stays small. */
static float sin_tab[SIN_N + 1];                    /* sin(k * TAU / SIN_N) */
static float lin_tab[257];                          /* (k/256)^2.2: display colour to linear light */
static aura_params_t state_params[FACE_MODE_SLEEP + 1];   /* the table above, colours as linear light */
static float border_w[HUB75_WIDTH / 2];             /* frame weight by distance from the edge */
static float red_lin[3], text_lin[3];
static float shade[AURA_RES][AURA_RES][3];          /* the shader's output, linear light */
static uint8_t text_mask[HUB75_HEIGHT][HUB75_WIDTH]; /* 2 glyph, 1 halo, 0 clear */
static char mask_text[FACE_TEXT_MAX + 1];
static int mask_y = -1;
static bool inited;

/* The tween: what is on the panel now, where it came from, where it is going. */
static struct {
    bool live;                          /* false until the first draw after a reset */
    face_mode_t to_mode;
    bool to_text;
    aura_params_t from, cur, to;
    float u, v;                            /* progress of the shape tween and of the colour fade, 0..1 */
    int64_t last_us;
    float anim;                         /* turbulence phase, radians, 0..TAU */
    float pulse;                        /* pulse phase, turns, 0..1 */
} tw;

static struct {
    int64_t sum_us, max_us, last_log_us;
    uint32_t frames;
} stats;

/* ------------------------------------------------------------------------ */
/* Fast maths. sinf, sqrtf, floorf and float division are all library calls  */
/* on Xtensa; the pixel loop below runs ~80 sines and ~25 roots per pixel.   */
/* ------------------------------------------------------------------------ */

static inline float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

/* sin of an angle given in table units (SIN_N per turn), linear interpolation.
 * |a| must stay below 8192: adding the bias makes truncation a floor and
 * keeps ~10 fractional bits. */
static inline float fsin_idx(float a)
{
    float b = a + 8192.0f;
    int k = (int)b;
    float f = b - (float)k;
    const float *p = &sin_tab[k & (SIN_N - 1)];
    return p[0] + (p[1] - p[0]) * f;
}

/* 2^x for x >= 0: exponent from the integer part, a quadratic on the rest
 * (max error 0.3 %). */
static inline float fexp2(float x)
{
    if (x > 30.0f) x = 30.0f;
    int ip = (int)x;
    float fp = x - (float)ip;
    float p = 1.0f + fp * (0.6565f + 0.3435f * fp);
    union { float f; uint32_t u; } e = { .u = (uint32_t)(ip + 127) << 23 };
    return p * e.f;
}

/* sqrt via the reciprocal-root seed and two Newton steps: ~5e-6 relative. */
static inline float fsqrt(float x)
{
    if (x <= 0.0f) return 0.0f;
    union { float f; uint32_t u; } v = { .f = x };
    v.u = 0x5f375a86u - (v.u >> 1);
    float y = v.f;
    y = y * (1.5f - 0.5f * x * y * y);
    y = y * (1.5f - 0.5f * x * y * y);
    return x * y;
}

/* Integer hash to [0, 1): replaces the JS sin-based hash(), which is noise in float. */
static inline float hash01(uint32_t n)
{
    n ^= n >> 16; n *= 0x7feb352dU; n ^= n >> 15; n *= 0x846ca68bU; n ^= n >> 16;
    return (float)(n >> 8) * (1.0f / 16777216.0f);
}

/* A random level held for `period` seconds and eased into the next one. The
 * sequence has `count` samples and repeats, so a wall time that wraps at
 * period * count stays continuous. */
static float held_random(float t, float period, int count, uint32_t salt)
{
    float q = t / period;
    int k = (int)q;
    float u = q - (float)k;
    u = u * u * (3.0f - 2.0f * u);
    float e0 = hash01((uint32_t)(k % count + 1) * salt);       /* +1: hash01(0) is 0, index 0 must not be a forced rest */
    float e1 = hash01((uint32_t)((k + 1) % count + 1) * salt);
    return e0 + (e1 - e0) * u;
}

/* The voice level, 0..1: syllables (a new level every AURA_SYLLABLE_S) under
 * a phrase loudness (a new one every AURA_PHRASE_S) gated so the quietest
 * stretches are rests. Speech in bursts with pauses, not noise. */
static float voice_level(float t)
{
    float syllable = held_random(t, AURA_SYLLABLE_S, (int)(TIME_WRAP_S / AURA_SYLLABLE_S + 0.5), 7919u);
    float phrase = held_random(t, AURA_PHRASE_S, (int)(TIME_WRAP_S / AURA_PHRASE_S + 0.5), 104729u);
    phrase = (phrase - 0.15f) / 0.7f;
    if (phrase < 0.0f) phrase = 0.0f; else if (phrase > 1.0f) phrase = 1.0f;
    phrase = phrase * phrase * (3.0f - 2.0f * phrase);
    return syllable * phrase;
}

static inline float to_linear(float v)          /* v in 0..1 */
{
    float a = v * 256.0f;
    int k = (int)a;
    if (k >= 256) return 1.0f;
    float f = a - (float)k;
    return lin_tab[k] + (lin_tab[k + 1] - lin_tab[k]) * f;
}

/* Linear light back to the display value the shader's HSV is defined on:
 * the inverse of the 2.2 power the tables use. */
static inline float to_display(float lin)
{
    if (lin <= 0.0f) return 0.0f;
    if (lin >= 1.0f) return 1.0f;
    return powf(lin, 1.0f / 2.2f);
}

static float srgb_to_linear(uint8_t v)
{
    return powf((float)v / 255.0f, 2.2f);
}

static void rgb2hsv(float r, float g, float b, float *h, float *s, float *v)
{
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float d = mx - mn, hh = 0.0f;
    if (d > 1e-6f) {
        if (mx == r) hh = fmodf((g - b) / d, 6.0f);
        else if (mx == g) hh = (b - r) / d + 2.0f;
        else hh = (r - g) / d + 4.0f;
        hh /= 6.0f;
        if (hh < 0.0f) hh += 1.0f;
    }
    *h = hh;
    *s = mx > 0.0f ? d / mx : 0.0f;
    *v = mx;
}

static void hsv2rgb(float h, float s, float v, float *out)
{
    int i = (int)floorf(h * 6.0f);
    float f = h * 6.0f - (float)i, p = v * (1.0f - s), q = v * (1.0f - f * s), u = v * (1.0f - (1.0f - f) * s);
    switch (((i % 6) + 6) % 6) {
    case 0: out[0] = v; out[1] = u; out[2] = p; break;
    case 1: out[0] = q; out[1] = v; out[2] = p; break;
    case 2: out[0] = p; out[1] = v; out[2] = u; break;
    case 3: out[0] = p; out[1] = q; out[2] = v; break;
    case 4: out[0] = u; out[1] = p; out[2] = v; break;
    default: out[0] = v; out[1] = p; out[2] = q; break;
    }
}

/* The haze one state colour byte makes, as a whole number of driver levels. */
static float haze_level(uint8_t byte)
{
    float v = (AURA_HAZE * srgb_to_linear(byte) - AURA_TOE) * (1.0f / (1.0f - AURA_TOE)) * (float)AURA_LEVELS;
    return v <= 0.0f ? 0.0f : floorf(v + 0.5f);
}

static void init_tables(void)
{
    for (int k = 0; k <= SIN_N; k++) sin_tab[k] = sinf((float)k * TAU_F / (float)SIN_N);
    for (int k = 0; k <= 256; k++) lin_tab[k] = powf((float)k / 256.0f, 2.2f);
    for (int m = 0; m <= FACE_MODE_SLEEP; m++) {
        const aura_state_t *s = &states[m];
        aura_params_t *p = &state_params[m];
        p->speed = s->speed; p->scale = s->scale; p->amp = s->amp; p->freq = s->freq;
        p->bright = s->bright; p->depth = s->depth; p->rate = s->rate; p->sharp = s->sharp;
        p->voice = s->voice; p->tremor = s->tremor; p->dy = s->dy; p->haze = s->haze; p->border = s->border;
        p->text = 0.0f;
        p->r = srgb_to_linear(s->col[0]); p->g = srgb_to_linear(s->col[1]); p->b = srgb_to_linear(s->col[2]);
        p->hr = haze_level(s->col[0]); p->hg = haze_level(s->col[1]); p->hb = haze_level(s->col[2]);
    }
    for (int e = 0; e < HUB75_WIDTH / 2; e++) border_w[e] = e == 0 ? 1.0f : expf(-(float)e / 2.5f) * 0.4f;
    red_lin[0] = srgb_to_linear(255); red_lin[1] = srgb_to_linear(30); red_lin[2] = srgb_to_linear(30);
    text_lin[0] = srgb_to_linear(255); text_lin[1] = srgb_to_linear(225); text_lin[2] = srgb_to_linear(180);
    inited = true;
}

/* ------------------------------------------------------------------------ */
/* The tween                                                                 */
/* ------------------------------------------------------------------------ */

static void params_of(face_mode_t mode, bool with_text, aura_params_t *out)
{
    *out = state_params[mode];
    out->text = with_text ? 1.0f : 0.0f;
}

/* out = a + (b - a) * e, field by field, except the colour and the haze,
 * which mix at f. The colour dips a little halfway, scaled by how different
 * the two colours are, so a blend of two saturated colours reads as one
 * giving way to the other rather than a flash of white between them, and a
 * change that keeps the colour does not dim. */
static void params_mix(const aura_params_t *a, const aura_params_t *b, float e, float f, aura_params_t *out)
{
    const float *fa = (const float *)a, *fb = (const float *)b;
    float *fo = (float *)out;
    for (size_t k = 0; k < AURA_NPARAMS; k++) fo[k] = fa[k] + (fb[k] - fa[k]) * e;
    float d = fabsf(b->r - a->r);
    if (fabsf(b->g - a->g) > d) d = fabsf(b->g - a->g);
    if (fabsf(b->b - a->b) > d) d = fabsf(b->b - a->b);
    if (d > 1.0f) d = 1.0f;
    float k = 1.0f - AURA_FADE_DIP * d * 4.0f * f * (1.0f - f);
    out->r = (a->r + (b->r - a->r) * f) * k;
    out->g = (a->g + (b->g - a->g) * f) * k;
    out->b = (a->b + (b->b - a->b) * f) * k;
    out->hr = a->hr + (b->hr - a->hr) * f;
    out->hg = a->hg + (b->hg - a->hg) * f;
    out->hb = a->hb + (b->hb - a->hb) * f;
}

/* Advance the tween and the phases to now_us; leaves the frame's numbers in
 * tw.cur, the turbulence phase in tw.anim and the pulse phase in tw.pulse. */
static void tween_step(face_mode_t mode, bool with_text, int64_t now_us)
{
    aura_params_t target;
    params_of(mode, with_text, &target);

    float dt = 0.0f;
    if (!tw.live) {
        /* Cold start: the target state with the light off, then fade in. */
        tw.from = target;
        tw.from.bright = 0.0f; tw.from.depth = 0.0f; tw.from.haze = 0.0f;
        tw.from.border = 0.0f; tw.from.text = 0.0f;
        tw.cur = tw.from;
        tw.to = target;
        tw.u = 0.0f;
        tw.v = 0.0f;
        tw.anim = 0.0f;
        tw.pulse = 0.0f;
        tw.live = true;
    } else {
        int64_t d = now_us - tw.last_us;
        dt = d > 0 ? (float)d * 1e-6f : 0.0f;
        if (dt > AURA_DT_MAX) dt = AURA_DT_MAX;
        if (mode != tw.to_mode || with_text != tw.to_text) {
            tw.from = tw.cur;                       /* restart from what is on the panel now */
            tw.to = target;
            tw.u = 0.0f;
            tw.v = 0.0f;
        }
    }
    tw.last_us = now_us;
    tw.to_mode = mode;
    tw.to_text = with_text;

    if (tw.u < 1.0f || tw.v < 1.0f) {
        tw.u += dt / AURA_TWEEN_S;
        tw.v += dt / AURA_FADE_S;
        if (tw.u > 1.0f) tw.u = 1.0f;
        if (tw.v > 1.0f) tw.v = 1.0f;
        float e = tw.u * tw.u * (3.0f - 2.0f * tw.u);   /* ease in and out */
        float f = tw.v * tw.v * (3.0f - 2.0f * tw.v);   /* the colour, on its slower clock */
        params_mix(&tw.from, &tw.to, e, f, &tw.cur);
    }

    /* animTime = t * 0.1 * speed * 0.5 upstream; integrated from the current
     * pace so a speed change accelerates instead of jumping. Both phases wrap
     * where the shader cannot tell: anim per turn (every layer uses an integer
     * multiple of it), pulse per turn. */
    tw.anim += dt * 0.05f * tw.cur.speed;
    if (tw.anim >= TAU_F) tw.anim -= TAU_F;
    if (tw.anim < 0.0f) tw.anim += TAU_F;
    tw.pulse += dt * tw.cur.rate;
    if (tw.pulse >= 1.0f) tw.pulse -= floorf(tw.pulse);
}

/* ------------------------------------------------------------------------ */
/* `t`: glyph pixels are 2, the one-pixel halo around them 1 (textMask in    */
/* aurora.js). Rebuilt only when the text or the row changes.                */
/* ------------------------------------------------------------------------ */

static void build_text_mask(const char *text, int text_y)
{
    memset(text_mask, 0, sizeof(text_mask));
    int len = (int)strlen(text);
    if (len > FACE_TEXT_MAX) len = FACE_TEXT_MAX;
    int w = len * 6 - 1, x0 = (HUB75_WIDTH - w) / 2;

    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        const uint8_t *g = font5x7[(ch >= FONT5X7_FIRST && ch <= FONT5X7_LAST) ? ch - FONT5X7_FIRST : 0];
        for (int c = 0; c < 5; c++) {
            for (int r = 0; r < 7; r++) {
                int x = x0 + i * 6 + c, y = text_y + r;
                if ((g[c] & (1 << r)) && x >= 0 && x < HUB75_WIDTH && y >= 0 && y < HUB75_HEIGHT) {
                    text_mask[y][x] = 2;
                }
            }
        }
    }
    for (int y = 0; y < HUB75_HEIGHT; y++) {
        for (int x = 0; x < HUB75_WIDTH; x++) {
            if (text_mask[y][x]) continue;
            bool near = false;
            for (int dy = -1; dy <= 1 && !near; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int xx = x + dx, yy = y + dy;
                    if (xx >= 0 && xx < HUB75_WIDTH && yy >= 0 && yy < HUB75_HEIGHT && text_mask[yy][xx] == 2) {
                        near = true;
                        break;
                    }
                }
            }
            if (near) text_mask[y][x] = 1;
        }
    }
    strncpy(mask_text, text, FACE_TEXT_MAX);
    mask_text[FACE_TEXT_MAX] = '\0';
    mask_y = text_y;
}

/* ------------------------------------------------------------------------ */
/* The shader                                                                */
/* ------------------------------------------------------------------------ */

/* Everything in turb() that does not depend on the pixel or the copy: the
 * rotation matrix per layer (column-major like GLSL: m00 m10 m01 m11) and the
 * frequency, in table units per shader unit, plus its reciprocal. */
typedef struct {
    float m[AURA_LAYERS][4];
    float f_idx[AURA_LAYERS];
    float inv_f[AURA_LAYERS];
} turb_consts_t;

static void turb_consts(turb_consts_t *c, float freq)
{
    float m00 = 0.6f, m10 = -0.25f, m01 = 0.25f, m11 = 0.9f;
    float frequency = 2.0f + 13.0f * freq;
    for (int i = 0; i < AURA_LAYERS; i++) {
        c->m[i][0] = m00; c->m[i][1] = m10; c->m[i][2] = m01; c->m[i][3] = m11;
        c->f_idx[i] = frequency * IDX_PER_RAD;
        c->inv_f[i] = 1.0f / frequency;
        /* rotation *= mat2(0.6, -0.8, 0.8, 0.6) */
        float n00 = m00 * 0.6f + m01 * -0.8f, n10 = m10 * 0.6f + m11 * -0.8f;
        float n01 = m00 * 0.8f + m01 * 0.6f, n11 = m10 * 0.8f + m11 * 0.6f;
        m00 = n00; m10 = n10; m01 = n01; m11 = n11;
        frequency *= 1.4f;
    }
}

/* auraTurb(): the turbulence warp, AURA_LAYERS layers of rotated sine
 * displacement. ph holds (i * animTime + it) per layer in table units. */
static inline void turb(const turb_consts_t *c, const float *ph, float amplitude, float px, float py,
                        float *ox, float *oy)
{
    for (int i = 0; i < AURA_LAYERS; i++) {
        const float *M = c->m[i];
        float rx = px * M[0] + py * M[1];
        float ry = px * M[2] + py * M[3];
        float wx = fsin_idx(c->f_idx[i] * rx + ph[i]);
        float wy = fsin_idx(c->f_idx[i] * ry + ph[i]);
        float k = amplitude * c->inv_f[i];
        px += k * M[0] * wx;
        py += k * M[1] * wy;
        amplitude *= 1.0f + ((wx > wy ? wx : wy) - 1.0f) * AURA_VARIANCE;
    }
    *ox = px;
    *oy = py;
}

/* sceneAura() up to the tonemap, on the 32x32 grid, into shade[] as linear
 * light. p is this frame's numbers; bright, scale, dx, dy are the per-frame
 * values derived from them; hsv is the base colour as HSV of its display value. */
static void render_shader(const aura_params_t *p, float bright, float scale, float dx, float dy, const float *hsv)
{
    const float N = (float)AURA_COPIES;

    /* Per-copy colour: the hue drifts a little across the copies. */
    float cols[AURA_COPIES][3];
    for (int i = 1; i <= AURA_COPIES; i++) {
        float hh = hsv[0] + (1.0f - (float)i / N) * AURA_COLOR_SHIFT * 0.3f;
        hh -= floorf(hh);
        hsv2rgb(hh, hsv[1], hsv[2], cols[i - 1]);
    }

    /* Per-copy, per-layer sine phase: i * animTime + it, in table units.
     * Entry 0 is the `prev` copy at it = -1/N. */
    const float spacing = 1.0f + (TAU_F - 1.0f) * AURA_SPACING;
    float ph[AURA_COPIES + 1][AURA_LAYERS];
    for (int k = 0; k <= AURA_COPIES; k++) {
        float it = k == 0 ? -1.0f / N : ((float)k / N) * spacing;
        for (int i = 0; i < AURA_LAYERS; i++) ph[k][i] = ((float)i * tw.anim + it) * IDX_PER_RAD;
    }

    turb_consts_t tc;
    turb_consts(&tc, p->freq);

    const float amp = p->amp, gain = 1.2f * 4.0f / N;

    for (int j = 0; j < AURA_RES; j++) {
        float py = 0.5f - ((float)j + 0.5f) / (float)AURA_RES + dy;
        for (int i = 0; i < AURA_RES; i++) {
            float px = ((float)i + 0.5f) / (float)AURA_RES - 0.5f + dx;
            float prevx, prevy, ppr = 0.0f, ppg = 0.0f, ppb = 0.0f;
            turb(&tc, ph[0], amp, px, py, &prevx, &prevy);
            for (int k = 1; k <= AURA_COPIES; k++) {
                float sx, sy;
                turb(&tc, ph[k], amp, px, py, &sx, &sy);
                float d = fsqrt(sx * sx + sy * sy) - scale;
                if (d < 0.0f) d = -d;
                float ddx = sx - prevx, ddy = sy - prevy;
                float pd = fsqrt(ddx * ddx + ddy * ddy);
                prevx = sx;
                prevy = sy;
                float blur = (fexp2(pd * 2.0f) - 1.0f) * AURA_SMOOTHING;
                float edge = AURA_BLUR * 0.05f + (blur > 0.001f ? blur : 0.001f);
                if (d < edge) {                     /* smoothstep(0, edge, d) - 1: -1 on the outline, 0 away from it */
                    float u = d / edge;
                    float ds = u * u * (3.0f - 2.0f * u) - 1.0f;
                    ppr += ds * cols[k - 1][0];
                    ppg += ds * cols[k - 1][1];
                    ppb += ds * cols[k - 1][2];
                }
            }
            /* color = Tonemap(-pp * 1.2), Tonemap(x) = 4x / (1 + 4x), times brightness. */
            float r = -ppr * gain, g = -ppg * gain, b = -ppb * gain;
            r = clamp01(r / (1.0f + r) * bright);
            g = clamp01(g / (1.0f + g) * bright);
            b = clamp01(b / (1.0f + b) * bright);
            /* The shader's output is display colour; the panel wants linear light. */
            shade[j][i][0] = to_linear(r);
            shade[j][i][1] = to_linear(g);
            shade[j][i][2] = to_linear(b);
        }
    }
}

/* Linear light to the driver byte: 5-bit level with an ordered dither whose
 * amplitude fades in between the first and second level (nothing below the
 * first level speckles), a black toe, then the byte that maps back to it. */
static inline uint8_t quantise(float lin, float th)
{
    float v = (lin - AURA_TOE) * (1.0f / (1.0f - AURA_TOE));
    if (v <= 0.0f) return 0;
    if (v > 1.0f) v = 1.0f;
    v *= (float)AURA_LEVELS;
    float sm = v - 1.0f;                            /* smooth(1, 2, v) */
    sm = sm <= 0.0f ? 0.0f : sm >= 1.0f ? 1.0f : sm * sm * (3.0f - 2.0f * sm);
    int lv = (int)(v + 0.5f + (th - 0.5f) * sm);
    if (lv > AURA_LEVELS) lv = AURA_LEVELS;
    return inv_gamma[lv];
}

/* Bilinear 2x from shade[], then the haze (driver levels per channel: whole
 * ones land flat, a fraction is dithered between two levels, so the
 * background blends through a fade instead of switching), the frame (weight
 * `border`, beating with the pulse `wave`), `t` at weight `text` (its halo
 * dims what sits above the haze, never the haze itself), and the quantiser. */
static void render_panel(const float *haze, float border, float wave, float text, const uint8_t *mask)
{
    const float border_pulse = border * (0.4f + 0.6f * wave);
    const float halo = 1.0f - 0.75f * text, keep = 1.0f - 0.8f * text, ink = 0.8f * text;

    for (int y = 0; y < HUB75_HEIGHT; y++) {
        int j = y / AURA_STEP, jo = (y & 1) ? j + 1 : j - 1;
        if (jo < 0) jo = 0;
        if (jo >= AURA_RES) jo = AURA_RES - 1;
        int ey = y < HUB75_HEIGHT - 1 - y ? y : HUB75_HEIGHT - 1 - y;
        for (int x = 0; x < HUB75_WIDTH; x++) {
            int i = x / AURA_STEP, io = (x & 1) ? i + 1 : i - 1;
            if (io < 0) io = 0;
            if (io >= AURA_RES) io = AURA_RES - 1;
            const float *a = shade[j][i];
            float th = ((float)bayer4[(y & 3) * 4 + (x & 3)] + 0.5f) * (1.0f / 16.0f);
            /* Red and green dither against blue's complement, so a fade between
             * a blue haze and a red or green one swaps pixels rather than
             * leaving half of them dark halfway. */
            const float thc[3] = { 1.0f - th, 1.0f - th, th };
            float c[3], hz[3];
            for (int ch = 0; ch < 3; ch++) {
                int l = (int)(haze[ch] + thc[ch]);
                hz[ch] = l > 0 ? AURA_TOE + (float)l * LEVEL_LIN : 0.0f;
            }
#if AURA_STEP == 2
            /* Bilinear 2x: output centres sit a quarter pixel from the grid,
             * so the nearest sample weighs 9/16, its two neighbours 3/16, the diagonal 1/16. */
            const float *bx = shade[j][io], *by = shade[jo][i], *bd = shade[jo][io];
            for (int ch = 0; ch < 3; ch++) {
                c[ch] = 0.5625f * a[ch] + 0.1875f * (bx[ch] + by[ch]) + 0.0625f * bd[ch] + hz[ch];
            }
#else
            (void)io; (void)jo;
            for (int ch = 0; ch < 3; ch++) c[ch] = a[ch] + hz[ch];
#endif
            if (border_pulse > 0.0f) {
                int ex = x < HUB75_WIDTH - 1 - x ? x : HUB75_WIDTH - 1 - x;
                int e = ex < ey ? ex : ey;
                float bw = border_w[e] * border_pulse;
                c[0] += bw * red_lin[0]; c[1] += bw * red_lin[1]; c[2] += bw * red_lin[2];
            }
            if (mask) {
                uint8_t mv = mask[y * HUB75_WIDTH + x];
                if (mv == 1) {
                    /* Dim toward the haze, not toward black: the halo never
                     * goes darker than the background around it, so it does
                     * not punch a black hole where the haze is a level or two. */
                    c[0] = hz[0] + (c[0] - hz[0]) * halo;
                    c[1] = hz[1] + (c[1] - hz[1]) * halo;
                    c[2] = hz[2] + (c[2] - hz[2]) * halo;
                } else if (mv == 2) {
                    c[0] = c[0] * keep + text_lin[0] * ink;
                    c[1] = c[1] * keep + text_lin[1] * ink;
                    c[2] = c[2] * keep + text_lin[2] * ink;
                }
            }
            hub75_draw_pixel(x, y, quantise(c[0], th), quantise(c[1], th), quantise(c[2], th));
        }
    }
}

void aura_reset(void)
{
    tw.live = false;
}

void aura_draw(face_mode_t mode, bool with_text, int64_t now_us, const char *text, int text_y)
{
    if (!inited) init_tables();
    if ((unsigned)mode > FACE_MODE_SLEEP) mode = FACE_MODE_IDLE;
    int64_t t0 = esp_timer_get_time();

    tween_step(mode, with_text, now_us);
    const aura_params_t *p = &tw.cur;

    /* The voice and the tremor run on wall time modulo 70 s, which their
     * periods divide, so they stay continuous for days where a float t would not. */
    float t = (float)fmod((double)now_us * 1e-6, TIME_WRAP_S);

    /* The pulse: a raised cosine, sharpened into a flash or a beat by the exponent. */
    float wave = 0.5f - 0.5f * cosf(TAU_F * tw.pulse);
    if (p->sharp != 1.0f) wave = powf(wave, p->sharp);
    float bright = p->bright + p->depth * wave;

    /* The voice: a level in syllables under a phrase envelope swells the ring and lifts its glow. */
    float scale = p->scale;
    if (p->voice > 0.0f) {
        float level = p->voice * voice_level(t);
        scale += AURA_VOICE * level;
        bright += AURA_VOICE_GLOW * level;
    }

    /* The tremor: two sines per axis, 7 to 13 Hz, wander the centre. */
    float dx = 0.0f, dy = p->dy;
    if (p->tremor > 0.0f) {
        float k = p->tremor * AURA_TREMOR;
        dx += k * (0.6f * sinf(TAU_F * 7.0f * t) + 0.4f * sinf(TAU_F * 11.0f * t));
        dy += k * (0.6f * sinf(TAU_F * 9.0f * t + 1.0f) + 0.4f * sinf(TAU_F * 13.0f * t + 2.0f));
    }

    /* The base colour is kept in linear light (that is what fades); the
     * shader wants it as HSV to drift the hue across the copies. */
    const float lin[3] = { p->r, p->g, p->b };
    float hsv[3], haze[3];
    rgb2hsv(to_display(lin[0]), to_display(lin[1]), to_display(lin[2]), &hsv[0], &hsv[1], &hsv[2]);
    haze[0] = p->haze * p->hr; haze[1] = p->haze * p->hg; haze[2] = p->haze * p->hb;   /* driver levels */

    const uint8_t *mask = NULL;
    if (p->text > 0.0f && text && text[0]) {
        if (mask_y != text_y || strncmp(mask_text, text, FACE_TEXT_MAX) != 0) build_text_mask(text, text_y);
        mask = &text_mask[0][0];
    }

    render_shader(p, bright, scale, dx, dy, hsv);
    render_panel(haze, p->border, wave, p->text, mask);

    int64_t dt = esp_timer_get_time() - t0;
    stats.sum_us += dt;
    stats.frames++;
    if (dt > stats.max_us) stats.max_us = dt;
    if (stats.last_log_us == 0) stats.last_log_us = t0;
    if (t0 - stats.last_log_us >= AURA_LOG_US) {
        int64_t avg = stats.sum_us / stats.frames;
        ESP_LOGI(TAG, "draw %lld us avg, %lld us max over %lu frames (%dx%d, %d copies, %d layers): %lld fps if back to back",
                 (long long)avg, (long long)stats.max_us, (unsigned long)stats.frames,
                 AURA_RES, AURA_RES, AURA_COPIES, AURA_LAYERS, (long long)(avg > 0 ? 1000000 / avg : 0));
        stats.sum_us = 0;
        stats.max_us = 0;
        stats.frames = 0;
        stats.last_log_us = t0;
    }
}

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
 *   - a haze of the state's colour over the whole panel.
 *
 * How it works: a circle outline is drawn AURA_COPIES times, each time seen
 * through a turbulence warp at a slightly different phase, and the copies are
 * averaged. Where the copies agree the band is solid; where they fan out it
 * dissolves. The reference for every number here is reference/aurora.js
 * (sceneAura); where this file and that one differ, that one is right.
 *
 * Per frame: shader at 32x32 -> display colour -> linear light -> bilinear
 * 2x -> haze, error border, `t` -> 5-bit level with a 4x4 Bayer dither -> the
 * byte the driver's gamma table maps back to exactly that level.
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
#define AURA_HAZE           0.03f       /* linear light, in the state's colour */
#define AURA_TOE            0.006f      /* linear light below this is level 0 */
#define AURA_LEVELS         31          /* the driver's 5 bit planes */
#define AURA_LOG_US         5000000     /* frame-time log period */

#define TAU_F               6.28318530717958647692f
#define TAU_D               6.28318530717958647692
#define TIME_WRAP_S         70.0        /* every rhythm below repeats within 70 s (0.7, 1, 0.5, 5, 0.08) */
#define SIN_N               256         /* sine table entries per turn */
#define IDX_PER_RAD         ((float)SIN_N / TAU_F)

typedef enum { BR_CONST, BR_PULSE, BR_FLASH, BR_STROBE } bright_kind_t;

typedef struct {
    float speed, scale, amp, freq;
    bright_kind_t kind;
    float b0, b1;                       /* BR_CONST: b0; BR_PULSE: b0..b1 on the hook's 0.35 s mirrored pulse */
    bool voice;                         /* speak: scale follows a random level resampled every 80 ms */
    bool shake;                         /* error: a per-frame random shift of +-0.03 */
    float dx, dy;                       /* shift of the sample point in the shader's frame */
    bool haze;
    uint8_t col[3];                     /* sRGB */
} aura_cfg_t;

/* AURA and AURA_COL from aurora.js. The idle/listen/think/speak numbers are the
 * ones LiveKit's hook animates to; work, alert, error and sleep are added in
 * the same vocabulary. */
static const aura_cfg_t cfgs[FACE_MODE_SLEEP + 1] = {
    [FACE_MODE_IDLE]   = { .speed = 10, .scale = 0.24f, .amp = 0.9f,  .freq = 0.4f,  .kind = BR_CONST,  .b0 = 1.0f, .haze = true, .col = { 0x1F, 0xD5, 0xF9 } },
    [FACE_MODE_LISTEN] = { .speed = 20, .scale = 0.30f, .amp = 1.0f,  .freq = 0.7f,  .kind = BR_PULSE,  .b0 = 1.5f, .b1 = 2.0f, .haze = true, .col = { 0x3C, 0xF0, 0x8C } },
    [FACE_MODE_THINK]  = { .speed = 30, .scale = 0.30f, .amp = 0.7f,  .freq = 1.0f,  .kind = BR_PULSE,  .b0 = 0.5f, .b1 = 2.5f, .dx = -0.06f, .dy = -0.06f, .haze = true, .col = { 0xB4, 0x6E, 0xFF } },
    [FACE_MODE_WORK]   = { .speed = 40, .scale = 0.28f, .amp = 0.6f,  .freq = 1.0f,  .kind = BR_CONST,  .b0 = 1.5f, .haze = true, .col = { 0xFF, 0xA0, 0x28 } },
    [FACE_MODE_SPEAK]  = { .speed = 70, .scale = 0.30f, .amp = 0.75f, .freq = 1.25f, .kind = BR_CONST,  .b0 = 1.5f, .voice = true, .haze = true, .col = { 0x1F, 0xD5, 0xF9 } },
    [FACE_MODE_ALERT]  = { .speed = 10, .scale = 0.24f, .amp = 0.9f,  .freq = 0.4f,  .kind = BR_FLASH,  .haze = true, .col = { 0xFF, 0xC8, 0x14 } },
    [FACE_MODE_ERROR]  = { .speed = 40, .scale = 0.25f, .amp = 2.0f,  .freq = 0.8f,  .kind = BR_STROBE, .shake = true, .haze = true, .col = { 0xFF, 0x3C, 0x3C } },
    [FACE_MODE_SLEEP]  = { .speed = 6,  .scale = 0.18f, .amp = 1.2f,  .freq = 0.4f,  .kind = BR_CONST,  .b0 = 0.5f, .dy = 0.12f, .haze = false, .col = { 0x50, 0x50, 0xC8 } },
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
static float haze_lin[FACE_MODE_SLEEP + 1][3];      /* AURA_HAZE * lin(col) per state, 0 without haze */
static float border_w[HUB75_WIDTH / 2];             /* error border weight by distance from the edge */
static float red_lin[3], text_lin[3];
static float shade[AURA_RES][AURA_RES][3];          /* the shader's output, linear light */
static uint8_t text_mask[HUB75_HEIGHT][HUB75_WIDTH]; /* 2 glyph, 1 halo, 0 clear */
static char mask_text[FACE_TEXT_MAX + 1];
static int mask_y = -1;
static bool inited;

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

static inline float to_linear(float v)          /* v in 0..1 */
{
    float a = v * 256.0f;
    int k = (int)a;
    if (k >= 256) return 1.0f;
    float f = a - (float)k;
    return lin_tab[k] + (lin_tab[k + 1] - lin_tab[k]) * f;
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

static void init_tables(void)
{
    for (int k = 0; k <= SIN_N; k++) sin_tab[k] = sinf((float)k * TAU_F / (float)SIN_N);
    for (int k = 0; k <= 256; k++) lin_tab[k] = powf((float)k / 256.0f, 2.2f);
    for (int m = 0; m <= FACE_MODE_SLEEP; m++) {
        for (int c = 0; c < 3; c++) {
            haze_lin[m][c] = cfgs[m].haze ? AURA_HAZE * srgb_to_linear(cfgs[m].col[c]) : 0.0f;
        }
    }
    for (int e = 0; e < HUB75_WIDTH / 2; e++) border_w[e] = e == 0 ? 1.0f : expf(-(float)e / 2.5f) * 0.4f;
    red_lin[0] = srgb_to_linear(255); red_lin[1] = srgb_to_linear(30); red_lin[2] = srgb_to_linear(30);
    text_lin[0] = srgb_to_linear(255); text_lin[1] = srgb_to_linear(225); text_lin[2] = srgb_to_linear(180);
    inited = true;
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

/* sceneAura() up to the tonemap, on the 32x32 grid, into shade[] as linear light. */
static void render_shader(const aura_cfg_t *cfg, face_mode_t mode, float t, float anim, uint32_t frame)
{
    const float N = (float)AURA_COPIES;
    float pulse = 0.5f + 0.5f * sinf(TAU_F * t);

    float bright;
    switch (cfg->kind) {
    case BR_PULSE: {                                /* the hook's 0.35 s mirrored pulse, eased out */
        float ph = t / 0.7f;
        ph -= floorf(ph);
        float tri = ph < 0.5f ? ph * 2.0f : 2.0f - ph * 2.0f;
        tri = 1.0f - (1.0f - tri) * (1.0f - tri);
        bright = cfg->b0 + (cfg->b1 - cfg->b0) * tri;
        break;
    }
    case BR_FLASH:  bright = 1.0f + 1.5f * pulse * pulse; break;
    case BR_STROBE: bright = sinf(TAU_F * 2.0f * t) > 0.0f ? 2.2f : 0.6f; break;
    default:        bright = cfg->b0; break;
    }
    if (mode == FACE_MODE_SLEEP) bright *= 0.85f + 0.15f * sinf(TAU_F * t / 5.0f);

    float scale = cfg->scale;
    if (cfg->voice) {                               /* a random level every 80 ms, eased between samples */
        float q = t / 0.08f;
        int kk = (int)q;
        float u = q - (float)kk;
        u = u * u * (3.0f - 2.0f * u);
        float e0 = hash01((uint32_t)kk * 7919u), e1 = hash01((uint32_t)(kk + 1) * 7919u);
        scale = 0.2f + 0.2f * (e0 + (e1 - e0) * u);
    }

    /* Per-copy colour: the hue drifts a little across the copies. */
    float cols[AURA_COPIES][3];
    float h, s, v;
    rgb2hsv((float)cfg->col[0] / 255.0f, (float)cfg->col[1] / 255.0f, (float)cfg->col[2] / 255.0f, &h, &s, &v);
    for (int i = 1; i <= AURA_COPIES; i++) {
        float hh = h + (1.0f - (float)i / N) * AURA_COLOR_SHIFT * 0.3f;
        hh -= floorf(hh);
        hsv2rgb(hh, s, v, cols[i - 1]);
    }

    /* Per-copy, per-layer sine phase: i * animTime + it, in table units.
     * Entry 0 is the `prev` copy at it = -1/N. */
    const float spacing = 1.0f + (TAU_F - 1.0f) * AURA_SPACING;
    float ph[AURA_COPIES + 1][AURA_LAYERS];
    for (int k = 0; k <= AURA_COPIES; k++) {
        float it = k == 0 ? -1.0f / N : ((float)k / N) * spacing;
        for (int i = 0; i < AURA_LAYERS; i++) ph[k][i] = ((float)i * anim + it) * IDX_PER_RAD;
    }

    turb_consts_t tc;
    turb_consts(&tc, cfg->freq);

    float dx = cfg->dx, dy = cfg->dy;
    if (cfg->shake) {
        dx += (hash01(frame * 3u) - 0.5f) * 0.06f;
        dy += (hash01(frame * 3u + 1u) - 0.5f) * 0.06f;
    }
    const float amp = cfg->amp, gain = 1.2f * 4.0f / N;

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

/* Bilinear 2x from shade[], then haze, the error border, `t`, and the quantiser. */
static void render_panel(face_mode_t mode, float t, const uint8_t *mask)
{
    const float *haze = haze_lin[mode];
    const bool border = mode == FACE_MODE_ERROR;
    const float border_pulse = 0.6f + 0.4f * sinf(TAU_F * 2.0f * t);

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
            float c[3];
#if AURA_STEP == 2
            /* Bilinear 2x: output centres sit a quarter pixel from the grid,
             * so the nearest sample weighs 9/16, its two neighbours 3/16, the diagonal 1/16. */
            const float *bx = shade[j][io], *by = shade[jo][i], *bd = shade[jo][io];
            for (int ch = 0; ch < 3; ch++) {
                c[ch] = 0.5625f * a[ch] + 0.1875f * (bx[ch] + by[ch]) + 0.0625f * bd[ch] + haze[ch];
            }
#else
            (void)io; (void)jo;
            for (int ch = 0; ch < 3; ch++) c[ch] = a[ch] + haze[ch];
#endif
            if (border) {
                int ex = x < HUB75_WIDTH - 1 - x ? x : HUB75_WIDTH - 1 - x;
                int e = ex < ey ? ex : ey;
                float bw = border_w[e] * border_pulse;
                c[0] += bw * red_lin[0]; c[1] += bw * red_lin[1]; c[2] += bw * red_lin[2];
            }
            if (mask) {
                uint8_t mv = mask[y * HUB75_WIDTH + x];
                if (mv == 1) {
                    c[0] *= 0.25f; c[1] *= 0.25f; c[2] *= 0.25f;
                } else if (mv == 2) {
                    c[0] = c[0] * 0.2f + text_lin[0] * 0.8f;
                    c[1] = c[1] * 0.2f + text_lin[1] * 0.8f;
                    c[2] = c[2] * 0.2f + text_lin[2] * 0.8f;
                }
            }
            float th = ((float)bayer4[(y & 3) * 4 + (x & 3)] + 0.5f) * (1.0f / 16.0f);
            hub75_draw_pixel(x, y, quantise(c[0], th), quantise(c[1], th), quantise(c[2], th));
        }
    }
}

void aura_draw(face_mode_t mode, int64_t now_us, uint32_t frame, const char *text, int text_y)
{
    if (!inited) init_tables();
    if ((unsigned)mode > FACE_MODE_SLEEP) mode = FACE_MODE_IDLE;
    const aura_cfg_t *cfg = &cfgs[mode];
    int64_t t0 = esp_timer_get_time();

    /* The rhythms take t modulo 70 s, which every period divides; the
     * turbulence takes animTime = (t/2) * 0.1 * speed modulo one turn. Both
     * stay continuous for days, where a float t would not. */
    double td = (double)now_us * 1e-6;
    float t = (float)fmod(td, TIME_WRAP_S);
    float anim = (float)fmod(td * 0.05 * (double)cfg->speed, TAU_D);

    const uint8_t *mask = NULL;
    if (text && text[0]) {
        if (mask_y != text_y || strncmp(mask_text, text, FACE_TEXT_MAX) != 0) build_text_mask(text, text_y);
        mask = &text_mask[0][0];
    }

    render_shader(cfg, mode, t, anim, frame);
    render_panel(mode, t, mask);

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

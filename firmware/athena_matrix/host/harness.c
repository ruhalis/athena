/* harness.c - host driver for aura_draw(). Feeds it a scripted sequence of
 * modes, reads back host_frame (hub75_stub.c) after every draw, and reports:
 *   - one CSV line per frame on stdout (frame,t_s,mode,mad,maxd), where mad
 *     and maxd are computed on the same 5-bit levels the LED driver would
 *     actually show, so they measure perceived motion, not raw RGB noise;
 *   - a .ppm snapshot every --every frames, and always the first and the last
 *     frame of a segment (--every 0 writes only those), under --out as
 *     f<frame>_<mode>.ppm, gamma-decoded back to sRGB bytes for viewing.
 *
 * CLI: aura_host [--fps 40] [--every 4] [--out DIR] [--text 14:32]
 *      [--no-reset] mode:seconds [mode:seconds ...]
 *
 * Built by build.sh against the real main/aura.c; sheet.py and parity.mjs
 * drive it. If aura_draw()'s signature changes, this file follows.
 */
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "aura.h"
#include "protocol.h"

extern uint8_t host_frame[64][64][3];

typedef struct {
    face_mode_t mode;
    const char *mode_name;
    double seconds;
} segment_t;

static int mkdir_p(const char *path)
{
    char tmp[1024];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, path, len + 1);
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/* level = (int)lroundf(powf(v/255.0f, 2.2f) * 31) - the same quantisation the
 * driver applies to a drawn byte before dithering it onto a 5-bit plane. */
static int to_level(uint8_t v)
{
    return (int)lroundf(powf(v / 255.0f, 2.2f) * 31.0f);
}

/* Inverse of to_level(), for a byte a monitor will show at the right
 * perceived brightness. */
static uint8_t level_to_srgb_byte(int level)
{
    float v = 255.0f * powf(level / 31.0f, 1.0f / 2.2f);
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return (uint8_t)lroundf(v);
}

static int write_ppm(const char *out_dir, int frame_idx, const char *mode_name,
                      const int levels[64][64][3])
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/f%05d_%s.ppm", out_dir, frame_idx, mode_name);

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "aura_host: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f, "P6\n64 64\n255\n");
    uint8_t row[64 * 3];
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            row[x * 3 + 0] = level_to_srgb_byte(levels[y][x][0]);
            row[x * 3 + 1] = level_to_srgb_byte(levels[y][x][1]);
            row[x * 3 + 2] = level_to_srgb_byte(levels[y][x][2]);
        }
        fwrite(row, 1, sizeof(row), f);
    }
    fclose(f);
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--fps 40] [--every 4] [--out DIR] [--text 14:32] "
        "[--no-reset] mode:seconds [mode:seconds ...]\n", argv0);
}

int main(int argc, char **argv)
{
    int fps = 40;
    int every = 4;
    const char *out_dir = "frames";
    const char *text = "14:32";
    bool do_reset = true;

    segment_t *segments = NULL;
    int n_segments = 0;
    int cap_segments = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--every") == 0 && i + 1 < argc) {
            every = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (strcmp(argv[i], "--text") == 0 && i + 1 < argc) {
            text = argv[++i];
        } else if (strcmp(argv[i], "--no-reset") == 0) {
            do_reset = false;
        } else if (strchr(argv[i], ':') != NULL) {
            char *colon = strchr(argv[i], ':');
            size_t name_len = (size_t)(colon - argv[i]);
            char name_buf[32];
            if (name_len >= sizeof(name_buf)) {
                fprintf(stderr, "aura_host: mode name too long: %s\n", argv[i]);
                return 1;
            }
            memcpy(name_buf, argv[i], name_len);
            name_buf[name_len] = '\0';

            face_mode_t mode;
            if (!face_mode_from_string(name_buf, &mode)) {
                fprintf(stderr, "aura_host: unknown mode: %s\n", name_buf);
                return 1;
            }

            if (n_segments == cap_segments) {
                cap_segments = cap_segments ? cap_segments * 2 : 8;
                segments = realloc(segments, (size_t)cap_segments * sizeof(*segments));
            }
            segments[n_segments].mode = mode;
            segments[n_segments].mode_name = face_mode_name(mode);
            segments[n_segments].seconds = atof(colon + 1);
            n_segments++;
        } else {
            fprintf(stderr, "aura_host: unrecognised argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (n_segments == 0 || fps <= 0) {
        usage(argv[0]);
        return 1;
    }

    if (mkdir_p(out_dir) != 0) {
        fprintf(stderr, "aura_host: cannot create %s: %s\n", out_dir, strerror(errno));
        return 1;
    }

    if (do_reset) {
        aura_reset();
    }

    static int prev_levels[64][64][3];
    static int cur_levels[64][64][3];
    bool have_prev = false;

    long k = 0;
    for (int s = 0; s < n_segments; s++) {
        face_mode_t mode = segments[s].mode;
        const char *mode_name = segments[s].mode_name;
        bool with_text = (mode == FACE_MODE_IDLE || mode == FACE_MODE_ALERT);
        long frame_count = lround(segments[s].seconds * fps);

        for (long f = 0; f < frame_count; f++, k++) {
            int64_t now_us = (int64_t)llround((double)k * 1000000.0 / fps);

            aura_draw(mode, with_text, now_us, text, 29);

            double sum_abs = 0.0;
            int max_abs = 0;
            for (int y = 0; y < 64; y++) {
                for (int x = 0; x < 64; x++) {
                    for (int c = 0; c < 3; c++) {
                        int level = to_level(host_frame[y][x][c]);
                        cur_levels[y][x][c] = level;
                        if (have_prev) {
                            int d = level - prev_levels[y][x][c];
                            if (d < 0) d = -d;
                            sum_abs += d;
                            if (d > max_abs) max_abs = d;
                        }
                    }
                }
            }
            double mad = have_prev ? sum_abs / (64.0 * 64.0 * 3.0) : 0.0;
            int maxd = have_prev ? max_abs : 0;

            printf("%ld,%.6f,%s,%.4f,%d\n", k, now_us / 1000000.0, mode_name, mad, maxd);

            bool write_frame = (f == 0) || (f == frame_count - 1) || (every > 0 && (k % every) == 0);
            if (write_frame) {
                write_ppm(out_dir, (int)k, mode_name, cur_levels);
            }

            memcpy(prev_levels, cur_levels, sizeof(prev_levels));
            have_prev = true;
        }
    }

    free(segments);
    return 0;
}

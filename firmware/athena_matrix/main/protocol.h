/* protocol.h - the serial face protocol, shared by the serial and render tasks.
 *
 * One JSON object per LF-terminated line, at most 256 bytes, on UART0 or on
 * TCP port FACE_TCP_PORT (the Mac reaches it as athena-matrix.local):
 *   {"mode":"think"}  {"mode":"idle","t":"14:32"}  {"mode":"work","ttl":600}  {"brightness":80}  {}
 * Every line is answered with `ok` or `err <reason>` on the transport it came
 * in on. command.c parses and validates a line into a face_cmd_t and posts it
 * to one queue; the render task applies it at the next frame. Nothing else
 * crosses between the transports and the renderer.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    FACE_MODE_IDLE = 0,             /* nothing happening */
    FACE_MODE_LISTEN,               /* the user is talking or typing */
    FACE_MODE_THINK,                /* LLM request in flight */
    FACE_MODE_WORK,                 /* a tool is running */
    FACE_MODE_SPEAK,                /* the reply is being delivered */
    FACE_MODE_ALERT,                /* needs the user */
    FACE_MODE_ERROR,                /* something failed */
    FACE_MODE_SLEEP,                /* night / do not disturb */
    FACE_MODE_TEST,                 /* wiring check, the boot state */
    FACE_MODE_OFF,                  /* blank, output disabled */
    FACE_MODE_COUNT,
} face_mode_t;

#define FACE_TEXT_MAX   8           /* `t` is at most 8 characters */
#define FACE_LINE_MAX   256         /* longer lines are dropped with `err too long` */
#define FACE_TCP_PORT   7075        /* the `net` task listens here; scripts/face.py's default */

typedef struct {
    bool has_mode;
    face_mode_t mode;
    bool has_text;
    char text[FACE_TEXT_MAX + 1];   /* NUL-terminated; empty string clears the text */
    bool has_ttl;
    int32_t ttl_s;                  /* seconds until the fallback to idle, 0 = sticky */
    bool has_brightness;
    uint8_t brightness;             /* 0..255, applied at once, kept across modes */
} face_cmd_t;

typedef struct {
    const char *name;
    int32_t default_ttl_s;          /* used when a command names a mode without a ttl; 0 = sticky */
} face_mode_info_t;

/* Wire names and default ttls, indexed by face_mode_t. */
static const face_mode_info_t face_modes[FACE_MODE_COUNT] = {
    [FACE_MODE_IDLE]   = { "idle",   0 },
    [FACE_MODE_LISTEN] = { "listen", 30 },
    [FACE_MODE_THINK]  = { "think",  120 },
    [FACE_MODE_WORK]   = { "work",   300 },
    [FACE_MODE_SPEAK]  = { "speak",  8 },
    [FACE_MODE_ALERT]  = { "alert",  0 },
    [FACE_MODE_ERROR]  = { "error",  10 },
    [FACE_MODE_SLEEP]  = { "sleep",  0 },
    [FACE_MODE_TEST]   = { "test",   0 },
    [FACE_MODE_OFF]    = { "off",    0 },
};

static inline const char *face_mode_name(face_mode_t mode)
{
    return (unsigned)mode < FACE_MODE_COUNT ? face_modes[mode].name : "?";
}

static inline int32_t face_mode_default_ttl(face_mode_t mode)
{
    return (unsigned)mode < FACE_MODE_COUNT ? face_modes[mode].default_ttl_s : 0;
}

/* Exact, case-sensitive match against the wire names. */
static inline bool face_mode_from_string(const char *name, face_mode_t *out)
{
    if (!name) return false;
    for (int m = 0; m < FACE_MODE_COUNT; m++) {
        if (strcmp(name, face_modes[m].name) == 0) {
            *out = (face_mode_t)m;
            return true;
        }
    }
    return false;
}

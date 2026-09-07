/* aura.h - the aura face renderer: one frame of an agent state into the hub75
 * back buffer. The shader port, its per-state table, the tween between states
 * and the fast maths live in aura.c; face.c decides which mode is wanted and
 * whether `t` is shown. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

/* Draw one frame into the hub75 back buffer, which must already be cleared.
 * mode is one of the eight agent states (idle..sleep), the picture eases
 * toward it from whatever was on the panel over the next 0.8 s. with_text
 * fades `t` (centred at row text_y, warm white) in or out the same way; text
 * may be NULL or empty. now_us is the frame's esp_timer timestamp; the
 * animation is integrated from it, so call once per frame. Logs a rolling
 * draw-time average every few seconds. */
void aura_draw(face_mode_t mode, bool with_text, int64_t now_us, const char *text, int text_y);

/* Forget what is on the panel: the next aura_draw fades its state in from
 * dark instead of easing from the last aura frame. face.c calls it when the
 * panel shows something else (test, off). */
void aura_reset(void);

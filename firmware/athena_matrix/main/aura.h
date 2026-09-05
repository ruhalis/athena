/* aura.h - the aura face renderer: one frame of an agent state into the hub75
 * back buffer. The shader port, its per-state table and the fast maths live
 * in aura.c; face.c decides which mode is drawn and whether `t` is shown. */
#pragma once

#include <stdint.h>

#include "protocol.h"

/* Draw the aura for one of the eight agent states (idle..sleep) into the
 * hub75 back buffer, which must already be cleared. now_us is the frame's
 * esp_timer timestamp; frame counts frames since the mode was entered (error
 * shakes by it); text is drawn centred at row text_y in warm white when it is
 * non-NULL and non-empty. Logs a rolling draw-time average every few seconds. */
void aura_draw(face_mode_t mode, int64_t now_us, uint32_t frame, const char *text, int text_y);

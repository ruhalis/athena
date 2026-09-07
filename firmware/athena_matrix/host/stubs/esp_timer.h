/* esp_timer.h - host stub. Only the one call aura.c uses for its own
 * draw-time stats; hub75_stub.c provides the real implementation. */
#pragma once

#include <stdint.h>

int64_t esp_timer_get_time(void);

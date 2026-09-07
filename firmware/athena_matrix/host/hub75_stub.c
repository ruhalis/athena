/* hub75_stub.c - host stand-in for the real hub75.h driver. Instead of
 * bit-banging GPIOs it writes into host_frame, which harness.c reads back
 * after every aura_draw() call. */
#include <string.h>
#include <time.h>

#include "hub75.h"

uint8_t host_frame[64][64][3];

void hub75_clear(void)
{
    memset(host_frame, 0, sizeof(host_frame));
}

void hub75_draw_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || x >= HUB75_WIDTH || y < 0 || y >= HUB75_HEIGHT) {
        return;
    }
    host_frame[y][x][0] = r;
    host_frame[y][x][1] = g;
    host_frame[y][x][2] = b;
}

int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

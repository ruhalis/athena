/* command.h - the face protocol's line layer, shared by every transport.
 *
 * A transport (serial.c on UART0, net.c on TCP) feeds bytes into a cmd_line_t
 * and, when a line completes, hands it to cmd_handle(), which parses and
 * validates it against protocol.h, posts the face_cmd_t to the render queue
 * and writes `ok` or `err <reason>` back through the transport's reply
 * function. The transports never look inside a line.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "protocol.h"

typedef struct {
    char buf[FACE_LINE_MAX + 2];    /* one byte over the cap so a 256-byte line followed by CR LF still counts as 256 */
    size_t len;
    bool too_long;
} cmd_line_t;

/* One reply line, `text` is `len` bytes ending in LF. */
typedef void (*cmd_reply_fn)(void *ctx, const char *text, size_t len);

/* Remember the render queue every transport posts to. */
void cmd_init(QueueHandle_t queue);

void cmd_line_reset(cmd_line_t *l);

/* Feed one byte. Returns true when the LF arrived and `l` holds a line to
 * hand to cmd_handle(); an empty line completes nothing and is dropped. */
bool cmd_line_feed(cmd_line_t *l, char c);

/* Parse, validate and post the completed line, reply through `reply`, then
 * reset `l` for the next line. */
void cmd_handle(cmd_line_t *l, cmd_reply_fn reply, void *ctx);

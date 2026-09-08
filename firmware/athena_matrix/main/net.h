/* net.h - the `net` task: the face protocol on TCP port FACE_TCP_PORT. */
#pragma once

#include "esp_err.h"

/* Listen on every interface and serve up to a few clients at once, each with
 * its own line buffer, replies on the client's own socket. Binds before Wi-Fi
 * is up; connections arrive once it is. cmd_init() must have run first. */
esp_err_t net_start(void);

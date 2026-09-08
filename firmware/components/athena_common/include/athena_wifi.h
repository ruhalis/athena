/* athena_wifi.h - join the network from athena_secrets.h as a station, keep
 * rejoining when it drops, and answer as <hostname>.local over mDNS.
 *
 * Reconnects are the module's business: the caller starts it once and never
 * hears about drops. The board's own IP goes to the log on every join so a
 * Mac that cannot resolve .local can still find it on the serial console.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    const char *hostname;       /* DHCP and mDNS host name, e.g. "athena-matrix" answers as athena-matrix.local */
    const char *service;        /* mDNS service type to advertise, e.g. "_athena-face" (TCP), or NULL for none */
    uint16_t service_port;      /* port for `service` */
} athena_wifi_config_t;

/* Bring up NVS, the netif and the Wi-Fi driver, then start joining. Returns
 * once the driver runs; the join itself completes later, see
 * athena_wifi_is_connected(). `cfg` is copied, so it may live on the caller's stack. */
esp_err_t athena_wifi_start(const athena_wifi_config_t *cfg);

/* True between "got IP" and the next disconnect. */
bool athena_wifi_is_connected(void);

/* athena_wifi.c - station mode with a reconnect loop and an mDNS name.
 *
 * Event flow: STA_START -> connect; GOT_IP -> connected; DISCONNECTED -> retry
 * at once for the first few drops, then every RETRY_SLOW_MS through an
 * esp_timer so the event loop is never blocked. Power save is off: every
 * Athena board sits on a 5 V supply and the Mac's commands should land
 * without a DTIM wait.
 */
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs_flash.h"

#include "athena_wifi.h"

#if __has_include("athena_secrets.h")
#include "athena_secrets.h"
#else
#error "athena_secrets.h is missing: copy firmware/components/athena_common/include/athena_secrets.h.example next to it and fill it in"
#endif

static const char *TAG = "wifi";

#define RETRY_FAST_COUNT    5           /* immediate reconnects before backing off */
#define RETRY_SLOW_MS       5000

/* Our own copy: the caller's struct may live on a stack that is gone by the
 * time the first event fires (app_main returns, the main task is deleted). */
static char s_hostname[32];
static char s_service[32];
static uint16_t s_service_port;
static esp_timer_handle_t s_retry_timer;
static volatile bool s_connected;
static int s_drops;

/* Every reconnect goes through here: a connect call that fails on the spot
 * emits no event, so it must arm the slow timer itself or the loop ends. */
static void reconnect(bool now)
{
    if (now) {
        ESP_LOGI(TAG, "joining %s", ATHENA_WIFI_SSID);
        esp_err_t err = esp_wifi_connect();
        if (err == ESP_OK) return;
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
    esp_err_t err = esp_timer_start_once(s_retry_timer, (uint64_t)RETRY_SLOW_MS * 1000);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {      /* INVALID_STATE: already armed */
        ESP_LOGE(TAG, "cannot arm the retry timer: %s, no more reconnects", esp_err_to_name(err));
    }
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    reconnect(true);
}

/* A short reading of the reasons this network is most likely to hand out. */
static const char *reason_hint(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:           return "no AP with that SSID in range";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:     return "wrong password or a WPA3-only AP";
    case WIFI_REASON_ASSOC_LEAVE:           return "we left";
    case WIFI_REASON_BEACON_TIMEOUT:        return "AP went quiet";
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_ASSOC_EXPIRE:          return "AP dropped us";
    case WIFI_REASON_CONNECTION_FAIL:       return "association failed";
    default:                                return "see esp_wifi_types.h";
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        reconnect(true);
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *ev = data;
        s_connected = false;
        s_drops++;
        bool now = s_drops <= RETRY_FAST_COUNT;
        ESP_LOGW(TAG, "disconnected (reason %u: %s), retrying%s", ev->reason, reason_hint(ev->reason),
                 now ? "" : " in a few seconds");
        reconnect(now);
        break;
    }
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        s_connected = true;
        s_drops = 0;
        ESP_LOGI(TAG, "got ip " IPSTR " on %s, reachable as %s.local",
                 IP2STR(&ev->ip_info.ip), ATHENA_WIFI_SSID, s_hostname);
    } else if (id == IP_EVENT_STA_LOST_IP) {
        s_connected = false;
        ESP_LOGW(TAG, "lost ip");
    }
}

static esp_err_t nvs_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs partition is stale, erasing it");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase");
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t mdns_start(void)
{
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns_init");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(s_hostname), TAG, "mdns_hostname_set");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set("Athena board"), TAG, "mdns_instance_name_set");
    if (s_service[0]) {
        ESP_RETURN_ON_ERROR(mdns_service_add(NULL, s_service, "_tcp", s_service_port, NULL, 0),
                            TAG, "mdns_service_add");
    }
    return ESP_OK;
}

esp_err_t athena_wifi_start(const athena_wifi_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg && cfg->hostname, ESP_ERR_INVALID_ARG, TAG, "hostname required");
    ESP_RETURN_ON_FALSE(strlen(ATHENA_WIFI_SSID) > 0 && strlen(ATHENA_WIFI_SSID) < 32, ESP_ERR_INVALID_ARG, TAG,
                        "ATHENA_WIFI_SSID must be 1..31 bytes");
    ESP_RETURN_ON_FALSE(strlen(ATHENA_WIFI_PASS) < 64, ESP_ERR_INVALID_ARG, TAG, "ATHENA_WIFI_PASS must be under 64 bytes");
    ESP_RETURN_ON_FALSE(strlen(cfg->hostname) < sizeof(s_hostname), ESP_ERR_INVALID_ARG, TAG, "hostname too long");
    ESP_RETURN_ON_FALSE(!cfg->service || strlen(cfg->service) < sizeof(s_service), ESP_ERR_INVALID_ARG, TAG, "service too long");
    strcpy(s_hostname, cfg->hostname);
    strcpy(s_service, cfg->service ? cfg->service : "");
    s_service_port = cfg->service_port;

    ESP_RETURN_ON_ERROR(nvs_start(), TAG, "nvs");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(sta, ESP_FAIL, TAG, "no sta netif");
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(sta, s_hostname), TAG, "hostname");

    const esp_timer_create_args_t targs = { .callback = retry_timer_cb, .name = "wifi_retry" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_retry_timer), TAG, "retry timer");

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&ic), TAG, "esp_wifi_init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL), TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL), TAG, "ip handler");

    /* mDNS attaches itself to the netif events, so it can start before the join. */
    ESP_RETURN_ON_ERROR(mdns_start(), TAG, "mdns");

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, ATHENA_WIFI_SSID, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, ATHENA_WIFI_PASS, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = ATHENA_WIFI_PASS[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;     /* WPA2/WPA3 transition networks work either way */
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;   /* a coworking space has several APs with one name */

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "set config");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "set ps");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");

    ESP_LOGI(TAG, "station %s, mdns %s.local%s%s", s_hostname, s_hostname,
             s_service[0] ? ", service " : "", s_service);
    return ESP_OK;
}

bool athena_wifi_is_connected(void)
{
    return s_connected;
}

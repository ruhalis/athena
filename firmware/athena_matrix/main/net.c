/* net.c - the `net` task. The same LF-terminated JSON lines as UART0, over
 * TCP: the Mac connects to <hostname>.local:FACE_TCP_PORT, writes lines, reads
 * `ok`/`err <reason>` back. One select() loop serves the listener and up to
 * NET_MAX_CLIENTS connections so the plugin's long-lived socket and a
 * one-shot face.py never wait for each other; a further client is told
 * `err busy` and closed. Keepalive drops a peer that vanished (a sleeping
 * Mac) after about a minute so its slot comes back.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "command.h"
#include "net.h"

static const char *TAG = "net";

#define NET_MAX_CLIENTS     4
#define NET_BACKLOG         2
#define NET_KEEPIDLE_S      30
#define NET_KEEPINTVL_S     10
#define NET_KEEPCNT         3
#define NET_SEND_TIMEOUT_S  2           /* a peer that stops reading its replies is dropped, not waited for */
#define NET_RECV_CHUNK      128

typedef struct {
    int fd;                     /* -1 when the slot is free */
    bool dead;                  /* a reply failed: drop after the current chunk */
    cmd_line_t line;
    char peer[24];              /* "a.b.c.d:port" for the log */
} client_t;

static client_t s_clients[NET_MAX_CLIENTS];

static void reply(void *ctx, const char *text, size_t len)
{
    client_t *c = ctx;
    if (c->dead) return;
    if (send(c->fd, text, len, 0) < 0) {
        ESP_LOGW(TAG, "%s: reply failed (errno %d)", c->peer, errno);
        c->dead = true;
    }
}

static void drop(client_t *c, const char *why)
{
    ESP_LOGI(TAG, "%s: %s", c->peer, why);
    close(c->fd);
    c->fd = -1;
}

static void accept_client(int listen_fd)
{
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int fd = accept(listen_fd, (struct sockaddr *)&addr, &alen);
    if (fd < 0) {
        ESP_LOGW(TAG, "accept failed (errno %d)", errno);
        return;
    }

    char ip[16];
    inet_ntoa_r(addr.sin_addr, ip, sizeof(ip));

    client_t *c = NULL;
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (s_clients[i].fd < 0) { c = &s_clients[i]; break; }
    }
    if (!c) {
        static const char busy[] = "err busy\n";
        ESP_LOGW(TAG, "%s:%u refused, %d clients already", ip, ntohs(addr.sin_port), NET_MAX_CLIENTS);
        send(fd, busy, sizeof(busy) - 1, 0);
        close(fd);
        return;
    }

    int on = 1, idle = NET_KEEPIDLE_S, intvl = NET_KEEPINTVL_S, cnt = NET_KEEPCNT;
    struct timeval sndto = { .tv_sec = NET_SEND_TIMEOUT_S };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sndto, sizeof(sndto));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

    c->fd = fd;
    c->dead = false;
    cmd_line_reset(&c->line);
    snprintf(c->peer, sizeof(c->peer), "%s:%u", ip, ntohs(addr.sin_port));
    ESP_LOGI(TAG, "%s: connected", c->peer);
}

static void read_client(client_t *c)
{
    char chunk[NET_RECV_CHUNK];
    int n = recv(c->fd, chunk, sizeof(chunk), 0);
    if (n <= 0) {
        drop(c, n == 0 ? "closed" : "gone");
        return;
    }
    for (int i = 0; i < n && !c->dead; i++) {
        if (cmd_line_feed(&c->line, chunk[i])) cmd_handle(&c->line, reply, c);
    }
    if (c->dead) drop(c, "not reading its replies");
}

static int listen_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return -1;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(FACE_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, NET_BACKLOG) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void net_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < NET_MAX_CLIENTS; i++) s_clients[i].fd = -1;

    int listen_fd = listen_socket();
    while (listen_fd < 0) {
        ESP_LOGE(TAG, "cannot listen on port %d (errno %d), retrying in 5 s", FACE_TCP_PORT, errno);
        vTaskDelay(pdMS_TO_TICKS(5000));
        listen_fd = listen_socket();
    }
    ESP_LOGI(TAG, "listening on tcp port %d, up to %d clients", FACE_TCP_PORT, NET_MAX_CLIENTS);

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;
        for (int i = 0; i < NET_MAX_CLIENTS; i++) {
            int fd = s_clients[i].fd;
            if (fd < 0) continue;
            FD_SET(fd, &rfds);
            if (fd > maxfd) maxfd = fd;
        }

        int ready = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (ready < 0) {
            ESP_LOGW(TAG, "select failed (errno %d)", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        for (int i = 0; i < NET_MAX_CLIENTS; i++) {
            if (s_clients[i].fd >= 0 && FD_ISSET(s_clients[i].fd, &rfds)) read_client(&s_clients[i]);
        }
        if (FD_ISSET(listen_fd, &rfds)) accept_client(listen_fd);
    }
}

esp_err_t net_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(net_task, "net", 4096, NULL, 4, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "net task");
    return ESP_OK;
}

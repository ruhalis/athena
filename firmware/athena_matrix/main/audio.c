/* audio.c - the `audio` module: one I2S port in full duplex (an INMP441 or
 * ICS-43434 microphone in, a MAX98357A amplifier out, 16 kHz, 32-bit slots)
 * bridged to one TCP client on AUDIO_TCP_PORT. See audio.h for the stream.
 *
 * Four tasks, all on core 0 because core 1 is the panel's: `audio_rx` reads
 * the microphone, folds its slot to int16 and queues it; `audio_send` drains
 * that queue into the client's socket, so a Wi-Fi stall never holds up the
 * microphone read (the I2S DMA holds only 60 ms, the queue half a second, and
 * beyond that the newest samples are dropped and counted); `audio_tx` drains
 * the playback buffer into the amplifier; `audio_net` accepts the client and
 * fills that buffer, so a Mac that writes faster than real time is simply
 * held back by TCP once the buffer is full.
 *
 * No wake word and no echo cancellation live here: the classic ESP32 has
 * neither PSRAM nor a free core for ESP-SR, so this board is the ears and the
 * mouth and the Mac is the brain. AUDIO-BOARD.md is the S3 design that does
 * more on the board.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "audio.h"

static const char *TAG = "audio";

#define AUDIO_FRAMES            160     /* one block: 10 ms at 16 kHz */
#define AUDIO_DMA_DESCS         6       /* 60 ms of DMA per direction */
#define AUDIO_PLAY_BUF_MS       500     /* how far ahead of real time a client may write before TCP holds it */
#define AUDIO_PLAY_BUF_BYTES    (AUDIO_RATE_HZ * 2 * AUDIO_PLAY_BUF_MS / 1000)
#define AUDIO_MIC_BUF_MS        500     /* microphone waiting for a slow network; beyond it the newest samples are dropped */
#define AUDIO_MIC_BUF_BYTES     (AUDIO_RATE_HZ * 2 * AUDIO_MIC_BUF_MS / 1000)
#define AUDIO_MIC_SLOT          0       /* 0 = left slot (mic L/R to GND), 1 = right (L/R to 3V3); the log shows both */
#define AUDIO_MIC_SHIFT         14      /* 32-bit MSB-aligned mic word to int16: >>16 is unity, every bit less is +6 dB */
#define AUDIO_SEND_TIMEOUT_MS   2000    /* a client that stops reading the mic this long is dropped, not waited for */
#define AUDIO_LOG_PERIOD_US     (5 * 1000 * 1000)
#define AUDIO_BACKLOG           1
#define AUDIO_KEEPIDLE_S        30
#define AUDIO_KEEPINTVL_S       10
#define AUDIO_KEEPCNT           3

static i2s_chan_handle_t s_tx, s_rx;
static StreamBufferHandle_t s_play;         /* int16 mono, net -> tx */
static StreamBufferHandle_t s_mic;          /* int16 mono, rx -> send */
static SemaphoreHandle_t s_lock;            /* guards s_client and s_peer between audio_net and audio_send; the
                                             * 5 s log line in audio_rx peeks at both without it, a torn peer
                                             * string there costs nothing */
static uint32_t s_mic_dropped;              /* samples the network was too slow to take since the last log line */
static int s_client = -1;                   /* the socket audio_send may send to, -1 for none */
static volatile bool s_gone;                /* the peer left and audio_net is closing: stop queuing mic for it */
static char s_peer[24];                     /* "a.b.c.d:port" for the log */

/* dBFS of an RMS or peak value on the int16 scale, floored so silence prints. */
static float dbfs(float x)
{
    if (x < 1.0f) return -96.0f;
    return 20.0f * log10f(x / 32768.0f);
}

/* Send one block of int16 mono to the client, if there is one; without one
 * the block is discarded. The send never blocks: a client that cannot take
 * it right now (typically because it is busy playing) loses the block, not
 * the connection. Runs under the lock so the net task cannot close the
 * socket mid-send; only a real disconnect shuts it down, which wakes the net
 * task's select() to close it. */
static void send_to_client(const int16_t *pcm, size_t bytes)
{
    if (s_client < 0) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    int fd = s_client;
    if (fd >= 0) {
        int n = send(fd, pcm, bytes, MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* The client is not taking the mic right now, usually because it
             * is busy playing: never block or drop it for that, just lose
             * this block. The 5 s log line counts it. */
            s_mic_dropped += bytes / sizeof(int16_t);
        } else if (n != (int)bytes) {
            if (!s_gone) {      /* a peer that closed first is logged by audio_net, not here */
                ESP_LOGW(TAG, "%s: connection lost (sent %d of %u, errno %d)", s_peer, n, (unsigned)bytes, errno);
            }
            shutdown(fd, SHUT_RDWR);
            s_client = -1;
        }
    }
    xSemaphoreGive(s_lock);
}

static void rx_task(void *arg)
{
    (void)arg;
    static int32_t raw[AUDIO_FRAMES * 2];
    static int16_t pcm[AUDIO_FRAMES];
    uint64_t sq[2] = { 0, 0 };
    int32_t peak[2] = { 0, 0 };
    uint32_t count = 0;
    int64_t next_log = esp_timer_get_time() + AUDIO_LOG_PERIOD_US;

    for (;;) {
        size_t got = 0;
        esp_err_t err = i2s_channel_read(s_rx, raw, sizeof(raw), &got, portMAX_DELAY);
        if (err != ESP_OK || got == 0) {
            ESP_LOGW(TAG, "mic read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        size_t frames = got / (2 * sizeof(int32_t));
        for (size_t i = 0; i < frames; i++) {
            for (int s = 0; s < 2; s++) {
                int32_t v = raw[2 * i + s] >> 16;           /* top 16 bits, unity gain, for the level log */
                sq[s] += (uint64_t)((int64_t)v * v);
                if (abs(v) > peak[s]) peak[s] = abs(v);
            }
            int32_t v = raw[2 * i + AUDIO_MIC_SLOT] >> AUDIO_MIC_SHIFT;
            if (v > INT16_MAX) v = INT16_MAX;
            else if (v < INT16_MIN) v = INT16_MIN;
            pcm[i] = (int16_t)v;
        }
        count += frames;
        if (s_client >= 0 && !s_gone) {
            /* Never wait for the network here: the DMA behind this read holds
             * 60 ms. What the queue cannot take is lost and counted. */
            size_t bytes = frames * sizeof(int16_t);
            size_t queued = xStreamBufferSend(s_mic, pcm, bytes, 0);
            s_mic_dropped += (bytes - queued) / sizeof(int16_t);
        }

        int64_t now = esp_timer_get_time();
        if (now >= next_log && count) {
            float rms[2] = { sqrtf((float)sq[0] / count), sqrtf((float)sq[1] / count) };
            char dropped[40] = "";
            if (s_mic_dropped) {
                snprintf(dropped, sizeof(dropped), ", %lu samples dropped", (unsigned long)s_mic_dropped);
            }
            ESP_LOGI(TAG, "mic L %.0f dBFS (peak %.0f), R %.0f dBFS (peak %.0f), slot %c -> %s%s",
                     dbfs(rms[0]), dbfs((float)peak[0]), dbfs(rms[1]), dbfs((float)peak[1]),
                     AUDIO_MIC_SLOT ? 'R' : 'L', s_client >= 0 ? s_peer : "no client", dropped);
            s_mic_dropped = 0;
            sq[0] = sq[1] = 0;
            peak[0] = peak[1] = 0;
            count = 0;
            next_log = now + AUDIO_LOG_PERIOD_US;
        }
    }
}

/* The network side of the microphone: drains the queue into the client's
 * socket at whatever pace TCP allows, and throws it away while there is none. */
static void send_task(void *arg)
{
    (void)arg;
    static int16_t pcm[AUDIO_FRAMES];
    for (;;) {
        size_t got = xStreamBufferReceive(s_mic, pcm, sizeof(pcm), portMAX_DELAY);
        send_to_client(pcm, got);
    }
}

static void tx_task(void *arg)
{
    (void)arg;
    static int16_t pcm[AUDIO_FRAMES];
    static int32_t frames[AUDIO_FRAMES * 2];

    for (;;) {
        size_t got = xStreamBufferReceive(s_play, pcm, sizeof(pcm), portMAX_DELAY);
        size_t n = got / sizeof(int16_t);
        for (size_t i = 0; i < n; i++) {
            /* int16 into the top of a 32-bit slot; the same sample in both
             * slots, so the amp plays it whether its SD pin selects left,
             * right or the stereo average. */
            int32_t v = (int32_t)((uint32_t)(uint16_t)pcm[i] << 16);
            frames[2 * i] = v;
            frames[2 * i + 1] = v;
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, frames, n * 2 * sizeof(int32_t), &written, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "amp write failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static int listen_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return -1;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(AUDIO_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, AUDIO_BACKLOG) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Accept a connection: the first becomes the client, any further one is
 * closed at once so the caller sees EOF instead of a silent hang. */
static int accept_client(int listen_fd, int current)
{
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int fd = accept(listen_fd, (struct sockaddr *)&addr, &alen);
    if (fd < 0) {
        ESP_LOGW(TAG, "accept failed (errno %d)", errno);
        return current;
    }
    char ip[16];
    inet_ntoa_r(addr.sin_addr, ip, sizeof(ip));
    if (current >= 0) {
        ESP_LOGW(TAG, "%s:%u refused, %s already streams", ip, ntohs(addr.sin_port), s_peer);
        close(fd);
        return current;
    }

    int on = 1, idle = AUDIO_KEEPIDLE_S, intvl = AUDIO_KEEPINTVL_S, cnt = AUDIO_KEEPCNT;
    struct timeval sndto = { .tv_sec = AUDIO_SEND_TIMEOUT_MS / 1000, .tv_usec = (AUDIO_SEND_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sndto, sizeof(sndto));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_peer, sizeof(s_peer), "%s:%u", ip, ntohs(addr.sin_port));
    s_client = fd;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s: connected, mic out, speaker in", s_peer);
    return fd;
}

/* Feed what the client wrote into the playback buffer, whole samples only;
 * an odd trailing byte waits for its other half. Returns false when the
 * client is gone. */
static bool read_client(int fd, uint8_t *chunk, size_t size, size_t *carry)
{
    int n = recv(fd, chunk + *carry, size - *carry, 0);
    if (n <= 0) {
        ESP_LOGI(TAG, "%s: %s", s_peer, n == 0 ? "closed" : (s_client < 0 ? "dropped" : "gone"));
        return false;
    }
    size_t total = *carry + (size_t)n;
    size_t whole = total & ~(size_t)1;
    if (whole) xStreamBufferSend(s_play, chunk, whole, portMAX_DELAY);
    *carry = total - whole;
    if (*carry) chunk[0] = chunk[whole];
    return true;
}

static void net_task(void *arg)
{
    (void)arg;
    static uint8_t chunk[AUDIO_FRAMES * 2 * sizeof(int16_t)];
    size_t carry = 0;
    int client = -1;

    int listen_fd = listen_socket();
    while (listen_fd < 0) {
        ESP_LOGE(TAG, "cannot listen on port %d (errno %d), retrying in 5 s", AUDIO_TCP_PORT, errno);
        vTaskDelay(pdMS_TO_TICKS(5000));
        listen_fd = listen_socket();
    }
    ESP_LOGI(TAG, "listening on tcp port %d: pcm s16le %d Hz mono, mic out and speaker in on one connection",
             AUDIO_TCP_PORT, AUDIO_RATE_HZ);

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;
        if (client >= 0) {
            FD_SET(client, &rfds);
            if (client > maxfd) maxfd = client;
        }
        int ready = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (ready < 0) {
            ESP_LOGW(TAG, "select failed (errno %d)", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (client >= 0 && FD_ISSET(client, &rfds) && !read_client(client, chunk, sizeof(chunk), &carry)) {
            /* audio_send may sit in a send() the peer no longer acknowledges,
             * holding the lock until the send timeout: stop the mic from
             * queuing for that peer meanwhile, and shut the socket down so
             * the send returns now rather than at the timeout. */
            s_gone = true;
            shutdown(client, SHUT_RDWR);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            close(client);
            s_client = -1;
            s_gone = false;
            xSemaphoreGive(s_lock);
            client = -1;
            carry = 0;
        }
        if (FD_ISSET(listen_fd, &rfds)) client = accept_client(listen_fd, client);
    }
}

esp_err_t audio_start(const audio_pins_t *pins)
{
    ESP_RETURN_ON_FALSE(pins, ESP_ERR_INVALID_ARG, TAG, "no pins");
    s_lock = xSemaphoreCreateMutex();
    s_play = xStreamBufferCreate(AUDIO_PLAY_BUF_BYTES, sizeof(int16_t));
    s_mic = xStreamBufferCreate(AUDIO_MIC_BUF_BYTES, sizeof(int16_t));
    ESP_RETURN_ON_FALSE(s_lock && s_play && s_mic, ESP_ERR_NO_MEM, TAG, "no memory for the audio buffers");

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = AUDIO_DMA_DESCS;
    chan.dma_frame_num = AUDIO_FRAMES;
    chan.auto_clear = true;             /* the amp gets zeros, not a stale block, when nothing is queued */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &s_tx, &s_rx), TAG, "i2s channels");

    /* The same configuration for both directions is what makes the driver
     * pair them as full duplex on one BCLK and one WS. */
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)pins->bclk,
            .ws = (gpio_num_t)pins->ws,
            .dout = (gpio_num_t)pins->dout,
            .din = (gpio_num_t)pins->din,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s tx enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "i2s rx enable");

    BaseType_t ok = xTaskCreatePinnedToCore(rx_task, "audio_rx", 4096, NULL, 5, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "audio_rx task");
    ok = xTaskCreatePinnedToCore(tx_task, "audio_tx", 3072, NULL, 5, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "audio_tx task");
    ok = xTaskCreatePinnedToCore(send_task, "audio_send", 3072, NULL, 4, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "audio_send task");
    ok = xTaskCreatePinnedToCore(net_task, "audio_net", 4096, NULL, 4, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "audio_net task");

    ESP_LOGI(TAG, "i2s bclk %d ws %d dout %d din %d, %d Hz, 32-bit slots, mic slot %c, free heap %u",
             pins->bclk, pins->ws, pins->dout, pins->din, AUDIO_RATE_HZ, AUDIO_MIC_SLOT ? 'R' : 'L',
             (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}

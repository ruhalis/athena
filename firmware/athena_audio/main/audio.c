/* audio.c - the `audio` module: one I2S port in full duplex (an INMP441 or
 * ICS-43434 microphone in, a MAX98357A amplifier out, 16 kHz, 32-bit slots)
 * bridged to one TCP client on AUDIO_TCP_PORT. See audio.h for the stream.
 *
 * Four tasks: `audio_rx` reads the microphone, folds its slot to int16 and
 * queues it; `audio_send` drains that queue into the client's socket and
 * waits there when the socket is full, so a Wi-Fi stall never holds up the
 * microphone read and never costs a sample either: the I2S DMA holds only
 * 60 ms, but behind it lwIP's send buffer holds half a second and the queue
 * (in PSRAM) two more, which a recovered link drains in a moment. Only
 * beyond that are the newest blocks dropped, whole, and counted;
 * `audio_tx` drains the playback buffer into the
 * amplifier; `audio_net` accepts the client and fills that buffer, so a Mac
 * that writes faster than real time is simply held back by TCP once the
 * buffer is full. The I2S pair runs on core 1 next to the AFE (sr.c), the
 * network pair on core 0 next to Wi-Fi, the split AUDIO-BOARD.md asks for.
 * `audio_rx` also hands every block to sr_feed(), so the wake word and the
 * VAD hear exactly what the client gets (not in a build without
 * CONFIG_ATHENA_AUDIO_SR, which has no sr.c).
 *
 * This is the raw bridge the microphone and the amplifier are brought up
 * with, stages 2-4 of AUDIO-BOARD.md with the Mac as the meter: no echo
 * cancellation yet and the Mac is still the brain. The amp's SD_MODE pin
 * is driven high once at start (on, left slot); gating it per utterance to
 * kill the idle hiss comes with the later stages.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "audio.h"
#if CONFIG_ATHENA_AUDIO_SR
#include "sr.h"
#endif

static const char *TAG = "audio";

#define AUDIO_FRAMES            160     /* one block: 10 ms at 16 kHz */
#define AUDIO_DMA_DESCS         6       /* 60 ms of DMA per direction */
#define AUDIO_PLAY_BUF_MS       500     /* how far ahead of real time a client may write before TCP holds it */
#define AUDIO_PLAY_BUF_BYTES    (AUDIO_RATE_HZ * 2 * AUDIO_PLAY_BUF_MS / 1000)
#define AUDIO_MIC_BUF_MS        2000    /* microphone waiting for a slow network; beyond it the newest blocks are dropped */
#define AUDIO_MIC_BUF_BYTES     (AUDIO_RATE_HZ * 2 * AUDIO_MIC_BUF_MS / 1000)
#define AUDIO_MIC_BACKLOG_LOG_MS 50     /* a queue that stood deeper than this shows on the 5 s log line: the link stalled */
#define AUDIO_MIC_SLOT          0       /* 0 = left slot (mic L/R to GND), 1 = right (L/R to 3V3); the log shows both */
#define AUDIO_MIC_SHIFT         12      /* 32-bit MSB-aligned mic word to int16: >>16 is unity, every bit less is +6 dB.
                                         * +24 dB: at 14 (+12 dB) speech at a metre sat at -25..-32 dBFS and WakeNet
                                         * only fired at -19; at 12 the open office floor is about -26 dBFS RMS and
                                         * a raised voice up close clips, which the clamp below takes */
#define AUDIO_SEND_POLL_MS      10      /* how often audio_send tries a full socket again */
#define AUDIO_STALLED_MS        3000    /* a client whose socket took nothing for this long gives way to a new connection */
#define AUDIO_LOG_PERIOD_US     (5 * 1000 * 1000)
#define AUDIO_BACKLOG           1
#define AUDIO_KEEPIDLE_S        30
#define AUDIO_KEEPINTVL_S       10
#define AUDIO_KEEPCNT           3
#define AUDIO_I2S_CORE          1       /* audio_rx and audio_tx: the I2S side, AUDIO-BOARD.md's AFE core */
#define AUDIO_NET_CORE          0       /* audio_send and audio_net, next to Wi-Fi and lwIP */

static i2s_chan_handle_t s_tx, s_rx;
static StreamBufferHandle_t s_play;         /* int16 mono, net -> tx */
static StreamBufferHandle_t s_mic;          /* int16 mono, rx -> send */
static SemaphoreHandle_t s_lock;            /* guards s_client and s_peer between audio_net and audio_send; the
                                             * 5 s log line in audio_rx peeks at both without it, a torn peer
                                             * string there costs nothing */
static uint32_t s_mic_dropped;              /* samples the queue could not take since the last log line; audio_rx only */
static uint32_t s_mic_backlog;              /* the deepest the queue stood since the last log line, in bytes; audio_rx only */
static int s_client = -1;                   /* the socket audio_send may send to, -1 for none */
static volatile uint32_t s_client_gen;      /* counts accepted clients, written under s_lock: nothing queued for one client reaches the next */
static int64_t s_sent_at;                   /* esp_timer time of the last byte the client's socket took, or of its accept; under s_lock */
static volatile bool s_gone;                /* the peer left and audio_net is closing: stop queuing mic for it */
static char s_peer[24];                     /* "a.b.c.d:port" for the log */

/* dBFS of an RMS or peak value on the int16 scale, floored so silence prints. */
static float dbfs(float x)
{
    if (x < 1.0f) return -96.0f;
    return 20.0f * log10f(x / 32768.0f);
}

/* Send one block of int16 mono to the client, if there is one; without one
 * the block is discarded. All of it goes out or none of it matters any more:
 * a full socket (the link stalls, or the client is busy playing and reads
 * slowly) is tried again every AUDIO_SEND_POLL_MS while the microphone backs
 * up in s_mic behind this task, and a send lwIP took only part of (a
 * non-blocking write is cut to the free send buffer) resumes where it
 * stopped, so the client never sees a block with a hole in it or a stream
 * that lost the byte alignment of its samples. The client is never dropped
 * for being slow. Each try runs under the lock so the net task cannot close
 * the socket mid-send, and the wait between tries does not, so it can; a
 * block meant for client `gen` is abandoned once another one has been
 * accepted (s_client_gen), sent or half sent, so the next client starts on a
 * whole block of its own. Only a real disconnect shuts the socket down, which
 * wakes the net task's select() to close it. */
static void send_to_client(const uint8_t *data, size_t bytes, uint32_t gen)
{
    size_t off = 0;
    while (off < bytes) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return;
        int fd = s_client;
        if (fd < 0 || s_gone || gen != s_client_gen) {
            xSemaphoreGive(s_lock);
            return;
        }
        int n = send(fd, data + off, bytes - off, MSG_DONTWAIT);
        if (n > 0) {
            off += (size_t)n;
            s_sent_at = esp_timer_get_time();
        } else if (!(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
            if (!s_gone) {      /* a peer that closed first is logged by audio_net, not here */
                ESP_LOGW(TAG, "%s: connection lost (sent %u of %u, errno %d)", s_peer, (unsigned)off, (unsigned)bytes, errno);
            }
            shutdown(fd, SHUT_RDWR);
            s_client = -1;
            xSemaphoreGive(s_lock);
            return;
        }
        xSemaphoreGive(s_lock);
        if (off < bytes) vTaskDelay(pdMS_TO_TICKS(AUDIO_SEND_POLL_MS));
    }
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
#if CONFIG_ATHENA_AUDIO_SR
        sr_feed(pcm, frames);           /* the AFE (sr.c) hears the same block the client gets; a no-op until sr_start() */
#endif
        if (s_client >= 0 && !s_gone) {
            /* Never wait for the network here: the DMA behind this read holds
             * 60 ms. Whole blocks only, like sr_feed(): what the queue cannot
             * take is lost and counted, and it takes AUDIO_MIC_BUF_MS of a
             * stalled link on top of lwIP's send buffer to get there. */
            size_t bytes = frames * sizeof(int16_t);
            if (xStreamBufferSpacesAvailable(s_mic) < bytes || xStreamBufferSend(s_mic, pcm, bytes, 0) != bytes) {
                s_mic_dropped += frames;
            }
            size_t backlog = xStreamBufferBytesAvailable(s_mic);
            if (backlog > s_mic_backlog) s_mic_backlog = backlog;
        }

        int64_t now = esp_timer_get_time();
        if (now >= next_log && count) {
            float rms[2] = { sqrtf((float)sq[0] / count), sqrtf((float)sq[1] / count) };
            char dropped[80] = "";
            uint32_t backlog_ms = s_mic_backlog * 1000 / (AUDIO_RATE_HZ * sizeof(int16_t));
            if (s_mic_dropped) {
                snprintf(dropped, sizeof(dropped), ", queued up to %lu ms, %lu samples dropped",
                         (unsigned long)backlog_ms, (unsigned long)s_mic_dropped);
            } else if (backlog_ms >= AUDIO_MIC_BACKLOG_LOG_MS) {
                snprintf(dropped, sizeof(dropped), ", queued up to %lu ms, nothing dropped", (unsigned long)backlog_ms);
            }
            ESP_LOGI(TAG, "mic L %.0f dBFS (peak %.0f), R %.0f dBFS (peak %.0f), slot %c -> %s%s",
                     dbfs(rms[0]), dbfs((float)peak[0]), dbfs(rms[1]), dbfs((float)peak[1]),
                     AUDIO_MIC_SLOT ? 'R' : 'L', s_client >= 0 ? s_peer : "no client", dropped);
            s_mic_dropped = 0;
            s_mic_backlog = 0;
            sq[0] = sq[1] = 0;
            peak[0] = peak[1] = 0;
            count = 0;
            next_log = now + AUDIO_LOG_PERIOD_US;
        }
    }
}

/* The network side of the microphone: drains the queue into the client's
 * socket at whatever pace TCP allows, a backlog in blocks of up to 40 ms so
 * a recovered link catches up at once, and throws it away while there is no
 * client. A new client starts live: what the queue holds when it is accepted
 * was captured for the one before it (a stalled client that gave way leaves
 * up to AUDIO_MIC_BUF_MS behind) and would reach this one seconds old, so it
 * is thrown away too. */
static void send_task(void *arg)
{
    (void)arg;
    static int16_t pcm[AUDIO_FRAMES * 4];
    uint32_t client = 0;                /* the s_client_gen the queue was last drained for */
    for (;;) {
        size_t got = xStreamBufferReceive(s_mic, pcm, sizeof(pcm), portMAX_DELAY);
        uint32_t gen = s_client_gen;
        if (gen != client) {
            client = gen;
            while (xStreamBufferReceive(s_mic, pcm, sizeof(pcm), 0) > 0) {
            }
            continue;
        }
        send_to_client((const uint8_t *)pcm, got, gen);
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

/* Close the client. audio_send may be between two tries of a send the peer
 * no longer acknowledges: stop the mic from queuing for that peer meanwhile,
 * and shut the socket down first so a send in flight returns now. */
static void drop_client(int fd)
{
    s_gone = true;
    shutdown(fd, SHUT_RDWR);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    close(fd);
    s_client = -1;
    s_gone = false;
    xSemaphoreGive(s_lock);
}

/* Accept a connection: the first becomes the client, any further one is
 * closed at once so the caller sees EOF instead of a silent hang. The one
 * exception is a client whose socket has taken nothing for AUDIO_STALLED_MS:
 * that is a peer that vanished without a FIN (its Wi-Fi dropped, it was
 * killed), which lwIP would keep retransmitting to for over a minute while
 * the same machine, reconnecting, was refused. It gives way to the new one. */
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
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int64_t stalled_ms = (esp_timer_get_time() - s_sent_at) / 1000;
        xSemaphoreGive(s_lock);
        if (stalled_ms < AUDIO_STALLED_MS) {
            ESP_LOGW(TAG, "%s:%u refused, %s already streams", ip, ntohs(addr.sin_port), s_peer);
            close(fd);
            return current;
        }
        ESP_LOGW(TAG, "%s: took nothing for %lld ms, gives way to %s:%u", s_peer, (long long)stalled_ms, ip, ntohs(addr.sin_port));
        drop_client(current);
    }

    int on = 1, idle = AUDIO_KEEPIDLE_S, intvl = AUDIO_KEEPINTVL_S, cnt = AUDIO_KEEPCNT;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_peer, sizeof(s_peer), "%s:%u", ip, ntohs(addr.sin_port));
    s_client_gen++;
    s_sent_at = esp_timer_get_time();
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
            drop_client(client);
            client = -1;
            carry = 0;
        }
        if (FD_ISSET(listen_fd, &rfds)) {
            int accepted = accept_client(listen_fd, client);
            if (accepted != client) carry = 0;      /* a new client, a stalled one's odd byte goes with it */
            client = accepted;
        }
    }
}

esp_err_t audio_start(const audio_pins_t *pins)
{
    ESP_RETURN_ON_FALSE(pins, ESP_ERR_INVALID_ARG, TAG, "no pins");
    s_lock = xSemaphoreCreateMutex();
    s_play = xStreamBufferCreate(AUDIO_PLAY_BUF_BYTES, sizeof(int16_t));
    /* Two seconds of microphone is 64 kB: from PSRAM, the internal heap is
     * what Wi-Fi and the AFE live on. One writer (audio_rx), one reader
     * (audio_send), as a stream buffer wants. */
    s_mic = xStreamBufferCreateWithCaps(AUDIO_MIC_BUF_BYTES, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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

    if (pins->sd_mode >= 0) {
        /* The amp's SD_MODE: high turns it on and, at 3.3 V, selects the left
         * slot; tx_task writes the same sample to both slots anyway. */
        gpio_config_t sd = {
            .pin_bit_mask = 1ULL << pins->sd_mode,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&sd), TAG, "amp sd_mode pin");
        ESP_RETURN_ON_ERROR(gpio_set_level((gpio_num_t)pins->sd_mode, 1), TAG, "amp sd_mode high");
    }

    BaseType_t ok = xTaskCreatePinnedToCore(rx_task, "audio_rx", 4096, NULL, 5, NULL, AUDIO_I2S_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "audio_rx task");
    ok = xTaskCreatePinnedToCore(tx_task, "audio_tx", 3072, NULL, 5, NULL, AUDIO_I2S_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "audio_tx task");
    ok = xTaskCreatePinnedToCore(send_task, "audio_send", 3072, NULL, 4, NULL, AUDIO_NET_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "audio_send task");
    ok = xTaskCreatePinnedToCore(net_task, "audio_net", 4096, NULL, 4, NULL, AUDIO_NET_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "audio_net task");

    ESP_LOGI(TAG, "i2s bclk %d ws %d dout %d din %d, sd_mode %d, %d Hz, 32-bit slots, mic slot %c, free heap %u",
             pins->bclk, pins->ws, pins->dout, pins->din, pins->sd_mode, AUDIO_RATE_HZ, AUDIO_MIC_SLOT ? 'R' : 'L',
             (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}

/* command.c - line assembly, JSON parsing and validation for the face
 * protocol. See command.h for how transports use it.
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "cJSON.h"

#include "command.h"

static const char *TAG = "command";

#define REPLY_MAX       48
#define LOG_PREVIEW     64          /* how much of a rejected line goes to the log */

static QueueHandle_t s_queue;

void cmd_init(QueueHandle_t queue)
{
    s_queue = queue;
}

void cmd_line_reset(cmd_line_t *l)
{
    l->len = 0;
    l->too_long = false;
}

bool cmd_line_feed(cmd_line_t *l, char c)
{
    if (c != '\n') {
        if (l->len <= FACE_LINE_MAX) {
            l->buf[l->len++] = c;
        } else {
            l->too_long = true;         /* keep draining until the LF */
        }
        return false;
    }
    if (l->len && l->buf[l->len - 1] == '\r') l->len--;
    l->buf[l->len] = '\0';
    if (l->len == 0 && !l->too_long) return false;    /* an empty line is not a command */
    return true;
}

/* An integer in lo..hi, refusing fractions and anything that is not a number. */
static bool json_int(const cJSON *item, int32_t lo, int32_t hi, int32_t *out)
{
    if (!cJSON_IsNumber(item)) return false;
    double d = item->valuedouble;
    if (d < (double)lo || d > (double)hi) return false;
    int32_t v = (int32_t)d;
    if ((double)v != d) return false;
    *out = v;
    return true;
}

/* Fill `cmd` from the recognised keys of `root`. Returns NULL when every
 * present key is valid, else the reason for `err <reason>`. Unknown keys are
 * ignored, so `{}` and objects with only unknown keys come back clean and empty. */
static const char *parse_object(const cJSON *root, face_cmd_t *cmd)
{
    const cJSON *item;
    int32_t v;

    item = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (item) {
        if (!cJSON_IsString(item) || !face_mode_from_string(item->valuestring, &cmd->mode)) {
            return "bad mode";
        }
        cmd->has_mode = true;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (item) {
        if (!cJSON_IsString(item)) return "bad t";
        size_t len = strlen(item->valuestring);
        if (len > FACE_TEXT_MAX) return "bad t";
        memcpy(cmd->text, item->valuestring, len + 1);
        cmd->has_text = true;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "ttl");
    if (item) {
        if (!json_int(item, 0, INT32_MAX, &v)) return "bad ttl";
        cmd->ttl_s = v;
        cmd->has_ttl = true;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "brightness");
    if (item) {
        if (!json_int(item, 0, 255, &v)) return "bad brightness";
        cmd->brightness = (uint8_t)v;
        cmd->has_brightness = true;
    }

    return NULL;
}

/* NULL when the line is accepted (a ping, or a command the render task now owns). */
static const char *dispatch(const char *line, size_t len)
{
    face_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));

    cJSON *root = cJSON_ParseWithLength(line, len);
    const char *reason = "bad json";
    if (root && cJSON_IsObject(root)) {
        reason = parse_object(root, &cmd);
    }
    cJSON_Delete(root);
    if (reason) return reason;

    if (cmd.has_mode || cmd.has_text || cmd.has_ttl || cmd.has_brightness) {
        if (xQueueSend(s_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) return "busy";
    }
    return NULL;
}

void cmd_handle(cmd_line_t *l, cmd_reply_fn reply, void *ctx)
{
    const char *reason = (l->too_long || l->len > FACE_LINE_MAX) ? "too long" : dispatch(l->buf, l->len);
    if (!reason) {
        reply(ctx, "ok\n", 3);
    } else {
        char out[REPLY_MAX];
        int n = snprintf(out, sizeof(out), "err %s\n", reason);
        if (n >= (int)sizeof(out)) n = (int)sizeof(out) - 1;
        ESP_LOGW(TAG, "rejected (%s): %.*s", reason, LOG_PREVIEW, l->buf);
        reply(ctx, out, (size_t)n);
    }
    cmd_line_reset(l);
}

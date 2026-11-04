#include "websocket_internal.h"
#include "loop_internal.h"
#include "server_internal.h"

#include <stdlib.h>
#include <string.h>

static rotmos_status owner(rotmos_websocket *ws, rotmos_error *error)
{
    if (ws == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "websocket",
                           "provide a WebSocket");
    return rotmos_loop_owner(ws->server->loop, error);
}

void rotmos_websocket_config_init(rotmos_websocket_config *config)
{
    if (config != NULL)
        memset(config, 0, sizeof(*config));
}

void rotmos_websocket_route_config_init(rotmos_websocket_route_config *config)
{
    if (config != NULL)
        memset(config, 0, sizeof(*config));
}

rotmos_websocket_phase rotmos_websocket_state(const rotmos_websocket *ws)
{
    return ws != NULL ? ws->phase : ROTMOS_WEBSOCKET_CLOSED;
}

rotmos_server *rotmos_websocket_server(const rotmos_websocket *ws)
{
    return ws != NULL ? ws->server : NULL;
}

rotmos_loop *rotmos_websocket_loop(const rotmos_websocket *ws)
{
    return ws != NULL ? ws->server->loop : NULL;
}

size_t rotmos_websocket_queued_bytes(const rotmos_websocket *ws)
{
    return ws != NULL && ws->channel != NULL ? ws->channel->retained : 0;
}

void *rotmos_websocket_userdata(const rotmos_websocket *ws)
{
    return ws != NULL ? ws->config.userdata : NULL;
}

rotmos_status rotmos_websocket_set_userdata(rotmos_websocket *ws, void *data,
                                            rotmos_error *error)
{
    rotmos_status status;
    status = owner(ws, error);
    if (status != ROTMOS_OK)
        return status;
    if (ws->phase == ROTMOS_WEBSOCKET_CLOSED)
        return rotmos_fail(error, ROTMOS_STATE, "websocket_userdata",
                           "WebSocket is closed");
    ws->config.userdata = data;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

int rotmos_websocket_code(unsigned short code)
{
    return (code >= 1000 && code <= 1014 && code != 1004 && code != 1005 &&
            code != 1006) ||
           (code >= 3000 && code <= 4999);
}

int rotmos_websocket_utf8(const unsigned char *s, size_t n)
{
    size_t i;
    unsigned int need;
    unsigned long value;
    unsigned long minimum;
    i = 0;
    while (i < n) {
        value = s[i++];
        if (value < 128)
            continue;
        if (value >= 194 && value <= 223) {
            need = 1;
            minimum = 128;
            value &= 31;
        } else if (value >= 224 && value <= 239) {
            need = 2;
            minimum = 2048;
            value &= 15;
        } else if (value >= 240 && value <= 244) {
            need = 3;
            minimum = 65536;
            value &= 7;
        } else
            return 0;
        if (n - i < need)
            return 0;
        while (need-- != 0) {
            if ((s[i] & 192) != 128)
                return 0;
            value = (value << 6) | (s[i++] & 63);
        }
        if (value < minimum || value > 0x10ffffUL ||
            (value >= 0xd800UL && value <= 0xdfffUL))
            return 0;
    }
    return 1;
}

rotmos_status rotmos_websocket_queue(rotmos_websocket *ws,
                                     rotmos_websocket_opcode opcode,
                                     const void *data, size_t length,
                                     rotmos_error *error)
{
    unsigned char *wire;
    size_t head;
    size_t at;
    size_t value;
    rotmos_status status;
    head = length < 126 ? 2 : (length <= 65535 ? 4 : 10);
    if (length > (size_t)-1 - head ||
        length + head > ws->channel->config.send_bytes)
        return rotmos_fail(error, ROTMOS_LIMIT, "websocket_send",
                           "complete frame exceeds output budget");
    if (length + head > ws->channel->config.send_bytes - ws->channel->retained)
        return ROTMOS_AGAIN;
    wire = malloc(head + length);
    if (wire == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "websocket_send",
                           "cannot allocate complete frame");
    wire[0] = (unsigned char)(128 | opcode);
    wire[1] = (unsigned char)(head == 2 ? length : (head == 4 ? 126 : 127));
    value = length;
    for (at = head; at > 2;) {
        wire[--at] = (unsigned char)(value & 255);
        value >>= 8;
    }
    if (length != 0)
        memcpy(wire + head, data, length);
    status = rotmos_channel_enqueue(ws->channel, wire, head + length, error);
    if (status != ROTMOS_OK)
        free(wire);
    return status;
}

rotmos_status rotmos_websocket_send(rotmos_websocket *ws,
                                    rotmos_websocket_opcode opcode,
                                    const void *data, size_t length,
                                    rotmos_error *error)
{
    rotmos_status status;
    size_t head;
    unsigned short code;
    const unsigned char *bytes;
    status = owner(ws, error);
    if (status != ROTMOS_OK)
        return status;
    if (ws->phase != ROTMOS_WEBSOCKET_OPEN || ws->channel == NULL)
        return rotmos_fail(error, ROTMOS_STATE, "websocket_send",
                           "send requires an open WebSocket");
    if (opcode == ROTMOS_WEBSOCKET_CONTINUATION)
        return rotmos_fail(error, ROTMOS_NOT_SUPPORTED, "websocket_send",
                           "continuation frames are unsupported");
    if ((data == NULL && length != 0) ||
        (opcode != ROTMOS_WEBSOCKET_TEXT && opcode != ROTMOS_WEBSOCKET_BINARY &&
         opcode != ROTMOS_WEBSOCKET_PING && opcode != ROTMOS_WEBSOCKET_PONG &&
         opcode != ROTMOS_WEBSOCKET_CLOSE))
        return rotmos_fail(error, ROTMOS_INVALID, "websocket_send",
                           "invalid frame bytes or opcode");
    if (length > ws->config.max_frame_bytes ||
        (opcode >= ROTMOS_WEBSOCKET_CLOSE && length > 125))
        return rotmos_fail(error, ROTMOS_LIMIT, "websocket_send",
                           "frame exceeds its payload bound");
    if (opcode == ROTMOS_WEBSOCKET_TEXT && !rotmos_websocket_utf8(data, length))
        return rotmos_fail(error, ROTMOS_INVALID, "websocket_send",
                           "text must be valid UTF-8");
    code = 0;
    if (opcode == ROTMOS_WEBSOCKET_CLOSE) {
        if (length == 1)
            return rotmos_fail(error, ROTMOS_INVALID, "websocket_send",
                               "close requires an empty or valid code/reason");
        bytes = data;
        code = length == 0 ? 0 : (unsigned short)(bytes[0] * 256 + bytes[1]);
        if ((length != 0 && !rotmos_websocket_code(code)) ||
            (length > 2 && !rotmos_websocket_utf8(bytes + 2, length - 2)))
            return rotmos_fail(error, ROTMOS_INVALID, "websocket_send",
                               "invalid close code or UTF-8 reason");
    }
    status = rotmos_websocket_queue(ws, opcode, data, length, error);
    if (status == ROTMOS_OK && opcode == ROTMOS_WEBSOCKET_CLOSE) {
        rotmos_websocket_stop(ws, ROTMOS_OK, code);
        ws->close_queued = 1;
    }
    if (status == ROTMOS_AGAIN) {
        head = length < 126 ? 2 : (length <= 65535 ? 4 : 10);
        ws->writable_need = length + head;
        rotmos_websocket_schedule(ws);
    }
    if (status == ROTMOS_OK || status == ROTMOS_AGAIN)
        rotmos_error_clear(error);
    return status;
}

rotmos_status rotmos_websocket_send_text(rotmos_websocket *ws, const char *text,
                                         size_t length, rotmos_error *error)
{
    return rotmos_websocket_send(ws, ROTMOS_WEBSOCKET_TEXT, text, length,
                                 error);
}

rotmos_status rotmos_websocket_send_binary(rotmos_websocket *ws,
                                           const void *data, size_t length,
                                           rotmos_error *error)
{
    return rotmos_websocket_send(ws, ROTMOS_WEBSOCKET_BINARY, data, length,
                                 error);
}

rotmos_status rotmos_websocket_resume(rotmos_websocket *ws, rotmos_error *error)
{
    rotmos_status status;
    status = owner(ws, error);
    if (status != ROTMOS_OK)
        return status;
    if (ws->phase != ROTMOS_WEBSOCKET_OPEN)
        return rotmos_fail(error, ROTMOS_STATE, "websocket_resume",
                           "resume requires an open WebSocket");
    if (ws->paused || ws->in_frame) {
        ws->resume_requested = 1;
        rotmos_websocket_schedule(ws);
    }
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_websocket_close(rotmos_websocket *ws, unsigned short code,
                                     rotmos_bytes reason, rotmos_error *error)
{
    rotmos_status status;
    status = owner(ws, error);
    if (status != ROTMOS_OK)
        return status;
    if (ws->phase == ROTMOS_WEBSOCKET_CLOSED || ws->close_started) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    if ((reason.data == NULL && reason.length != 0) || reason.length > 123 ||
        (code == 0 && reason.length != 0) ||
        (code != 0 && !rotmos_websocket_code(code)) ||
        !rotmos_websocket_utf8(reason.data, reason.length))
        return rotmos_fail(error, ROTMOS_INVALID, "websocket_close",
                           "provide a valid close code and UTF-8 reason");
    rotmos_websocket_stop(ws, ROTMOS_OK, code);
    if (code != 0 && reason.length != 0) {
        memcpy(ws->close_payload + 2, reason.data, reason.length);
        ws->close_length += reason.length;
    }
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

void rotmos_websocket_stop(rotmos_websocket *ws, rotmos_status cause,
                           unsigned short code)
{
    if (ws->phase == ROTMOS_WEBSOCKET_CLOSED)
        return;
    if (ws->outcome.cause == ROTMOS_OK)
        ws->outcome.cause = cause;
    if (!ws->close_started) {
        ws->close_started = 1;
        ws->phase = ROTMOS_WEBSOCKET_CLOSING;
        ws->outcome.local_code = code;
        ws->close_length = code == 0 ? 0 : 2;
        ws->close_payload[0] = (unsigned char)(code >> 8);
        ws->close_payload[1] = (unsigned char)(code & 255);
        (void)rotmos_loop_monotonic(&ws->close_start, NULL);
        ws->paused = 0;
        if (ws->frame_ready)
            rotmos_websocket_reset_frame(ws);
    }
    rotmos_websocket_schedule(ws);
}

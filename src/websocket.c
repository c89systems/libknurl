#include "websocket_internal.h"
#include "loop_internal.h"
#include "server_internal.h"

#include <stdlib.h>
#include <string.h>

static knurl_status owner(knurl_websocket *ws, knurl_error *error)
{
    if (ws == NULL)
        return knurl_fail(error, KNURL_INVALID, "websocket",
                          "provide a WebSocket");
    return knurl_loop_owner(ws->server->loop, error);
}

void knurl_websocket_config_init(knurl_websocket_config *config)
{
    if (config != NULL)
        memset(config, 0, sizeof(*config));
}

void knurl_websocket_route_config_init(knurl_websocket_route_config *config)
{
    if (config != NULL)
        memset(config, 0, sizeof(*config));
}

knurl_websocket_phase knurl_websocket_state(const knurl_websocket *ws)
{
    return ws != NULL ? ws->phase : KNURL_WEBSOCKET_CLOSED;
}

knurl_server *knurl_websocket_server(const knurl_websocket *ws)
{
    return ws != NULL ? ws->server : NULL;
}

knurl_loop *knurl_websocket_loop(const knurl_websocket *ws)
{
    return ws != NULL ? ws->server->loop : NULL;
}

size_t knurl_websocket_queued_bytes(const knurl_websocket *ws)
{
    return ws != NULL && ws->channel != NULL ? ws->channel->retained : 0;
}

void *knurl_websocket_userdata(const knurl_websocket *ws)
{
    return ws != NULL ? ws->config.userdata : NULL;
}

knurl_status knurl_websocket_set_userdata(knurl_websocket *ws, void *data,
                                          knurl_error *error)
{
    knurl_status status;
    status = owner(ws, error);
    if (status != KNURL_OK)
        return status;
    if (ws->phase == KNURL_WEBSOCKET_CLOSED)
        return knurl_fail(error, KNURL_STATE, "websocket_userdata",
                          "WebSocket is closed");
    ws->config.userdata = data;
    knurl_error_clear(error);
    return KNURL_OK;
}

int knurl_websocket_code(unsigned short code)
{
    return (code >= 1000 && code <= 1014 && code != 1004 && code != 1005 &&
            code != 1006) ||
           (code >= 3000 && code <= 4999);
}

int knurl_websocket_utf8(const unsigned char *s, size_t n)
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

knurl_status knurl_websocket_queue(knurl_websocket *ws,
                                   knurl_websocket_opcode opcode,
                                   const void *data, size_t length,
                                   knurl_error *error)
{
    unsigned char *wire;
    size_t head;
    size_t at;
    size_t value;
    knurl_status status;
    head = length < 126 ? 2 : (length <= 65535 ? 4 : 10);
    if (length > (size_t)-1 - head ||
        length + head > ws->channel->config.send_bytes)
        return knurl_fail(error, KNURL_LIMIT, "websocket_send",
                          "complete frame exceeds output budget");
    if (length + head > ws->channel->config.send_bytes - ws->channel->retained)
        return KNURL_AGAIN;
    wire = malloc(head + length);
    if (wire == NULL)
        return knurl_fail(error, KNURL_NOMEM, "websocket_send",
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
    status = knurl_channel_enqueue(ws->channel, wire, head + length, error);
    if (status != KNURL_OK)
        free(wire);
    return status;
}

knurl_status knurl_websocket_send(knurl_websocket *ws,
                                  knurl_websocket_opcode opcode,
                                  const void *data, size_t length,
                                  knurl_error *error)
{
    knurl_status status;
    size_t head;
    status = owner(ws, error);
    if (status != KNURL_OK)
        return status;
    if (ws->phase != KNURL_WEBSOCKET_OPEN || ws->channel == NULL)
        return knurl_fail(error, KNURL_STATE, "websocket_send",
                          "send requires an open WebSocket");
    if ((data == NULL && length != 0) ||
        (opcode != KNURL_WEBSOCKET_CONTINUATION &&
         opcode != KNURL_WEBSOCKET_TEXT && opcode != KNURL_WEBSOCKET_BINARY &&
         opcode != KNURL_WEBSOCKET_PING && opcode != KNURL_WEBSOCKET_PONG &&
         opcode != KNURL_WEBSOCKET_CLOSE))
        return knurl_fail(error, KNURL_INVALID, "websocket_send",
                          "invalid frame bytes or opcode");
    status = knurl_websocket_queue(ws, opcode, data, length, error);
    if (status == KNURL_AGAIN) {
        head = length < 126 ? 2 : (length <= 65535 ? 4 : 10);
        ws->writable_need = length + head;
        knurl_websocket_schedule(ws);
    }
    if (status == KNURL_OK || status == KNURL_AGAIN)
        knurl_error_clear(error);
    return status;
}

knurl_status knurl_websocket_send_text(knurl_websocket *ws, const char *text,
                                       size_t length, knurl_error *error)
{
    return knurl_websocket_send(ws, KNURL_WEBSOCKET_TEXT, text, length, error);
}

knurl_status knurl_websocket_send_binary(knurl_websocket *ws, const void *data,
                                         size_t length, knurl_error *error)
{
    return knurl_websocket_send(ws, KNURL_WEBSOCKET_BINARY, data, length,
                                error);
}

knurl_status knurl_websocket_resume(knurl_websocket *ws, knurl_error *error)
{
    knurl_status status;
    status = owner(ws, error);
    if (status != KNURL_OK)
        return status;
    if (ws->phase != KNURL_WEBSOCKET_OPEN)
        return knurl_fail(error, KNURL_STATE, "websocket_resume",
                          "resume requires an open WebSocket");
    if (ws->paused || ws->in_frame) {
        ws->resume_requested = 1;
        knurl_websocket_schedule(ws);
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_websocket_close(knurl_websocket *ws, unsigned short code,
                                   knurl_bytes reason, knurl_error *error)
{
    knurl_status status;
    status = owner(ws, error);
    if (status != KNURL_OK)
        return status;
    if (ws->phase == KNURL_WEBSOCKET_CLOSED || ws->close_started) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    if ((reason.data == NULL && reason.length != 0) || reason.length > 123 ||
        (code == 0 && reason.length != 0) ||
        (code != 0 && !knurl_websocket_code(code)) ||
        !knurl_websocket_utf8(reason.data, reason.length))
        return knurl_fail(error, KNURL_INVALID, "websocket_close",
                          "provide a valid close code and UTF-8 reason");
    knurl_websocket_stop(ws, KNURL_OK, code);
    if (code != 0 && reason.length != 0) {
        memcpy(ws->close_payload + 2, reason.data, reason.length);
        ws->close_length += reason.length;
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_websocket_stop(knurl_websocket *ws, knurl_status cause,
                          unsigned short code)
{
    if (ws->phase == KNURL_WEBSOCKET_CLOSED)
        return;
    if (ws->outcome.cause == KNURL_OK)
        ws->outcome.cause = cause;
    if (!ws->close_started) {
        ws->close_started = 1;
        ws->phase = KNURL_WEBSOCKET_CLOSING;
        ws->outcome.local_code = code;
        ws->close_length = code == 0 ? 0 : 2;
        ws->close_payload[0] = (unsigned char)(code >> 8);
        ws->close_payload[1] = (unsigned char)(code & 255);
        (void)knurl_loop_monotonic(&ws->close_start, NULL);
        ws->paused = 0;
        if (ws->frame_ready)
            knurl_websocket_reset_frame(ws);
    }
    knurl_websocket_schedule(ws);
}

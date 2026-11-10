#include "websocket_internal.h"
#include "loop_internal.h"

#include <limits.h>
#include <string.h>

void knurl_websocket_reset_frame(knurl_websocket *ws)
{
    ws->header_used = 0;
    ws->header_need = 2;
    ws->payload_used = ws->payload_length = 0;
    ws->frame_ready = 0;
    ws->control_pending = 0;
}

static void protocol_error(knurl_websocket *ws, knurl_status cause,
                           unsigned short code)
{
    (void)code;
    knurl_channel_finish(ws->channel, cause);
}

static knurl_status receive(knurl_websocket *ws, void *data, size_t need,
                            size_t *used)
{
    size_t count;
    knurl_status status;
    status = knurl_channel_read(ws->channel, data, need, &count, NULL);
    if (status == KNURL_OK)
        *used += count;
    else if (status == KNURL_EOF && !ws->peer_closed)
        knurl_channel_finish(ws->channel, KNURL_IO);
    return status;
}

static int frame(knurl_websocket *ws)
{
    size_t length;
    size_t at;
    size_t extra;
    size_t i;
    unsigned int opcode;
    while (!ws->frame_ready) {
        if (ws->header_used < ws->header_need) {
            if (receive(ws, ws->header + ws->header_used,
                        ws->header_need - ws->header_used,
                        &ws->header_used) != KNURL_OK)
                return 0;
            if (ws->header_used < ws->header_need)
                continue;
        }
        if (ws->header_need == 2) {
            opcode = ws->header[0] & 15;
            if ((ws->header[0] & 112) != 0 || !(ws->header[1] & 128) ||
                (opcode != 0 && opcode != 1 && opcode != 2 && opcode != 8 &&
                 opcode != 9 && opcode != 10)) {
                protocol_error(ws, KNURL_INVALID, 1002);
                return 0;
            }
            if (opcode >= 8 &&
                (!(ws->header[0] & 128) || (ws->header[1] & 127) > 125)) {
                protocol_error(ws, KNURL_INVALID, 1002);
                return 0;
            }
            if (opcode == 0) {
                protocol_error(ws, KNURL_NOT_SUPPORTED, 1003);
                return 0;
            }
            ws->opcode = (knurl_websocket_opcode)opcode;
            extra = ws->header[1] & 127;
            ws->header_need = 6 + (extra < 126 ? 0 : (extra == 126 ? 2 : 8));
            continue;
        }
        if (ws->payload_used == 0) {
            extra = ws->header[1] & 127;
            length = extra;
            if (extra >= 126) {
                if (extra == 127 && (ws->header[2] & 128)) {
                    protocol_error(ws, KNURL_INVALID, 1002);
                    return 0;
                }
                length = 0;
                for (at = 2; at < ws->header_need - 4; ++at) {
                    if (length > ((size_t)-1 - ws->header[at]) / 256) {
                        protocol_error(ws, KNURL_LIMIT, 1009);
                        return 0;
                    }
                    length = length * 256 + ws->header[at];
                }
            }
            if (length > ws->config.max_frame_bytes) {
                protocol_error(ws, KNURL_LIMIT, 1009);
                return 0;
            }
            ws->payload_length = length;
        }
        if (ws->payload_used < ws->payload_length) {
            if (receive(ws, ws->payload + ws->payload_used,
                        ws->payload_length - ws->payload_used,
                        &ws->payload_used) != KNURL_OK)
                return 0;
            if (ws->payload_used < ws->payload_length)
                continue;
        }
        for (i = 0; i < ws->payload_length; ++i)
            ws->payload[i] ^= ws->header[ws->header_need - 4 + (i & 3)];
        ws->frame_ready = 1;
    }
    return 1;
}

static int control(knurl_websocket *ws)
{
    knurl_status status;
    unsigned short code;
    if (ws->opcode == KNURL_WEBSOCKET_PING) {
        status = knurl_websocket_queue(ws, KNURL_WEBSOCKET_PONG, ws->payload,
                                       ws->payload_length, NULL);
        if (status == KNURL_AGAIN) {
            ws->control_pending = 1;
            return 0;
        }
        if (status != KNURL_OK) {
            knurl_channel_finish(ws->channel, status);
            return 0;
        }
    } else if (ws->opcode == KNURL_WEBSOCKET_CLOSE) {
        code = ws->payload_length < 2
                   ? 1005
                   : (unsigned short)(ws->payload[0] * 256 + ws->payload[1]);
        memcpy(ws->peer_payload, ws->payload, ws->payload_length);
        ws->outcome.peer_code = code;
        ws->outcome.peer_reason.data = ws->peer_payload + 2;
        ws->outcome.peer_reason.length =
            ws->payload_length > 2 ? ws->payload_length - 2 : 0;
        ws->peer_closed = 1;
        if (!ws->close_started) {
            knurl_websocket_stop(ws, KNURL_OK, 0);
        }
    }
    knurl_websocket_reset_frame(ws);
    return 1;
}

static void close_step(knurl_websocket *ws)
{
    knurl_status status;
    if (!ws->close_started || ws->channel->phase == KNURL_CHANNEL_CLOSED)
        return;
    if (!ws->close_queued) {
        status =
            knurl_websocket_queue(ws, KNURL_WEBSOCKET_CLOSE, ws->close_payload,
                                  ws->close_length, NULL);
        if (status == KNURL_AGAIN)
            return;
        if (status != KNURL_OK) {
            knurl_channel_finish(ws->channel, status);
            return;
        }
        ws->close_queued = 1;
    }
    if (ws->channel->queued == 0 && (ws->peer_closed || ws->read_failed)) {
        if (!ws->channel->shutdown_requested)
            (void)knurl_channel_shutdown_write(ws->channel, NULL);
        if (ws->channel->outcome.write_shutdown) {
            ws->outcome.clean = ws->peer_closed && !ws->read_failed;
            knurl_channel_finish(ws->channel, KNURL_OK);
        }
    }
}

void knurl_websocket_step(knurl_websocket *ws)
{
    knurl_status status;
    knurl_bytes bytes;
    knurl_error error;
    unsigned int budget;
    unsigned int interest;
    if (ws->paused && ws->resume_requested) {
        ws->resume_requested = 0;
        ws->paused = 0;
    }
    budget = 64;
    while (!ws->paused && !ws->peer_closed && !ws->read_failed &&
           ws->channel->phase != KNURL_CHANNEL_CLOSED && budget-- != 0) {
        if (!frame(ws))
            break;
        if (ws->opcode >= KNURL_WEBSOCKET_CLOSE) {
            if (!control(ws))
                break;
            continue;
        }
        if (ws->phase == KNURL_WEBSOCKET_OPEN && ws->config.on_frame != NULL) {
            bytes.data = ws->payload;
            bytes.length = ws->payload_length;
            ws->in_frame = 1;
            knurl_error_clear(&error);
            status = ws->config.on_frame(ws, ws->opcode, bytes,
                                         ws->config.userdata, &error);
            ws->in_frame = 0;
            if (ws->phase == KNURL_WEBSOCKET_OPEN && status == KNURL_AGAIN) {
                ws->paused = 1;
                break;
            }
            ws->resume_requested = 0;
            if (status != KNURL_OK && status != KNURL_AGAIN) {
                knurl_websocket_stop(ws,
                                     status > KNURL_EOF &&
                                             status <= KNURL_BACKEND
                                         ? status
                                         : KNURL_INVALID,
                                     1011);
                break;
            }
        }
        knurl_websocket_reset_frame(ws);
    }
    close_step(ws);
    if (ws->channel->phase == KNURL_CHANNEL_CLOSED)
        return;
    if (ws->phase == KNURL_WEBSOCKET_OPEN && !ws->control_pending &&
        ws->writable_need != 0 &&
        ws->writable_need <=
            ws->channel->config.send_bytes - ws->channel->retained) {
        ws->writable_need = 0;
        if (ws->config.on_writable != NULL)
            ws->config.on_writable(ws, ws->config.userdata);
    }
    interest = 0;
    if (!ws->paused && !ws->control_pending && !ws->peer_closed &&
        !ws->read_failed)
        interest |= KNURL_READABLE;
    if (ws->writable_need != 0 || ws->control_pending ||
        (ws->close_started && !ws->close_queued))
        interest |= KNURL_WRITABLE;
    (void)knurl_channel_interest(ws->channel, interest, NULL);
    if (budget == (unsigned int)-1 || ws->resume_requested)
        knurl_websocket_schedule(ws);
}

long knurl_websocket_wait(knurl_websocket *ws)
{
    knurl_u64 now;
    knurl_u64 elapsed;
    size_t value;
    int order;
    if (!ws->close_started)
        return LONG_MAX;
    if (knurl_loop_monotonic(&now, NULL) != KNURL_OK)
        return 0;
    (void)knurl_u64_compare(now, ws->close_start, &order, NULL);
    if (order < 0)
        return ws->config.close_timeout_ms;
    elapsed.high = now.high - ws->close_start.high -
                   (now.low < ws->close_start.low ? 1 : 0);
    elapsed.low = (now.low - ws->close_start.low) & 0xffffffffUL;
    if (knurl_u64_to_size(elapsed, &value, NULL) != KNURL_OK ||
        value >= (size_t)ws->config.close_timeout_ms)
        return 0;
    return ws->config.close_timeout_ms - (long)value;
}

#include "websocket_internal.h"
#include "loop_internal.h"

#include <limits.h>
#include <string.h>

void knurl_websocket_reset_frame(knurl_websocket *ws)
{
    ws->impl->header_used = 0;
    ws->impl->header_need = 2;
    ws->impl->payload_used = ws->impl->payload_length = 0;
    ws->impl->frame_ready = 0;
    ws->impl->control_pending = 0;
}

static void protocol_error(knurl_websocket *ws, knurl_status cause,
                           unsigned short code)
{
    (void)code;
    knurl_channel_finish(ws->impl->channel, cause);
}

static knurl_status receive(knurl_websocket *ws, void *data, size_t need,
                            size_t *used)
{
    size_t count;
    knurl_status status;
    status = knurl_channel_read(ws->impl->channel, data, need, &count, NULL);
    if (status == KNURL_OK)
        *used += count;
    else if (status == KNURL_EOF && !ws->impl->peer_closed)
        knurl_channel_finish(ws->impl->channel, KNURL_IO);
    return status;
}

static int frame(knurl_websocket *ws)
{
    size_t length;
    size_t at;
    size_t extra;
    size_t i;
    unsigned int opcode;
    while (!ws->impl->frame_ready) {
        if (ws->impl->header_used < ws->impl->header_need) {
            if (receive(ws, ws->impl->header + ws->impl->header_used,
                        ws->impl->header_need - ws->impl->header_used,
                        &ws->impl->header_used) != KNURL_OK)
                return 0;
            if (ws->impl->header_used < ws->impl->header_need)
                continue;
        }
        if (ws->impl->header_need == 2) {
            opcode = ws->impl->header[0] & 15;
            if ((ws->impl->header[0] & 112) != 0 ||
                !(ws->impl->header[1] & 128) ||
                (opcode != 0 && opcode != 1 && opcode != 2 && opcode != 8 &&
                 opcode != 9 && opcode != 10)) {
                protocol_error(ws, KNURL_INVALID, 1002);
                return 0;
            }
            if (opcode >= 8 && (!(ws->impl->header[0] & 128) ||
                                (ws->impl->header[1] & 127) > 125)) {
                protocol_error(ws, KNURL_INVALID, 1002);
                return 0;
            }
            if (opcode == 0) {
                protocol_error(ws, KNURL_NOT_SUPPORTED, 1003);
                return 0;
            }
            ws->impl->opcode = (knurl_websocket_opcode)opcode;
            extra = ws->impl->header[1] & 127;
            ws->impl->header_need =
                6 + (extra < 126 ? 0 : (extra == 126 ? 2 : 8));
            continue;
        }
        if (ws->impl->payload_used == 0) {
            extra = ws->impl->header[1] & 127;
            length = extra;
            if (extra >= 126) {
                if (extra == 127 && (ws->impl->header[2] & 128)) {
                    protocol_error(ws, KNURL_INVALID, 1002);
                    return 0;
                }
                length = 0;
                for (at = 2; at < ws->impl->header_need - 4; ++at) {
                    if (length > ((size_t)-1 - ws->impl->header[at]) / 256) {
                        protocol_error(ws, KNURL_LIMIT, 1009);
                        return 0;
                    }
                    length = length * 256 + ws->impl->header[at];
                }
            }
            if (length > ws->impl->config.max_frame_bytes) {
                protocol_error(ws, KNURL_LIMIT, 1009);
                return 0;
            }
            ws->impl->payload_length = length;
        }
        if (ws->impl->payload_used < ws->impl->payload_length) {
            if (receive(ws, ws->impl->payload + ws->impl->payload_used,
                        ws->impl->payload_length - ws->impl->payload_used,
                        &ws->impl->payload_used) != KNURL_OK)
                return 0;
            if (ws->impl->payload_used < ws->impl->payload_length)
                continue;
        }
        for (i = 0; i < ws->impl->payload_length; ++i)
            ws->impl->payload[i] ^=
                ws->impl->header[ws->impl->header_need - 4 + (i & 3)];
        ws->impl->frame_ready = 1;
    }
    return 1;
}

static int control(knurl_websocket *ws)
{
    knurl_status status;
    unsigned short code;
    if (ws->impl->opcode == KNURL_WEBSOCKET_PING) {
        status =
            knurl_websocket_queue(ws, KNURL_WEBSOCKET_PONG, ws->impl->payload,
                                  ws->impl->payload_length, NULL);
        if (status == KNURL_AGAIN) {
            ws->impl->control_pending = 1;
            return 0;
        }
        if (status != KNURL_OK) {
            knurl_channel_finish(ws->impl->channel, status);
            return 0;
        }
    } else if (ws->impl->opcode == KNURL_WEBSOCKET_CLOSE) {
        code = ws->impl->payload_length < 2
                   ? 1005
                   : (unsigned short)(ws->impl->payload[0] * 256 +
                                      ws->impl->payload[1]);
        memcpy(ws->impl->peer_payload, ws->impl->payload,
               ws->impl->payload_length);
        ws->impl->outcome.peer_code = code;
        ws->impl->outcome.peer_reason.data = ws->impl->peer_payload + 2;
        ws->impl->outcome.peer_reason.length =
            ws->impl->payload_length > 2 ? ws->impl->payload_length - 2 : 0;
        ws->impl->peer_closed = 1;
        if (!ws->impl->close_started) {
            knurl_websocket_stop(ws, KNURL_OK, 0);
        }
    }
    knurl_websocket_reset_frame(ws);
    return 1;
}

static void close_step(knurl_websocket *ws)
{
    knurl_status status;
    if (!ws->impl->close_started ||
        ws->impl->channel->impl->phase == KNURL_CHANNEL_CLOSED)
        return;
    if (!ws->impl->close_queued) {
        status = knurl_websocket_queue(ws, KNURL_WEBSOCKET_CLOSE,
                                       ws->impl->close_payload,
                                       ws->impl->close_length, NULL);
        if (status == KNURL_AGAIN)
            return;
        if (status != KNURL_OK) {
            knurl_channel_finish(ws->impl->channel, status);
            return;
        }
        ws->impl->close_queued = 1;
    }
    if (ws->impl->channel->impl->queued == 0 &&
        (ws->impl->peer_closed || ws->impl->read_failed)) {
        if (!ws->impl->channel->impl->shutdown_requested)
            (void)knurl_channel_shutdown_write(ws->impl->channel, NULL);
        if (ws->impl->channel->impl->outcome.write_shutdown) {
            ws->impl->outcome.clean =
                ws->impl->peer_closed && !ws->impl->read_failed;
            knurl_channel_finish(ws->impl->channel, KNURL_OK);
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
    if (ws->impl->paused && ws->impl->resume_requested) {
        ws->impl->resume_requested = 0;
        ws->impl->paused = 0;
    }
    budget = 64;
    while (!ws->impl->paused && !ws->impl->peer_closed &&
           !ws->impl->read_failed &&
           ws->impl->channel->impl->phase != KNURL_CHANNEL_CLOSED &&
           budget-- != 0) {
        if (!frame(ws))
            break;
        if (ws->impl->opcode >= KNURL_WEBSOCKET_CLOSE) {
            if (!control(ws))
                break;
            continue;
        }
        if (ws->impl->phase == KNURL_WEBSOCKET_OPEN &&
            ws->impl->config.on_frame != NULL) {
            bytes.data = ws->impl->payload;
            bytes.length = ws->impl->payload_length;
            ws->impl->in_frame = 1;
            knurl_error_clear(&error);
            status = ws->impl->config.on_frame(
                ws, ws->impl->opcode, bytes, ws->impl->config.userdata, &error);
            ws->impl->in_frame = 0;
            if (ws->impl->phase == KNURL_WEBSOCKET_OPEN &&
                status == KNURL_AGAIN) {
                ws->impl->paused = 1;
                break;
            }
            ws->impl->resume_requested = 0;
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
    if (ws->impl->channel->impl->phase == KNURL_CHANNEL_CLOSED)
        return;
    if (ws->impl->phase == KNURL_WEBSOCKET_OPEN && !ws->impl->control_pending &&
        ws->impl->writable_need != 0 &&
        ws->impl->writable_need <= ws->impl->channel->impl->config.send_bytes -
                                       ws->impl->channel->impl->retained) {
        ws->impl->writable_need = 0;
        if (ws->impl->config.on_writable != NULL)
            ws->impl->config.on_writable(ws, ws->impl->config.userdata);
    }
    interest = 0;
    if (!ws->impl->paused && !ws->impl->control_pending &&
        !ws->impl->peer_closed && !ws->impl->read_failed)
        interest |= KNURL_READABLE;
    if (ws->impl->writable_need != 0 || ws->impl->control_pending ||
        (ws->impl->close_started && !ws->impl->close_queued))
        interest |= KNURL_WRITABLE;
    (void)knurl_channel_interest(ws->impl->channel, interest, NULL);
    if (budget == (unsigned int)-1 || ws->impl->resume_requested)
        knurl_websocket_schedule(ws);
}

long knurl_websocket_wait(knurl_websocket *ws)
{
    knurl_u64 now;
    knurl_u64 elapsed;
    size_t value;
    int order;
    if (!ws->impl->close_started)
        return LONG_MAX;
    if (knurl_loop_monotonic(&now, NULL) != KNURL_OK)
        return 0;
    (void)knurl_u64_compare(now, ws->impl->close_start, &order, NULL);
    if (order < 0)
        return ws->impl->config.close_timeout_ms;
    elapsed.high = now.high - ws->impl->close_start.high -
                   (now.low < ws->impl->close_start.low ? 1 : 0);
    elapsed.low = (now.low - ws->impl->close_start.low) & 0xffffffffUL;
    if (knurl_u64_to_size(elapsed, &value, NULL) != KNURL_OK ||
        value >= (size_t)ws->impl->config.close_timeout_ms)
        return 0;
    return ws->impl->config.close_timeout_ms - (long)value;
}

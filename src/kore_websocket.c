#include "kore_internal.h"
#include "websocket_internal.h"
#include "response_internal.h"

#include <stdlib.h>
#include <string.h>

void rotmos_websocket_schedule(rotmos_websocket *ws)
{
    ws->ready = 1;
    if (ws->server->loop != NULL)
        ws->server->loop->backend.wake(ws->runtime);
}

static void channel_schedule(rotmos_channel *channel)
{
    rotmos_websocket_schedule(channel->schedule_data);
}

static void channel_ready(rotmos_channel *channel, unsigned int events,
                          void *userdata)
{
    (void)channel;
    (void)events;
    rotmos_websocket_schedule(userdata);
}

static void channel_closed(rotmos_channel *channel,
                           const rotmos_channel_outcome *out, void *userdata)
{
    rotmos_websocket *ws;
    (void)channel;
    ws = userdata;
    if (ws->outcome.cause == ROTMOS_OK)
        ws->outcome.cause = out->cause;
    rotmos_websocket_schedule(ws);
}

static int field_is(const rotmos_header *h, const char *name)
{
    const unsigned char *a;
    size_t i;
    unsigned char c;
    if (h->name.data == NULL || h->name.length != strlen(name))
        return 0;
    a = h->name.data;
    for (i = 0; i < h->name.length; ++i) {
        c = a[i];
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c + 'a' - 'A');
        if (c != (unsigned char)name[i])
            return 0;
    }
    return 1;
}

rotmos_status rotmos_websocket_accept(rotmos_request *request,
                                      const rotmos_websocket_config *config,
                                      rotmos_websocket **out,
                                      rotmos_error *error)
{
    rotmos_websocket *ws;
    rotmos_websocket_config resolved;
    rotmos_channel_config transport;
    rotmos_header *headers;
    rotmos_status status;
    size_t i;
    size_t count;
    char accept[29];
    if (out != NULL)
        *out = NULL;
    if (request == NULL || config == NULL || out == NULL ||
        (config->header_count != 0 && config->headers == NULL))
        return rotmos_fail(error, ROTMOS_INVALID, "websocket_accept",
                           "provide request, configuration and output");
    status = rotmos_loop_owner(request->server->loop, error);
    if (status != ROTMOS_OK)
        return status;
    if (!request->in_headers || request->channel != NULL ||
        request->close_notified)
        return rotmos_fail(
            error, ROTMOS_STATE, "websocket_accept",
            "accept is exclusive and available at headers dispatch");
    resolved = *config;
    if (resolved.max_frame_bytes == 0)
        resolved.max_frame_bytes =
            request->server->config.limits.websocket_max_frame_bytes;
    if (resolved.output_queue_bytes == 0)
        resolved.output_queue_bytes = (size_t)-1;
    if (resolved.close_timeout_ms == 0)
        resolved.close_timeout_ms = 5000;
    if (resolved.max_frame_bytes >
            request->server->config.limits.websocket_max_frame_bytes ||
        resolved.output_queue_bytes < 127 || resolved.close_timeout_ms < 0)
        return rotmos_fail(error, ROTMOS_LIMIT, "websocket_accept",
                           "receive frame must fit server limit; output needs "
                           "127 bytes and close timeout must be nonnegative");
    if (!rotmos_websocket_accept_key(request, accept))
        return rotmos_fail(error, ROTMOS_INVALID, "websocket_accept",
                           "request is not a valid WebSocket handshake");
    count = resolved.header_count;
    if (count > (size_t)-1 / sizeof(*headers) - 4)
        return rotmos_fail(error, ROTMOS_LIMIT, "websocket_accept",
                           "handshake field count exceeds finite capacity");
    for (i = 0; i < count; ++i)
        if (field_is(&resolved.headers[i], "sec-websocket-extensions"))
            return rotmos_fail(
                error, ROTMOS_NOT_SUPPORTED, "websocket_accept",
                "application WebSockets do not negotiate extensions");
    for (i = 0; i < request->response->headers->header_count; ++i)
        if (field_is(&request->response->headers->headers[i],
                     "sec-websocket-extensions"))
            return rotmos_fail(
                error, ROTMOS_NOT_SUPPORTED, "websocket_accept",
                "application WebSockets do not negotiate extensions");
    headers = malloc((count + 4) * sizeof(*headers));
    ws = calloc(1, sizeof(*ws));
    if (headers == NULL || ws == NULL) {
        free(headers);
        free(ws);
        return rotmos_fail(error, ROTMOS_NOMEM, "websocket_accept",
                           "cannot allocate handshake state");
    }
    ws->payload =
        malloc(resolved.max_frame_bytes > 125 ? resolved.max_frame_bytes : 125);
    if (ws->payload == NULL) {
        free(headers);
        free(ws);
        return rotmos_fail(error, ROTMOS_NOMEM, "websocket_accept",
                           "cannot allocate bounded frame storage");
    }
    ws->server = request->server;
    ws->runtime = kore_library_hooks.arg;
    ws->config = resolved;
    /* Strings/fields belong only to the handshake. Never retain caller text. */
    ws->config.protocol = NULL;
    ws->config.headers = NULL;
    ws->config.header_count = 0;
    rotmos_websocket_reset_frame(ws);
    rotmos_channel_config_init(&transport);
    transport.send_bytes = resolved.output_queue_bytes;
    transport.interest = 0;
    transport.on_event = channel_ready;
    transport.on_close = channel_closed;
    transport.userdata = ws;
    status =
        rotmos_kore_channel_prepare(request, &transport, &ws->channel, error);
    if (count != 0)
        memcpy(headers, resolved.headers, count * sizeof(*headers));
    headers[count].name = rotmos_view("Upgrade");
    headers[count++].value = rotmos_view("websocket");
    headers[count].name = rotmos_view("Connection");
    headers[count++].value = rotmos_view("Upgrade");
    headers[count].name = rotmos_view("Sec-WebSocket-Accept");
    headers[count++].value = rotmos_view(accept);
    if (resolved.protocol != NULL) {
        headers[count].name = rotmos_view("Sec-WebSocket-Protocol");
        headers[count++].value = rotmos_view(resolved.protocol);
    }
    if (status == ROTMOS_OK)
        status = rotmos_channel_upgrade(ws->channel, headers, count, error);
    free(headers);
    if (status != ROTMOS_OK) {
        rotmos_channel_release(ws->channel, 0);
        free(ws->payload);
        free(ws);
        return status;
    }
    ws->channel->idle = ws->server->config.timeouts.websocket_idle;
    request->websocket = ws;
    ws->next = ws->runtime->websockets;
    ws->runtime->websockets = ws;
    rotmos_kore_channel_attach(ws->channel);
    *out = ws;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

void rotmos_kore_websocket_handshake(rotmos_request *request, int upgraded)
{
    rotmos_websocket *ws;
    ws = request->websocket;
    if (ws == NULL)
        return;
    if (upgraded) {
        ws->channel->schedule = channel_schedule;
        ws->channel->schedule_data = ws;
        rotmos_kore_channel_detach(ws->channel);
        request->channel = NULL;
    } else {
        ws->channel = NULL;
        if (ws->outcome.cause == ROTMOS_OK)
            ws->outcome.cause = request->outcome.cause != ROTMOS_OK
                                    ? request->outcome.cause
                                    : ROTMOS_CANCELLED;
    }
    ws->handshake_done = 1;
    rotmos_websocket_schedule(ws);
}

static void release(rotmos_websocket *ws)
{
    ws->phase = ROTMOS_WEBSOCKET_CLOSED;
    if (ws->channel != NULL) {
        if (ws->outcome.cause == ROTMOS_OK)
            ws->outcome.cause = ws->channel->outcome.cause;
        rotmos_channel_notify_closed(ws->channel);
    }
    if (!ws->peer_closed)
        ws->outcome.peer_code = 1006;
    if (ws->config.on_close != NULL)
        ws->config.on_close(ws, &ws->outcome, ws->config.userdata);
    rotmos_channel_release(ws->channel, 0);
    free(ws->payload);
    free(ws);
}

void rotmos_kore_websockets_turn(struct rotmos_runtime *runtime)
{
    rotmos_websocket **slot;
    rotmos_websocket *ws;
    rotmos_status status;
    rotmos_error error;
    slot = &runtime->websockets;
    while ((ws = *slot) != NULL) {
        if (!ws->handshake_done) {
            slot = &ws->next;
            continue;
        }
        if (ws->channel != NULL &&
            ws->channel->phase != ROTMOS_CHANNEL_CLOSED) {
            if (rotmos_websocket_wait(ws) == 0)
                rotmos_channel_finish(ws->channel, ROTMOS_TIMEOUT);
            if (rotmos_channel_wait(ws->channel) == 0) {
                if (ws->close_started)
                    rotmos_channel_finish(ws->channel, ROTMOS_TIMEOUT);
                else {
                    rotmos_websocket_stop(ws, ROTMOS_TIMEOUT, 1001);
                    ws->channel->idle.disabled = 1;
                }
            }
            if (ws->ready && ws->channel->phase != ROTMOS_CHANNEL_CLOSED) {
                ws->ready = 0;
                if (ws->phase == ROTMOS_WEBSOCKET_CONNECTING) {
                    ws->phase = ROTMOS_WEBSOCKET_OPEN;
                    ws->outcome.opened = 1;
                    rotmos_error_clear(&error);
                    status = ws->config.on_open != NULL
                                 ? ws->config.on_open(ws, ws->config.userdata,
                                                      &error)
                                 : ROTMOS_OK;
                    if (status != ROTMOS_OK)
                        rotmos_websocket_stop(ws,
                                              status > ROTMOS_EOF &&
                                                      status <= ROTMOS_BACKEND
                                                  ? status
                                                  : ROTMOS_INVALID,
                                              1011);
                }
                rotmos_channel_turn(ws->channel);
                if (ws->channel->phase != ROTMOS_CHANNEL_CLOSED)
                    rotmos_websocket_step(ws);
            }
        }
        if (ws->channel == NULL ||
            ws->channel->phase == ROTMOS_CHANNEL_CLOSED) {
            *slot = ws->next;
            release(ws);
        } else
            slot = &ws->next;
    }
}

u_int64_t rotmos_kore_websockets_wait(struct rotmos_runtime *runtime,
                                      u_int64_t maximum)
{
    rotmos_websocket *ws;
    long value;
    for (ws = runtime->websockets; ws != NULL; ws = ws->next) {
        if (!ws->handshake_done)
            continue;
        if (ws->ready || ws->channel == NULL ||
            ws->channel->phase == ROTMOS_CHANNEL_CLOSED)
            return 0;
        value = rotmos_websocket_wait(ws);
        if ((u_int64_t)value < maximum)
            maximum = (u_int64_t)value;
        value = rotmos_channel_wait(ws->channel);
        if ((u_int64_t)value < maximum)
            maximum = (u_int64_t)value;
    }
    return maximum;
}

void rotmos_kore_websockets_stopping(struct rotmos_runtime *runtime)
{
    rotmos_websocket *ws;
    long deadline;
    deadline = runtime->server->config.workers.shutdown_timeout_ms;
    for (ws = runtime->websockets; ws != NULL; ws = ws->next) {
        if (deadline < ws->config.close_timeout_ms)
            ws->config.close_timeout_ms = deadline;
        rotmos_websocket_stop(ws, ROTMOS_CANCELLED, 1001);
    }
}

void rotmos_kore_websockets_shutdown(struct rotmos_runtime *runtime)
{
    rotmos_websocket *ws;
    while ((ws = runtime->websockets) != NULL) {
        runtime->websockets = ws->next;
        if (ws->channel != NULL)
            rotmos_channel_finish(ws->channel, ROTMOS_CANCELLED);
        release(ws);
    }
}

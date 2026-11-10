#include "kore_internal.h"
#include "websocket_internal.h"
#include "response_internal.h"

#include <stdlib.h>
#include <string.h>

void knurl_websocket_schedule(knurl_websocket *ws)
{
    ws->ready = 1;
    if (ws->server->loop != NULL)
        ws->server->loop->backend.wake(ws->runtime);
}

static void channel_schedule(knurl_channel *channel)
{
    knurl_websocket_schedule(channel->schedule_data);
}

static void channel_ready(knurl_channel *channel, unsigned int events,
                          void *userdata)
{
    (void)channel;
    (void)events;
    knurl_websocket_schedule(userdata);
}

static void channel_closed(knurl_channel *channel,
                           const knurl_channel_outcome *out, void *userdata)
{
    knurl_websocket *ws;
    (void)channel;
    ws = userdata;
    if (ws->outcome.cause == KNURL_OK)
        ws->outcome.cause = out->cause;
    knurl_websocket_schedule(ws);
}

static int field_is(const knurl_header *h, const char *name)
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

knurl_status knurl_websocket_accept(knurl_request *request,
                                    const knurl_websocket_config *config,
                                    knurl_websocket **out, knurl_error *error)
{
    knurl_websocket *ws;
    knurl_websocket_config resolved;
    knurl_channel_config transport;
    knurl_header *headers;
    knurl_status status;
    size_t i;
    size_t count;
    char accept[29];
    if (out != NULL)
        *out = NULL;
    if (request == NULL || config == NULL || out == NULL ||
        (config->header_count != 0 && config->headers == NULL))
        return knurl_fail(error, KNURL_INVALID, "websocket_accept",
                          "provide request, configuration and output");
    status = knurl_loop_owner(request->server->loop, error);
    if (status != KNURL_OK)
        return status;
    if (!request->in_headers || request->channel != NULL ||
        request->close_notified)
        return knurl_fail(
            error, KNURL_STATE, "websocket_accept",
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
        return knurl_fail(error, KNURL_LIMIT, "websocket_accept",
                          "receive frame must fit server limit; output needs "
                          "127 bytes and close timeout must be nonnegative");
    if (!knurl_websocket_accept_key(request, accept))
        return knurl_fail(error, KNURL_INVALID, "websocket_accept",
                          "request is not a valid WebSocket handshake");
    count = resolved.header_count;
    if (count > (size_t)-1 / sizeof(*headers) - 4)
        return knurl_fail(error, KNURL_LIMIT, "websocket_accept",
                          "handshake field count exceeds finite capacity");
    for (i = 0; i < count; ++i)
        if (field_is(&resolved.headers[i], "sec-websocket-extensions"))
            return knurl_fail(
                error, KNURL_NOT_SUPPORTED, "websocket_accept",
                "application WebSockets do not negotiate extensions");
    for (i = 0; i < request->response->headers->header_count; ++i)
        if (field_is(&request->response->headers->headers[i],
                     "sec-websocket-extensions"))
            return knurl_fail(
                error, KNURL_NOT_SUPPORTED, "websocket_accept",
                "application WebSockets do not negotiate extensions");
    headers = malloc((count + 4) * sizeof(*headers));
    ws = calloc(1, sizeof(*ws));
    if (headers == NULL || ws == NULL) {
        free(headers);
        free(ws);
        return knurl_fail(error, KNURL_NOMEM, "websocket_accept",
                          "cannot allocate handshake state");
    }
    ws->payload =
        malloc(resolved.max_frame_bytes > 125 ? resolved.max_frame_bytes : 125);
    if (ws->payload == NULL) {
        free(headers);
        free(ws);
        return knurl_fail(error, KNURL_NOMEM, "websocket_accept",
                          "cannot allocate bounded frame storage");
    }
    ws->server = request->server;
    ws->runtime = kore_library_hooks.arg;
    ws->config = resolved;
    /* Strings/fields belong only to the handshake. Never retain caller text. */
    ws->config.protocol = NULL;
    ws->config.headers = NULL;
    ws->config.header_count = 0;
    knurl_websocket_reset_frame(ws);
    knurl_channel_config_init(&transport);
    transport.send_bytes = resolved.output_queue_bytes;
    transport.interest = 0;
    transport.on_event = channel_ready;
    transport.on_close = channel_closed;
    transport.userdata = ws;
    status =
        knurl_kore_channel_prepare(request, &transport, &ws->channel, error);
    if (count != 0)
        memcpy(headers, resolved.headers, count * sizeof(*headers));
    headers[count].name = knurl_view("Upgrade");
    headers[count++].value = knurl_view("websocket");
    headers[count].name = knurl_view("Connection");
    headers[count++].value = knurl_view("Upgrade");
    headers[count].name = knurl_view("Sec-WebSocket-Accept");
    headers[count++].value = knurl_view(accept);
    if (resolved.protocol != NULL) {
        headers[count].name = knurl_view("Sec-WebSocket-Protocol");
        headers[count++].value = knurl_view(resolved.protocol);
    }
    if (status == KNURL_OK)
        status = knurl_channel_upgrade(ws->channel, headers, count, error);
    free(headers);
    if (status != KNURL_OK) {
        knurl_channel_release(ws->channel, 0);
        free(ws->payload);
        free(ws);
        return status;
    }
    ws->channel->idle = ws->server->config.timeouts.websocket_idle;
    request->websocket = ws;
    ws->next = ws->runtime->websockets;
    ws->runtime->websockets = ws;
    knurl_kore_channel_attach(ws->channel);
    *out = ws;
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_kore_websocket_handshake(knurl_request *request, int upgraded)
{
    knurl_websocket *ws;
    ws = request->websocket;
    if (ws == NULL)
        return;
    if (upgraded) {
        ws->channel->schedule = channel_schedule;
        ws->channel->schedule_data = ws;
        knurl_kore_channel_detach(ws->channel);
        request->channel = NULL;
    } else {
        ws->channel = NULL;
        if (ws->outcome.cause == KNURL_OK)
            ws->outcome.cause = request->outcome.cause != KNURL_OK
                                    ? request->outcome.cause
                                    : KNURL_CANCELLED;
    }
    ws->handshake_done = 1;
    knurl_websocket_schedule(ws);
}

static void release(knurl_websocket *ws)
{
    ws->phase = KNURL_WEBSOCKET_CLOSED;
    if (ws->channel != NULL) {
        if (ws->outcome.cause == KNURL_OK)
            ws->outcome.cause = ws->channel->outcome.cause;
        knurl_channel_notify_closed(ws->channel);
    }
    if (!ws->peer_closed)
        ws->outcome.peer_code = 1006;
    if (ws->config.on_close != NULL)
        ws->config.on_close(ws, &ws->outcome, ws->config.userdata);
    knurl_channel_release(ws->channel, 0);
    free(ws->payload);
    free(ws);
}

void knurl_kore_websockets_turn(struct knurl_runtime *runtime)
{
    knurl_websocket **slot;
    knurl_websocket *ws;
    knurl_status status;
    knurl_error error;
    slot = &runtime->websockets;
    while ((ws = *slot) != NULL) {
        if (!ws->handshake_done) {
            slot = &ws->next;
            continue;
        }
        if (ws->channel != NULL && ws->channel->phase != KNURL_CHANNEL_CLOSED) {
            if (knurl_websocket_wait(ws) == 0)
                knurl_channel_finish(ws->channel, KNURL_TIMEOUT);
            if (knurl_channel_wait(ws->channel) == 0) {
                if (ws->close_started)
                    knurl_channel_finish(ws->channel, KNURL_TIMEOUT);
                else {
                    knurl_websocket_stop(ws, KNURL_TIMEOUT, 1001);
                    ws->channel->idle.disabled = 1;
                }
            }
            if (ws->ready && ws->channel->phase != KNURL_CHANNEL_CLOSED) {
                ws->ready = 0;
                if (ws->phase == KNURL_WEBSOCKET_CONNECTING) {
                    ws->phase = KNURL_WEBSOCKET_OPEN;
                    ws->outcome.opened = 1;
                    knurl_error_clear(&error);
                    status = ws->config.on_open != NULL
                                 ? ws->config.on_open(ws, ws->config.userdata,
                                                      &error)
                                 : KNURL_OK;
                    if (status != KNURL_OK)
                        knurl_websocket_stop(ws,
                                             status > KNURL_EOF &&
                                                     status <= KNURL_BACKEND
                                                 ? status
                                                 : KNURL_INVALID,
                                             1011);
                }
                knurl_channel_turn(ws->channel);
                if (ws->channel->phase != KNURL_CHANNEL_CLOSED)
                    knurl_websocket_step(ws);
            }
        }
        if (ws->channel == NULL || ws->channel->phase == KNURL_CHANNEL_CLOSED) {
            *slot = ws->next;
            release(ws);
        } else
            slot = &ws->next;
    }
}

u_int64_t knurl_kore_websockets_wait(struct knurl_runtime *runtime,
                                     u_int64_t maximum)
{
    knurl_websocket *ws;
    long value;
    for (ws = runtime->websockets; ws != NULL; ws = ws->next) {
        if (!ws->handshake_done)
            continue;
        if (ws->ready || ws->channel == NULL ||
            ws->channel->phase == KNURL_CHANNEL_CLOSED)
            return 0;
        value = knurl_websocket_wait(ws);
        if ((u_int64_t)value < maximum)
            maximum = (u_int64_t)value;
        value = knurl_channel_wait(ws->channel);
        if ((u_int64_t)value < maximum)
            maximum = (u_int64_t)value;
    }
    return maximum;
}

void knurl_kore_websockets_stopping(struct knurl_runtime *runtime)
{
    knurl_websocket *ws;
    long deadline;
    deadline = runtime->server->config.workers.shutdown_timeout_ms;
    for (ws = runtime->websockets; ws != NULL; ws = ws->next) {
        if (deadline < ws->config.close_timeout_ms)
            ws->config.close_timeout_ms = deadline;
        knurl_websocket_stop(ws, KNURL_CANCELLED, 1001);
    }
}

void knurl_kore_websockets_shutdown(struct knurl_runtime *runtime)
{
    knurl_websocket *ws;
    while ((ws = runtime->websockets) != NULL) {
        runtime->websockets = ws->next;
        if (ws->channel != NULL)
            knurl_channel_finish(ws->channel, KNURL_CANCELLED);
        release(ws);
    }
}

#include "kore_internal.h"
#include "websocket_internal.h"
#include "response_internal.h"

#include <stdlib.h>
#include <string.h>

void knurl_websocket_schedule(knurl_websocket *ws)
{
    ws->impl->ready = 1;
    if (ws->impl->server->impl->loop != NULL)
        ws->impl->server->impl->loop->impl->backend.wake(ws->impl->runtime);
}

static void channel_schedule(knurl_channel *channel)
{
    knurl_websocket_schedule(channel->impl->schedule_data);
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
    if (ws->impl->outcome.cause == KNURL_OK)
        ws->impl->outcome.cause = out->cause;
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
    status = knurl_loop_owner(request->impl->server->impl->loop, error);
    if (status != KNURL_OK)
        return status;
    if (!request->impl->in_headers || request->impl->channel != NULL ||
        request->impl->close_notified)
        return knurl_fail(
            error, KNURL_STATE, "websocket_accept",
            "accept is exclusive and available at headers dispatch");
    resolved = *config;
    if (resolved.max_frame_bytes == 0)
        resolved.max_frame_bytes = request->impl->server->impl->config.limits
                                       .websocket_max_frame_bytes;
    if (resolved.max_message_bytes == 0)
        resolved.max_message_bytes = resolved.max_frame_bytes;
    if (resolved.output_queue_bytes == 0) {
        if (resolved.max_frame_bytes > (size_t)-1 - 14)
            return knurl_fail(error, KNURL_LIMIT, "websocket_accept",
                              "frame limit cannot fit finite wire budget");
        resolved.output_queue_bytes = resolved.max_frame_bytes + 14;
        if (resolved.output_queue_bytes < 127)
            resolved.output_queue_bytes = 127;
    }
    if (resolved.close_timeout_ms == 0)
        resolved.close_timeout_ms = 5000;
    if (resolved.max_frame_bytes > request->impl->server->impl->config.limits
                                       .websocket_max_frame_bytes ||
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
    for (i = 0; i < request->impl->response->impl->headers->impl->header_count;
         ++i)
        if (field_is(&request->impl->response->impl->headers->impl->headers[i],
                     "sec-websocket-extensions"))
            return knurl_fail(
                error, KNURL_NOT_SUPPORTED, "websocket_accept",
                "application WebSockets do not negotiate extensions");
    headers = malloc((count + 4) * sizeof(*headers));
    ws = calloc(1, sizeof(struct knurl_websocket_impl));
    if (headers == NULL || ws == NULL) {
        free(headers);
        free(ws);
        return knurl_fail(error, KNURL_NOMEM, "websocket_accept",
                          "cannot allocate handshake state");
    }
    knurl_websocket_receiver_init(ws);
    ws->impl->payload =
        malloc(resolved.max_frame_bytes > 125 ? resolved.max_frame_bytes : 125);
    if (ws->impl->payload == NULL) {
        free(headers);
        free(ws);
        return knurl_fail(error, KNURL_NOMEM, "websocket_accept",
                          "cannot allocate bounded frame storage");
    }
    ws->impl->server = request->impl->server;
    ws->impl->runtime = kore_library_hooks.arg;
    ws->impl->config = resolved;
    /* Strings/fields belong only to the handshake. Never retain caller text. */
    ws->impl->config.protocol = NULL;
    ws->impl->config.headers = NULL;
    ws->impl->config.header_count = 0;
    knurl_websocket_reset_frame(ws);
    knurl_channel_config_init(&transport);
    transport.send_bytes = resolved.output_queue_bytes;
    transport.interest = 0;
    transport.on_event = channel_ready;
    transport.on_close = channel_closed;
    transport.userdata = ws;
    status = knurl_kore_channel_prepare(request, &transport, &ws->impl->channel,
                                        error);
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
        status =
            knurl_channel_upgrade(ws->impl->channel, headers, count, error);
    free(headers);
    if (status != KNURL_OK) {
        knurl_channel_release(ws->impl->channel, 0);
        free(ws->impl->payload);
        free(ws);
        return status;
    }
    ws->impl->channel->impl->idle =
        ws->impl->server->impl->config.timeouts.websocket_idle;
    request->impl->websocket = ws;
    ws->impl->next = ws->impl->runtime->websockets;
    ws->impl->runtime->websockets = ws;
    knurl_kore_channel_attach(ws->impl->channel);
    *out = ws;
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_kore_websocket_handshake(knurl_request *request, int upgraded)
{
    knurl_websocket *ws;
    ws = request->impl->websocket;
    if (ws == NULL)
        return;
    if (upgraded) {
        ws->impl->channel->impl->schedule = channel_schedule;
        ws->impl->channel->impl->schedule_data = ws;
        knurl_kore_channel_detach(ws->impl->channel);
        request->impl->channel = NULL;
    } else {
        ws->impl->channel = NULL;
        if (ws->impl->outcome.cause == KNURL_OK)
            ws->impl->outcome.cause = request->impl->outcome.cause != KNURL_OK
                                          ? request->impl->outcome.cause
                                          : KNURL_CANCELLED;
    }
    ws->impl->handshake_done = 1;
    knurl_websocket_schedule(ws);
}

static void release(knurl_websocket *ws)
{
    ws->impl->phase = KNURL_WEBSOCKET_CLOSED;
    if (ws->impl->channel != NULL) {
        if (ws->impl->outcome.cause == KNURL_OK)
            ws->impl->outcome.cause = ws->impl->channel->impl->outcome.cause;
        knurl_channel_notify_closed(ws->impl->channel);
    }
    if (!ws->impl->peer_closed)
        ws->impl->outcome.peer_code = 1006;
    if (ws->impl->config.on_close != NULL)
        ws->impl->config.on_close(ws, &ws->impl->outcome,
                                  ws->impl->config.userdata);
    knurl_channel_release(ws->impl->channel, 0);
    free(ws->impl->payload);
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
        if (!ws->impl->handshake_done) {
            slot = &ws->impl->next;
            continue;
        }
        if (ws->impl->channel != NULL &&
            ws->impl->channel->impl->phase != KNURL_CHANNEL_CLOSED) {
            if (knurl_websocket_wait(ws) == 0)
                knurl_channel_finish(ws->impl->channel, KNURL_TIMEOUT);
            if (knurl_channel_wait(ws->impl->channel) == 0) {
                if (ws->impl->close_started)
                    knurl_channel_finish(ws->impl->channel, KNURL_TIMEOUT);
                else {
                    knurl_websocket_stop(ws, KNURL_TIMEOUT, 1001);
                    ws->impl->channel->impl->idle.disabled = 1;
                }
            }
            if (ws->impl->ready &&
                ws->impl->channel->impl->phase != KNURL_CHANNEL_CLOSED) {
                ws->impl->ready = 0;
                if (ws->impl->phase == KNURL_WEBSOCKET_CONNECTING) {
                    ws->impl->phase = KNURL_WEBSOCKET_OPEN;
                    ws->impl->outcome.opened = 1;
                    knurl_error_clear(&error);
                    status = ws->impl->config.on_open != NULL
                                 ? ws->impl->config.on_open(
                                       ws, ws->impl->config.userdata, &error)
                                 : KNURL_OK;
                    if (status != KNURL_OK)
                        knurl_websocket_stop(ws,
                                             status > KNURL_EOF &&
                                                     status <= KNURL_BACKEND
                                                 ? status
                                                 : KNURL_INVALID,
                                             1011);
                }
                knurl_channel_turn(ws->impl->channel);
                if (ws->impl->channel->impl->phase != KNURL_CHANNEL_CLOSED)
                    knurl_websocket_step(ws);
            }
        }
        if (ws->impl->channel == NULL ||
            ws->impl->channel->impl->phase == KNURL_CHANNEL_CLOSED) {
            *slot = ws->impl->next;
            release(ws);
        } else
            slot = &ws->impl->next;
    }
}

u_int64_t knurl_kore_websockets_wait(struct knurl_runtime *runtime,
                                     u_int64_t maximum)
{
    knurl_websocket *ws;
    long value;
    for (ws = runtime->websockets; ws != NULL; ws = ws->impl->next) {
        if (!ws->impl->handshake_done)
            continue;
        if (ws->impl->ready || ws->impl->channel == NULL ||
            ws->impl->channel->impl->phase == KNURL_CHANNEL_CLOSED)
            return 0;
        value = knurl_websocket_wait(ws);
        if ((u_int64_t)value < maximum)
            maximum = (u_int64_t)value;
        value = knurl_channel_wait(ws->impl->channel);
        if ((u_int64_t)value < maximum)
            maximum = (u_int64_t)value;
    }
    return maximum;
}

void knurl_kore_websockets_stopping(struct knurl_runtime *runtime)
{
    knurl_websocket *ws;
    long deadline;
    deadline = runtime->server->impl->config.workers.shutdown_timeout_ms;
    for (ws = runtime->websockets; ws != NULL; ws = ws->impl->next) {
        if (deadline < ws->impl->config.close_timeout_ms)
            ws->impl->config.close_timeout_ms = deadline;
        knurl_websocket_stop(ws, KNURL_CANCELLED, 1001);
    }
}

void knurl_kore_websockets_shutdown(struct knurl_runtime *runtime)
{
    knurl_websocket *ws;
    while ((ws = runtime->websockets) != NULL) {
        runtime->websockets = ws->impl->next;
        if (ws->impl->channel != NULL)
            knurl_channel_finish(ws->impl->channel, KNURL_CANCELLED);
        release(ws);
    }
}

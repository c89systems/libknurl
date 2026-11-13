#define _POSIX_C_SOURCE 200809L
#include "kore_internal.h"
#include "response_internal.h"
#include "channel_internal.h"

#include <stdlib.h>
#include <string.h>

static knurl_status copy_buffer(struct kore_buf *buffer, knurl_owned_bytes *out,
                                knurl_error *error)
{
    out->data = malloc(buffer->offset);
    if (out->data == NULL)
        return knurl_fail(error, KNURL_NOMEM, "response_head",
                          "cannot copy backend output");
    memcpy(out->data, buffer->data, buffer->offset);
    out->length = buffer->offset;
    return KNURL_OK;
}
knurl_status knurl_response_head(knurl_response *r, knurl_owned_bytes *out,
                                 knurl_error *error)
{
    struct http_request *native = r->impl->request->impl->native;
    struct http_response_field *fields;
    struct kore_buf buffer;
    size_t count, i;
    int flags, closed;
    u_int64_t length;
    knurl_status status;
    knurl_owned_bytes_init(out);
    fields =
        malloc((r->impl->headers->impl->header_count + 2) * sizeof(*fields));
    if (fields == NULL)
        return knurl_fail(error, KNURL_NOMEM, "response_head",
                          "cannot allocate header views");
    count = r->impl->headers->impl->header_count;
    for (i = 0; i < count; ++i) {
        fields[i].name = r->impl->headers->impl->headers[i].name.data;
        fields[i].value = r->impl->headers->impl->headers[i].value.data;
    }
    if (r->impl->chunked) {
        fields[count].name = "transfer-encoding";
        fields[count++].value = "chunked";
    }
    if (r->impl->trailer_names != NULL && *r->impl->trailer_names != '\0') {
        fields[count].name = "trailer";
        fields[count++].value = r->impl->trailer_names;
    }
    flags = HTTP_RESPONSE_HEAD_COUNTED;
    if (r->impl->close_connection)
        flags |= HTTP_RESPONSE_HEAD_CLOSE;
    if (r->impl->explicit_length_header ||
        (!r->impl->has_length && r->impl->outcome.status != 205))
        flags |= HTTP_RESPONSE_HEAD_NO_LENGTH;
    if (r->impl->outcome.status == 101)
        flags |= HTTP_RESPONSE_HEAD_UPGRADE;
    length =
        r->impl->outcome.status == 205
            ? 0
            : ((u_int64_t)r->impl->length.high << 32) | r->impl->length.low;
    kore_buf_init(&buffer, 1024);
    closed =
        http_response_head_build(native, native->owner, r->impl->outcome.status,
                                 flags, length, fields, count, &buffer);
    free(fields);
    if (buffer.offset > r->impl->header_bound)
        status = knurl_fail(error, KNURL_LIMIT, "response_head",
                            "response header limit exceeded");
    else
        status = copy_buffer(&buffer, out, error);
    kore_buf_cleanup(&buffer);
    if (status == KNURL_OK) {
        r->impl->close_connection = closed;
        native->status = r->impl->outcome.status;
        native->content_length = r->impl->has_length ? length : 0;
    }
    return status;
}
knurl_status knurl_response_pretty(int code, knurl_owned_bytes *out,
                                   knurl_error *error)
{
    struct kore_buf buffer;
    knurl_status status = KNURL_OK;
    knurl_owned_bytes_init(out);
    if (!http_pretty_error || code < 400)
        return KNURL_OK;
    kore_buf_init(&buffer, 1024);
    if (http_response_error_body(code, &buffer))
        status = copy_buffer(&buffer, out, error);
    kore_buf_cleanup(&buffer);
    return status;
}

int knurl_response_file_metadata(knurl_response *response, time_t modified,
                                 char *stamp, size_t capacity)
{
    return http_response_file_metadata(response->impl->request->impl->native,
                                       modified, stamp, capacity);
}

struct knurl_delivery {
    struct knurl_runtime *runtime;
    knurl_response *response;
    struct knurl_delivery *next;
    struct knurl_delivery *pending_next;
    struct netbuf *buffer;
    int queued;
    int completed;
    int failed;
    u_int64_t last_progress;
    int output_pending;
};

static int pending_output(const struct knurl_delivery *delivery)
{
    const struct knurl_response_segment *head;
    struct http_request *native;
    native = delivery->response->impl->request->impl->native;
    if (!delivery->response->impl->inflight && native->owner != NULL &&
        !TAILQ_EMPTY(&native->owner->send_queue))
        return 0;
    head = delivery->response->impl->head;
    /* A completion callback precedes native buffer removal. Its retained
     * allocation consumes queue capacity, but is no longer stalled output. */
    return head != NULL && (!delivery->completed || head->next != NULL);
}

static void schedule(knurl_response *response)
{
    struct knurl_delivery *delivery;
    struct knurl_runtime *runtime;
    delivery = response->impl->backend;
    if (delivery == NULL)
        return;
    if (response->impl->queued != 0 && !delivery->output_pending) {
        delivery->output_pending = 1;
        delivery->last_progress = kore_time_ms();
    }
    if (delivery->queued)
        return;
    runtime = delivery->runtime;
    delivery->queued = 1;
    delivery->pending_next = NULL;
    if (runtime->response_tail != NULL)
        runtime->response_tail->pending_next = delivery;
    else
        runtime->response_head = delivery;
    runtime->response_tail = delivery;
    ++runtime->response_count;
    if (runtime->server->impl->loop != NULL)
        runtime->server->impl->loop->impl->backend.wake(runtime);
}

knurl_status knurl_kore_response_create(struct knurl_runtime *runtime,
                                        knurl_request *request,
                                        knurl_error *error)
{
    struct knurl_delivery *delivery;
    struct http_request *native;
    knurl_status status;
    delivery = calloc(1, sizeof(*delivery));
    if (delivery == NULL)
        return knurl_fail(error, KNURL_NOMEM, "response",
                          "cannot allocate transport delivery");
    status = knurl_response_create(request, schedule, &request->impl->response,
                                   error);
    if (status != KNURL_OK) {
        free(delivery);
        return status;
    }
    native = request->impl->native;
    delivery->runtime = runtime;
    delivery->last_progress = kore_time_ms();
    delivery->response = request->impl->response;
    request->impl->response->impl->backend = delivery;
    delivery->next = runtime->responses;
    runtime->responses = delivery;
    native->flags |= HTTP_REQUEST_LIBRARY_MANAGED;
    native->start = kore_time_ms();
    http_request_sleep(native);
    native->owner->flags |= CONN_IS_BUSY;
    kore_platform_disable_read(native->owner->fd);
    /* Linux disable_read removes the whole epoll registration. Restore the
     * write edge so a saturated downstream can make progress again. */
    kore_platform_schedule_write(native->owner->fd, native->owner);
    native->owner->evt.flags &= ~KORE_EVENT_READ;
    ++native->owner->http_response_count;
    return KNURL_OK;
}

static int completed(struct netbuf *buffer)
{
    struct knurl_delivery *delivery;
    delivery = buffer->extra;
    delivery->buffer = NULL;
    delivery->completed = 1;
    if (buffer->owner->state == CONN_STATE_DISCONNECTING ||
        buffer->s_off != buffer->b_len)
        delivery->failed = 1;
    /* Native removes the netbuf AFTER calling us. Never free/requeue here. */
    schedule(delivery->response);
    return KORE_RESULT_OK;
}

void knurl_kore_response_progress(void *userdata, struct netbuf *buffer,
                                  size_t amount, int retry)
{
    struct knurl_delivery *delivery;
    (void)userdata;
    knurl_kore_channel_http_progress(buffer->owner, amount);
    if (buffer->cb != completed)
        return;
    delivery = buffer->extra;
    if (amount != 0)
        delivery->last_progress = kore_time_ms();
    knurl_response_sent(delivery->response, buffer->s_off, amount, retry);
}

int knurl_kore_response_release(void *userdata, struct http_request *native)
{
    knurl_request *request;
    (void)userdata;
    request = native->hdlr_extra;
    if (request == NULL || request->impl->response == NULL)
        return 1;
    if (request->impl->response->impl->phase < KNURL_RESPONSE_COMPLETE) {
        request->impl->disconnect_side = KNURL_DISCONNECT_DOWNSTREAM;
        knurl_kore_body_cancel(request, KNURL_IO);
        (void)knurl_response_abort(request->impl->response, KNURL_IO, NULL);
    }
    schedule(request->impl->response);
    return 0;
}

static void terminal(struct knurl_delivery *delivery)
{
    knurl_response *response;
    knurl_request *request;
    struct http_request *native;
    struct connection *connection;
    struct knurl_delivery **slot;
    struct knurl_delivery *previous;
    int close_connection;
    int restore_channel;
    int upgraded;
    response = delivery->response;
    request = response->impl->request;
    native = request->impl->native;
    connection = native->owner;
    close_connection = response->impl->close_connection;
    restore_channel = 0;
    upgraded = request->impl->websocket != NULL &&
               request->impl->channel != NULL &&
               request->impl->channel->impl->phase == KNURL_CHANNEL_TUNNEL &&
               response->impl->phase == KNURL_RESPONSE_COMPLETE &&
               response->impl->outcome.cause == KNURL_OK;
    request->impl->outcome.upgraded = upgraded;
    if (request->impl->channel != NULL && !upgraded) {
        if (request->impl->channel->impl->phase != KNURL_CHANNEL_CLOSED)
            knurl_channel_finish(
                request->impl->channel,
                response->impl->outcome.cause != KNURL_OK
                    ? response->impl->outcome.cause
                    : (request->impl->channel->impl->input_complete
                           ? KNURL_OK
                           : KNURL_CANCELLED));
        knurl_channel_notify_closed(request->impl->channel);
        restore_channel =
            request->impl->channel->impl->previous_phase ==
                KNURL_CHANNEL_HTTP_INPUT &&
            request->impl->channel->impl->input_complete &&
            !request->impl->channel->impl->outcome.read_eof &&
            request->impl->channel->impl->outcome.cause == KNURL_OK &&
            !close_connection;
        if (!restore_channel)
            close_connection = 1;
    }
    /* End/abort accepted during this turn may have queued a later turn. */
    if (delivery->queued) {
        previous = NULL;
        slot = &delivery->runtime->response_head;
        while (*slot != delivery) {
            previous = *slot;
            slot = &(*slot)->pending_next;
        }
        *slot = delivery->pending_next;
        if (delivery->runtime->response_tail == delivery)
            delivery->runtime->response_tail = previous;
        --delivery->runtime->response_count;
        delivery->queued = 0;
    }
    native->status = response->impl->outcome.status;
    native->end = kore_time_ms();
    native->total = native->end - native->start;
    native->ms = native->total;
    knurl_response_terminal(response);
    native->content_length =
        ((u_int64_t)response->impl->outcome.body_bytes.high << 32) |
        response->impl->outcome.body_bytes.low;
    knurl_kore_request_closed(request);
    knurl_kore_websocket_handshake(request, upgraded);
    knurl_channel_release(request->impl->channel, restore_channel);
    request->impl->channel = NULL;
    if (native->rt->dom->accesslog || kore_accesslog_enabled())
        kore_accesslog_snapshot(native, request->impl->peer.address.data);
    native->flags &= ~HTTP_REQUEST_LIBRARY_MANAGED;
    http_request_wakeup(native);
    slot = &delivery->runtime->responses;
    while (*slot != delivery)
        slot = &(*slot)->next;
    *slot = delivery->next;
    response->impl->backend = NULL;
    free(delivery);
    http_request_free(native);
    if (connection != NULL && connection->state == CONN_STATE_ESTABLISHED) {
        connection->flags &= ~CONN_IS_BUSY;
        if (upgraded)
            return;
        if (close_connection)
            kore_connection_disconnect(connection);
        else {
            kore_platform_event_all(connection->fd, connection);
            http_start_recv(connection);
        }
    }
}

static void advance(struct knurl_delivery *delivery)
{
    knurl_response *response;
    knurl_request *request;
    struct http_request *native;
    struct connection *connection;
    struct netbuf *buffer;
    response = delivery->response;
    request = response->impl->request;
    native = response->impl->request->impl->native;
    connection = native->owner;
    if (delivery->completed) {
        delivery->completed = 0;
        response->impl->inflight = 0;
        knurl_response_shift(response);
    }
    if ((delivery->failed || connection == NULL ||
         connection->state == CONN_STATE_DISCONNECTING) &&
        response->impl->phase < KNURL_RESPONSE_COMPLETE) {
        request->impl->disconnect_side = KNURL_DISCONNECT_DOWNSTREAM;
        (void)knurl_response_abort(
            response,
            !request->impl->outcome.body_complete &&
                    request->impl->channel == NULL &&
                    (connection == NULL ||
                     connection->state == CONN_STATE_DISCONNECTING)
                ? KNURL_IO
                : KNURL_CANCELLED,
            NULL);
    }
    if (response->impl->phase == KNURL_RESPONSE_COMPLETE &&
        (connection == NULL || connection->state == CONN_STATE_DISCONNECTING)) {
        if (response->impl->head == NULL &&
            response->impl->pending_head == NULL) {
            terminal(delivery);
            return;
        }
        /* A tunnel can finish before its queued 101 drains. A released
         * native connection cannot deliver that remaining handshake. */
        response->impl->outcome.cause = KNURL_CANCELLED;
        response->impl->phase = KNURL_RESPONSE_ABORTED;
    }
    if (response->impl->phase == KNURL_RESPONSE_ABORTED) {
        knurl_channel_finish(request->impl->channel,
                             response->impl->outcome.cause);
        knurl_kore_body_cancel(response->impl->request,
                               response->impl->outcome.cause);
        if (delivery->buffer != NULL) {
            if (connection != NULL)
                kore_connection_disconnect(connection);
            return;
        }
        terminal(delivery);
        return;
    }
    if (request->impl->channel != NULL &&
        request->impl->channel->impl->phase == KNURL_CHANNEL_CLOSED &&
        !TAILQ_EMPTY(&connection->send_queue)) {
        if (!knurl_kore_channel_flush_http(request)) {
            (void)knurl_response_abort(response, KNURL_IO, NULL);
            kore_connection_disconnect(connection);
            schedule(response);
            return;
        }
    }
    if (request->impl->channel != NULL)
        knurl_channel_turn(request->impl->channel);
    else
        knurl_kore_body_step(response->impl->request);
    knurl_response_step(response);
    if (response->impl->phase == KNURL_RESPONSE_ABORTED) {
        schedule(response);
        return;
    }
    if (request->impl->websocket != NULL &&
        response->impl->phase == KNURL_RESPONSE_FINISHING &&
        response->impl->outcome.status == 101 && response->impl->head == NULL &&
        response->impl->pending_head == NULL && !response->impl->inflight &&
        TAILQ_EMPTY(&connection->send_queue) &&
        request->impl->channel->impl->phase == KNURL_CHANNEL_TUNNEL)
        response->impl->phase = KNURL_RESPONSE_COMPLETE;
    if (((response->impl->phase == KNURL_RESPONSE_FINISHING &&
          (request->impl->channel == NULL ||
           request->impl->channel->impl->phase != KNURL_CHANNEL_TUNNEL)) ||
         (response->impl->phase == KNURL_RESPONSE_COMPLETE &&
          request->impl->channel != NULL)) &&
        response->impl->head == NULL && response->impl->pending_head == NULL) {
        terminal(delivery);
        return;
    }
    if (response->impl->head == NULL || response->impl->inflight) {
        if (response->impl->head == NULL)
            delivery->output_pending = 0;
        return;
    }
    if (request->impl->channel != NULL && !TAILQ_EMPTY(&connection->send_queue))
        return;
    if (!response->impl->outcome.committed)
        delivery->last_progress = kore_time_ms();
    net_send_stream(connection, response->impl->head->data,
                    response->impl->head->length, completed, &buffer);
    buffer->extra = delivery;
    delivery->buffer = buffer;
    response->impl->inflight = 1;
    if (!(request->impl->channel != NULL
              ? knurl_kore_channel_flush_http(request)
              : net_send_flush(connection))) {
        delivery->failed = 1;
        kore_connection_disconnect(connection);
    }
}

void knurl_kore_responses_turn(struct knurl_runtime *runtime)
{
    struct knurl_delivery *delivery;
    size_t budget;
    u_int64_t now;
    knurl_deadline timeout;
    for (delivery = runtime->responses; delivery != NULL;
         delivery = delivery->next) {
        knurl_kore_body_timeout(delivery->response->impl->request);
        knurl_channel_check_timeout(
            delivery->response->impl->request->impl->channel);
        /* Timeout callbacks can queue output and refresh last_progress. */
        now = kore_time_ms();
        timeout =
            pending_output(delivery)
                ? runtime->server->impl->config.timeouts.response_write_idle
                : runtime->server->impl->config.timeouts.deferred_response;
        if (!timeout.disabled &&
            ((delivery->response->impl->phase == KNURL_RESPONSE_BUILDING &&
              delivery->response->impl->request->impl->outcome.body_complete) ||
             pending_output(delivery)) &&
            delivery->response->impl->phase < KNURL_RESPONSE_COMPLETE &&
            now - delivery->last_progress >= (u_int64_t)timeout.milliseconds) {
            delivery->last_progress = now;
            knurl_kore_request_fail(delivery->response->impl->request,
                                    KNURL_TIMEOUT, 500);
        }
    }
    budget = runtime->response_count;
    if (budget > 64)
        budget = 64;
    while (budget-- != 0 && runtime->response_head != NULL) {
        delivery = runtime->response_head;
        runtime->response_head = delivery->pending_next;
        if (runtime->response_head == NULL)
            runtime->response_tail = NULL;
        delivery->queued = 0;
        --runtime->response_count;
        advance(delivery);
    }
}

u_int64_t knurl_kore_responses_wait(struct knurl_runtime *runtime,
                                    u_int64_t maximum)
{
    struct knurl_delivery *delivery;
    knurl_deadline timeout;
    u_int64_t now;
    u_int64_t elapsed;
    u_int64_t wait;
    now = kore_time_ms();
    for (delivery = runtime->responses; delivery != NULL;
         delivery = delivery->next) {
        maximum =
            knurl_kore_body_wait(delivery->response->impl->request, maximum);
        if (delivery->response->impl->request->impl->channel != NULL &&
            (u_int64_t)knurl_channel_wait(
                delivery->response->impl->request->impl->channel) < maximum)
            maximum = (u_int64_t)knurl_channel_wait(
                delivery->response->impl->request->impl->channel);
        timeout =
            pending_output(delivery)
                ? runtime->server->impl->config.timeouts.response_write_idle
                : runtime->server->impl->config.timeouts.deferred_response;
        if (timeout.disabled ||
            (!pending_output(delivery) &&
             (delivery->response->impl->phase != KNURL_RESPONSE_BUILDING ||
              !delivery->response->impl->request->impl->outcome
                   .body_complete)) ||
            delivery->response->impl->phase >= KNURL_RESPONSE_COMPLETE)
            continue;
        elapsed = now - delivery->last_progress;
        wait = elapsed >= (u_int64_t)timeout.milliseconds
                   ? 0
                   : (u_int64_t)timeout.milliseconds - elapsed;
        if (wait < maximum)
            maximum = wait;
    }
    return maximum;
}

void knurl_kore_responses_shutdown(struct knurl_runtime *runtime)
{
    struct knurl_delivery *delivery;
    /* Connection cleanup has already removed all netbufs and detached owners.
     */
    runtime->response_head = runtime->response_tail = NULL;
    runtime->response_count = 0;
    while (runtime->responses != NULL) {
        delivery = runtime->responses;
        delivery->queued = 0;
        delivery->response->impl->outcome.cause = KNURL_CANCELLED;
        delivery->response->impl->phase = KNURL_RESPONSE_ABORTED;
        knurl_channel_finish(delivery->response->impl->request->impl->channel,
                             KNURL_CANCELLED);
        knurl_kore_body_cancel(delivery->response->impl->request,
                               KNURL_CANCELLED);
        terminal(delivery);
    }
}

void knurl_kore_channels_stopping(struct knurl_runtime *runtime)
{
    struct knurl_delivery *delivery;
    for (delivery = runtime->responses; delivery != NULL;
         delivery = delivery->next)
        knurl_channel_finish(delivery->response->impl->request->impl->channel,
                             KNURL_CANCELLED);
}

void knurl_kore_response_body_end(knurl_request *request)
{
    struct knurl_delivery *delivery;
    delivery = request->impl->response->impl->backend;
    if (request->impl->response->impl->phase == KNURL_RESPONSE_BUILDING)
        delivery->last_progress = kore_time_ms();
}

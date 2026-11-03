#define _POSIX_C_SOURCE 200809L
#include "kore_internal.h"
#include "response_internal.h"

#include <stdlib.h>
#include <string.h>

struct rotmos_delivery {
    struct rotmos_runtime *runtime;
    rotmos_response *response;
    struct rotmos_delivery *next;
    struct rotmos_delivery *pending_next;
    struct netbuf *buffer;
    int queued;
    int completed;
    int failed;
    u_int64_t last_progress;
    int output_pending;
};

static int pending_output(const struct rotmos_delivery *delivery)
{
    const struct rotmos_response_segment *head;
    head = delivery->response->head;
    /* A completion callback precedes native buffer removal. Its retained
     * allocation consumes queue capacity, but is no longer stalled output. */
    return head != NULL && (!delivery->completed || head->next != NULL);
}

static void schedule(rotmos_response *response)
{
    struct rotmos_delivery *delivery;
    struct rotmos_runtime *runtime;
    delivery = response->backend;
    if (delivery == NULL)
        return;
    if (response->queued != 0 && !delivery->output_pending) {
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
    if (runtime->server->loop != NULL)
        runtime->server->loop->backend.wake(runtime);
}

rotmos_status rotmos_kore_response_create(struct rotmos_runtime *runtime,
                                          rotmos_request *request,
                                          rotmos_error *error)
{
    struct rotmos_delivery *delivery;
    struct http_request *native;
    rotmos_status status;
    rotmos_bytes value;
    size_t i;
    const char *text;
    delivery = calloc(1, sizeof(*delivery));
    if (delivery == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "response",
                           "cannot allocate transport delivery");
    status =
        rotmos_response_create(request, schedule, &request->response, error);
    if (status != ROTMOS_OK) {
        free(delivery);
        return status;
    }
    native = request->native;
    delivery->runtime = runtime;
    delivery->last_progress = kore_time_ms();
    delivery->response = request->response;
    request->response->backend = delivery;
    delivery->next = runtime->responses;
    runtime->responses = delivery;
    native->flags |= HTTP_REQUEST_LIBRARY_MANAGED;
    native->owner->flags |= CONN_IS_BUSY;
    kore_platform_disable_read(native->owner->fd);
    /* Linux disable_read removes the whole epoll registration. Restore the
     * write edge so a saturated downstream can make progress again. */
    kore_platform_schedule_write(native->owner->fd, native->owner);
    native->owner->evt.flags &= ~KORE_EVENT_READ;
    ++native->owner->http_response_count;
    request->response->close_connection =
        !http_keepalive_time ||
        (http_keepalive_max_requests > 0 &&
         native->owner->http_response_count >= http_keepalive_max_requests);
    for (i = 0; i < request->header_count; ++i) {
        if (!rotmos_text_equal_case(request->headers[i].name.data,
                                    "connection"))
            continue;
        value = request->headers[i].value;
        text = value.data;
        while (value.length != 0) {
            size_t count;
            while (value.length &&
                   (*text == ' ' || *text == '\t' || *text == ',')) {
                ++text;
                --value.length;
            }
            count = 0;
            while (count < value.length && text[count] != ',')
                ++count;
            if (count >= 5 && (text[0] == 'c' || text[0] == 'C') &&
                (text[1] == 'l' || text[1] == 'L') &&
                (text[2] == 'o' || text[2] == 'O') &&
                (text[3] == 's' || text[3] == 'S') &&
                (text[4] == 'e' || text[4] == 'E') &&
                (count == 5 || text[5] == ' ' || text[5] == '\t'))
                request->response->close_connection = 1;
            text += count;
            value.length -= count;
        }
    }
    return ROTMOS_OK;
}

static int completed(struct netbuf *buffer)
{
    struct rotmos_delivery *delivery;
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

void rotmos_kore_response_progress(void *userdata, struct netbuf *buffer,
                                   size_t amount, int retry)
{
    struct rotmos_delivery *delivery;
    (void)userdata;
    if (buffer->cb != completed)
        return;
    delivery = buffer->extra;
    if (amount != 0)
        delivery->last_progress = kore_time_ms();
    rotmos_response_sent(delivery->response, buffer->s_off, amount, retry);
}

int rotmos_kore_response_release(void *userdata, struct http_request *native)
{
    rotmos_request *request;
    (void)userdata;
    request = native->hdlr_extra;
    if (request == NULL || request->response == NULL)
        return 1;
    if (request->response->phase < ROTMOS_RESPONSE_COMPLETE)
        (void)rotmos_response_abort(request->response, ROTMOS_CANCELLED, NULL);
    schedule(request->response);
    return 0;
}

static void terminal(struct rotmos_delivery *delivery)
{
    rotmos_response *response;
    rotmos_request *request;
    struct http_request *native;
    struct connection *connection;
    struct rotmos_delivery **slot;
    struct rotmos_delivery *previous;
    int close_connection;
    response = delivery->response;
    request = response->request;
    native = request->native;
    connection = native->owner;
    close_connection = response->close_connection;
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
    rotmos_response_terminal(response);
    native->status = response->outcome.status;
    if (native->rt->dom->accesslog || kore_accesslog_enabled())
        kore_accesslog(native);
    native->flags &= ~HTTP_REQUEST_LIBRARY_MANAGED;
    http_request_wakeup(native);
    slot = &delivery->runtime->responses;
    while (*slot != delivery)
        slot = &(*slot)->next;
    *slot = delivery->next;
    response->backend = NULL;
    free(delivery);
    http_request_free(native);
    if (connection != NULL && connection->state == CONN_STATE_ESTABLISHED) {
        connection->flags &= ~CONN_IS_BUSY;
        if (close_connection)
            kore_connection_disconnect(connection);
        else {
            kore_platform_event_all(connection->fd, connection);
            http_start_recv(connection);
        }
    }
}

static void advance(struct rotmos_delivery *delivery)
{
    rotmos_response *response;
    struct http_request *native;
    struct connection *connection;
    struct netbuf *buffer;
    response = delivery->response;
    native = response->request->native;
    connection = native->owner;
    if (delivery->completed) {
        delivery->completed = 0;
        response->inflight = 0;
        rotmos_response_shift(response);
    }
    if ((delivery->failed || connection == NULL ||
         connection->state == CONN_STATE_DISCONNECTING) &&
        response->phase < ROTMOS_RESPONSE_COMPLETE)
        (void)rotmos_response_abort(response, ROTMOS_CANCELLED, NULL);
    if (response->phase == ROTMOS_RESPONSE_ABORTED) {
        if (delivery->buffer != NULL) {
            if (connection != NULL)
                kore_connection_disconnect(connection);
            return;
        }
        terminal(delivery);
        return;
    }
    rotmos_response_step(response);
    if (response->phase == ROTMOS_RESPONSE_ABORTED) {
        schedule(response);
        return;
    }
    if (response->phase == ROTMOS_RESPONSE_FINISHING &&
        response->head == NULL) {
        terminal(delivery);
        return;
    }
    if (response->head == NULL || response->inflight) {
        if (response->head == NULL)
            delivery->output_pending = 0;
        return;
    }
    net_send_stream(connection, response->head->data, response->head->length,
                    completed, &buffer);
    buffer->extra = delivery;
    delivery->buffer = buffer;
    response->inflight = 1;
    if (!net_send_flush(connection)) {
        delivery->failed = 1;
        kore_connection_disconnect(connection);
    }
}

void rotmos_kore_responses_turn(struct rotmos_runtime *runtime)
{
    struct rotmos_delivery *delivery;
    size_t budget;
    u_int64_t now;
    rotmos_deadline timeout;
    now = kore_time_ms();
    for (delivery = runtime->responses; delivery != NULL;
         delivery = delivery->next) {
        timeout = delivery->response->phase == ROTMOS_RESPONSE_BUILDING
                      ? runtime->server->config.timeouts.deferred_response
                      : runtime->server->config.timeouts.response_write_idle;
        if (!timeout.disabled &&
            (delivery->response->phase == ROTMOS_RESPONSE_BUILDING ||
             pending_output(delivery)) &&
            delivery->response->phase < ROTMOS_RESPONSE_COMPLETE &&
            now - delivery->last_progress >= (u_int64_t)timeout.milliseconds) {
            delivery->last_progress = now;
            rotmos_response_fail(delivery->response, ROTMOS_TIMEOUT);
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

u_int64_t rotmos_kore_responses_wait(struct rotmos_runtime *runtime,
                                     u_int64_t maximum)
{
    struct rotmos_delivery *delivery;
    rotmos_deadline timeout;
    u_int64_t now;
    u_int64_t elapsed;
    u_int64_t wait;
    now = kore_time_ms();
    for (delivery = runtime->responses; delivery != NULL;
         delivery = delivery->next) {
        timeout = delivery->response->phase == ROTMOS_RESPONSE_BUILDING
                      ? runtime->server->config.timeouts.deferred_response
                      : runtime->server->config.timeouts.response_write_idle;
        if (timeout.disabled ||
            (delivery->response->phase != ROTMOS_RESPONSE_BUILDING &&
             !pending_output(delivery)) ||
            delivery->response->phase >= ROTMOS_RESPONSE_COMPLETE)
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

void rotmos_kore_responses_shutdown(struct rotmos_runtime *runtime)
{
    struct rotmos_delivery *delivery;
    /* Connection cleanup has already removed all netbufs and detached owners.
     */
    runtime->response_head = runtime->response_tail = NULL;
    runtime->response_count = 0;
    while (runtime->responses != NULL) {
        delivery = runtime->responses;
        delivery->queued = 0;
        delivery->response->outcome.cause = ROTMOS_CANCELLED;
        delivery->response->phase = ROTMOS_RESPONSE_ABORTED;
        terminal(delivery);
    }
}

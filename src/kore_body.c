#define _POSIX_C_SOURCE 200809L
#include "kore_internal.h"
#include "response_internal.h"
#include "body_internal.h"
#include "policy_internal.h"

#include <stdlib.h>
#include <string.h>

struct knurl_input {
    knurl_request *request;
    const unsigned char *pending;
    size_t pending_length;
    const unsigned char *surplus;
    size_t surplus_length;
    u_int64_t remaining;
    u_int64_t received;
    u_int64_t started;
    u_int64_t paused_at;
    u_int64_t pause_time;
    u_int64_t waiting_since;
    int paused;
    int waiting;
    int dead;
    int end_called;
    int in_callback;
    int resume_requested;
};

static void counter(u_int64_t value, knurl_u64 *out)
{
    out->high = (unsigned long)(value >> 32);
    out->low = (unsigned long)(value & 0xffffffffUL);
}

static void stop_read(knurl_request *request)
{
    struct http_request *native;
    struct connection *connection;
    native = request->native;
    connection = native->owner;
    if (connection == NULL || connection->state != CONN_STATE_ESTABLISHED)
        return;
    if (request->channel != NULL)
        return;
    kore_platform_disable_read(connection->fd);
    kore_platform_schedule_write(connection->fd, connection);
    connection->evt.flags &= ~KORE_EVENT_READ;
    connection->http_timeout = 0;
}

knurl_status knurl_kore_body_create(knurl_request *request, const void *data,
                                    size_t length, knurl_error *error)
{
    struct knurl_input *input;
    struct http_request *native;
    size_t available;
    input = calloc(1, sizeof(*input));
    if (input == NULL)
        return knurl_fail(error, KNURL_NOMEM, "body_start",
                          "cannot allocate bounded input state");
    input->request = request;
    input->remaining = request->has_length
                           ? ((u_int64_t)request->content_length.high << 32) |
                                 request->content_length.low
                           : 0;
    available = length;
    if ((u_int64_t)available > input->remaining)
        available = (size_t)input->remaining;
    input->pending = data;
    input->pending_length = available;
    input->remaining -= available;
    input->received = available;
    input->surplus =
        length > available ? (const unsigned char *)data + available : NULL;
    input->surplus_length = length - available;
    input->started = kore_time_ms();
    counter(input->received, &request->outcome.received_body_bytes);
    request->input = input;
    native = request->native;
    native->owner->http_timeout = 0;
    return KNURL_OK;
}

void knurl_kore_body_cancel(knurl_request *request, knurl_status cause)
{
    struct knurl_input *input;
    input = request->input;
    if (input != NULL) {
        input->dead = 1;
        input->waiting = input->paused = 0;
        input->pending = NULL;
        input->pending_length = 0;
    }
    if (!request->outcome.body_complete) {
        request->outcome.body_cause = cause;
        if (request->body_store != NULL)
            request->body_store->failure = cause;
    }
    stop_read(request);
}

void knurl_kore_request_fail(knurl_request *request, knurl_status cause,
                             int code)
{
    knurl_kore_request_fail_message(request, cause, code, NULL);
}
void knurl_kore_request_fail_message(knurl_request *request, knurl_status cause,
                                     int code, const char *message)
{
    if (cause <= KNURL_EOF || cause > KNURL_BACKEND)
        cause = KNURL_INVALID;
    if (request->outcome.cause == KNURL_OK)
        request->outcome.cause = cause;
    knurl_kore_body_cancel(request, cause);
    request->response->close_connection = 1;
    if (request->response->phase == KNURL_RESPONSE_FINISHING &&
        request->response->outcome.status >= 400)
        return;
    knurl_response_error_message(request->response, cause, code, message);
}

static int received(struct netbuf *buffer)
{
    struct knurl_input *input;
    knurl_request *request;
    if (buffer->s_off == 0)
        return KORE_RESULT_OK;
    input = buffer->extra;
    request = input->request;
    input->pending = buffer->buf;
    input->pending_length = buffer->s_off;
    input->remaining -= buffer->s_off;
    input->received += buffer->s_off;
    counter(input->received, &request->outcome.received_body_bytes);
    input->waiting = 0;
    stop_read(request);
    request->response->schedule(request->response);
    return KORE_RESULT_OK;
}

static void arm_read(struct knurl_input *input)
{
    struct connection *connection;
    struct http_request *native;
    knurl_request *request;
    size_t capacity;
    request = input->request;
    native = request->native;
    connection = native->owner;
    capacity = request->server->config.io.chunk_bytes;
    if (capacity > request->server->config.io.input_queue_bytes)
        capacity = request->server->config.io.input_queue_bytes;
    if ((u_int64_t)capacity > input->remaining)
        capacity = (size_t)input->remaining;
    net_recv_reset(connection, capacity, received);
    connection->rnb->extra = input;
    connection->rnb->flags |= NETBUF_CALL_CB_ALWAYS;
    if (connection->http_pipeline_len != 0) {
        connection->rnb->s_off = kore_library_http_pipeline_read(
            connection, connection->rnb->buf, capacity);
        (void)received(connection->rnb);
        return;
    }
    input->waiting = 1;
    input->waiting_since = kore_time_ms();
    kore_platform_event_all(connection->fd, connection);
}

static u_int64_t rate_wait(struct knurl_input *input)
{
    const knurl_server_config *config;
    u_int64_t elapsed;
    u_int64_t rate;
    u_int64_t seconds, deadline;
    config = &input->request->server->config;
    if (config->limits.body_min_rate_disabled)
        return KORE_WAIT_INFINITE;
    rate =
        ((u_int64_t)config->limits.body_min_rate_bytes_per_second.high << 32) |
        config->limits.body_min_rate_bytes_per_second.low;
    if (rate == 0)
        return KORE_WAIT_INFINITE;
    /* received / (elapsed / 1000) first falls below rate at this second.
     * Divide before multiplying so even 64-bit byte/rate values are safe. */
    seconds = input->received / rate;
    if (seconds >= (u_int64_t)-1 / 1000)
        return KORE_WAIT_INFINITE;
    deadline = (seconds + 1) * 1000;
    if (deadline < (u_int64_t)config->timeouts.body_min_rate_grace.milliseconds)
        deadline = (u_int64_t)config->timeouts.body_min_rate_grace.milliseconds;
    elapsed = kore_time_ms() - input->started - input->pause_time;
    return elapsed >= deadline ? 0 : deadline - elapsed;
}

static void finish_metadata(knurl_request *request, struct knurl_input *input)
{
    struct http_request *native;
    request->outcome.body_complete = 1;
    request->outcome.body_cause = KNURL_OK;
    request->body_store->complete = 1;
    knurl_kore_response_body_end(request);
    request->body_ready =
        request->body_store->policy.mode != KNURL_BODY_STREAM &&
        request->body_store->fd == -1;
    if (request->body_ready)
        (void)knurl_body_bytes(request->body_store, &request->body, NULL);
    native = request->native;
    native->content_length = input->received;
    native->http_body_length = input->received;
    if (native->owner != NULL) {
        native->owner->rnb->extra = NULL;
        if (input->surplus_length != 0)
            kore_library_http_pipeline_save(native->owner, input->surplus,
                                            input->surplus_length);
    }
    input->surplus = NULL;
    input->surplus_length = 0;
}

knurl_status knurl_kore_body_empty_reply(knurl_request *request,
                                         knurl_error *error)
{
    knurl_body_policy policy;
    knurl_status status;
    knurl_body_policy_init(&policy);
    status = knurl_body_create(&policy, NULL, &request->body_store, error);
    if (status != KNURL_OK)
        return status;
    finish_metadata(request, request->input);
    return KNURL_OK;
}

void knurl_kore_body_step(knurl_request *request)
{
    struct knurl_input *input;
    knurl_status status;
    knurl_error error;
    size_t count;
    size_t consumed;
    input = request->input;
    if (input == NULL || input->dead || request->outcome.body_complete)
        return;
    if (request->response->phase >= KNURL_RESPONSE_FINISHING) {
        knurl_kore_body_cancel(request, KNURL_CANCELLED);
        request->response->close_connection = 1;
        return;
    }
    if (input->paused || input->waiting)
        return;
    if (rate_wait(input) == 0) {
        knurl_kore_request_fail(request, KNURL_TIMEOUT, 408);
        return;
    }
    if (input->pending_length != 0) {
        count = input->pending_length;
        if (count > request->server->config.io.chunk_bytes)
            count = request->server->config.io.chunk_bytes;
        if (count > request->server->config.io.input_queue_bytes)
            count = request->server->config.io.input_queue_bytes;
        consumed = count;
        knurl_error_clear(&error);
        if (request->plan.body.mode == KNURL_BODY_STREAM) {
            consumed = 0;
            input->in_callback = 1;
            status =
                request->plan.on_body(request, input->pending, count, &consumed,
                                      request->plan.userdata, &error);
            input->in_callback = 0;
            if ((status != KNURL_OK && status != KNURL_AGAIN) ||
                consumed > count || (status == KNURL_AGAIN && consumed != 0)) {
                knurl_kore_request_fail(request,
                                        status > KNURL_EOF &&
                                                status <= KNURL_BACKEND
                                            ? status
                                            : KNURL_INVALID,
                                        500);
                return;
            }
        }
        status = knurl_body_append(request->body_store, input->pending,
                                   consumed, &error);
        if (status != KNURL_OK) {
            knurl_kore_request_fail_message(request, status, 413,
                                            "failed to stream request body");
            return;
        }
        input->pending += consumed;
        input->pending_length -= consumed;
        request->outcome.delivered_body_bytes = request->body_store->length;
        if (consumed < count) {
            input->paused = !input->resume_requested;
            input->resume_requested = 0;
            input->paused_at = kore_time_ms();
            return;
        }
        input->resume_requested = 0;
        if (request->response->phase >= KNURL_RESPONSE_FINISHING) {
            request->response->schedule(request->response);
            return;
        }
        if (input->pending_length != 0) {
            request->response->schedule(request->response);
            return;
        }
    }
    if (input->remaining != 0) {
        arm_read(input);
        return;
    }
    input->end_called = 1;
    finish_metadata(request, input);
    if (request->plan.on_body_end != NULL) {
        knurl_error_clear(&error);
        status =
            request->plan.on_body_end(request, request->plan.userdata, &error);
        if (status != KNURL_OK)
            knurl_kore_request_fail(request, status, 500);
    }
}

void knurl_kore_body_initial(knurl_request *request, knurl_bytes *out)
{
    struct knurl_input *input;
    input = request->input;
    out->data = input->pending_length != 0 ? input->pending : input->surplus;
    out->length = input->pending_length + input->surplus_length;
}

void knurl_kore_body_transferred(knurl_request *request)
{
    struct knurl_input *input;
    input = request->input;
    input->dead = 1;
    input->waiting = input->paused = 0;
    input->pending = input->surplus = NULL;
    input->pending_length = input->surplus_length = 0;
}

knurl_status knurl_request_body_resume(knurl_request *request,
                                       knurl_error *error)
{
    struct knurl_input *input;
    knurl_status status;
    if (request == NULL)
        return knurl_fail(error, KNURL_INVALID, "body_resume",
                          "request is required");
    status = knurl_loop_owner(request->server->loop, error);
    if (status != KNURL_OK)
        return status;
    input = request->input;
    if (request->close_notified || request->observing || input == NULL ||
        input->dead || request->outcome.body_complete ||
        request->plan.body.mode != KNURL_BODY_STREAM)
        return knurl_fail(error, KNURL_STATE, "body_resume",
                          "request has no live streaming body");
    if (input->paused) {
        input->pause_time += kore_time_ms() - input->paused_at;
        input->paused = 0;
    }
    if (input->in_callback)
        input->resume_requested = 1;
    request->response->schedule(request->response);
    knurl_error_clear(error);
    return KNURL_OK;
}

u_int64_t knurl_kore_body_wait(knurl_request *request, u_int64_t maximum)
{
    struct knurl_input *input;
    knurl_deadline timeout;
    u_int64_t elapsed;
    u_int64_t wait;
    input = request->input;
    timeout = request->server->config.timeouts.request_body_idle;
    if (input == NULL || !input->waiting || input->dead)
        return maximum;
    if (!timeout.disabled) {
        elapsed = kore_time_ms() - input->waiting_since;
        wait = elapsed >= (u_int64_t)timeout.milliseconds
                   ? 0
                   : (u_int64_t)timeout.milliseconds - elapsed;
        if (wait < maximum)
            maximum = wait;
    }
    wait = rate_wait(input);
    if (wait < maximum)
        maximum = wait;
    return maximum;
}

void knurl_kore_body_timeout(knurl_request *request)
{
    struct knurl_input *input;
    input = request->input;
    if (input != NULL && input->waiting &&
        knurl_kore_body_wait(request, KORE_WAIT_INFINITE) == 0)
        knurl_kore_request_fail(request, KNURL_TIMEOUT, 408);
}

void knurl_kore_request_closed(knurl_request *request)
{
    if (request->close_notified)
        return;
    request->close_notified = 1;
    if (!request->outcome.body_complete &&
        request->outcome.body_cause == KNURL_OK)
        request->outcome.body_cause = KNURL_CANCELLED;
    request->outcome.response = request->response->outcome;
    if (request->outcome.cause == KNURL_OK)
        request->outcome.cause = request->response->outcome.cause;
    if (request->dispatch_started && request->plan.on_close != NULL)
        request->plan.on_close(request, &request->outcome,
                               request->plan.userdata);
    knurl_policy_closed(request);
    free(request->input);
    request->input = NULL;
}

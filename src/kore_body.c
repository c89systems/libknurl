#define _POSIX_C_SOURCE 200809L
#include "kore_internal.h"
#include "response_internal.h"
#include "body_internal.h"
#include "policy_internal.h"
#include "http_framing.h"

#include <stdlib.h>
#include <string.h>

struct knurl_input {
    knurl_request *request;
    const unsigned char *pending;
    size_t pending_length;
    const unsigned char *surplus;
    size_t surplus_length;
    knurl_http_body_framer framer;
    knurl_http_body_framer *received_framer;
    u_int64_t receive_remaining;
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
    knurl_status failure;
    int failure_code;
};

static void counter(u_int64_t value, knurl_u64 *out)
{
    out->high = (unsigned long)(value >> 32);
    out->low = (unsigned long)(value & 0xffffffffUL);
}

static size_t count_payload(void *data, const unsigned char *bytes,
                            size_t count)
{
    struct knurl_input *input = data;
    (void)bytes;
    if ((u_int64_t)count > (u_int64_t)-1 - input->received) {
        input->failure = KNURL_LIMIT;
        input->failure_code = 413;
        return 0;
    }
    input->received += count;
    counter(input->received,
            &input->request->impl->outcome.received_body_bytes);
    return count;
}

static int count_trailer(void *data, const char *name, const char *value)
{
    (void)data;
    (void)name;
    (void)value;
    return 1;
}

/* Observe each new transport span once, independently of consumer retries.
 * Chunked input needs only a second bounded framing cursor, never payload
 * storage. Fixed input uses a scalar remainder and excludes pipeline bytes. */
static void count_received(struct knurl_input *input, const void *bytes,
                           size_t length)
{
    size_t consumed;
    if (input->received_framer != NULL) {
        (void)knurl_http_body_framer_feed(input->received_framer, bytes, length,
                                          &consumed, count_payload,
                                          count_trailer, input);
    } else {
        if ((u_int64_t)length > input->receive_remaining)
            length = (size_t)input->receive_remaining;
        (void)count_payload(input, bytes, length);
        input->receive_remaining -= length;
    }
}

static void stop_read(knurl_request *request)
{
    struct http_request *native;
    struct connection *connection;
    native = request->impl->native;
    connection = native->owner;
    if (connection == NULL || connection->state != CONN_STATE_ESTABLISHED)
        return;
    if (request->impl->channel != NULL)
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
    if (request->impl->has_coding) {
        input->received_framer = malloc(sizeof(*input->received_framer));
        if (input->received_framer == NULL) {
            free(input);
            return knurl_fail(error, KNURL_NOMEM, "body_start",
                              "cannot allocate bounded receive framing");
        }
        knurl_http_body_framer_chunked(&input->framer);
        knurl_http_body_framer_chunked(input->received_framer);
    } else {
        knurl_http_body_framer_fixed(
            &input->framer,
            request->impl->has_length
                ? ((u_int64_t)request->impl->content_length.high << 32) |
                      request->impl->content_length.low
                : 0);
        input->receive_remaining = input->framer.remaining;
    }
    available = length;
    if (!request->impl->has_coding &&
        (u_int64_t)available > input->framer.remaining)
        available = (size_t)input->framer.remaining;
    input->pending = data;
    input->pending_length = available;
    input->surplus =
        length > available ? (const unsigned char *)data + available : NULL;
    input->surplus_length = length - available;
    input->started = kore_time_ms();
    counter(input->received, &request->impl->outcome.received_body_bytes);
    request->impl->input = input;
    count_received(input, data, available);
    native = request->impl->native;
    native->owner->http_timeout = 0;
    return KNURL_OK;
}

void knurl_kore_body_cancel(knurl_request *request, knurl_status cause)
{
    struct knurl_input *input;
    input = request->impl->input;
    if (input != NULL) {
        input->dead = 1;
        input->waiting = input->paused = 0;
        input->pending = NULL;
        input->pending_length = 0;
    }
    if (!request->impl->outcome.body_complete) {
        request->impl->outcome.body_cause = cause;
        if (request->impl->body_store != NULL)
            request->impl->body_store->impl->failure = cause;
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
    if (request->impl->outcome.cause == KNURL_OK)
        request->impl->outcome.cause = cause;
    knurl_kore_body_cancel(request, cause);
    request->impl->response->impl->close_connection = 1;
    if (request->impl->response->impl->phase == KNURL_RESPONSE_FINISHING &&
        request->impl->response->impl->outcome.status >= 400)
        return;
    knurl_response_error_message(request->impl->response, cause, code, message);
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
    count_received(input, input->pending, input->pending_length);
    input->waiting = 0;
    stop_read(request);
    request->impl->response->impl->schedule(request->impl->response);
    return KORE_RESULT_OK;
}

static void arm_read(struct knurl_input *input)
{
    struct connection *connection;
    struct http_request *native;
    knurl_request *request;
    size_t capacity;
    request = input->request;
    native = request->impl->native;
    connection = native->owner;
    capacity = request->impl->server->impl->config.io.chunk_bytes;
    if (capacity > request->impl->server->impl->config.io.input_queue_bytes)
        capacity = request->impl->server->impl->config.io.input_queue_bytes;
    if (!request->impl->has_coding &&
        (u_int64_t)capacity > input->framer.remaining)
        capacity = (size_t)input->framer.remaining;
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
    config = &input->request->impl->server->impl->config;
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
    request->impl->outcome.body_complete = 1;
    request->impl->outcome.body_cause = KNURL_OK;
    request->impl->body_store->impl->complete = 1;
    knurl_kore_response_body_end(request);
    request->impl->body_ready =
        request->impl->body_store->impl->policy.mode != KNURL_BODY_STREAM &&
        request->impl->body_store->impl->fd == -1;
    if (request->impl->body_ready)
        (void)knurl_body_bytes(request->impl->body_store, &request->impl->body,
                               NULL);
    native = request->impl->native;
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
    status =
        knurl_body_create(&policy, NULL, &request->impl->body_store, error);
    if (status != KNURL_OK)
        return status;
    finish_metadata(request, request->impl->input);
    return KNURL_OK;
}

static size_t accept_payload(void *data, const unsigned char *bytes,
                             size_t count)
{
    struct knurl_input *input = data;
    knurl_request *request = input->request;
    knurl_u64 next, amount;
    knurl_status status;
    knurl_error error;
    size_t consumed;
    int order;
    if (request->impl->response->impl->phase >= KNURL_RESPONSE_FINISHING)
        return 0;
    if (count > request->impl->server->impl->config.io.chunk_bytes)
        count = request->impl->server->impl->config.io.chunk_bytes;
    if (count > request->impl->server->impl->config.io.input_queue_bytes)
        count = request->impl->server->impl->config.io.input_queue_bytes;
    (void)knurl_u64_from_size(count, &amount, NULL);
    status = knurl_u64_add(request->impl->body_store->impl->length, amount,
                           &next, &error);
    order = 1;
    if (status == KNURL_OK)
        (void)knurl_u64_compare(
            next, request->impl->body_store->impl->policy.max_bytes, &order,
            NULL);
    if (status != KNURL_OK || order > 0) {
        input->failure = KNURL_LIMIT;
        input->failure_code = 413;
        return 0;
    }
    consumed = count;
    knurl_error_clear(&error);
    if (request->impl->plan.body.mode == KNURL_BODY_STREAM) {
        consumed = 0;
        input->in_callback = 1;
        status =
            request->impl->plan.on_body(request, bytes, count, &consumed,
                                        request->impl->plan.userdata, &error);
        input->in_callback = 0;
        if ((status != KNURL_OK && status != KNURL_AGAIN) || consumed > count ||
            (status == KNURL_AGAIN && consumed != 0)) {
            input->failure = status > KNURL_EOF && status <= KNURL_BACKEND
                                 ? status
                                 : KNURL_INVALID;
            input->failure_code = 500;
            return 0;
        }
    }
    status =
        knurl_body_append(request->impl->body_store, bytes, consumed, &error);
    if (status != KNURL_OK) {
        input->failure = status;
        input->failure_code = 413;
        return 0;
    }
    request->impl->outcome.delivered_body_bytes =
        request->impl->body_store->impl->length;
    if (consumed < count) {
        input->paused = !input->resume_requested;
        input->paused_at = kore_time_ms();
    }
    input->resume_requested = 0;
    return consumed;
}

static int accept_trailer(void *data, const char *name, const char *value)
{
    struct knurl_input *input = data;
    knurl_request *request = input->request;
    size_t n = strlen(name), v = strlen(value),
           count = request->impl->trailer_count;
    knurl_header *headers;
    char *copy;
    copy = malloc(n + v + 2);
    if (copy == NULL)
        goto nomem;
    headers = realloc(request->impl->trailers, (count + 1) * sizeof(*headers));
    if (headers == NULL) {
        free(copy);
        goto nomem;
    }
    memcpy(copy, name, n + 1);
    memcpy(copy + n + 1, value, v + 1);
    request->impl->trailers = headers;
    headers[count].name = knurl_view(copy);
    headers[count].value = knurl_view(copy + n + 1);
    request->impl->trailer_count = count + 1;
    return 1;
nomem:
    input->failure = KNURL_NOMEM;
    input->failure_code = 500;
    return 0;
}

void knurl_kore_body_step(knurl_request *request)
{
    struct knurl_input *input;
    knurl_status status;
    knurl_error error;
    size_t consumed;
    knurl_http_frame_result framed;
    input = request->impl->input;
    if (input == NULL || input->dead || request->impl->outcome.body_complete)
        return;
    if (request->impl->response->impl->phase >= KNURL_RESPONSE_FINISHING) {
        knurl_kore_body_cancel(request, KNURL_CANCELLED);
        request->impl->response->impl->close_connection = 1;
        return;
    }
    if (input->paused || input->waiting)
        return;
    if (rate_wait(input) == 0) {
        knurl_kore_request_fail(request, KNURL_TIMEOUT, 408);
        return;
    }
    if (input->pending_length != 0) {
        framed = knurl_http_body_framer_feed(
            &input->framer, input->pending, input->pending_length, &consumed,
            accept_payload, accept_trailer, input);
        if (input->failure != KNURL_OK) {
            knurl_kore_request_fail_message(
                request, input->failure, input->failure_code,
                input->failure_code == 413 ? "failed to stream request body"
                                           : NULL);
            return;
        }
        if (framed == KNURL_HTTP_FRAME_INVALID) {
            knurl_kore_request_fail_message(request, KNURL_INVALID, 400,
                                            "malformed request body framing");
            return;
        }
        input->pending += consumed;
        input->pending_length -= consumed;
        if (input->paused)
            return;
        if (request->impl->response->impl->phase >= KNURL_RESPONSE_FINISHING) {
            request->impl->response->impl->schedule(request->impl->response);
            return;
        }
        if (framed == KNURL_HTTP_FRAME_COMPLETE && input->pending_length != 0) {
            input->surplus = input->pending;
            input->surplus_length = input->pending_length;
            input->pending = NULL;
            input->pending_length = 0;
        } else if (input->pending_length != 0) {
            request->impl->response->impl->schedule(request->impl->response);
            return;
        }
    }
    if (input->framer.phase != KNURL_HTTP_PHASE_COMPLETE) {
        arm_read(input);
        return;
    }
    input->end_called = 1;
    finish_metadata(request, input);
    if (request->impl->plan.on_body_end != NULL) {
        knurl_error_clear(&error);
        status = request->impl->plan.on_body_end(
            request, request->impl->plan.userdata, &error);
        if (status != KNURL_OK)
            knurl_kore_request_fail(request, status, 500);
    }
}

void knurl_kore_body_initial(knurl_request *request, knurl_bytes *out)
{
    struct knurl_input *input;
    input = request->impl->input;
    out->data = input->pending_length != 0 ? input->pending : input->surplus;
    out->length = input->pending_length + input->surplus_length;
}

void knurl_kore_body_transferred(knurl_request *request)
{
    struct knurl_input *input;
    input = request->impl->input;
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
    status = knurl_loop_owner(request->impl->server->impl->loop, error);
    if (status != KNURL_OK)
        return status;
    input = request->impl->input;
    if (request->impl->close_notified || request->impl->observing ||
        input == NULL || input->dead || request->impl->outcome.body_complete ||
        request->impl->plan.body.mode != KNURL_BODY_STREAM)
        return knurl_fail(error, KNURL_STATE, "body_resume",
                          "request has no live streaming body");
    if (input->paused) {
        input->pause_time += kore_time_ms() - input->paused_at;
        input->paused = 0;
    }
    if (input->in_callback)
        input->resume_requested = 1;
    request->impl->response->impl->schedule(request->impl->response);
    knurl_error_clear(error);
    return KNURL_OK;
}

u_int64_t knurl_kore_body_wait(knurl_request *request, u_int64_t maximum)
{
    struct knurl_input *input;
    knurl_deadline timeout;
    u_int64_t elapsed;
    u_int64_t wait;
    input = request->impl->input;
    timeout = request->impl->server->impl->config.timeouts.request_body_idle;
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
    input = request->impl->input;
    if (input != NULL && input->waiting &&
        knurl_kore_body_wait(request, KORE_WAIT_INFINITE) == 0)
        knurl_kore_request_fail(request, KNURL_TIMEOUT, 408);
}

void knurl_kore_request_closed(knurl_request *request)
{
    struct knurl_input *input;
    if (request->impl->close_notified)
        return;
    request->impl->close_notified = 1;
    if (!request->impl->outcome.body_complete &&
        request->impl->outcome.body_cause == KNURL_OK)
        request->impl->outcome.body_cause = KNURL_CANCELLED;
    request->impl->outcome.response = request->impl->response->impl->outcome;
    if (request->impl->outcome.cause == KNURL_OK)
        request->impl->outcome.cause =
            request->impl->response->impl->outcome.cause;
    if (request->impl->dispatch_started && request->impl->plan.on_close != NULL)
        request->impl->plan.on_close(request, &request->impl->outcome,
                                     request->impl->plan.userdata);
    knurl_policy_closed(request);
    input = request->impl->input;
    if (input != NULL)
        free(input->received_framer);
    free(input);
    request->impl->input = NULL;
}

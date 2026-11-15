#include "channel_internal.h"
#include <knurl/channel.h>
#include "response_internal.h"
#include "server_internal.h"
#include "loop_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static knurl_status owner(knurl_channel *c, knurl_error *error)
{
    if (c == NULL)
        return knurl_fail(error, KNURL_INVALID, "channel",
                          "channel is required");
    if (c->impl->phase == KNURL_CHANNEL_CLOSED ||
        (c->impl->request != NULL && c->impl->request->impl->close_notified))
        return knurl_fail(error, KNURL_STATE, "channel", "channel is closed");
    return knurl_loop_owner(c->impl->server->impl->loop, error);
}

static void schedule_response(knurl_channel *c)
{
    c->impl->response->impl->schedule(c->impl->response);
}

static knurl_u64 subtract(knurl_u64 a, knurl_u64 b)
{
    knurl_u64 result;
    result.high = a.high - b.high - (a.low < b.low ? 1 : 0);
    result.low = (a.low - b.low) & 0xffffffffUL;
    return result;
}

static int add(knurl_u64 *value, size_t count)
{
    knurl_u64 amount;
    return knurl_u64_from_size(count, &amount, NULL) == KNURL_OK &&
           knurl_u64_add(*value, amount, value, NULL) == KNURL_OK;
}

void knurl_channel_config_init(knurl_channel_config *config)
{
    if (config == NULL)
        return;
    config->receive_bytes = config->send_bytes = config->tail_bytes = 0;
    config->shutdown_timeout_ms = 0;
    config->interest = 0;
    config->on_event = NULL;
    config->on_close = NULL;
    config->userdata = NULL;
}

knurl_status knurl_channel_create(knurl_request *request,
                                  const knurl_channel_config *config,
                                  knurl_bytes initial, knurl_bytes backlog,
                                  knurl_channel **out, knurl_error *error)
{
    knurl_channel *c;
    knurl_channel_config resolved;
    knurl_status status;
    *out = NULL;
    if (config == NULL || config->on_event == NULL ||
        (config->interest & ~(KNURL_READABLE | KNURL_WRITABLE)) != 0)
        return knurl_fail(
            error, KNURL_INVALID, "request_takeover",
            "provide an event callback and logical read/write interests");
    resolved = *config;
    if (resolved.receive_bytes == 0)
        resolved.receive_bytes =
            request->impl->server->impl->config.io.input_queue_bytes;
    if (resolved.send_bytes == 0)
        resolved.send_bytes =
            request->impl->server->impl->config.io.output_queue_bytes;
    if (resolved.tail_bytes == 0)
        resolved.tail_bytes = resolved.receive_bytes;
    if (resolved.shutdown_timeout_ms == 0)
        resolved.shutdown_timeout_ms = 5000;
    if (resolved.receive_bytes >
            request->impl->server->impl->config.io.input_queue_bytes ||
        resolved.tail_bytes >
            request->impl->server->impl->config.io.input_queue_bytes ||
        resolved.shutdown_timeout_ms < 0)
        return knurl_fail(
            error, KNURL_LIMIT, "request_takeover",
            "receive/tail capacities must fit server input bounds; "
            "shutdown timeout must be nonnegative");
    if (initial.length > resolved.receive_bytes ||
        backlog.length > resolved.receive_bytes - initial.length)
        return knurl_fail(error, KNURL_LIMIT, "request_takeover",
                          "coalesced input exceeds channel receive capacity");
    c = calloc(1, sizeof(struct knurl_channel_impl));
    if (c == NULL)
        return knurl_fail(error, KNURL_NOMEM, "request_takeover",
                          "cannot allocate channel");
    knurl_channel_receiver_init(c);
    c->impl->receive = malloc(resolved.receive_bytes);
    if (c->impl->receive == NULL) {
        free(c);
        return knurl_fail(error, KNURL_NOMEM, "request_takeover",
                          "cannot allocate bounded channel receive storage");
    }
    c->impl->request = request;
    c->impl->server = request->impl->server;
    c->impl->response = request->impl->response;
    c->impl->schedule = schedule_response;
    c->impl->idle =
        request->impl->server->impl->config.timeouts.connection_idle;
    c->impl->config = resolved;
    c->impl->receive_length = initial.length + backlog.length;
    if (initial.length != 0)
        memcpy(c->impl->receive, initial.data, initial.length);
    if (backlog.length != 0)
        memcpy(c->impl->receive + initial.length, backlog.data, backlog.length);
    (void)add(&c->impl->outcome.received_bytes, c->impl->receive_length);
    status = knurl_loop_monotonic(&c->impl->last_activity, error);
    if (status != KNURL_OK) {
        free(c->impl->receive);
        free(c);
        return status;
    }
    c->impl->last_write = c->impl->last_activity;
    c->impl->io_ready = KNURL_WRITABLE;
    *out = c;
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_channel_schedule(knurl_channel *c) { c->impl->schedule(c); }

knurl_channel_phase knurl_channel_state(const knurl_channel *c)
{
    return c != NULL ? c->impl->phase : KNURL_CHANNEL_CLOSED;
}

size_t knurl_channel_queued_bytes(const knurl_channel *c)
{
    return c != NULL ? c->impl->queued : 0;
}

void knurl_channel_progress(knurl_channel *c, size_t received, size_t sent)
{
    if (!add(&c->impl->outcome.received_bytes, received) ||
        !add(&c->impl->outcome.sent_bytes, sent)) {
        knurl_channel_finish(c, KNURL_OVERFLOW);
        return;
    }
    if (received != 0 || sent != 0)
        (void)knurl_loop_monotonic(&c->impl->last_activity, NULL);
    if (c->impl->response != NULL &&
        c->impl->response->impl->progress != NULL) {
        if (received != 0)
            c->impl->response->impl->progress(
                c->impl->response->impl->progress_data, received);
        if (sent != 0)
            c->impl->response->impl->progress(
                c->impl->response->impl->progress_data, sent);
    }
    if (sent != 0) {
        c->impl->last_write = c->impl->last_activity;
        c->impl->write_notified = 0;
    }
}

knurl_status knurl_channel_read(knurl_channel *c, void *buffer, size_t capacity,
                                size_t *received, knurl_error *error)
{
    knurl_status status;
    size_t count;
    size_t limit;
    if (received != NULL)
        *received = 0;
    status = owner(c, error);
    if (status != KNURL_OK)
        return status;
    if (buffer == NULL || capacity == 0 || received == NULL)
        return knurl_fail(error, KNURL_INVALID, "channel_read",
                          "provide a buffer, positive capacity and byte count");
    if (c->impl->input_complete && c->impl->phase == KNURL_CHANNEL_HTTP_INPUT)
        return knurl_fail(error, KNURL_STATE, "channel_read",
                          "HTTP input boundary was already declared");
    if (c->impl->receive_length == 0 && !c->impl->outcome.read_eof) {
        c->impl->receive_offset = 0;
        limit = c->impl->server->impl->config.io.chunk_bytes;
        if (limit > c->impl->config.receive_bytes)
            limit = c->impl->config.receive_bytes;
        count = 0;
        status = c->impl->ops.read(c, c->impl->receive, limit, &count, error);
        if (status == KNURL_EOF) {
            c->impl->outcome.read_eof = 1;
            c->impl->read_wait = 0;
        } else if (status == KNURL_OK) {
            if (count == 0 || count > limit) {
                knurl_channel_finish(c, KNURL_BACKEND);
                return knurl_fail(error, KNURL_BACKEND, "channel_read",
                                  "backend returned an invalid count");
            }
            c->impl->receive_length = count;
            knurl_channel_progress(c, count, 0);
        } else if (status != KNURL_AGAIN) {
            knurl_channel_finish(c, status);
            return status;
        }
        c->impl->ops.interest(c);
        if (c->impl->phase == KNURL_CHANNEL_CLOSED)
            return knurl_fail(error, c->impl->outcome.cause, "channel_read",
                              "channel transport failed");
    }
    if (c->impl->receive_length == 0) {
        c->impl->read_notified = 0;
        c->impl->ops.interest(c);
        if (c->impl->outcome.read_eof) {
            knurl_channel_schedule(c);
            knurl_error_clear(error);
            return KNURL_EOF;
        }
        knurl_error_clear(error);
        return KNURL_AGAIN;
    }
    count = c->impl->receive_length;
    if (count > capacity)
        count = capacity;
    memcpy(buffer, c->impl->receive + c->impl->receive_offset, count);
    c->impl->receive_offset += count;
    c->impl->receive_length -= count;
    if (!add(&c->impl->outcome.delivered_bytes, count)) {
        knurl_channel_finish(c, KNURL_OVERFLOW);
        return knurl_fail(error, KNURL_OVERFLOW, "channel_read",
                          "delivery counter overflow");
    }
    c->impl->read_notified = 0;
    *received = count;
    c->impl->ops.interest(c);
    knurl_channel_schedule(c);
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_channel_write(knurl_channel *c, const void *data,
                                 size_t length, size_t *accepted,
                                 knurl_error *error)
{
    struct knurl_channel_chunk *chunk;
    knurl_status status;
    size_t count;
    if (accepted != NULL)
        *accepted = 0;
    status = owner(c, error);
    if (status != KNURL_OK)
        return status;
    if ((data == NULL && length != 0) || accepted == NULL)
        return knurl_fail(error, KNURL_INVALID, "channel_write",
                          "provide bytes and an accepted count");
    if (c->impl->phase != KNURL_CHANNEL_TUNNEL || c->impl->shutdown_requested)
        return knurl_fail(error, KNURL_STATE, "channel_write",
                          "raw writes require an open tunnel write half");
    count = c->impl->config.send_bytes - c->impl->queued;
    if (length == 0) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    if (count == 0) {
        knurl_error_clear(error);
        return KNURL_AGAIN;
    }
    if (count > length)
        count = length;
    if (count > c->impl->server->impl->config.io.chunk_bytes)
        count = c->impl->server->impl->config.io.chunk_bytes;
    chunk = malloc(sizeof(*chunk));
    if (chunk == NULL)
        return knurl_fail(error, KNURL_NOMEM, "channel_write",
                          "cannot allocate output segment");
    chunk->data = malloc(count);
    if (chunk->data == NULL) {
        free(chunk);
        return knurl_fail(error, KNURL_NOMEM, "channel_write",
                          "cannot copy bounded output");
    }
    memcpy(chunk->data, data, count);
    chunk->length = count;
    chunk->offset = 0;
    chunk->next = NULL;
    if (c->impl->last != NULL)
        c->impl->last->next = chunk;
    else
        c->impl->head = chunk;
    c->impl->last = chunk;
    if (c->impl->queued == 0)
        (void)knurl_loop_monotonic(&c->impl->last_write, NULL);
    c->impl->queued += count;
    c->impl->retained += count;
    c->impl->write_notified = 0;
    *accepted = count;
    c->impl->ops.interest(c);
    knurl_channel_schedule(c);
    knurl_error_clear(error);
    return KNURL_OK;
}

/* Takes ownership only on success. One allocation pins an entire frame across
 * transport retries, and no prefix can be accepted on allocation failure. */
knurl_status knurl_channel_enqueue(knurl_channel *c, unsigned char *data,
                                   size_t length, knurl_error *error)
{
    struct knurl_channel_chunk *chunk;
    if (length > c->impl->config.send_bytes)
        return knurl_fail(error, KNURL_LIMIT, "channel_enqueue",
                          "frame exceeds output capacity");
    if (length > c->impl->config.send_bytes - c->impl->retained)
        return KNURL_AGAIN;
    chunk = malloc(sizeof(*chunk));
    if (chunk == NULL)
        return knurl_fail(error, KNURL_NOMEM, "channel_enqueue",
                          "cannot allocate complete frame segment");
    chunk->data = data;
    chunk->length = length;
    chunk->offset = 0;
    chunk->next = NULL;
    if (c->impl->last != NULL)
        c->impl->last->next = chunk;
    else
        c->impl->head = chunk;
    c->impl->last = chunk;
    if (c->impl->queued == 0)
        (void)knurl_loop_monotonic(&c->impl->last_write, NULL);
    c->impl->queued += length;
    c->impl->retained += length;
    c->impl->write_notified = 0;
    c->impl->ops.interest(c);
    knurl_channel_schedule(c);
    return KNURL_OK;
}

knurl_status knurl_channel_interest(knurl_channel *c, unsigned int interest,
                                    knurl_error *error)
{
    knurl_status status;
    status = owner(c, error);
    if (status != KNURL_OK)
        return status;
    if ((interest & ~(KNURL_READABLE | KNURL_WRITABLE)) != 0)
        return knurl_fail(error, KNURL_INVALID, "channel_interest",
                          "select logical readable/writable interests");
    if (interest != c->impl->config.interest) {
        if ((interest & KNURL_READABLE) &&
            !(c->impl->config.interest & KNURL_READABLE))
            c->impl->read_notified = 0;
        if ((interest & KNURL_WRITABLE) &&
            !(c->impl->config.interest & KNURL_WRITABLE))
            c->impl->write_notified = 0;
        c->impl->config.interest = interest;
        c->impl->ops.interest(c);
        knurl_channel_schedule(c);
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_channel_body_complete(knurl_channel *c, const void *tail,
                                         size_t length, knurl_error *error)
{
    knurl_status status;
    knurl_u64 amount;
    unsigned char *copy;
    size_t total;
    int order;
    status = owner(c, error);
    if (status != KNURL_OK)
        return status;
    if (c->impl->phase != KNURL_CHANNEL_HTTP_INPUT || c->impl->input_complete)
        return knurl_fail(error, KNURL_STATE, "channel_body_complete",
                          "HTTP input was already completed or upgraded");
    if (tail == NULL && length != 0)
        return knurl_fail(error, KNURL_INVALID, "channel_body_complete",
                          "provide a valid suffix");
    if (length > c->impl->config.tail_bytes ||
        c->impl->receive_length > c->impl->config.tail_bytes - length)
        return knurl_fail(error, KNURL_LIMIT, "channel_body_complete",
                          "pipeline tail exceeds its finite bound");
    (void)knurl_u64_from_size(length, &amount, NULL);
    (void)knurl_u64_compare(amount, c->impl->outcome.delivered_bytes, &order,
                            NULL);
    if (order > 0)
        return knurl_fail(error, KNURL_INVALID, "channel_body_complete",
                          "suffix exceeds bytes delivered to the framer");
    total = length + c->impl->receive_length;
    copy = total != 0 ? malloc(total) : NULL;
    if (total != 0 && copy == NULL)
        return knurl_fail(error, KNURL_NOMEM, "channel_body_complete",
                          "cannot preserve pipeline tail");
    if (length != 0)
        memcpy(copy, tail, length);
    if (c->impl->receive_length != 0)
        memcpy(copy + length, c->impl->receive + c->impl->receive_offset,
               c->impl->receive_length);
    c->impl->tail = copy;
    c->impl->tail_length = total;
    c->impl->request->impl->outcome.delivered_body_bytes =
        subtract(c->impl->outcome.delivered_bytes, amount);
    (void)knurl_u64_from_size(total, &amount, NULL);
    c->impl->request->impl->outcome.received_body_bytes =
        subtract(c->impl->outcome.received_bytes, amount);
    c->impl->request->impl->outcome.body_complete = 1;
    c->impl->request->impl->outcome.body_cause = KNURL_OK;
    c->impl->receive_length = c->impl->receive_offset = 0;
    c->impl->input_complete = 1;
    c->impl->read_wait = 0;
    c->impl->ops.completed(c);
    c->impl->ops.interest(c);
    knurl_channel_schedule(c);
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_channel_shutdown_write(knurl_channel *c, knurl_error *error)
{
    knurl_status status;
    status = owner(c, error);
    if (status != KNURL_OK)
        return status;
    if (c->impl->phase != KNURL_CHANNEL_TUNNEL)
        return knurl_fail(error, KNURL_STATE, "channel_shutdown_write",
                          "write shutdown requires a tunnel");
    if (!c->impl->shutdown_requested) {
        c->impl->shutdown_requested = 1;
        (void)knurl_loop_monotonic(&c->impl->shutdown_started, NULL);
        c->impl->ops.interest(c);
        knurl_channel_schedule(c);
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_channel_finish(knurl_channel *c, knurl_status cause)
{
    if (c == NULL || c->impl->phase == KNURL_CHANNEL_CLOSED)
        return;
    c->impl->previous_phase = c->impl->phase;
    c->impl->phase = KNURL_CHANNEL_CLOSED;
    c->impl->outcome.cause = cause;
    c->impl->config.interest = 0;
    if (c->impl->request != NULL &&
        !c->impl->request->impl->outcome.body_complete) {
        c->impl->request->impl->outcome.body_cause =
            cause == KNURL_OK ? KNURL_CANCELLED : cause;
        c->impl->request->impl->response->impl->close_connection = 1;
    }
    if (c->impl->request != NULL && cause != KNURL_OK &&
        c->impl->request->impl->outcome.cause == KNURL_OK)
        c->impl->request->impl->outcome.cause = cause;
    if (c->impl->response != NULL &&
        c->impl->previous_phase == KNURL_CHANNEL_TUNNEL) {
        if (cause == KNURL_OK)
            c->impl->request->impl->response->impl->phase =
                KNURL_RESPONSE_COMPLETE;
        else
            (void)knurl_response_abort(c->impl->request->impl->response, cause,
                                       NULL);
    } else if (c->impl->response != NULL && cause != KNURL_OK &&
               c->impl->request->impl->response->impl->phase <
                   KNURL_RESPONSE_FINISHING)
        knurl_response_error(c->impl->request->impl->response, cause, 500);
    c->impl->ops.interest(c);
    knurl_channel_schedule(c);
}

knurl_status knurl_channel_close(knurl_channel *c, knurl_status cause,
                                 knurl_error *error)
{
    knurl_status status;
    status = owner(c, error);
    if (status != KNURL_OK)
        return status;
    if (cause < KNURL_OK || cause == KNURL_AGAIN || cause == KNURL_EOF ||
        cause > KNURL_BACKEND)
        return knurl_fail(error, KNURL_INVALID, "channel_close",
                          "provide OK or a terminal cause");
    if (cause == KNURL_OK &&
        (c->impl->queued != 0 ||
         (c->impl->phase == KNURL_CHANNEL_TUNNEL &&
          ((c->impl->response != NULL &&
            (c->impl->response->impl->head != NULL ||
             c->impl->response->impl->pending_head != NULL)) ||
           (c->impl->shutdown_requested &&
            !c->impl->outcome.write_shutdown))) ||
         (c->impl->phase == KNURL_CHANNEL_HTTP_INPUT &&
          !c->impl->input_complete)))
        return knurl_fail(
            error, KNURL_STATE, "channel_close",
            "orderly close requires drained output and completed HTTP input");
    knurl_channel_finish(c, cause);
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_channel_notify_closed(knurl_channel *c)
{
    if (c == NULL || c->impl->phase != KNURL_CHANNEL_CLOSED ||
        c->impl->close_notified || c->impl->in_event)
        return;
    c->impl->close_notified = 1;
    if (c->impl->config.on_close != NULL)
        c->impl->config.on_close(c, &c->impl->outcome,
                                 c->impl->config.userdata);
}

void knurl_channel_turn(knurl_channel *c)
{
    unsigned int ready;
    if (c == NULL)
        return;
    if (c->impl->phase == KNURL_CHANNEL_CLOSED) {
        knurl_channel_notify_closed(c);
        return;
    }
    c->impl->ops.pump(c);
    if (c->impl->phase == KNURL_CHANNEL_CLOSED)
        return;
    ready = 0;
    if ((c->impl->config.interest & KNURL_READABLE) &&
        !c->impl->input_complete) {
        if (!c->impl->outcome.read_eof && !c->impl->read_notified &&
            (c->impl->receive_length != 0 ||
             (c->impl->io_ready &
              (c->impl->read_wait ? c->impl->read_wait : KNURL_READABLE)))) {
            ready |= KNURL_READABLE;
            c->impl->read_notified = 1;
        }
        if (c->impl->outcome.read_eof && c->impl->receive_length == 0 &&
            !c->impl->eof_notified) {
            ready |= KNURL_READ_CLOSED;
            c->impl->eof_notified = 1;
        }
    }
    if (c->impl->phase == KNURL_CHANNEL_TUNNEL &&
        (c->impl->config.interest & KNURL_WRITABLE) &&
        !c->impl->write_notified && !c->impl->shutdown_requested &&
        c->impl->queued < c->impl->config.send_bytes) {
        ready |= KNURL_WRITABLE;
        c->impl->write_notified = 1;
    }
    if (c->impl->outcome.write_shutdown && !c->impl->shutdown_notified) {
        ready |= KNURL_WRITE_CLOSED;
        c->impl->shutdown_notified = 1;
    }
    if (ready != 0) {
        c->impl->in_event = 1;
        c->impl->config.on_event(c, ready, c->impl->config.userdata);
        c->impl->in_event = 0;
    }
    c->impl->ops.interest(c);
}

static long remaining(knurl_u64 now, knurl_u64 start, long timeout)
{
    knurl_u64 elapsed;
    size_t value;
    int order;
    (void)knurl_u64_compare(now, start, &order, NULL);
    if (order < 0)
        return timeout;
    elapsed = subtract(now, start);
    if (knurl_u64_to_size(elapsed, &value, NULL) != KNURL_OK ||
        value >= (size_t)timeout)
        return 0;
    return timeout - (long)value;
}

long knurl_channel_wait(knurl_channel *c)
{
    knurl_u64 now;
    const knurl_timeout_config *timeouts;
    long wait;
    long value;
    wait = LONG_MAX;
    if (c == NULL || c->impl->phase == KNURL_CHANNEL_CLOSED)
        return wait;
    if (knurl_loop_monotonic(&now, NULL) != KNURL_OK)
        return 0;
    timeouts = &c->impl->server->impl->config.timeouts;
    if (!c->impl->idle.disabled)
        wait =
            remaining(now, c->impl->last_activity, c->impl->idle.milliseconds);
    if (c->impl->queued != 0 && !timeouts->response_write_idle.disabled) {
        value = remaining(now, c->impl->last_write,
                          timeouts->response_write_idle.milliseconds);
        if (value < wait)
            wait = value;
    }
    if (c->impl->shutdown_requested && !c->impl->outcome.write_shutdown) {
        value = remaining(now, c->impl->shutdown_started,
                          c->impl->config.shutdown_timeout_ms);
        if (value < wait)
            wait = value;
    }
    return wait;
}

void knurl_channel_check_timeout(knurl_channel *c)
{
    if (c != NULL && knurl_channel_wait(c) == 0)
        knurl_channel_finish(c, KNURL_TIMEOUT);
}

void knurl_channel_release(knurl_channel *c, int restore)
{
    struct knurl_channel_chunk *chunk;
    if (c == NULL)
        return;
    knurl_channel_notify_closed(c);
    if (c->impl->ops.release != NULL)
        c->impl->ops.release(c, restore);
    while (c->impl->head != NULL) {
        chunk = c->impl->head;
        c->impl->head = chunk->next;
        free(chunk->data);
        free(chunk);
    }
    free(c->impl->receive);
    free(c->impl->tail);
    free(c);
}

void knurl_channel_receiver_init(knurl_channel *receiver)
{
    static const knurl_channel methods = {
        knurl_channel_state,    knurl_channel_queued_bytes,
        knurl_channel_read,     knurl_channel_write,
        knurl_channel_interest, knurl_channel_body_complete,
        knurl_channel_upgrade,  knurl_channel_shutdown_write,
        knurl_channel_close,    NULL};
    *receiver = methods;
    receiver->impl = (struct knurl_channel_impl *)receiver;
}

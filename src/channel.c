#include "channel_internal.h"
#include "response_internal.h"
#include "server_internal.h"
#include "loop_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static rotmos_status owner(rotmos_channel *c, rotmos_error *error)
{
    if (c == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "channel",
                           "channel is required");
    if (c->phase == ROTMOS_CHANNEL_CLOSED ||
        (c->request != NULL && c->request->close_notified))
        return rotmos_fail(error, ROTMOS_STATE, "channel", "channel is closed");
    return rotmos_loop_owner(c->server->loop, error);
}

static void schedule_response(rotmos_channel *c)
{
    c->response->schedule(c->response);
}

static rotmos_u64 subtract(rotmos_u64 a, rotmos_u64 b)
{
    rotmos_u64 result;
    result.high = a.high - b.high - (a.low < b.low ? 1 : 0);
    result.low = (a.low - b.low) & 0xffffffffUL;
    return result;
}

static int add(rotmos_u64 *value, size_t count)
{
    rotmos_u64 amount;
    return rotmos_u64_from_size(count, &amount, NULL) == ROTMOS_OK &&
           rotmos_u64_add(*value, amount, value, NULL) == ROTMOS_OK;
}

void rotmos_channel_config_init(rotmos_channel_config *config)
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

rotmos_status rotmos_channel_create(rotmos_request *request,
                                    const rotmos_channel_config *config,
                                    rotmos_bytes initial, rotmos_bytes backlog,
                                    rotmos_channel **out, rotmos_error *error)
{
    rotmos_channel *c;
    rotmos_channel_config resolved;
    rotmos_status status;
    *out = NULL;
    if (config == NULL || config->on_event == NULL ||
        (config->interest & ~(ROTMOS_READABLE | ROTMOS_WRITABLE)) != 0)
        return rotmos_fail(
            error, ROTMOS_INVALID, "request_takeover",
            "provide an event callback and logical read/write interests");
    resolved = *config;
    if (resolved.receive_bytes == 0)
        resolved.receive_bytes = request->server->config.io.input_queue_bytes;
    if (resolved.send_bytes == 0)
        resolved.send_bytes = request->server->config.io.output_queue_bytes;
    if (resolved.tail_bytes == 0)
        resolved.tail_bytes = resolved.receive_bytes;
    if (resolved.shutdown_timeout_ms == 0)
        resolved.shutdown_timeout_ms = 5000;
    if (resolved.receive_bytes > request->server->config.io.input_queue_bytes ||
        resolved.tail_bytes > request->server->config.io.input_queue_bytes ||
        resolved.shutdown_timeout_ms < 0)
        return rotmos_fail(
            error, ROTMOS_LIMIT, "request_takeover",
            "receive/tail capacities must fit server input bounds; "
            "shutdown timeout must be nonnegative");
    if (initial.length > resolved.receive_bytes ||
        backlog.length > resolved.receive_bytes - initial.length)
        return rotmos_fail(error, ROTMOS_LIMIT, "request_takeover",
                           "coalesced input exceeds channel receive capacity");
    c = calloc(1, sizeof(*c));
    if (c == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "request_takeover",
                           "cannot allocate channel");
    c->receive = malloc(resolved.receive_bytes);
    if (c->receive == NULL) {
        free(c);
        return rotmos_fail(error, ROTMOS_NOMEM, "request_takeover",
                           "cannot allocate bounded channel receive storage");
    }
    c->request = request;
    c->server = request->server;
    c->response = request->response;
    c->schedule = schedule_response;
    c->idle = request->server->config.timeouts.connection_idle;
    c->config = resolved;
    c->receive_length = initial.length + backlog.length;
    if (initial.length != 0)
        memcpy(c->receive, initial.data, initial.length);
    if (backlog.length != 0)
        memcpy(c->receive + initial.length, backlog.data, backlog.length);
    (void)add(&c->outcome.received_bytes, c->receive_length);
    status = rotmos_loop_monotonic(&c->last_activity, error);
    if (status != ROTMOS_OK) {
        free(c->receive);
        free(c);
        return status;
    }
    c->last_write = c->last_activity;
    c->io_ready = ROTMOS_WRITABLE;
    *out = c;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

void rotmos_channel_schedule(rotmos_channel *c) { c->schedule(c); }

rotmos_channel_phase rotmos_channel_state(const rotmos_channel *c)
{
    return c != NULL ? c->phase : ROTMOS_CHANNEL_CLOSED;
}

size_t rotmos_channel_queued_bytes(const rotmos_channel *c)
{
    return c != NULL ? c->queued : 0;
}

void rotmos_channel_progress(rotmos_channel *c, size_t received, size_t sent)
{
    if (!add(&c->outcome.received_bytes, received) ||
        !add(&c->outcome.sent_bytes, sent)) {
        rotmos_channel_finish(c, ROTMOS_OVERFLOW);
        return;
    }
    if (received != 0 || sent != 0)
        (void)rotmos_loop_monotonic(&c->last_activity, NULL);
    if (c->response != NULL && c->response->progress != NULL) {
        if (received != 0)
            c->response->progress(c->response->progress_data, received);
        if (sent != 0)
            c->response->progress(c->response->progress_data, sent);
    }
    if (sent != 0) {
        c->last_write = c->last_activity;
        c->write_notified = 0;
    }
}

rotmos_status rotmos_channel_read(rotmos_channel *c, void *buffer,
                                  size_t capacity, size_t *received,
                                  rotmos_error *error)
{
    rotmos_status status;
    size_t count;
    size_t limit;
    if (received != NULL)
        *received = 0;
    status = owner(c, error);
    if (status != ROTMOS_OK)
        return status;
    if (buffer == NULL || capacity == 0 || received == NULL)
        return rotmos_fail(
            error, ROTMOS_INVALID, "channel_read",
            "provide a buffer, positive capacity and byte count");
    if (c->input_complete && c->phase == ROTMOS_CHANNEL_HTTP_INPUT)
        return rotmos_fail(error, ROTMOS_STATE, "channel_read",
                           "HTTP input boundary was already declared");
    if (c->receive_length == 0 && !c->outcome.read_eof) {
        c->receive_offset = 0;
        limit = c->server->config.io.chunk_bytes;
        if (limit > c->config.receive_bytes)
            limit = c->config.receive_bytes;
        count = 0;
        status = c->ops.read(c, c->receive, limit, &count, error);
        if (status == ROTMOS_EOF) {
            c->outcome.read_eof = 1;
            c->read_wait = 0;
        } else if (status == ROTMOS_OK) {
            if (count == 0 || count > limit) {
                rotmos_channel_finish(c, ROTMOS_BACKEND);
                return rotmos_fail(error, ROTMOS_BACKEND, "channel_read",
                                   "backend returned an invalid count");
            }
            c->receive_length = count;
            rotmos_channel_progress(c, count, 0);
        } else if (status != ROTMOS_AGAIN) {
            rotmos_channel_finish(c, status);
            return status;
        }
        c->ops.interest(c);
        if (c->phase == ROTMOS_CHANNEL_CLOSED)
            return rotmos_fail(error, c->outcome.cause, "channel_read",
                               "channel transport failed");
    }
    if (c->receive_length == 0) {
        c->read_notified = 0;
        c->ops.interest(c);
        if (c->outcome.read_eof) {
            rotmos_channel_schedule(c);
            rotmos_error_clear(error);
            return ROTMOS_EOF;
        }
        rotmos_error_clear(error);
        return ROTMOS_AGAIN;
    }
    count = c->receive_length;
    if (count > capacity)
        count = capacity;
    memcpy(buffer, c->receive + c->receive_offset, count);
    c->receive_offset += count;
    c->receive_length -= count;
    if (!add(&c->outcome.delivered_bytes, count)) {
        rotmos_channel_finish(c, ROTMOS_OVERFLOW);
        return rotmos_fail(error, ROTMOS_OVERFLOW, "channel_read",
                           "delivery counter overflow");
    }
    c->read_notified = 0;
    *received = count;
    c->ops.interest(c);
    rotmos_channel_schedule(c);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_channel_write(rotmos_channel *c, const void *data,
                                   size_t length, size_t *accepted,
                                   rotmos_error *error)
{
    struct rotmos_channel_chunk *chunk;
    rotmos_status status;
    size_t count;
    if (accepted != NULL)
        *accepted = 0;
    status = owner(c, error);
    if (status != ROTMOS_OK)
        return status;
    if ((data == NULL && length != 0) || accepted == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "channel_write",
                           "provide bytes and an accepted count");
    if (c->phase != ROTMOS_CHANNEL_TUNNEL || c->shutdown_requested)
        return rotmos_fail(error, ROTMOS_STATE, "channel_write",
                           "raw writes require an open tunnel write half");
    count = c->config.send_bytes - c->queued;
    if (length == 0) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    if (count == 0) {
        rotmos_error_clear(error);
        return ROTMOS_AGAIN;
    }
    if (count > length)
        count = length;
    if (count > c->server->config.io.chunk_bytes)
        count = c->server->config.io.chunk_bytes;
    chunk = malloc(sizeof(*chunk));
    if (chunk == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "channel_write",
                           "cannot allocate output segment");
    chunk->data = malloc(count);
    if (chunk->data == NULL) {
        free(chunk);
        return rotmos_fail(error, ROTMOS_NOMEM, "channel_write",
                           "cannot copy bounded output");
    }
    memcpy(chunk->data, data, count);
    chunk->length = count;
    chunk->offset = 0;
    chunk->next = NULL;
    if (c->last != NULL)
        c->last->next = chunk;
    else
        c->head = chunk;
    c->last = chunk;
    if (c->queued == 0)
        (void)rotmos_loop_monotonic(&c->last_write, NULL);
    c->queued += count;
    c->retained += count;
    c->write_notified = 0;
    *accepted = count;
    c->ops.interest(c);
    rotmos_channel_schedule(c);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

/* Takes ownership only on success. One allocation pins an entire frame across
 * transport retries, and no prefix can be accepted on allocation failure. */
rotmos_status rotmos_channel_enqueue(rotmos_channel *c, unsigned char *data,
                                     size_t length, rotmos_error *error)
{
    struct rotmos_channel_chunk *chunk;
    if (length > c->config.send_bytes)
        return rotmos_fail(error, ROTMOS_LIMIT, "channel_enqueue",
                           "frame exceeds output capacity");
    if (length > c->config.send_bytes - c->retained)
        return ROTMOS_AGAIN;
    chunk = malloc(sizeof(*chunk));
    if (chunk == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "channel_enqueue",
                           "cannot allocate complete frame segment");
    chunk->data = data;
    chunk->length = length;
    chunk->offset = 0;
    chunk->next = NULL;
    if (c->last != NULL)
        c->last->next = chunk;
    else
        c->head = chunk;
    c->last = chunk;
    if (c->queued == 0)
        (void)rotmos_loop_monotonic(&c->last_write, NULL);
    c->queued += length;
    c->retained += length;
    c->write_notified = 0;
    c->ops.interest(c);
    rotmos_channel_schedule(c);
    return ROTMOS_OK;
}

rotmos_status rotmos_channel_interest(rotmos_channel *c, unsigned int interest,
                                      rotmos_error *error)
{
    rotmos_status status;
    status = owner(c, error);
    if (status != ROTMOS_OK)
        return status;
    if ((interest & ~(ROTMOS_READABLE | ROTMOS_WRITABLE)) != 0)
        return rotmos_fail(error, ROTMOS_INVALID, "channel_interest",
                           "select logical readable/writable interests");
    if (interest != c->config.interest) {
        if ((interest & ROTMOS_READABLE) &&
            !(c->config.interest & ROTMOS_READABLE))
            c->read_notified = 0;
        if ((interest & ROTMOS_WRITABLE) &&
            !(c->config.interest & ROTMOS_WRITABLE))
            c->write_notified = 0;
        c->config.interest = interest;
        c->ops.interest(c);
        rotmos_channel_schedule(c);
    }
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_channel_body_complete(rotmos_channel *c, const void *tail,
                                           size_t length, rotmos_error *error)
{
    rotmos_status status;
    rotmos_u64 amount;
    unsigned char *copy;
    size_t total;
    int order;
    status = owner(c, error);
    if (status != ROTMOS_OK)
        return status;
    if (c->phase != ROTMOS_CHANNEL_HTTP_INPUT || c->input_complete)
        return rotmos_fail(error, ROTMOS_STATE, "channel_body_complete",
                           "HTTP input was already completed or upgraded");
    if (tail == NULL && length != 0)
        return rotmos_fail(error, ROTMOS_INVALID, "channel_body_complete",
                           "provide a valid suffix");
    if (length > c->config.tail_bytes ||
        c->receive_length > c->config.tail_bytes - length)
        return rotmos_fail(error, ROTMOS_LIMIT, "channel_body_complete",
                           "pipeline tail exceeds its finite bound");
    (void)rotmos_u64_from_size(length, &amount, NULL);
    (void)rotmos_u64_compare(amount, c->outcome.delivered_bytes, &order, NULL);
    if (order > 0)
        return rotmos_fail(error, ROTMOS_INVALID, "channel_body_complete",
                           "suffix exceeds bytes delivered to the framer");
    total = length + c->receive_length;
    copy = total != 0 ? malloc(total) : NULL;
    if (total != 0 && copy == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "channel_body_complete",
                           "cannot preserve pipeline tail");
    if (length != 0)
        memcpy(copy, tail, length);
    if (c->receive_length != 0)
        memcpy(copy + length, c->receive + c->receive_offset,
               c->receive_length);
    c->tail = copy;
    c->tail_length = total;
    c->request->outcome.delivered_body_bytes =
        subtract(c->outcome.delivered_bytes, amount);
    (void)rotmos_u64_from_size(total, &amount, NULL);
    c->request->outcome.received_body_bytes =
        subtract(c->outcome.received_bytes, amount);
    c->request->outcome.body_complete = 1;
    c->request->outcome.body_cause = ROTMOS_OK;
    c->receive_length = c->receive_offset = 0;
    c->input_complete = 1;
    c->read_wait = 0;
    c->ops.completed(c);
    c->ops.interest(c);
    rotmos_channel_schedule(c);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_channel_shutdown_write(rotmos_channel *c,
                                            rotmos_error *error)
{
    rotmos_status status;
    status = owner(c, error);
    if (status != ROTMOS_OK)
        return status;
    if (c->phase != ROTMOS_CHANNEL_TUNNEL)
        return rotmos_fail(error, ROTMOS_STATE, "channel_shutdown_write",
                           "write shutdown requires a tunnel");
    if (!c->shutdown_requested) {
        c->shutdown_requested = 1;
        (void)rotmos_loop_monotonic(&c->shutdown_started, NULL);
        c->ops.interest(c);
        rotmos_channel_schedule(c);
    }
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

void rotmos_channel_finish(rotmos_channel *c, rotmos_status cause)
{
    if (c == NULL || c->phase == ROTMOS_CHANNEL_CLOSED)
        return;
    c->previous_phase = c->phase;
    c->phase = ROTMOS_CHANNEL_CLOSED;
    c->outcome.cause = cause;
    c->config.interest = 0;
    if (c->request != NULL && !c->request->outcome.body_complete) {
        c->request->outcome.body_cause =
            cause == ROTMOS_OK ? ROTMOS_CANCELLED : cause;
        c->request->response->close_connection = 1;
    }
    if (c->request != NULL && cause != ROTMOS_OK &&
        c->request->outcome.cause == ROTMOS_OK)
        c->request->outcome.cause = cause;
    if (c->response != NULL && c->previous_phase == ROTMOS_CHANNEL_TUNNEL) {
        if (cause == ROTMOS_OK)
            c->request->response->phase = ROTMOS_RESPONSE_COMPLETE;
        else
            (void)rotmos_response_abort(c->request->response, cause, NULL);
    } else if (c->response != NULL && cause != ROTMOS_OK &&
               c->request->response->phase < ROTMOS_RESPONSE_FINISHING)
        rotmos_response_error(c->request->response, cause, 500);
    c->ops.interest(c);
    rotmos_channel_schedule(c);
}

rotmos_status rotmos_channel_close(rotmos_channel *c, rotmos_status cause,
                                   rotmos_error *error)
{
    rotmos_status status;
    status = owner(c, error);
    if (status != ROTMOS_OK)
        return status;
    if (cause < ROTMOS_OK || cause == ROTMOS_AGAIN || cause == ROTMOS_EOF ||
        cause > ROTMOS_BACKEND)
        return rotmos_fail(error, ROTMOS_INVALID, "channel_close",
                           "provide OK or a terminal cause");
    if (cause == ROTMOS_OK &&
        (c->queued != 0 ||
         (c->phase == ROTMOS_CHANNEL_TUNNEL &&
          ((c->response != NULL &&
            (c->response->head != NULL || c->response->pending_head != NULL)) ||
           (c->shutdown_requested && !c->outcome.write_shutdown))) ||
         (c->phase == ROTMOS_CHANNEL_HTTP_INPUT && !c->input_complete)))
        return rotmos_fail(
            error, ROTMOS_STATE, "channel_close",
            "orderly close requires drained output and completed HTTP input");
    rotmos_channel_finish(c, cause);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

void rotmos_channel_notify_closed(rotmos_channel *c)
{
    if (c == NULL || c->phase != ROTMOS_CHANNEL_CLOSED || c->close_notified ||
        c->in_event)
        return;
    c->close_notified = 1;
    if (c->config.on_close != NULL)
        c->config.on_close(c, &c->outcome, c->config.userdata);
}

void rotmos_channel_turn(rotmos_channel *c)
{
    unsigned int ready;
    if (c == NULL)
        return;
    if (c->phase == ROTMOS_CHANNEL_CLOSED) {
        rotmos_channel_notify_closed(c);
        return;
    }
    c->ops.pump(c);
    if (c->phase == ROTMOS_CHANNEL_CLOSED)
        return;
    ready = 0;
    if ((c->config.interest & ROTMOS_READABLE) && !c->input_complete) {
        if (!c->outcome.read_eof && !c->read_notified &&
            (c->receive_length != 0 ||
             (c->io_ready & (c->read_wait ? c->read_wait : ROTMOS_READABLE)))) {
            ready |= ROTMOS_READABLE;
            c->read_notified = 1;
        }
        if (c->outcome.read_eof && c->receive_length == 0 && !c->eof_notified) {
            ready |= ROTMOS_READ_CLOSED;
            c->eof_notified = 1;
        }
    }
    if (c->phase == ROTMOS_CHANNEL_TUNNEL &&
        (c->config.interest & ROTMOS_WRITABLE) && !c->write_notified &&
        !c->shutdown_requested && c->queued < c->config.send_bytes) {
        ready |= ROTMOS_WRITABLE;
        c->write_notified = 1;
    }
    if (c->outcome.write_shutdown && !c->shutdown_notified) {
        ready |= ROTMOS_WRITE_CLOSED;
        c->shutdown_notified = 1;
    }
    if (ready != 0) {
        c->in_event = 1;
        c->config.on_event(c, ready, c->config.userdata);
        c->in_event = 0;
    }
    c->ops.interest(c);
}

static long remaining(rotmos_u64 now, rotmos_u64 start, long timeout)
{
    rotmos_u64 elapsed;
    size_t value;
    int order;
    (void)rotmos_u64_compare(now, start, &order, NULL);
    if (order < 0)
        return timeout;
    elapsed = subtract(now, start);
    if (rotmos_u64_to_size(elapsed, &value, NULL) != ROTMOS_OK ||
        value >= (size_t)timeout)
        return 0;
    return timeout - (long)value;
}

long rotmos_channel_wait(rotmos_channel *c)
{
    rotmos_u64 now;
    const rotmos_timeout_config *timeouts;
    long wait;
    long value;
    wait = LONG_MAX;
    if (c == NULL || c->phase == ROTMOS_CHANNEL_CLOSED)
        return wait;
    if (rotmos_loop_monotonic(&now, NULL) != ROTMOS_OK)
        return 0;
    timeouts = &c->server->config.timeouts;
    if (!c->idle.disabled)
        wait = remaining(now, c->last_activity, c->idle.milliseconds);
    if (c->queued != 0 && !timeouts->response_write_idle.disabled) {
        value = remaining(now, c->last_write,
                          timeouts->response_write_idle.milliseconds);
        if (value < wait)
            wait = value;
    }
    if (c->shutdown_requested && !c->outcome.write_shutdown) {
        value =
            remaining(now, c->shutdown_started, c->config.shutdown_timeout_ms);
        if (value < wait)
            wait = value;
    }
    return wait;
}

void rotmos_channel_check_timeout(rotmos_channel *c)
{
    if (c != NULL && rotmos_channel_wait(c) == 0)
        rotmos_channel_finish(c, ROTMOS_TIMEOUT);
}

void rotmos_channel_release(rotmos_channel *c, int restore)
{
    struct rotmos_channel_chunk *chunk;
    if (c == NULL)
        return;
    rotmos_channel_notify_closed(c);
    if (c->ops.release != NULL)
        c->ops.release(c, restore);
    while (c->head != NULL) {
        chunk = c->head;
        c->head = chunk->next;
        free(chunk->data);
        free(chunk);
    }
    free(c->receive);
    free(c->tail);
    free(c);
}

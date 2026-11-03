#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "response_internal.h"
#include "server_internal.h"
#include "loop_internal.h"

#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static rotmos_status access_response(rotmos_response *r, rotmos_error *error)
{
    if (r == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "response",
                           "response is required");
    if (r->busy || r->phase >= ROTMOS_RESPONSE_COMPLETE)
        return rotmos_fail(error, ROTMOS_STATE, "response",
                           "response is busy or terminal");
    return rotmos_loop_owner(r->request->server->loop, error);
}

static rotmos_status building(rotmos_response *r, rotmos_error *error)
{
    rotmos_status status;
    status = access_response(r, error);
    if (status == ROTMOS_OK && r->phase != ROTMOS_RESPONSE_BUILDING)
        return rotmos_fail(error, ROTMOS_STATE, "response",
                           "headers have already begun");
    return status;
}

static int forbidden(const char *name)
{
    return rotmos_text_equal_case(name, "content-length") ||
           rotmos_text_equal_case(name, "transfer-encoding") ||
           rotmos_text_equal_case(name, "connection") ||
           rotmos_text_equal_case(name, "keep-alive") ||
           rotmos_text_equal_case(name, "trailer") ||
           rotmos_text_equal_case(name, "upgrade");
}

static int trailer_forbidden(const char *name)
{
    return forbidden(name) || rotmos_text_equal_case(name, "host") ||
           rotmos_text_equal_case(name, "authorization") ||
           rotmos_text_equal_case(name, "proxy-authorization") ||
           rotmos_text_equal_case(name, "www-authenticate") ||
           rotmos_text_equal_case(name, "proxy-authenticate") ||
           rotmos_text_equal_case(name, "content-encoding") ||
           rotmos_text_equal_case(name, "content-type") ||
           rotmos_text_equal_case(name, "content-range") ||
           rotmos_text_equal_case(name, "set-cookie");
}

static int bodyless(rotmos_response *r)
{
    return r->request->method == ROTMOS_HTTP_HEAD || r->outcome.status == 204 ||
           r->outcome.status == 205 || r->outcome.status == 304;
}

static int compare(rotmos_u64 a, rotmos_u64 b)
{
    int order;
    (void)rotmos_u64_compare(a, b, &order, NULL);
    return order;
}

static rotmos_u64 difference(rotmos_u64 a, rotmos_u64 b)
{
    rotmos_u64 value;
    value.high = a.high - b.high - (a.low < b.low ? 1 : 0);
    value.low = (a.low - b.low) & 0xffffffffUL;
    return value;
}

static rotmos_status add_size(rotmos_u64 a, size_t count, rotmos_u64 *out)
{
    rotmos_u64 value;
    rotmos_status status;
    status = rotmos_u64_from_size(count, &value, NULL);
    return status == ROTMOS_OK ? rotmos_u64_add(a, value, out, NULL) : status;
}

static void clear_queue(rotmos_response *r)
{
    while (r->head != NULL)
        rotmos_response_shift(r);
}

static void retire_source(rotmos_response *r, rotmos_status cause)
{
    rotmos_source *source;
    source = r->source;
    if (source != NULL) {
        source->failure = cause;
        source->retired_next = r->retired;
        r->retired = source;
        r->source = NULL;
    }
}

static void clear_producer(rotmos_response *r, rotmos_status cause)
{
    retire_source(r, cause);
    if (r->file != -1)
        (void)close(r->file);
    r->file = -1;
    free(r->memory);
    r->memory = NULL;
    r->memory_length = r->memory_position = 0;
    r->producer = r->paused = r->eof = 0;
}

rotmos_status rotmos_response_create(rotmos_request *request,
                                     void (*schedule)(rotmos_response *),
                                     rotmos_response **out, rotmos_error *error)
{
    rotmos_response *r;
    rotmos_status status;
    *out = NULL;
    r = calloc(1, sizeof(*r));
    if (r == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "response",
                           "cannot allocate response");
    r->request = request;
    r->schedule = schedule;
    r->file = -1;
    r->outcome.status = 200;
    status = rotmos_loop_monotonic(&r->started, error);
    if (status != ROTMOS_OK) {
        free(r);
        return status;
    }
    r->bound = request->server->config.io.output_queue_bytes;
    r->chunk = request->server->config.io.chunk_bytes;
    r->header_bound = request->server->config.limits.max_response_header_bytes;
    status = rotmos_reply_new(&r->headers, error);
    if (status != ROTMOS_OK) {
        free(r);
        return status;
    }
    *out = r;
    return ROTMOS_OK;
}

rotmos_response *rotmos_request_response(rotmos_request *request)
{
    return request != NULL ? request->response : NULL;
}

rotmos_response_phase rotmos_response_state(const rotmos_response *r)
{
    return r != NULL ? r->phase : ROTMOS_RESPONSE_ABORTED;
}

int rotmos_response_is_committed(const rotmos_response *r)
{
    return r != NULL && r->outcome.committed;
}

size_t rotmos_response_queued_bytes(const rotmos_response *r)
{
    return r != NULL ? r->queued : 0;
}

rotmos_status rotmos_response_callbacks(rotmos_response *r,
                                        rotmos_response_writable_fn writable,
                                        rotmos_response_close_fn closed,
                                        void *userdata, rotmos_error *error)
{
    rotmos_status status;
    status = building(r, error);
    if (status != ROTMOS_OK)
        return status;
    r->writable = writable;
    r->closed = closed;
    r->userdata = userdata;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_response_status(rotmos_response *r, int code,
                                     rotmos_error *error)
{
    rotmos_status status;
    status = building(r, error);
    if (status != ROTMOS_OK)
        return status;
    if (code < 200 || code > 599)
        return rotmos_fail(error, ROTMOS_INVALID, "response_status",
                           "final status must be 200..599");
    r->outcome.status = code;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

static rotmos_status header(rotmos_response *r, const char *name,
                            const char *value, int set, rotmos_error *error)
{
    rotmos_status status;
    size_t total;
    size_t i;
    status = building(r, error);
    if (status != ROTMOS_OK)
        return status;
    if (!rotmos_header_name_valid(name) || !rotmos_header_value_valid(value) ||
        forbidden(name))
        return rotmos_fail(error, ROTMOS_INVALID, "response_header",
                           "invalid or transport-owned field");
    total = 0;
    for (i = 0; i < r->headers->header_count; ++i) {
        if (set &&
            rotmos_text_equal_case(r->headers->headers[i].name.data, name))
            continue;
        total += r->headers->headers[i].name.length +
                 r->headers->headers[i].value.length + 4;
    }
    if (strlen(name) > r->header_bound || strlen(value) > r->header_bound ||
        total > r->header_bound - strlen(name) ||
        total + strlen(name) > r->header_bound - strlen(value) ||
        total + strlen(name) + strlen(value) > r->header_bound - 4)
        return rotmos_fail(error, ROTMOS_LIMIT, "response_header",
                           "header bound exceeded");
    return set ? rotmos_reply_header_set(r->headers, name, value, error)
               : rotmos_reply_header_add(r->headers, name, value, error);
}

rotmos_status rotmos_response_header_set(rotmos_response *r, const char *name,
                                         const char *value, rotmos_error *error)
{
    return header(r, name, value, 1, error);
}

rotmos_status rotmos_response_header_add(rotmos_response *r, const char *name,
                                         const char *value, rotmos_error *error)
{
    return header(r, name, value, 0, error);
}

rotmos_status rotmos_response_known_length(rotmos_response *r,
                                           rotmos_u64 length,
                                           rotmos_error *error)
{
    rotmos_status status;
    status = building(r, error);
    if (status != ROTMOS_OK)
        return status;
    if (!rotmos_u64_valid(length))
        return rotmos_fail(error, ROTMOS_INVALID, "response_length",
                           "invalid length words");
    if (r->producer || r->trailers_enabled)
        return rotmos_fail(
            error, ROTMOS_STATE, "response_length",
            "length is owned by the producer or trailer framing");
    r->has_length = 1;
    r->length = length;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_response_trailers_enable(rotmos_response *r,
                                              const char *const *names,
                                              size_t count, rotmos_error *error)
{
    rotmos_status status;
    size_t length;
    size_t i;
    char *text;
    status = building(r, error);
    if (status != ROTMOS_OK)
        return status;
    if ((count != 0 && names == NULL) || r->has_length || bodyless(r) ||
        r->request->version.length != 8 ||
        memcmp(r->request->version.data, "HTTP/1.1", 8) != 0)
        return rotmos_fail(
            error, ROTMOS_INVALID, "response_trailers",
            "trailers require a body-bearing unknown-length HTTP/1.1 response");
    length = 0;
    for (i = 0; i < count; ++i) {
        if (!rotmos_header_name_valid(names[i]) || trailer_forbidden(names[i]))
            return rotmos_fail(error, ROTMOS_INVALID, "response_trailers",
                               "invalid trailer name");
        if (strlen(names[i]) + 2 > r->header_bound - length)
            return rotmos_fail(error, ROTMOS_LIMIT, "response_trailers",
                               "trailer names exceed header bound");
        length += strlen(names[i]) + (i ? 2 : 0);
    }
    text = malloc(length + 1);
    if (text == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "response_trailers",
                           "cannot copy trailer names");
    text[0] = '\0';
    for (i = 0; i < count; ++i) {
        if (i != 0)
            strcat(text, ", ");
        strcat(text, names[i]);
    }
    free(r->trailer_names);
    r->trailer_names = text;
    r->trailers_enabled = 1;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

static rotmos_status enqueue(rotmos_response *r, unsigned char *data,
                             size_t length, size_t body_start,
                             size_t body_length, rotmos_error *error)
{
    struct rotmos_response_segment *segment;
    if (length > r->bound - r->queued)
        return rotmos_fail(error, ROTMOS_AGAIN, "response_queue",
                           "wait for output capacity");
    segment = malloc(sizeof(*segment));
    if (segment == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "response_queue",
                           "cannot allocate output segment");
    segment->next = NULL;
    segment->data = data;
    segment->length = length;
    segment->body_start = body_start;
    segment->body_length = body_length;
    if (r->tail != NULL)
        r->tail->next = segment;
    else
        r->head = segment;
    r->tail = segment;
    r->queued += length;
    r->schedule(r);
    return ROTMOS_OK;
}

rotmos_status rotmos_response_begin(rotmos_response *r, rotmos_error *error)
{
    rotmos_status status;
    unsigned char *data;
    size_t length;
    size_t offset;
    size_t i;
    char decimal[21];
    size_t decimal_length;
    const char *version;
    status = building(r, error);
    if (status != ROTMOS_OK)
        return status;
    r->suppressed = bodyless(r);
    if (r->trailers_enabled && (r->has_length || r->suppressed))
        return rotmos_fail(error, ROTMOS_INVALID, "response_begin",
                           "trailers conflict with final framing");
    version = memcmp(r->request->version.data, "HTTP/1.0", 8) == 0 ? "HTTP/1.0"
                                                                   : "HTTP/1.1";
    r->chunked =
        !r->suppressed && !r->has_length && strcmp(version, "HTTP/1.1") == 0;
    if (strcmp(version, "HTTP/1.0") == 0)
        r->close_connection = 1;
    length = 128;
    for (i = 0; i < r->headers->header_count; ++i)
        length += r->headers->headers[i].name.length +
                  r->headers->headers[i].value.length + 4;
    if (r->trailer_names != NULL)
        length += strlen(r->trailer_names) + 11;
    if (length > r->header_bound || length > r->bound)
        return rotmos_fail(error, ROTMOS_LIMIT, "response_begin",
                           "response headers cannot fit their finite bound");
    data = malloc(length);
    if (data == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "response_begin",
                           "cannot allocate response headers");
    offset = (size_t)sprintf((char *)data, "%s %d %s\r\n", version,
                             r->outcome.status,
                             r->outcome.status >= 400 ? "Error" : "OK");
    for (i = 0; i < r->headers->header_count; ++i)
        offset +=
            (size_t)sprintf((char *)data + offset, "%s: %s\r\n",
                            (const char *)r->headers->headers[i].name.data,
                            (const char *)r->headers->headers[i].value.data);
    if (r->outcome.status == 205) {
        offset +=
            (size_t)sprintf((char *)data + offset, "content-length: 0\r\n");
    } else if (r->has_length && r->outcome.status != 204) {
        (void)rotmos_u64_format_decimal(r->length, decimal, sizeof(decimal),
                                        &decimal_length, NULL);
        offset += (size_t)sprintf((char *)data + offset,
                                  "content-length: %s\r\n", decimal);
    } else if (r->chunked) {
        offset += (size_t)sprintf((char *)data + offset,
                                  "transfer-encoding: chunked\r\n");
    }
    if (r->trailer_names != NULL && r->trailer_names[0] != '\0')
        offset += (size_t)sprintf((char *)data + offset, "trailer: %s\r\n",
                                  r->trailer_names);
    if (r->close_connection)
        offset +=
            (size_t)sprintf((char *)data + offset, "connection: close\r\n");
    memcpy(data + offset, "\r\n", 2);
    offset += 2;
    status = enqueue(r, data, offset, 0, 0, error);
    if (status != ROTMOS_OK) {
        free(data);
        return status;
    }
    r->phase = ROTMOS_RESPONSE_ACTIVE;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

static rotmos_status write_body(rotmos_response *r, const void *data,
                                size_t length, size_t *accepted,
                                rotmos_error *error)
{
    rotmos_status status;
    rotmos_u64 next;
    size_t count;
    size_t capacity;
    size_t prefix;
    unsigned char *copy;
    *accepted = 0;
    if (r->phase != ROTMOS_RESPONSE_ACTIVE || r->suppressed)
        return rotmos_fail(error, ROTMOS_STATE, "response_write",
                           "response does not accept payload");
    status = add_size(r->accepted, length, &next);
    if (status != ROTMOS_OK || (r->has_length && compare(next, r->length) > 0))
        return rotmos_fail(
            error, status != ROTMOS_OK ? status : ROTMOS_OVERFLOW,
            "response_write", "offered bytes exceed declared length");
    if (length == 0) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    capacity = r->bound - r->queued;
    if (capacity <= 32) {
        r->blocked = 1;
        rotmos_error_clear(error);
        return ROTMOS_AGAIN;
    }
    count = length;
    if (count > r->chunk)
        count = r->chunk;
    if (count > capacity - 32)
        count = capacity - 32;
    copy = malloc(count + 32);
    if (copy == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "response_write",
                           "cannot allocate bounded chunk");
    prefix = r->chunked ? (size_t)sprintf((char *)copy, "%lx\r\n",
                                          (unsigned long)count)
                        : 0;
    memcpy(copy + prefix, data, count);
    if (r->chunked)
        memcpy(copy + prefix + count, "\r\n", 2);
    status = enqueue(r, copy, prefix + count + (r->chunked ? 2 : 0), prefix,
                     count, error);
    if (status != ROTMOS_OK) {
        free(copy);
        return status;
    }
    (void)add_size(r->accepted, count, &r->accepted);
    *accepted = count;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_response_write(rotmos_response *r, const void *data,
                                    size_t length, size_t *accepted,
                                    rotmos_error *error)
{
    rotmos_status status;
    if (accepted != NULL)
        *accepted = 0;
    status = access_response(r, error);
    if (status != ROTMOS_OK)
        return status;
    if ((data == NULL && length != 0) || accepted == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "response_write",
                           "provide bytes and an accepted count");
    if (r->producer)
        return rotmos_fail(error, ROTMOS_STATE, "response_write",
                           "producer owns payload delivery");
    return write_body(r, data, length, accepted, error);
}

static rotmos_status make_ending(rotmos_response *r,
                                 const rotmos_header *trailers, size_t count,
                                 unsigned char **out, size_t *length,
                                 rotmos_error *error)
{
    size_t i;
    size_t size;
    size_t offset;
    char *name;
    char *value;
    unsigned char *text;
    if ((count != 0 && trailers == NULL) ||
        (count != 0 && !r->trailers_enabled))
        return rotmos_fail(error, ROTMOS_INVALID, "response_end",
                           "trailers were not enabled");
    size = r->chunked ? 5 : 0;
    for (i = 0; i < count; ++i) {
        if (trailers[i].name.data == NULL || trailers[i].value.data == NULL ||
            trailers[i].name.length > r->header_bound ||
            trailers[i].value.length > r->header_bound ||
            memchr(trailers[i].name.data, '\0', trailers[i].name.length) !=
                NULL ||
            memchr(trailers[i].value.data, '\0', trailers[i].value.length) !=
                NULL)
            return rotmos_fail(error, ROTMOS_INVALID, "response_end",
                               "invalid trailer text");
        if (trailers[i].name.length + trailers[i].value.length + 4 >
            r->header_bound - size)
            return rotmos_fail(error, ROTMOS_LIMIT, "response_end",
                               "trailers exceed finite header bound");
        size += trailers[i].name.length + trailers[i].value.length + 4;
    }
    if (size > r->bound)
        return rotmos_fail(error, ROTMOS_LIMIT, "response_end",
                           "trailers cannot fit output queue");
    text = malloc(size + 1);
    if (text == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "response_end",
                           "cannot copy trailers");
    offset = 0;
    if (r->chunked) {
        memcpy(text, "0\r\n", 3);
        offset = 3;
    }
    for (i = 0; i < count; ++i) {
        name = (char *)text + offset;
        memcpy(name, trailers[i].name.data, trailers[i].name.length);
        name[trailers[i].name.length] = '\0';
        if (!rotmos_header_name_valid(name) || trailer_forbidden(name)) {
            free(text);
            return rotmos_fail(error, ROTMOS_INVALID, "response_end",
                               "forbidden trailer name");
        }
        offset += trailers[i].name.length;
        memcpy(text + offset, ": ", 2);
        offset += 2;
        value = (char *)text + offset;
        memcpy(value, trailers[i].value.data, trailers[i].value.length);
        value[trailers[i].value.length] = '\0';
        if (!rotmos_header_value_valid(value)) {
            free(text);
            return rotmos_fail(error, ROTMOS_INVALID, "response_end",
                               "invalid trailer value");
        }
        offset += trailers[i].value.length;
        memcpy(text + offset, "\r\n", 2);
        offset += 2;
    }
    if (r->chunked) {
        memcpy(text + offset, "\r\n", 2);
        offset += 2;
    }
    *out = text;
    *length = offset;
    return ROTMOS_OK;
}

static rotmos_status end_response(rotmos_response *r,
                                  const rotmos_header *trailers, size_t count,
                                  rotmos_error *error)
{
    rotmos_status status;
    unsigned char *ending;
    unsigned char *copy;
    size_t length;
    if (r->phase != ROTMOS_RESPONSE_ACTIVE &&
        r->phase != ROTMOS_RESPONSE_FINISHING)
        return rotmos_fail(error, ROTMOS_STATE, "response_end",
                           "response has not begun");
    status = make_ending(r, trailers, count, &ending, &length, error);
    if (status != ROTMOS_OK)
        return status;
    if (r->phase == ROTMOS_RESPONSE_FINISHING) {
        status =
            length == r->ending_length && memcmp(ending, r->ending, length) == 0
                ? ROTMOS_OK
                : ROTMOS_STATE;
        free(ending);
        if (status != ROTMOS_OK)
            return rotmos_fail(error, status, "response_end",
                               "end was accepted with different trailers");
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    if (!r->suppressed && r->has_length &&
        compare(r->accepted, r->length) != 0) {
        free(ending);
        return rotmos_response_abort(r, ROTMOS_IO, error) == ROTMOS_OK
                   ? rotmos_fail(error, ROTMOS_IO, "response_end",
                                 "response ended short of its declared length")
                   : ROTMOS_STATE;
    }
    if (length != 0) {
        copy = malloc(length);
        if (copy == NULL) {
            free(ending);
            return rotmos_fail(error, ROTMOS_NOMEM, "response_end",
                               "cannot allocate final framing");
        }
        memcpy(copy, ending, length);
        status = enqueue(r, copy, length, 0, 0, error);
        if (status != ROTMOS_OK) {
            free(copy);
            free(ending);
            if (status == ROTMOS_AGAIN)
                r->blocked = 1;
            return status;
        }
    }
    r->ending = ending;
    r->ending_length = length;
    r->phase = ROTMOS_RESPONSE_FINISHING;
    r->schedule(r);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_response_end(rotmos_response *r,
                                  const rotmos_header *trailers, size_t count,
                                  rotmos_error *error)
{
    rotmos_status status;
    status = access_response(r, error);
    if (status != ROTMOS_OK)
        return status;
    if (r->producer)
        return rotmos_fail(error, ROTMOS_STATE, "response_end",
                           "producer owns end of stream");
    return end_response(r, trailers, count, error);
}

rotmos_status rotmos_response_replace_uncommitted(rotmos_response *r,
                                                  rotmos_error *error)
{
    rotmos_status status;
    rotmos_reply *headers;
    status = access_response(r, error);
    if (status != ROTMOS_OK)
        return status;
    if (r->outcome.committed || r->inflight)
        return rotmos_fail(error, ROTMOS_STATE, "response_replace",
                           "transport has committed or holds an active buffer");
    status = rotmos_reply_new(&headers, error);
    if (status != ROTMOS_OK)
        return status;
    clear_queue(r);
    clear_producer(r, ROTMOS_CANCELLED);
    (void)rotmos_reply_destroy(r->headers, NULL);
    r->headers = headers;
    free(r->trailer_names);
    free(r->ending);
    r->trailer_names = NULL;
    r->ending = NULL;
    r->ending_length = 0;
    r->has_length = r->trailers_enabled = r->chunked = r->suppressed =
        r->blocked = 0;
    r->length.high = r->length.low = r->accepted.high = r->accepted.low = 0;
    r->outcome.status = 200;
    r->outcome.cause = ROTMOS_OK;
    r->phase = ROTMOS_RESPONSE_BUILDING;
    r->schedule(r);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_response_abort(rotmos_response *r, rotmos_status cause,
                                    rotmos_error *error)
{
    rotmos_status status;
    status = access_response(r, error);
    if (status != ROTMOS_OK)
        return status;
    if (cause <= ROTMOS_EOF || cause > ROTMOS_BACKEND)
        return rotmos_fail(error, ROTMOS_INVALID, "response_abort",
                           "provide a terminal failure cause");
    r->outcome.cause = cause;
    r->phase = ROTMOS_RESPONSE_ABORTED;
    r->close_connection = 1;
    r->schedule(r);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

static rotmos_status available_producer(rotmos_response *r, rotmos_error *error)
{
    rotmos_status status;
    status = building(r, error);
    if (status != ROTMOS_OK)
        return status;
    if (r->producer)
        return rotmos_fail(error, ROTMOS_STATE, "response_producer",
                           "a producer is already installed");
    return ROTMOS_OK;
}

rotmos_status rotmos_response_producer(rotmos_response *r,
                                       rotmos_source *source,
                                       rotmos_error *error)
{
    rotmos_status status;
    status = available_producer(r, error);
    if (status != ROTMOS_OK)
        return status;
    if (source == NULL || source->owner != NULL || source->busy ||
        source->failure != ROTMOS_OK)
        return rotmos_fail(error, ROTMOS_STATE, "response_producer",
                           "source is unavailable");
    if (source->config.execution != ROTMOS_SOURCE_OWNER_LOOP)
        return rotmos_fail(error, ROTMOS_NOT_SUPPORTED, "response_producer",
                           "native-reader execution is not available");
    if (r->has_length &&
        (!source->config.has_length ||
         compare(r->length,
                 difference(source->config.length, source->position)) != 0))
        return rotmos_fail(error, ROTMOS_INVALID, "response_producer",
                           "source and response lengths differ");
    if (r->trailers_enabled && source->config.has_length)
        return rotmos_fail(error, ROTMOS_INVALID, "response_producer",
                           "known-length source conflicts with trailers");
    r->source = source;
    source->owner = r;
    r->producer = 1;
    if (source->config.has_length) {
        r->has_length = 1;
        r->length = difference(source->config.length, source->position);
    }
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_response_producer_resume(rotmos_response *r,
                                              rotmos_error *error)
{
    rotmos_status status;
    status = access_response(r, error);
    if (status != ROTMOS_OK)
        return status;
    if (r->source == NULL || r->eof)
        return rotmos_fail(error, ROTMOS_STATE, "response_resume",
                           "no live source producer");
    r->paused = 0;
    r->schedule(r);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

static int file_offset(rotmos_u64 value, off_t *out)
{
    off_t result;
    unsigned long high;
    int bit;
    result = 0;
    high = value.high;
    /* Construct without an out-of-range signed conversion or a 32-bit shift. */
    for (bit = 63; bit >= 0; --bit) {
        if (bit >= 32 && sizeof(off_t) <= 4 && (high >> (bit - 32) & 1UL))
            return 0;
        if ((sizeof(off_t) <= 4 && bit == 31 && (value.low >> 31 & 1UL)) ||
            (bit == 63 && (high >> 31 & 1UL)))
            return 0;
        result = result * 2 + (off_t)(bit >= 32 ? (high >> (bit - 32) & 1UL)
                                                : (value.low >> bit & 1UL));
    }
    *out = result;
    return 1;
}

rotmos_status rotmos_response_file_take(rotmos_response *r, int fd,
                                        rotmos_u64 offset, rotmos_u64 length,
                                        rotmos_error *error)
{
    rotmos_status status;
    struct stat st;
    rotmos_u64 end;
    off_t start;
    off_t finish;
    status = available_producer(r, error);
    if (status != ROTMOS_OK)
        return status;
    if (!rotmos_u64_valid(offset) || !rotmos_u64_valid(length) ||
        rotmos_u64_add(offset, length, &end, NULL) != ROTMOS_OK ||
        !file_offset(offset, &start) || !file_offset(end, &finish))
        return rotmos_fail(
            error, ROTMOS_OVERFLOW, "response_file",
            "file range does not fit the target seek representation");
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < finish)
        return rotmos_fail(error, ROTMOS_INVALID, "response_file",
                           "provide an open regular file containing the range");
    if (r->trailers_enabled ||
        (r->has_length && compare(r->length, length) != 0))
        return rotmos_fail(error, ROTMOS_INVALID, "response_file",
                           "file length conflicts with framing");
    r->file = fd;
    r->file_offset = offset;
    r->file_remaining = length;
    r->length = length;
    r->has_length = r->producer = 1;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_response_bytes(rotmos_response *r, int code,
                                    const char *type, rotmos_bytes bytes,
                                    rotmos_error *error)
{
    rotmos_status status;
    rotmos_reply *reply;
    rotmos_bytes payload;
    rotmos_u64 length;
    char decimal[21];
    size_t written;
    int suppressed;
    status = available_producer(r, error);
    if (status != ROTMOS_OK)
        return status;
    if (r->has_length || r->trailers_enabled || r->headers->header_count != 0)
        return rotmos_fail(error, ROTMOS_STATE, "response_bytes",
                           "complete reply requires an empty declaration");
    /* Bound the copied convenience. Larger existing values use the staged
     * facade, which transfers its owned payload without a second whole copy. */
    if (bytes.data == NULL && bytes.length != 0)
        return rotmos_fail(error, ROTMOS_INVALID, "response_bytes",
                           "provide valid bytes");
    suppressed = r->request->method == ROTMOS_HTTP_HEAD || code == 204 ||
                 code == 205 || code == 304;
    if (!suppressed && bytes.length > r->bound)
        return rotmos_fail(error, ROTMOS_LIMIT, "response_bytes",
                           "complete copied reply exceeds output bound");
    status = rotmos_reply_new(&reply, error);
    if (status != ROTMOS_OK)
        return status;
    payload = bytes;
    if (suppressed) {
        payload.data = NULL;
        payload.length = 0;
    }
    status = rotmos_reply_bytes(reply, code, type, payload, error);
    if (status == ROTMOS_OK &&
        (r->request->method == ROTMOS_HTTP_HEAD || code == 304)) {
        status = rotmos_u64_from_size(bytes.length, &length, error);
        if (status == ROTMOS_OK)
            status = rotmos_u64_format_decimal(length, decimal, sizeof(decimal),
                                               &written, error);
        if (status == ROTMOS_OK)
            status = rotmos_reply_header_add(reply, "content-length", decimal,
                                             error);
    }
    if (status == ROTMOS_OK)
        status = rotmos_response_submit(r, reply, error);
    (void)rotmos_reply_destroy(reply, NULL);
    return status;
}

rotmos_status rotmos_response_submit(rotmos_response *r, rotmos_reply *reply,
                                     rotmos_error *error)
{
    rotmos_status status;
    rotmos_reply *saved;
    rotmos_u64 saved_length;
    int saved_status;
    int saved_has_length;
    int saved_close;
    size_t i;
    rotmos_u64 representation;
    int has_representation;
    status = available_producer(r, error);
    if (status != ROTMOS_OK)
        return status;
    saved = r->headers;
    saved_status = r->outcome.status;
    saved_length = r->length;
    saved_has_length = r->has_length;
    saved_close = r->close_connection;
    status = rotmos_reply_new(&r->headers, error);
    if (status != ROTMOS_OK) {
        r->headers = saved;
        return status;
    }
    r->outcome.status = reply->selected ? reply->status : 204;
    has_representation = 0;
    for (i = 0; status == ROTMOS_OK && i < reply->header_count; ++i) {
        if (rotmos_text_equal_case(reply->headers[i].name.data,
                                   "content-length")) {
            if (r->request->method == ROTMOS_HTTP_HEAD ||
                r->outcome.status == 304) {
                status = rotmos_u64_from_decimal(reply->headers[i].value.data,
                                                 reply->headers[i].value.length,
                                                 &representation, error);
                has_representation = 1;
            }
        } else if (!forbidden(reply->headers[i].name.data)) {
            status = header(r, reply->headers[i].name.data,
                            reply->headers[i].value.data, 0, error);
        }
    }
    if (status == ROTMOS_OK && reply->content_type != NULL)
        status = header(r, "content-type", reply->content_type, 1, error);
    if (status == ROTMOS_OK) {
        if (reply->kind == ROTMOS_REPLY_SOURCE) {
            reply->source->owner = NULL;
            status = rotmos_response_producer(r, reply->source, error);
            if (status != ROTMOS_OK)
                reply->source->owner = reply;
        } else if (reply->kind == ROTMOS_REPLY_FILE) {
            rotmos_u64 zero;
            zero.high = zero.low = 0;
            status = rotmos_response_file_take(r, reply->file, zero,
                                               reply->file_length, error);
        } else {
            r->has_length = !bodyless(r) || reply->body_length != 0;
            (void)rotmos_u64_from_size(reply->body_length, &r->length, NULL);
        }
    }
    if (status == ROTMOS_OK && has_representation) {
        r->has_length = 1;
        r->length = representation;
    }
    if (status == ROTMOS_OK)
        status = rotmos_response_begin(r, error);
    if (status != ROTMOS_OK) {
        if (r->source != NULL) {
            r->source->owner = reply;
            r->source = NULL;
        }
        r->file = -1;
        r->producer = r->chunked = r->suppressed = 0;
        r->has_length = saved_has_length;
        r->length = saved_length;
        r->close_connection = saved_close;
        (void)rotmos_reply_destroy(r->headers, NULL);
        r->headers = saved;
        r->outcome.status = saved_status;
        return status;
    }
    (void)rotmos_reply_destroy(saved, NULL);
    if (reply->kind == ROTMOS_REPLY_SOURCE)
        reply->source = NULL;
    else if (reply->kind == ROTMOS_REPLY_FILE)
        reply->file = -1;
    else {
        r->memory = reply->body;
        r->memory_length = reply->body_length;
        reply->body = NULL;
        reply->body_length = 0;
        r->producer = 1;
    }
    return ROTMOS_OK;
}

void rotmos_response_fail(rotmos_response *r, rotmos_status cause)
{
    if (!r->outcome.committed && !r->inflight) {
        retire_source(r, cause);
        if (rotmos_response_replace_uncommitted(r, NULL) == ROTMOS_OK) {
            r->outcome.status = 500;
            r->has_length = 1;
            if (rotmos_response_begin(r, NULL) == ROTMOS_OK &&
                end_response(r, NULL, 0, NULL) == ROTMOS_OK)
                return;
        }
    }
    (void)rotmos_response_abort(r, cause, NULL);
}

void rotmos_response_step(rotmos_response *r)
{
    rotmos_source *retired;
    unsigned char *buffer;
    size_t capacity;
    size_t count;
    size_t accepted;
    size_t remaining;
    off_t offset;
    ssize_t result;
    rotmos_status status;
    rotmos_u64 amount;
    r->busy = 1;
    rotmos_reply_retire(r->request->reply);
    r->busy = 0;
    while (r->retired != NULL) {
        retired = r->retired;
        r->retired = retired->retired_next;
        r->busy = 1;
        rotmos_source_release(retired, retired->failure);
        r->busy = 0;
    }
    if (r->phase != ROTMOS_RESPONSE_ACTIVE)
        return;
    if (r->suppressed) {
        r->eof = 1;
        (void)end_response(r, NULL, 0, NULL);
        return;
    }
    if (r->blocked && r->bound - r->queued > 32) {
        r->blocked = 0;
        if (r->writable != NULL)
            r->writable(r, r->userdata);
        if (r->phase != ROTMOS_RESPONSE_ACTIVE)
            return;
    }
    if (!r->producer || r->paused)
        return;
    if (r->eof) {
        status = end_response(r, NULL, 0, NULL);
        if (status != ROTMOS_OK && status != ROTMOS_AGAIN)
            rotmos_response_fail(r, status);
        return;
    }
    capacity = r->bound - r->queued;
    if (capacity <= 32)
        return;
    capacity -= 32;
    if (capacity > r->chunk)
        capacity = r->chunk;
    buffer = NULL;
    count = 0;
    status = ROTMOS_EOF;
    if (r->source != NULL || r->file != -1) {
        buffer = malloc(capacity);
        if (buffer == NULL) {
            rotmos_response_fail(r, ROTMOS_NOMEM);
            return;
        }
    }
    if (r->source != NULL) {
        r->busy = 1;
        status =
            rotmos_source_pull(r->source, r, buffer, capacity, &count, NULL);
        r->busy = 0;
    } else if (r->file != -1 &&
               (r->file_remaining.high || r->file_remaining.low)) {
        if (rotmos_u64_to_size(r->file_remaining, &remaining, NULL) ==
                ROTMOS_OK &&
            capacity > remaining)
            capacity = remaining;
        (void)file_offset(r->file_offset, &offset);
        do {
            result = pread(r->file, buffer, capacity, offset);
        } while (result == -1 && errno == EINTR);
        status = result > 0 ? ROTMOS_OK : ROTMOS_IO;
        if (result > 0) {
            count = (size_t)result;
            (void)add_size(r->file_offset, count, &r->file_offset);
            (void)rotmos_u64_from_size(count, &amount, NULL);
            r->file_remaining = difference(r->file_remaining, amount);
        }
    } else if (r->memory_position < r->memory_length) {
        count = r->memory_length - r->memory_position;
        if (count > capacity)
            count = capacity;
        status = ROTMOS_OK;
    }
    if (status == ROTMOS_OK) {
        status = write_body(
            r, buffer != NULL ? buffer : r->memory + r->memory_position, count,
            &accepted, NULL);
        if (status == ROTMOS_OK && accepted != count)
            status = ROTMOS_IO;
        if (buffer == NULL)
            r->memory_position += accepted;
    } else if (status == ROTMOS_AGAIN) {
        r->paused = 1;
    } else if (status == ROTMOS_EOF) {
        r->eof = 1;
        status = end_response(r, NULL, 0, NULL);
    }
    free(buffer);
    if (status != ROTMOS_OK && status != ROTMOS_AGAIN)
        rotmos_response_fail(r, status);
}

void rotmos_response_sent(rotmos_response *r, size_t offset, size_t count,
                          int retry)
{
    size_t start;
    size_t end;
    struct rotmos_response_segment *segment;
    segment = r->head;
    if (count != 0 || retry)
        r->outcome.committed = 1;
    if (add_size(r->outcome.wire_bytes, count, &r->outcome.wire_bytes) !=
        ROTMOS_OK) {
        (void)rotmos_response_abort(r, ROTMOS_OVERFLOW, NULL);
        return;
    }
    start = offset > segment->body_start ? offset : segment->body_start;
    end = offset + count < segment->body_start + segment->body_length
              ? offset + count
              : segment->body_start + segment->body_length;
    if (end > start && add_size(r->outcome.body_bytes, end - start,
                                &r->outcome.body_bytes) != ROTMOS_OK)
        (void)rotmos_response_abort(r, ROTMOS_OVERFLOW, NULL);
}

void rotmos_response_shift(rotmos_response *r)
{
    struct rotmos_response_segment *segment;
    segment = r->head;
    r->head = segment->next;
    if (r->head == NULL)
        r->tail = NULL;
    r->queued -= segment->length;
    free(segment->data);
    free(segment);
}

void rotmos_response_terminal(rotmos_response *r)
{
    rotmos_u64 now;
    if (r->terminal_notified)
        return;
    r->terminal_notified = 1;
    if (rotmos_loop_monotonic(&now, NULL) == ROTMOS_OK &&
        compare(now, r->started) >= 0)
        r->outcome.duration_ms = difference(now, r->started);
    if (r->phase != ROTMOS_RESPONSE_ABORTED)
        r->phase = ROTMOS_RESPONSE_COMPLETE;
    r->busy = 1;
    rotmos_reply_retire(r->request->reply);
    clear_producer(r, r->outcome.cause != ROTMOS_OK
                          ? r->outcome.cause
                          : (r->suppressed ? ROTMOS_CANCELLED : ROTMOS_OK));
    while (r->retired != NULL) {
        rotmos_source *source;
        source = r->retired;
        r->retired = source->retired_next;
        rotmos_source_release(source, source->failure);
    }
    if (r->closed != NULL)
        r->closed(r, &r->outcome, r->userdata);
    r->busy = 0;
}

void rotmos_response_release(rotmos_response *r)
{
    if (r == NULL)
        return;
    rotmos_response_terminal(r);
    clear_queue(r);
    (void)rotmos_reply_destroy(r->headers, NULL);
    free(r->ending);
    free(r->trailer_names);
    free(r);
}

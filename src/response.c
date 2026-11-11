#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "response_internal.h"
#include <knurl/response.h>
#include "body_internal.h"
#include "server_internal.h"
#include "loop_internal.h"
#include "policy_internal.h"

#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static knurl_status access_response(knurl_response *r, knurl_error *error)
{
    if (r == NULL)
        return knurl_fail(error, KNURL_INVALID, "response",
                          "response is required");
    if (r->impl->busy || r->impl->phase >= KNURL_RESPONSE_COMPLETE)
        return knurl_fail(error, KNURL_STATE, "response",
                          "response is busy or terminal");
    return knurl_loop_owner(r->impl->request->impl->server->impl->loop, error);
}

static knurl_status building(knurl_response *r, knurl_error *error)
{
    knurl_status status;
    status = access_response(r, error);
    if (status == KNURL_OK && r->impl->phase != KNURL_RESPONSE_BUILDING)
        return knurl_fail(error, KNURL_STATE, "response",
                          "headers have already begun");
    return status;
}

static int forbidden(const char *name)
{
    return knurl_text_equal_case(name, "content-length") ||
           knurl_text_equal_case(name, "transfer-encoding") ||
           knurl_text_equal_case(name, "connection") ||
           knurl_text_equal_case(name, "keep-alive") ||
           knurl_text_equal_case(name, "trailer") ||
           knurl_text_equal_case(name, "upgrade");
}

static int trailer_forbidden(const char *name)
{
    return forbidden(name) || knurl_text_equal_case(name, "host") ||
           knurl_text_equal_case(name, "authorization") ||
           knurl_text_equal_case(name, "proxy-authorization") ||
           knurl_text_equal_case(name, "www-authenticate") ||
           knurl_text_equal_case(name, "proxy-authenticate") ||
           knurl_text_equal_case(name, "content-encoding") ||
           knurl_text_equal_case(name, "content-type") ||
           knurl_text_equal_case(name, "content-range") ||
           knurl_text_equal_case(name, "set-cookie");
}

static int status_bodyless(int code)
{
    return code < 200 || code == 204 || code == 205 || code == 304;
}
static int bodyless(knurl_response *r)
{
    return status_bodyless(r->impl->outcome.status) ||
           r->impl->request->impl->method == KNURL_HTTP_HEAD;
}

static int compare(knurl_u64 a, knurl_u64 b)
{
    int order;
    (void)knurl_u64_compare(a, b, &order, NULL);
    return order;
}

static knurl_u64 difference(knurl_u64 a, knurl_u64 b)
{
    knurl_u64 value;
    value.high = a.high - b.high - (a.low < b.low ? 1 : 0);
    value.low = (a.low - b.low) & 0xffffffffUL;
    return value;
}

static knurl_status add_size(knurl_u64 a, size_t count, knurl_u64 *out)
{
    knurl_u64 value;
    knurl_status status;
    status = knurl_u64_from_size(count, &value, NULL);
    return status == KNURL_OK ? knurl_u64_add(a, value, out, NULL) : status;
}

static void clear_queue(knurl_response *r)
{
    while (r->impl->head != NULL)
        knurl_response_shift(r);
    free(r->impl->pending_head);
    r->impl->pending_head = NULL;
    r->impl->pending_head_length = r->impl->pending_head_position = 0;
}

static void retire_source(knurl_response *r, knurl_status cause)
{
    knurl_source *source;
    source = r->impl->source;
    if (source != NULL) {
        knurl_source_revoke(source);
        source->impl->failure = cause;
        source->impl->retired_next = r->impl->retired;
        r->impl->retired = source;
        r->impl->source = NULL;
    }
}

static void clear_producer(knurl_response *r, knurl_status cause)
{
    retire_source(r, cause);
    if (r->impl->file != -1)
        (void)close(r->impl->file);
    r->impl->file = -1;
    free(r->impl->memory);
    r->impl->memory = NULL;
    r->impl->memory_length = r->impl->memory_position = 0;
    r->impl->producer = r->impl->paused = r->impl->eof = 0;
}

knurl_status knurl_response_create(knurl_request *request,
                                   void (*schedule)(knurl_response *),
                                   knurl_response **out, knurl_error *error)
{
    knurl_response *r;
    knurl_status status;
    *out = NULL;
    r = calloc(1, sizeof(struct knurl_response_impl));
    if (r == NULL)
        return knurl_fail(error, KNURL_NOMEM, "response",
                          "cannot allocate response");
    knurl_response_receiver_init(r);
    r->impl->request = request;
    r->impl->schedule = schedule;
    r->impl->file = -1;
    r->impl->outcome.status = 200;
    status = knurl_loop_monotonic(&r->impl->started, error);
    if (status != KNURL_OK) {
        free(r);
        return status;
    }
    r->impl->bound = request->impl->server->impl->config.io.output_queue_bytes;
    r->impl->chunk = request->impl->server->impl->config.io.chunk_bytes;
    r->impl->header_bound =
        request->impl->server->impl->config.limits.max_response_header_bytes;
    if (r->impl->header_bound == 0)
        r->impl->header_bound = (size_t)-1;
    status = knurl_reply_new(&r->impl->headers, error);
    if (status != KNURL_OK) {
        free(r);
        return status;
    }
    *out = r;
    return KNURL_OK;
}

knurl_response *knurl_request_response(knurl_request *request)
{
    return request != NULL ? request->impl->response : NULL;
}

knurl_response_phase knurl_response_state(const knurl_response *r)
{
    return r != NULL ? r->impl->phase : KNURL_RESPONSE_ABORTED;
}

int knurl_response_is_committed(const knurl_response *r)
{
    return r != NULL && r->impl->outcome.committed;
}

size_t knurl_response_queued_bytes(const knurl_response *r)
{
    return r != NULL ? r->impl->queued : 0;
}

knurl_status knurl_response_callbacks(knurl_response *r,
                                      knurl_response_writable_fn writable,
                                      knurl_response_close_fn closed,
                                      void *userdata, knurl_error *error)
{
    knurl_status status;
    status = building(r, error);
    if (status != KNURL_OK)
        return status;
    r->impl->writable = writable;
    r->impl->closed = closed;
    r->impl->userdata = userdata;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_response_status(knurl_response *r, int code,
                                   knurl_error *error)
{
    knurl_status status;
    status = building(r, error);
    if (status != KNURL_OK)
        return status;
    if (code < 200 || code > 599)
        return knurl_fail(error, KNURL_INVALID, "response_status",
                          "final status must be 200..599");
    r->impl->outcome.status = code;
    knurl_error_clear(error);
    return KNURL_OK;
}

static knurl_status header(knurl_response *r, const char *name,
                           const char *value, int set, int staged,
                           knurl_error *error)
{
    knurl_status status;
    size_t total;
    size_t i;
    status = building(r, error);
    if (status != KNURL_OK)
        return status;
    if (!knurl_header_name_valid(name) || !knurl_header_value_valid(value) ||
        (!staged && forbidden(name)))
        return knurl_fail(error, KNURL_INVALID, "response_header",
                          "invalid or transport-owned field");
    total = 0;
    for (i = 0; i < r->impl->headers->impl->header_count; ++i) {
        if (set && knurl_text_equal_case(
                       r->impl->headers->impl->headers[i].name.data, name)) {
            if (staged)
                name = r->impl->headers->impl->headers[i].name.data;
            continue;
        }
        total += r->impl->headers->impl->headers[i].name.length +
                 r->impl->headers->impl->headers[i].value.length + 4;
    }
    if (strlen(name) > r->impl->header_bound ||
        strlen(value) > r->impl->header_bound ||
        total > r->impl->header_bound - strlen(name) ||
        total + strlen(name) > r->impl->header_bound - strlen(value) ||
        total + strlen(name) + strlen(value) > r->impl->header_bound - 4)
        return knurl_fail(error, KNURL_LIMIT, "response_header",
                          "header bound exceeded");
    return set ? knurl_reply_header_set(r->impl->headers, name, value, error)
               : knurl_reply_header_add(r->impl->headers, name, value, error);
}

knurl_status knurl_response_header_set(knurl_response *r, const char *name,
                                       const char *value, knurl_error *error)
{
    return header(r, name, value, 1, 0, error);
}

knurl_status knurl_response_header_add(knurl_response *r, const char *name,
                                       const char *value, knurl_error *error)
{
    return header(r, name, value, 0, 0, error);
}

knurl_status knurl_response_known_length(knurl_response *r, knurl_u64 length,
                                         knurl_error *error)
{
    knurl_status status;
    status = building(r, error);
    if (status != KNURL_OK)
        return status;
    if (!knurl_u64_valid(length))
        return knurl_fail(error, KNURL_INVALID, "response_length",
                          "invalid length words");
    if (r->impl->producer || r->impl->trailers_enabled)
        return knurl_fail(error, KNURL_STATE, "response_length",
                          "length is owned by the producer or trailer framing");
    r->impl->has_length = 1;
    r->impl->length = length;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_response_trailers_enable(knurl_response *r,
                                            const char *const *names,
                                            size_t count, knurl_error *error)
{
    knurl_status status;
    size_t length;
    size_t i;
    char *text;
    status = building(r, error);
    if (status != KNURL_OK)
        return status;
    if ((count != 0 && names == NULL) || r->impl->has_length || bodyless(r) ||
        r->impl->request->impl->version.length != 8 ||
        memcmp(r->impl->request->impl->version.data, "HTTP/1.1", 8) != 0)
        return knurl_fail(
            error, KNURL_INVALID, "response_trailers",
            "trailers require a body-bearing unknown-length HTTP/1.1 response");
    length = 0;
    for (i = 0; i < count; ++i) {
        if (!knurl_header_name_valid(names[i]) || trailer_forbidden(names[i]))
            return knurl_fail(error, KNURL_INVALID, "response_trailers",
                              "invalid trailer name");
        if (strlen(names[i]) + 2 > r->impl->header_bound - length)
            return knurl_fail(error, KNURL_LIMIT, "response_trailers",
                              "trailer names exceed header bound");
        length += strlen(names[i]) + (i ? 2 : 0);
    }
    text = malloc(length + 1);
    if (text == NULL)
        return knurl_fail(error, KNURL_NOMEM, "response_trailers",
                          "cannot copy trailer names");
    text[0] = '\0';
    for (i = 0; i < count; ++i) {
        if (i != 0)
            strcat(text, ", ");
        strcat(text, names[i]);
    }
    free(r->impl->trailer_names);
    r->impl->trailer_names = text;
    r->impl->trailers_enabled = 1;
    knurl_error_clear(error);
    return KNURL_OK;
}

static knurl_status enqueue(knurl_response *r, unsigned char *data,
                            size_t length, size_t body_start,
                            size_t body_length, knurl_error *error)
{
    struct knurl_response_segment *segment;
    if (length > r->impl->bound - r->impl->queued) {
        r->impl->blocked = 1;
        r->impl->writable_need = length;
        return knurl_fail(error, KNURL_AGAIN, "response_queue",
                          "wait for output capacity");
    }
    segment = malloc(sizeof(*segment));
    if (segment == NULL)
        return knurl_fail(error, KNURL_NOMEM, "response_queue",
                          "cannot allocate output segment");
    segment->next = NULL;
    segment->data = data;
    segment->length = length;
    segment->body_start = body_start;
    segment->body_length = body_length;
    segment->interim = 0;
    if (r->impl->tail != NULL)
        r->impl->tail->next = segment;
    else
        r->impl->head = segment;
    r->impl->tail = segment;
    r->impl->queued += length;
    r->impl->schedule(r);
    return KNURL_OK;
}

knurl_status knurl_response_interim(knurl_response *r, int code,
                                    const knurl_header *fields, size_t count,
                                    knurl_error *error)
{
    knurl_status status;
    knurl_reply *copy;
    unsigned char *data;
    const char *line;
    char *name;
    char *value;
    size_t total;
    size_t offset;
    size_t i;
    size_t bound;
    status = building(r, error);
    if (status != KNURL_OK)
        return status;
    if ((code != 100 && code != 103) || (count != 0 && fields == NULL))
        return knurl_fail(error, KNURL_INVALID, "response_interim",
                          "provide bounded HTTP 100/103 metadata");
    if (memcmp(r->impl->request->impl->version.data, "HTTP/1.1", 8) != 0)
        return knurl_fail(error, KNURL_NOT_SUPPORTED, "response_interim",
                          "interim responses require HTTP/1.1");
    if (r->impl->interim_count >= 16 || count > 100)
        return knurl_fail(error, KNURL_LIMIT, "response_interim",
                          "interim response count exceeded");
    line = code == 100 ? "HTTP/1.1 100 Continue\r\n"
                       : "HTTP/1.1 103 Early Hints\r\n";
    total = strlen(line) + 2;
    bound = r->impl->header_bound < r->impl->bound ? r->impl->header_bound
                                                   : r->impl->bound;
    if (total > bound)
        return knurl_fail(error, KNURL_LIMIT, "response_interim",
                          "interim head cannot fit its bound");
    status = knurl_reply_new(&copy, error);
    if (status != KNURL_OK)
        return status;
    for (i = 0; i < count; ++i) {
        if (fields[i].name.data == NULL || fields[i].name.length == 0 ||
            (fields[i].value.data == NULL && fields[i].value.length != 0)) {
            status = knurl_fail(error, KNURL_INVALID, "response_interim",
                                "invalid or oversized interim fields");
            break;
        }
        if (fields[i].name.length > bound - 4 ||
            fields[i].value.length > bound - 4 - fields[i].name.length ||
            total >
                bound - 4 - fields[i].name.length - fields[i].value.length) {
            status = knurl_fail(error, KNURL_LIMIT, "response_interim",
                                "interim fields cannot fit their bound");
            break;
        }
        if (memchr(fields[i].name.data, 0, fields[i].name.length) != NULL ||
            (fields[i].value.length != 0 &&
             memchr(fields[i].value.data, 0, fields[i].value.length) != NULL)) {
            status = knurl_fail(error, KNURL_INVALID, "response_interim",
                                "interim field contains NUL");
            break;
        }
        name = malloc(fields[i].name.length + 1);
        value = malloc(fields[i].value.length + 1);
        if (name == NULL || value == NULL) {
            free(name);
            free(value);
            status = knurl_fail(error, KNURL_NOMEM, "response_interim",
                                "cannot copy interim fields");
            break;
        }
        memcpy(name, fields[i].name.data, fields[i].name.length);
        name[fields[i].name.length] = 0;
        if (fields[i].value.length != 0)
            memcpy(value, fields[i].value.data, fields[i].value.length);
        value[fields[i].value.length] = 0;
        if (forbidden(name))
            status = knurl_fail(error, KNURL_INVALID, "response_interim",
                                "interim field is transport-owned");
        else
            status = knurl_reply_header_add(copy, name, value, error);
        free(name);
        free(value);
        if (status != KNURL_OK)
            break;
        total += fields[i].name.length + fields[i].value.length + 4;
    }
    data = NULL;
    if (status == KNURL_OK &&
        (total > r->impl->header_bound || total > r->impl->bound ||
         total > 131072UL - r->impl->interim_bytes))
        status = knurl_fail(error, KNURL_LIMIT, "response_interim",
                            "interim response exceeds finite budget");
    if (status == KNURL_OK) {
        data = malloc(total);
        if (data == NULL)
            status = knurl_fail(error, KNURL_NOMEM, "response_interim",
                                "cannot allocate interim response");
    }
    if (status == KNURL_OK) {
        offset = strlen(line);
        memcpy(data, line, offset);
        for (i = 0; i < copy->impl->header_count; ++i) {
            memcpy(data + offset, copy->impl->headers[i].name.data,
                   copy->impl->headers[i].name.length);
            offset += copy->impl->headers[i].name.length;
            memcpy(data + offset, ": ", 2);
            offset += 2;
            memcpy(data + offset, copy->impl->headers[i].value.data,
                   copy->impl->headers[i].value.length);
            offset += copy->impl->headers[i].value.length;
            memcpy(data + offset, "\r\n", 2);
            offset += 2;
        }
        memcpy(data + offset, "\r\n", 2);
        status = enqueue(r, data, total, 0, 0, error);
        if (status == KNURL_OK) {
            r->impl->tail->interim = 1;
            ++r->impl->interim_count;
            r->impl->interim_bytes += total;
        }
    }
    if (status != KNURL_OK)
        free(data);
    (void)knurl_reply_destroy(copy, NULL);
    if (status == KNURL_OK)
        knurl_error_clear(error);
    return status;
}

static knurl_status flush_head(knurl_response *r, knurl_error *error)
{
    unsigned char *copy;
    size_t count;
    knurl_status status;
    if (r->impl->pending_head == NULL)
        return KNURL_OK;
    count = r->impl->pending_head_length - r->impl->pending_head_position;
    if (count > r->impl->bound - r->impl->queued)
        count = r->impl->bound - r->impl->queued;
    if (count == 0)
        return KNURL_AGAIN;
    copy = malloc(count);
    if (copy == NULL)
        return knurl_fail(error, KNURL_NOMEM, "response_head",
                          "cannot queue response headers");
    memcpy(copy, r->impl->pending_head + r->impl->pending_head_position, count);
    status = enqueue(r, copy, count, 0, 0, error);
    if (status != KNURL_OK) {
        free(copy);
        return status;
    }
    r->impl->pending_head_position += count;
    if (r->impl->pending_head_position == r->impl->pending_head_length) {
        free(r->impl->pending_head);
        r->impl->pending_head = NULL;
        r->impl->pending_head_length = r->impl->pending_head_position = 0;
    }
    return KNURL_OK;
}

knurl_status knurl_response_begin(knurl_response *r, knurl_error *error)
{
    knurl_status status;
    knurl_owned_bytes head;
    const char *version;
    status = building(r, error);
    if (status != KNURL_OK)
        return status;
    r->impl->suppressed = bodyless(r);
    if (r->impl->trailers_enabled &&
        (r->impl->has_length || r->impl->suppressed))
        return knurl_fail(error, KNURL_INVALID, "response_begin",
                          "trailers conflict with final framing");
    version = memcmp(r->impl->request->impl->version.data, "HTTP/1.0", 8) == 0
                  ? "HTTP/1.0"
                  : "HTTP/1.1";
    r->impl->chunked = !r->impl->suppressed && !r->impl->has_length &&
                       strcmp(version, "HTTP/1.1") == 0;
    if (strcmp(version, "HTTP/1.0") == 0)
        r->impl->close_connection = 1;
    status = knurl_response_head(r, &head, error);
    if (status != KNURL_OK)
        return status;
    r->impl->pending_head = head.data;
    r->impl->pending_head_length = head.length;
    status = flush_head(r, error);
    if (status != KNURL_OK && status != KNURL_AGAIN) {
        free(r->impl->pending_head);
        r->impl->pending_head = NULL;
        r->impl->pending_head_length = r->impl->pending_head_position = 0;
        return status;
    }
    r->impl->phase = KNURL_RESPONSE_ACTIVE;
    r->impl->schedule(r);
    knurl_error_clear(error);
    return KNURL_OK;
}

static knurl_status write_body(knurl_response *r, const void *data,
                               size_t length, size_t *accepted,
                               knurl_error *error)
{
    knurl_status status;
    knurl_u64 next;
    size_t count;
    size_t capacity;
    size_t prefix;
    unsigned char *copy;
    *accepted = 0;
    if ((r->impl->phase != KNURL_RESPONSE_ACTIVE &&
         !(r->impl->phase == KNURL_RESPONSE_FINISHING &&
           r->impl->complete_reply)) ||
        r->impl->suppressed)
        return knurl_fail(error, KNURL_STATE, "response_write",
                          "response does not accept payload");
    status = add_size(r->impl->accepted, length, &next);
    if (status != KNURL_OK ||
        (r->impl->has_length && compare(next, r->impl->length) > 0))
        return knurl_fail(error, status != KNURL_OK ? status : KNURL_OVERFLOW,
                          "response_write",
                          "offered bytes exceed declared length");
    if (length == 0) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    if (r->impl->pending_head != NULL) {
        r->impl->blocked = 1;
        r->impl->writable_need = 1;
        knurl_error_clear(error);
        return KNURL_AGAIN;
    }
    capacity = r->impl->bound - r->impl->queued;
    if (capacity <= KNURL_RESPONSE_CHUNK_OVERHEAD) {
        r->impl->blocked = 1;
        r->impl->writable_need = KNURL_RESPONSE_CHUNK_OVERHEAD + 1;
        knurl_error_clear(error);
        return KNURL_AGAIN;
    }
    count = length;
    if (count > r->impl->chunk)
        count = r->impl->chunk;
    if (count > capacity - KNURL_RESPONSE_CHUNK_OVERHEAD)
        count = capacity - KNURL_RESPONSE_CHUNK_OVERHEAD;
    copy = malloc(count + KNURL_RESPONSE_CHUNK_OVERHEAD);
    if (copy == NULL)
        return knurl_fail(error, KNURL_NOMEM, "response_write",
                          "cannot allocate bounded chunk");
    prefix = r->impl->chunked ? (size_t)sprintf((char *)copy, "%lx\r\n",
                                                (unsigned long)count)
                              : 0;
    memcpy(copy + prefix, data, count);
    if (r->impl->chunked)
        memcpy(copy + prefix + count, "\r\n", 2);
    status = enqueue(r, copy, prefix + count + (r->impl->chunked ? 2 : 0),
                     prefix, count, error);
    if (status != KNURL_OK) {
        free(copy);
        return status;
    }
    (void)add_size(r->impl->accepted, count, &r->impl->accepted);
    *accepted = count;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_response_write(knurl_response *r, const void *data,
                                  size_t length, size_t *accepted,
                                  knurl_error *error)
{
    knurl_status status;
    if (accepted != NULL)
        *accepted = 0;
    status = access_response(r, error);
    if (status != KNURL_OK)
        return status;
    if ((data == NULL && length != 0) || accepted == NULL)
        return knurl_fail(error, KNURL_INVALID, "response_write",
                          "provide bytes and an accepted count");
    if (r->impl->producer)
        return knurl_fail(error, KNURL_STATE, "response_write",
                          "producer owns payload delivery");
    return write_body(r, data, length, accepted, error);
}

static knurl_status make_ending(knurl_response *r, const knurl_header *trailers,
                                size_t count, unsigned char **out,
                                size_t *length, knurl_error *error)
{
    size_t i;
    size_t size;
    size_t offset;
    char *name;
    char *value;
    unsigned char *text;
    *out = NULL;
    *length = 0;
    if ((count != 0 && trailers == NULL) ||
        (count != 0 && !r->impl->trailers_enabled))
        return knurl_fail(error, KNURL_INVALID, "response_end",
                          "trailers were not enabled");
    size = r->impl->chunked ? 5 : 0;
    for (i = 0; i < count; ++i) {
        if (trailers[i].name.data == NULL || trailers[i].value.data == NULL ||
            trailers[i].name.length > r->impl->header_bound ||
            trailers[i].value.length > r->impl->header_bound ||
            memchr(trailers[i].name.data, '\0', trailers[i].name.length) !=
                NULL ||
            memchr(trailers[i].value.data, '\0', trailers[i].value.length) !=
                NULL)
            return knurl_fail(error, KNURL_INVALID, "response_end",
                              "invalid trailer text");
        if (trailers[i].name.length + trailers[i].value.length + 4 >
            r->impl->header_bound - size)
            return knurl_fail(error, KNURL_LIMIT, "response_end",
                              "trailers exceed finite header bound");
        size += trailers[i].name.length + trailers[i].value.length + 4;
    }
    if (size > r->impl->bound)
        return knurl_fail(error, KNURL_LIMIT, "response_end",
                          "trailers cannot fit output queue");
    text = malloc(size + 1);
    if (text == NULL)
        return knurl_fail(error, KNURL_NOMEM, "response_end",
                          "cannot copy trailers");
    offset = 0;
    if (r->impl->chunked) {
        memcpy(text, "0\r\n", 3);
        offset = 3;
    }
    for (i = 0; i < count; ++i) {
        name = (char *)text + offset;
        memcpy(name, trailers[i].name.data, trailers[i].name.length);
        name[trailers[i].name.length] = '\0';
        if (!knurl_header_name_valid(name) || trailer_forbidden(name)) {
            free(text);
            return knurl_fail(error, KNURL_INVALID, "response_end",
                              "forbidden trailer name");
        }
        offset += trailers[i].name.length;
        memcpy(text + offset, ": ", 2);
        offset += 2;
        value = (char *)text + offset;
        memcpy(value, trailers[i].value.data, trailers[i].value.length);
        value[trailers[i].value.length] = '\0';
        if (!knurl_header_value_valid(value)) {
            free(text);
            return knurl_fail(error, KNURL_INVALID, "response_end",
                              "invalid trailer value");
        }
        offset += trailers[i].value.length;
        memcpy(text + offset, "\r\n", 2);
        offset += 2;
    }
    if (r->impl->chunked) {
        memcpy(text + offset, "\r\n", 2);
        offset += 2;
    }
    *out = text;
    *length = offset;
    return KNURL_OK;
}

static knurl_status end_response(knurl_response *r,
                                 const knurl_header *trailers, size_t count,
                                 knurl_error *error)
{
    knurl_status status;
    unsigned char *ending;
    unsigned char *copy;
    size_t length;
    if (r->impl->phase != KNURL_RESPONSE_ACTIVE &&
        r->impl->phase != KNURL_RESPONSE_FINISHING)
        return knurl_fail(error, KNURL_STATE, "response_end",
                          "response has not begun");
    status = make_ending(r, trailers, count, &ending, &length, error);
    if (status != KNURL_OK)
        return status;
    if (r->impl->phase == KNURL_RESPONSE_FINISHING && r->impl->ending != NULL) {
        status = length == r->impl->ending_length &&
                         memcmp(ending, r->impl->ending, length) == 0
                     ? KNURL_OK
                     : KNURL_STATE;
        free(ending);
        if (status != KNURL_OK)
            return knurl_fail(error, status, "response_end",
                              "end was accepted with different trailers");
        knurl_error_clear(error);
        return KNURL_OK;
    }
    if (!r->impl->suppressed && r->impl->has_length &&
        compare(r->impl->accepted, r->impl->length) != 0) {
        free(ending);
        return knurl_response_abort(r, KNURL_IO, error) == KNURL_OK
                   ? knurl_fail(error, KNURL_IO, "response_end",
                                "response ended short of its declared length")
                   : KNURL_STATE;
    }
    if (length != 0) {
        if (r->impl->pending_head != NULL) {
            free(ending);
            r->impl->blocked = 1;
            r->impl->writable_need = 1;
            knurl_error_clear(error);
            return KNURL_AGAIN;
        }
        copy = malloc(length);
        if (copy == NULL) {
            free(ending);
            return knurl_fail(error, KNURL_NOMEM, "response_end",
                              "cannot allocate final framing");
        }
        memcpy(copy, ending, length);
        status = enqueue(r, copy, length, 0, 0, error);
        if (status != KNURL_OK) {
            free(copy);
            free(ending);
            if (status == KNURL_AGAIN)
                r->impl->blocked = 1;
            return status;
        }
    }
    r->impl->ending = ending;
    r->impl->ending_length = length;
    r->impl->phase = KNURL_RESPONSE_FINISHING;
    r->impl->schedule(r);
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_response_end(knurl_response *r, const knurl_header *trailers,
                                size_t count, knurl_error *error)
{
    knurl_status status;
    status = access_response(r, error);
    if (status != KNURL_OK)
        return status;
    if (r->impl->producer)
        return knurl_fail(error, KNURL_STATE, "response_end",
                          "producer owns end of stream");
    return end_response(r, trailers, count, error);
}

knurl_status knurl_response_replace_uncommitted(knurl_response *r,
                                                knurl_error *error)
{
    knurl_status status;
    knurl_reply *headers;
    status = access_response(r, error);
    if (status != KNURL_OK)
        return status;
    if (r->impl->request->impl->websocket != NULL)
        return knurl_fail(
            error, KNURL_STATE, "response_replace",
            "accepted WebSocket owns the handshake; abort to cancel");
    if (r->impl->outcome.committed ||
        (r->impl->inflight &&
         (r->impl->head == NULL || !r->impl->head->interim)))
        return knurl_fail(error, KNURL_STATE, "response_replace",
                          "transport has committed or holds an active buffer");
    status = knurl_reply_new(&headers, error);
    if (status != KNURL_OK)
        return status;
    {
        struct knurl_response_segment **slot;
        struct knurl_response_segment *segment;
        slot = &r->impl->head;
        r->impl->tail = NULL;
        while ((segment = *slot) != NULL) {
            if (segment->interim) {
                r->impl->tail = segment;
                slot = &segment->next;
            } else {
                *slot = segment->next;
                r->impl->queued -= segment->length;
                free(segment->data);
                free(segment);
            }
        }
    }
    clear_producer(r, KNURL_CANCELLED);
    free(r->impl->pending_head);
    r->impl->pending_head = NULL;
    r->impl->pending_head_length = r->impl->pending_head_position = 0;
    (void)knurl_reply_destroy(r->impl->headers, NULL);
    r->impl->headers = headers;
    free(r->impl->trailer_names);
    free(r->impl->ending);
    r->impl->trailer_names = NULL;
    r->impl->ending = NULL;
    r->impl->ending_length = 0;
    r->impl->complete_reply = 0;
    r->impl->has_length = r->impl->trailers_enabled = r->impl->chunked =
        r->impl->suppressed = r->impl->blocked = 0;
    r->impl->explicit_length_header = 0;
    r->impl->length.high = r->impl->length.low = r->impl->accepted.high =
        r->impl->accepted.low = 0;
    r->impl->outcome.status = 200;
    r->impl->outcome.cause = KNURL_OK;
    r->impl->phase = KNURL_RESPONSE_BUILDING;
    r->impl->schedule(r);
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_response_abort(knurl_response *r, knurl_status cause,
                                  knurl_error *error)
{
    knurl_status status;
    status = access_response(r, error);
    if (status != KNURL_OK)
        return status;
    if (cause <= KNURL_EOF || cause > KNURL_BACKEND)
        return knurl_fail(error, KNURL_INVALID, "response_abort",
                          "provide a terminal failure cause");
    r->impl->outcome.cause = cause;
    r->impl->phase = KNURL_RESPONSE_ABORTED;
    r->impl->close_connection = 1;
    r->impl->schedule(r);
    knurl_error_clear(error);
    return KNURL_OK;
}

static knurl_status available_producer(knurl_response *r, knurl_error *error)
{
    knurl_status status;
    status = building(r, error);
    if (status != KNURL_OK)
        return status;
    if (r->impl->producer)
        return knurl_fail(error, KNURL_STATE, "response_producer",
                          "a producer is already installed");
    return KNURL_OK;
}

static void source_ready(void *data)
{
    knurl_source *source = data;
    knurl_response *r = source->impl->owner;
    if (r != NULL && r->impl->source == source && !r->impl->terminal_notified) {
        r->impl->paused = 0;
        r->impl->schedule(r);
    }
}
knurl_status knurl_response_producer(knurl_response *r, knurl_source *source,
                                     knurl_error *error)
{
    knurl_status status;
    status = available_producer(r, error);
    if (status != KNURL_OK)
        return status;
    if (source == NULL || source->impl->borrowed ||
        source->impl->owner != NULL || source->impl->busy ||
        source->impl->failure != KNURL_OK)
        return knurl_fail(error, KNURL_STATE, "response_producer",
                          "source is unavailable");
    if (r->impl->has_length &&
        (!source->impl->config.has_length ||
         compare(r->impl->length, difference(source->impl->config.length,
                                             source->impl->position)) != 0))
        return knurl_fail(error, KNURL_INVALID, "response_producer",
                          "source and response lengths differ");
    if (r->impl->trailers_enabled && source->impl->config.has_length)
        return knurl_fail(error, KNURL_INVALID, "response_producer",
                          "known-length source conflicts with trailers");
    r->impl->source = source;
    source->impl->owner = r;
    source->impl->loop = knurl_request_loop(r->impl->request);
    source->impl->resume = source_ready;
    source->impl->resume_data = source;
    r->impl->producer = 1;
    if (source->impl->config.has_length) {
        r->impl->has_length = 1;
        r->impl->length =
            difference(source->impl->config.length, source->impl->position);
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_response_producer_resume(knurl_response *r,
                                            knurl_error *error)
{
    knurl_status status;
    status = access_response(r, error);
    if (status != KNURL_OK)
        return status;
    if (r->impl->source == NULL || r->impl->eof)
        return knurl_fail(error, KNURL_STATE, "response_resume",
                          "no live source producer");
    return knurl_source_resume(r->impl->source, error);
}

knurl_status knurl_response_file_take(knurl_response *r, int fd,
                                      knurl_u64 offset, knurl_u64 length,
                                      knurl_error *error)
{
    knurl_status status;
    struct stat st;
    knurl_u64 end;
    off_t start;
    off_t finish;
    status = available_producer(r, error);
    if (status != KNURL_OK)
        return status;
    if (!knurl_u64_valid(offset) || !knurl_u64_valid(length) ||
        knurl_u64_add(offset, length, &end, NULL) != KNURL_OK ||
        !knurl_file_offset(offset, &start) || !knurl_file_offset(end, &finish))
        return knurl_fail(
            error, KNURL_OVERFLOW, "response_file",
            "file range does not fit the target seek representation");
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < finish)
        return knurl_fail(error, KNURL_INVALID, "response_file",
                          "provide an open regular file containing the range");
    if (r->impl->trailers_enabled ||
        (r->impl->has_length && compare(r->impl->length, length) != 0))
        return knurl_fail(error, KNURL_INVALID, "response_file",
                          "file length conflicts with framing");
    r->impl->file = fd;
    r->impl->file_offset = offset;
    r->impl->file_remaining = length;
    r->impl->length = length;
    r->impl->has_length = r->impl->producer = 1;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_response_bytes(knurl_response *r, int code, const char *type,
                                  knurl_bytes bytes, knurl_error *error)
{
    knurl_status status;
    knurl_reply *reply;
    knurl_bytes payload;
    knurl_u64 length;
    char decimal[21];
    size_t written;
    int suppressed;
    status = available_producer(r, error);
    if (status != KNURL_OK)
        return status;
    if (r->impl->has_length || r->impl->trailers_enabled ||
        r->impl->headers->impl->header_count != 0)
        return knurl_fail(error, KNURL_STATE, "response_bytes",
                          "complete reply requires an empty declaration");
    /* Bound the copied convenience. Larger existing values use the staged
     * facade, which transfers its owned payload without a second whole copy. */
    if (bytes.data == NULL && bytes.length != 0)
        return knurl_fail(error, KNURL_INVALID, "response_bytes",
                          "provide valid bytes");
    suppressed = r->impl->request->impl->method == KNURL_HTTP_HEAD ||
                 code == 204 || code == 205 || code == 304;
    if (!suppressed && bytes.length > r->impl->bound)
        return knurl_fail(error, KNURL_LIMIT, "response_bytes",
                          "complete copied reply exceeds output bound");
    status = knurl_reply_new(&reply, error);
    if (status != KNURL_OK)
        return status;
    payload = bytes;
    if (suppressed) {
        payload.data = NULL;
        payload.length = 0;
    }
    status = knurl_reply_bytes(reply, code, type, payload, error);
    if (status == KNURL_OK &&
        (r->impl->request->impl->method == KNURL_HTTP_HEAD || code == 304)) {
        status = knurl_u64_from_size(bytes.length, &length, error);
        if (status == KNURL_OK)
            status = knurl_u64_format_decimal(length, decimal, sizeof(decimal),
                                              &written, error);
        if (status == KNURL_OK)
            status =
                knurl_reply_header_add(reply, "content-length", decimal, error);
    }
    if (status == KNURL_OK)
        status = knurl_response_submit(r, reply, error);
    if (status == KNURL_OK) {
        r->impl->complete_reply = 1;
        r->impl->phase = KNURL_RESPONSE_FINISHING;
    }
    (void)knurl_reply_destroy(reply, NULL);
    return status;
}

knurl_status knurl_response_submit(knurl_response *r, knurl_reply *reply,
                                   knurl_error *error)
{
    knurl_status status;
    knurl_reply *saved;
    knurl_u64 saved_length;
    int saved_status;
    int saved_has_length;
    int saved_explicit_length;
    int saved_close;
    size_t i;
    knurl_owned_bytes pretty;
    char modified[128];
    int conditional = 0;
    status = available_producer(r, error);
    if (status != KNURL_OK)
        return status;
    knurl_owned_bytes_init(&pretty);
    saved = r->impl->headers;
    saved_status = r->impl->outcome.status;
    saved_length = r->impl->length;
    saved_has_length = r->impl->has_length;
    saved_explicit_length = r->impl->explicit_length_header;
    saved_close = r->impl->close_connection;
    status = knurl_reply_new(&r->impl->headers, error);
    if (status != KNURL_OK) {
        r->impl->headers = saved;
        return status;
    }
    r->impl->outcome.status = reply->impl->selected ? reply->impl->status : 204;
    r->impl->explicit_length_header = 0;
    for (i = 0; status == KNURL_OK && i < reply->impl->header_count; ++i) {
        if (knurl_text_equal_case(reply->impl->headers[i].name.data,
                                  "content-length")) {
            if (r->impl->outcome.status == 304 ||
                (r->impl->request->impl->method == KNURL_HTTP_HEAD &&
                 !status_bodyless(r->impl->outcome.status))) {
                status =
                    header(r, reply->impl->headers[i].name.data,
                           reply->impl->headers[i].value.data, 1, 1, error);
                if (status == KNURL_OK)
                    r->impl->explicit_length_header = 1;
            }
        } else if (!knurl_text_equal_case(reply->impl->headers[i].name.data,
                                          "transfer-encoding")) {
            status =
                header(r, reply->impl->headers[i].name.data,
                       reply->impl->headers[i].value.data,
                       !knurl_text_equal_case(reply->impl->headers[i].name.data,
                                              "set-cookie"),
                       1, error);
        }
    }
    if (status == KNURL_OK && reply->impl->content_type != NULL)
        status =
            header(r, "content-type", reply->impl->content_type, 1, 1, error);
    if (status == KNURL_OK) {
        if (reply->impl->kind == KNURL_REPLY_SOURCE) {
            reply->impl->source->impl->owner = NULL;
            if (reply->impl->source->impl->config.reset_before_reply &&
                reply->impl->source->impl->config.reset != NULL &&
                !bodyless(r)) {
                r->impl->busy = reply->impl->busy = 1;
                status = knurl_source_reset(reply->impl->source, error);
                r->impl->busy = reply->impl->busy = 0;
            }
            if (status == KNURL_OK)
                status = knurl_response_producer(r, reply->impl->source, error);
            if (status != KNURL_OK)
                reply->impl->source->impl->owner = reply;
        } else if (reply->impl->kind == KNURL_REPLY_FILE) {
            knurl_u64 zero;
            zero.high = zero.low = 0;
            if (!status_bodyless(r->impl->outcome.status)) {
                status = knurl_reply_open_file(reply, error);
                if (status == KNURL_EOF) {
                    r->impl->outcome.status = 404;
                    r->impl->has_length = r->impl->producer = 1;
                    r->impl->length = zero;
                    status = KNURL_OK;
                } else if (status == KNURL_OK) {
                    if (reply->impl->file_length.high ||
                        reply->impl->file_length.low) {
                        conditional = knurl_response_file_metadata(
                                          r, reply->impl->file_modified,
                                          modified, sizeof(modified)) == 304;
                        if (!conditional && modified[0] != '\0')
                            status = header(r, "last-modified", modified, 1, 1,
                                            error);
                    }
                    if (conditional) {
                        r->impl->outcome.status = 304;
                        r->impl->has_length = r->impl->producer = 1;
                        r->impl->length = zero;
                    } else if (status == KNURL_OK)
                        status = knurl_response_file_take(
                            r, reply->impl->file, zero,
                            reply->impl->file_length, error);
                }
            }
        } else {
            r->impl->has_length = !status_bodyless(r->impl->outcome.status);
            (void)knurl_u64_from_size(reply->impl->body_length,
                                      &r->impl->length, NULL);
        }
    }
    if (status_bodyless(r->impl->outcome.status) && !conditional)
        r->impl->has_length = 0;
    if (status == KNURL_OK && r->impl->outcome.status >= 400 &&
        reply->impl->kind != KNURL_REPLY_SOURCE &&
        reply->impl->body_length == 0 &&
        (reply->impl->kind != KNURL_REPLY_FILE ||
         (r->impl->file_remaining.high == 0 &&
          r->impl->file_remaining.low == 0))) {
        status = knurl_response_pretty(r->impl->outcome.status, &pretty, error);
        if (status == KNURL_OK && pretty.data != NULL) {
            r->impl->has_length = 1;
            status =
                knurl_u64_from_size(pretty.length, &r->impl->length, error);
        }
    }
    if (status == KNURL_OK)
        status = knurl_response_begin(r, error);
    if (status != KNURL_OK) {
        knurl_owned_bytes_cleanup(&pretty);
        if (r->impl->source != NULL) {
            r->impl->source->impl->owner = reply;
            r->impl->source->impl->resume = NULL;
            r->impl->source->impl->resume_data = NULL;
            r->impl->source->impl->loop = NULL;
            r->impl->source = NULL;
        }
        r->impl->file = -1;
        r->impl->producer = r->impl->chunked = r->impl->suppressed = 0;
        r->impl->has_length = saved_has_length;
        r->impl->explicit_length_header = saved_explicit_length;
        r->impl->length = saved_length;
        r->impl->close_connection = saved_close;
        (void)knurl_reply_destroy(r->impl->headers, NULL);
        r->impl->headers = saved;
        r->impl->outcome.status = saved_status;
        return status;
    }
    (void)knurl_reply_destroy(saved, NULL);
    if (reply->impl->kind == KNURL_REPLY_SOURCE)
        reply->impl->source = NULL;
    else if (reply->impl->kind == KNURL_REPLY_FILE &&
             r->impl->file == reply->impl->file && r->impl->file != -1)
        reply->impl->file = -1;
    else if (pretty.data == NULL) {
        r->impl->memory = reply->impl->body;
        r->impl->memory_length = reply->impl->body_length;
        reply->impl->body = NULL;
        reply->impl->body_length = 0;
        r->impl->producer = 1;
    }
    if (pretty.data != NULL) {
        r->impl->memory = pretty.data;
        r->impl->memory_length = pretty.length;
        r->impl->producer = 1;
    }
    return KNURL_OK;
}

void knurl_response_fail(knurl_response *r, knurl_status cause)
{
    knurl_response_error(r, cause, 500);
}

void knurl_response_error(knurl_response *r, knurl_status cause, int code)
{
    knurl_response_error_message(r, cause, code, NULL);
}

void knurl_response_error_message(knurl_response *r, knurl_status cause,
                                  int code, const char *message)
{
    knurl_reply *failure;
    knurl_status status;
    if (!r->impl->outcome.committed && !r->impl->inflight) {
        retire_source(r, cause);
        if (knurl_response_replace_uncommitted(r, NULL) == KNURL_OK) {
            status = knurl_reply_new(&failure, NULL);
            if (status == KNURL_OK) {
                status = knurl_reply_failure(failure, code, message);
                if (status == KNURL_OK)
                    status = knurl_response_submit(r, failure, NULL);
                (void)knurl_reply_destroy(failure, NULL);
                if (status == KNURL_OK)
                    return;
            }
        }
    }
    (void)knurl_response_abort(r, cause, NULL);
}

void knurl_response_step(knurl_response *r)
{
    knurl_source *retired;
    unsigned char *buffer;
    size_t capacity;
    size_t count;
    size_t accepted;
    size_t remaining;
    off_t offset;
    ssize_t result;
    knurl_status status;
    knurl_u64 amount;
    r->impl->busy = 1;
    knurl_reply_retire(r->impl->request->impl->reply);
    r->impl->busy = 0;
    while (r->impl->retired != NULL) {
        retired = r->impl->retired;
        r->impl->retired = retired->impl->retired_next;
        r->impl->busy = 1;
        knurl_source_release(retired, retired->impl->failure);
        r->impl->busy = 0;
    }
    if (r->impl->pending_head != NULL &&
        r->impl->phase != KNURL_RESPONSE_ABORTED) {
        status = flush_head(r, NULL);
        if (status != KNURL_OK && status != KNURL_AGAIN) {
            knurl_response_fail(r, status);
            return;
        }
        if (r->impl->pending_head != NULL)
            return;
    }
    if (r->impl->blocked &&
        (r->impl->phase == KNURL_RESPONSE_ACTIVE ||
         r->impl->phase == KNURL_RESPONSE_BUILDING) &&
        r->impl->bound - r->impl->queued >= r->impl->writable_need) {
        r->impl->blocked = 0;
        if (r->impl->writable != NULL)
            r->impl->writable(r, r->impl->userdata);
        if ((r->impl->phase == KNURL_RESPONSE_ACTIVE ||
             r->impl->phase == KNURL_RESPONSE_BUILDING) &&
            r->impl->request->impl->plan.on_writable != NULL)
            r->impl->request->impl->plan.on_writable(
                r->impl->request, r->impl->request->impl->plan.userdata);
    }
    if (r->impl->phase != KNURL_RESPONSE_ACTIVE &&
        !(r->impl->phase == KNURL_RESPONSE_FINISHING &&
          r->impl->complete_reply && r->impl->ending == NULL))
        return;
    if (r->impl->suppressed) {
        r->impl->eof = 1;
        (void)end_response(r, NULL, 0, NULL);
        return;
    }
    if (!r->impl->producer || r->impl->paused)
        return;
    if (r->impl->eof) {
        status = end_response(r, NULL, 0, NULL);
        if (status != KNURL_OK && status != KNURL_AGAIN)
            knurl_response_fail(r, status);
        return;
    }
    capacity = r->impl->bound - r->impl->queued;
    if (capacity <= KNURL_RESPONSE_CHUNK_OVERHEAD)
        return;
    capacity -= KNURL_RESPONSE_CHUNK_OVERHEAD;
    if (capacity > r->impl->chunk)
        capacity = r->impl->chunk;
    buffer = NULL;
    count = 0;
    status = KNURL_EOF;
    if (r->impl->source != NULL ||
        (r->impl->file != -1 &&
         (r->impl->file_remaining.high || r->impl->file_remaining.low))) {
        buffer = malloc(capacity);
        if (buffer == NULL) {
            knurl_response_fail(r, KNURL_NOMEM);
            return;
        }
    }
    if (r->impl->source != NULL) {
        r->impl->busy = 1;
        if (r->impl->source->impl->config.execution ==
            KNURL_SOURCE_NATIVE_READER)
            status = knurl_source_native_pull(r->impl->source, r, buffer,
                                              capacity, &count, NULL);
        else {
            r->impl->source->impl->resume_requested = 0;
            status = knurl_source_pull(r->impl->source, r, buffer, capacity,
                                       &count, NULL);
        }
        r->impl->busy = 0;
    } else if (r->impl->file != -1 &&
               (r->impl->file_remaining.high || r->impl->file_remaining.low)) {
        if (knurl_u64_to_size(r->impl->file_remaining, &remaining, NULL) ==
                KNURL_OK &&
            capacity > remaining)
            capacity = remaining;
        (void)knurl_file_offset(r->impl->file_offset, &offset);
        do {
            result = pread(r->impl->file, buffer, capacity, offset);
        } while (result == -1 && errno == EINTR);
        status = result > 0 ? KNURL_OK : KNURL_IO;
        if (result > 0) {
            count = (size_t)result;
            (void)add_size(r->impl->file_offset, count, &r->impl->file_offset);
            (void)knurl_u64_from_size(count, &amount, NULL);
            r->impl->file_remaining =
                difference(r->impl->file_remaining, amount);
        }
    } else if (r->impl->memory_position < r->impl->memory_length) {
        count = r->impl->memory_length - r->impl->memory_position;
        if (count > capacity)
            count = capacity;
        status = KNURL_OK;
    }
    if (status == KNURL_OK) {
        status = write_body(r,
                            buffer != NULL
                                ? buffer
                                : r->impl->memory + r->impl->memory_position,
                            count, &accepted, NULL);
        if (status == KNURL_OK && accepted != count)
            status = KNURL_IO;
        if (buffer == NULL)
            r->impl->memory_position += accepted;
    } else if (status == KNURL_AGAIN) {
        r->impl->paused = !(r->impl->source != NULL &&
                            r->impl->source->impl->config.execution ==
                                KNURL_SOURCE_OWNER_LOOP &&
                            r->impl->source->impl->resume_requested);
        if (!r->impl->paused)
            r->impl->schedule(r);
    } else if (status == KNURL_EOF) {
        r->impl->eof = 1;
        status = end_response(r, NULL, 0, NULL);
    }
    free(buffer);
    if (status != KNURL_OK && status != KNURL_AGAIN)
        knurl_response_fail(r, status);
}

void knurl_response_sent(knurl_response *r, size_t offset, size_t count,
                         int retry)
{
    size_t start;
    size_t end;
    struct knurl_response_segment *segment;
    segment = r->impl->head;
    if (!segment->interim && (count != 0 || retry) &&
        !r->impl->outcome.committed) {
        r->impl->outcome.committed = 1;
        knurl_policy_observe(r->impl->request, KNURL_OBSERVE_RESPONSE_COMMITTED,
                             NULL, r->impl->outcome.status, KNURL_OK);
    }
    if (add_size(r->impl->outcome.wire_bytes, count,
                 &r->impl->outcome.wire_bytes) != KNURL_OK) {
        (void)knurl_response_abort(r, KNURL_OVERFLOW, NULL);
        return;
    }
    if (segment->interim &&
        add_size(r->impl->outcome.interim_wire_bytes, count,
                 &r->impl->outcome.interim_wire_bytes) != KNURL_OK) {
        (void)knurl_response_abort(r, KNURL_OVERFLOW, NULL);
        return;
    }
    if (count != 0 && r->impl->progress != NULL)
        r->impl->progress(r->impl->progress_data, count);
    start = offset > segment->body_start ? offset : segment->body_start;
    end = offset + count < segment->body_start + segment->body_length
              ? offset + count
              : segment->body_start + segment->body_length;
    if (end > start && add_size(r->impl->outcome.body_bytes, end - start,
                                &r->impl->outcome.body_bytes) != KNURL_OK)
        (void)knurl_response_abort(r, KNURL_OVERFLOW, NULL);
}

void knurl_response_shift(knurl_response *r)
{
    struct knurl_response_segment *segment;
    segment = r->impl->head;
    r->impl->head = segment->next;
    if (r->impl->head == NULL)
        r->impl->tail = NULL;
    r->impl->queued -= segment->length;
    free(segment->data);
    free(segment);
}

void knurl_response_terminal(knurl_response *r)
{
    knurl_u64 now;
    if (r->impl->terminal_notified)
        return;
    r->impl->terminal_notified = 1;
    if (knurl_loop_monotonic(&now, NULL) == KNURL_OK &&
        compare(now, r->impl->started) >= 0)
        r->impl->outcome.duration_ms = difference(now, r->impl->started);
    if (r->impl->phase != KNURL_RESPONSE_ABORTED)
        r->impl->phase = KNURL_RESPONSE_COMPLETE;
    r->impl->busy = 1;
    knurl_reply_retire(r->impl->request->impl->reply);
    /* Failed/unselected staged sources were never transferred to r->source.
     * Close them while request metadata/application associations still live. */
    if (r->impl->request->impl->reply != NULL &&
        r->impl->request->impl->reply->impl->source != NULL) {
        knurl_source *source = r->impl->request->impl->reply->impl->source;
        r->impl->request->impl->reply->impl->source = NULL;
        knurl_source_release(source, source->impl->failure != KNURL_OK
                                         ? source->impl->failure
                                         : KNURL_CANCELLED);
    }
    clear_producer(r, r->impl->outcome.cause != KNURL_OK
                          ? r->impl->outcome.cause
                          : (r->impl->suppressed ? KNURL_CANCELLED : KNURL_OK));
    while (r->impl->retired != NULL) {
        knurl_source *source;
        source = r->impl->retired;
        r->impl->retired = source->impl->retired_next;
        knurl_source_release(source, source->impl->failure);
    }
    if (r->impl->closed != NULL)
        r->impl->closed(r, &r->impl->outcome, r->impl->userdata);
    r->impl->busy = 0;
}

void knurl_response_release(knurl_response *r)
{
    if (r == NULL)
        return;
    knurl_response_terminal(r);
    clear_queue(r);
    (void)knurl_reply_destroy(r->impl->headers, NULL);
    free(r->impl->ending);
    free(r->impl->trailer_names);
    free(r);
}

knurl_status knurl_response_upgrade_headers(knurl_response *r,
                                            const knurl_header *headers,
                                            size_t count, knurl_error *error)
{
    knurl_reply *saved;
    knurl_reply *copy;
    unsigned char *ending;
    knurl_status status;
    size_t i;
    int old_status;
    int old_close;
    status = building(r, error);
    if (status != KNURL_OK)
        return status;
    if (r->impl->producer || r->impl->has_length || r->impl->trailers_enabled)
        return knurl_fail(error, KNURL_STATE, "channel_upgrade",
                          "upgrade requires a fresh response without a body "
                          "or framing declaration");
    status = knurl_reply_new(&copy, error);
    if (status != KNURL_OK)
        return status;
    ending = malloc(1);
    if (ending == NULL) {
        (void)knurl_reply_destroy(copy, NULL);
        return knurl_fail(error, KNURL_NOMEM, "channel_upgrade",
                          "cannot allocate terminal handshake state");
    }
    /* The caller validates the complete set, including staged fields. */
    for (i = 0; status == KNURL_OK && i < count; ++i)
        status = knurl_reply_header_add(copy, headers[i].name.data,
                                        headers[i].value.data, error);
    saved = r->impl->headers;
    old_status = r->impl->outcome.status;
    old_close = r->impl->close_connection;
    if (status == KNURL_OK) {
        r->impl->headers = copy;
        r->impl->outcome.status = 101;
        r->impl->close_connection = 0;
        status = knurl_response_begin(r, error);
    }
    if (status != KNURL_OK) {
        r->impl->headers = saved;
        r->impl->outcome.status = old_status;
        r->impl->close_connection = old_close;
        r->impl->suppressed = r->impl->chunked = 0;
        free(ending);
        (void)knurl_reply_destroy(copy, NULL);
        return status;
    }
    (void)knurl_reply_destroy(saved, NULL);
    r->impl->ending = ending;
    r->impl->ending_length = 0;
    r->impl->eof = 1;
    r->impl->phase = KNURL_RESPONSE_FINISHING;
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_response_receiver_init(knurl_response *receiver)
{
    static const knurl_response methods = {knurl_response_state,
                                           knurl_response_is_committed,
                                           knurl_response_queued_bytes,
                                           knurl_response_callbacks,
                                           knurl_response_status,
                                           knurl_response_header_set,
                                           knurl_response_header_add,
                                           knurl_response_known_length,
                                           knurl_response_trailers_enable,
                                           knurl_response_begin,
                                           knurl_response_interim,
                                           knurl_response_write,
                                           knurl_response_end,
                                           knurl_response_replace_uncommitted,
                                           knurl_response_abort,
                                           knurl_response_bytes,
                                           knurl_response_file_take,
                                           knurl_response_producer,
                                           knurl_response_producer_resume,
                                           NULL};
    *receiver = methods;
    receiver->impl = (struct knurl_response_impl *)receiver;
}

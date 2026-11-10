#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "response_internal.h"
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
    if (r->busy || r->phase >= KNURL_RESPONSE_COMPLETE)
        return knurl_fail(error, KNURL_STATE, "response",
                          "response is busy or terminal");
    return knurl_loop_owner(r->request->server->loop, error);
}

static knurl_status building(knurl_response *r, knurl_error *error)
{
    knurl_status status;
    status = access_response(r, error);
    if (status == KNURL_OK && r->phase != KNURL_RESPONSE_BUILDING)
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
    return status_bodyless(r->outcome.status) ||
           r->request->method == KNURL_HTTP_HEAD;
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
    while (r->head != NULL)
        knurl_response_shift(r);
    free(r->pending_head);
    r->pending_head = NULL;
    r->pending_head_length = r->pending_head_position = 0;
}

static void retire_source(knurl_response *r, knurl_status cause)
{
    knurl_source *source;
    source = r->source;
    if (source != NULL) {
        knurl_source_revoke(source);
        source->failure = cause;
        source->retired_next = r->retired;
        r->retired = source;
        r->source = NULL;
    }
}

static void clear_producer(knurl_response *r, knurl_status cause)
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

knurl_status knurl_response_create(knurl_request *request,
                                   void (*schedule)(knurl_response *),
                                   knurl_response **out, knurl_error *error)
{
    knurl_response *r;
    knurl_status status;
    *out = NULL;
    r = calloc(1, sizeof(*r));
    if (r == NULL)
        return knurl_fail(error, KNURL_NOMEM, "response",
                          "cannot allocate response");
    r->request = request;
    r->schedule = schedule;
    r->file = -1;
    r->outcome.status = 200;
    status = knurl_loop_monotonic(&r->started, error);
    if (status != KNURL_OK) {
        free(r);
        return status;
    }
    r->bound = request->server->config.io.output_queue_bytes;
    r->chunk = request->server->config.io.chunk_bytes;
    r->header_bound = request->server->config.limits.max_response_header_bytes;
    if (r->header_bound == 0)
        r->header_bound = (size_t)-1;
    status = knurl_reply_new(&r->headers, error);
    if (status != KNURL_OK) {
        free(r);
        return status;
    }
    *out = r;
    return KNURL_OK;
}

knurl_response *knurl_request_response(knurl_request *request)
{
    return request != NULL ? request->response : NULL;
}

knurl_response_phase knurl_response_state(const knurl_response *r)
{
    return r != NULL ? r->phase : KNURL_RESPONSE_ABORTED;
}

int knurl_response_is_committed(const knurl_response *r)
{
    return r != NULL && r->outcome.committed;
}

size_t knurl_response_queued_bytes(const knurl_response *r)
{
    return r != NULL ? r->queued : 0;
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
    r->writable = writable;
    r->closed = closed;
    r->userdata = userdata;
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
    r->outcome.status = code;
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
    for (i = 0; i < r->headers->header_count; ++i) {
        if (set &&
            knurl_text_equal_case(r->headers->headers[i].name.data, name)) {
            if (staged)
                name = r->headers->headers[i].name.data;
            continue;
        }
        total += r->headers->headers[i].name.length +
                 r->headers->headers[i].value.length + 4;
    }
    if (strlen(name) > r->header_bound || strlen(value) > r->header_bound ||
        total > r->header_bound - strlen(name) ||
        total + strlen(name) > r->header_bound - strlen(value) ||
        total + strlen(name) + strlen(value) > r->header_bound - 4)
        return knurl_fail(error, KNURL_LIMIT, "response_header",
                          "header bound exceeded");
    return set ? knurl_reply_header_set(r->headers, name, value, error)
               : knurl_reply_header_add(r->headers, name, value, error);
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
    if (r->producer || r->trailers_enabled)
        return knurl_fail(error, KNURL_STATE, "response_length",
                          "length is owned by the producer or trailer framing");
    r->has_length = 1;
    r->length = length;
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
    if ((count != 0 && names == NULL) || r->has_length || bodyless(r) ||
        r->request->version.length != 8 ||
        memcmp(r->request->version.data, "HTTP/1.1", 8) != 0)
        return knurl_fail(
            error, KNURL_INVALID, "response_trailers",
            "trailers require a body-bearing unknown-length HTTP/1.1 response");
    length = 0;
    for (i = 0; i < count; ++i) {
        if (!knurl_header_name_valid(names[i]) || trailer_forbidden(names[i]))
            return knurl_fail(error, KNURL_INVALID, "response_trailers",
                              "invalid trailer name");
        if (strlen(names[i]) + 2 > r->header_bound - length)
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
    free(r->trailer_names);
    r->trailer_names = text;
    r->trailers_enabled = 1;
    knurl_error_clear(error);
    return KNURL_OK;
}

static knurl_status enqueue(knurl_response *r, unsigned char *data,
                            size_t length, size_t body_start,
                            size_t body_length, knurl_error *error)
{
    struct knurl_response_segment *segment;
    if (length > r->bound - r->queued) {
        r->blocked = 1;
        r->writable_need = length;
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
    if (r->tail != NULL)
        r->tail->next = segment;
    else
        r->head = segment;
    r->tail = segment;
    r->queued += length;
    r->schedule(r);
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
    if (memcmp(r->request->version.data, "HTTP/1.1", 8) != 0)
        return knurl_fail(error, KNURL_NOT_SUPPORTED, "response_interim",
                          "interim responses require HTTP/1.1");
    if (r->interim_count >= 16 || count > 100)
        return knurl_fail(error, KNURL_LIMIT, "response_interim",
                          "interim response count exceeded");
    line = code == 100 ? "HTTP/1.1 100 Continue\r\n"
                       : "HTTP/1.1 103 Early Hints\r\n";
    total = strlen(line) + 2;
    bound = r->header_bound < r->bound ? r->header_bound : r->bound;
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
    if (status == KNURL_OK && (total > r->header_bound || total > r->bound ||
                               total > 131072UL - r->interim_bytes))
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
        for (i = 0; i < copy->header_count; ++i) {
            memcpy(data + offset, copy->headers[i].name.data,
                   copy->headers[i].name.length);
            offset += copy->headers[i].name.length;
            memcpy(data + offset, ": ", 2);
            offset += 2;
            memcpy(data + offset, copy->headers[i].value.data,
                   copy->headers[i].value.length);
            offset += copy->headers[i].value.length;
            memcpy(data + offset, "\r\n", 2);
            offset += 2;
        }
        memcpy(data + offset, "\r\n", 2);
        status = enqueue(r, data, total, 0, 0, error);
        if (status == KNURL_OK) {
            r->tail->interim = 1;
            ++r->interim_count;
            r->interim_bytes += total;
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
    if (r->pending_head == NULL)
        return KNURL_OK;
    count = r->pending_head_length - r->pending_head_position;
    if (count > r->bound - r->queued)
        count = r->bound - r->queued;
    if (count == 0)
        return KNURL_AGAIN;
    copy = malloc(count);
    if (copy == NULL)
        return knurl_fail(error, KNURL_NOMEM, "response_head",
                          "cannot queue response headers");
    memcpy(copy, r->pending_head + r->pending_head_position, count);
    status = enqueue(r, copy, count, 0, 0, error);
    if (status != KNURL_OK) {
        free(copy);
        return status;
    }
    r->pending_head_position += count;
    if (r->pending_head_position == r->pending_head_length) {
        free(r->pending_head);
        r->pending_head = NULL;
        r->pending_head_length = r->pending_head_position = 0;
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
    r->suppressed = bodyless(r);
    if (r->trailers_enabled && (r->has_length || r->suppressed))
        return knurl_fail(error, KNURL_INVALID, "response_begin",
                          "trailers conflict with final framing");
    version = memcmp(r->request->version.data, "HTTP/1.0", 8) == 0 ? "HTTP/1.0"
                                                                   : "HTTP/1.1";
    r->chunked =
        !r->suppressed && !r->has_length && strcmp(version, "HTTP/1.1") == 0;
    if (strcmp(version, "HTTP/1.0") == 0)
        r->close_connection = 1;
    status = knurl_response_head(r, &head, error);
    if (status != KNURL_OK)
        return status;
    r->pending_head = head.data;
    r->pending_head_length = head.length;
    status = flush_head(r, error);
    if (status != KNURL_OK && status != KNURL_AGAIN) {
        free(r->pending_head);
        r->pending_head = NULL;
        r->pending_head_length = r->pending_head_position = 0;
        return status;
    }
    r->phase = KNURL_RESPONSE_ACTIVE;
    r->schedule(r);
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
    if ((r->phase != KNURL_RESPONSE_ACTIVE &&
         !(r->phase == KNURL_RESPONSE_FINISHING && r->complete_reply)) ||
        r->suppressed)
        return knurl_fail(error, KNURL_STATE, "response_write",
                          "response does not accept payload");
    status = add_size(r->accepted, length, &next);
    if (status != KNURL_OK || (r->has_length && compare(next, r->length) > 0))
        return knurl_fail(error, status != KNURL_OK ? status : KNURL_OVERFLOW,
                          "response_write",
                          "offered bytes exceed declared length");
    if (length == 0) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    if (r->pending_head != NULL) {
        r->blocked = 1;
        r->writable_need = 1;
        knurl_error_clear(error);
        return KNURL_AGAIN;
    }
    capacity = r->bound - r->queued;
    if (capacity <= KNURL_RESPONSE_CHUNK_OVERHEAD) {
        r->blocked = 1;
        r->writable_need = KNURL_RESPONSE_CHUNK_OVERHEAD + 1;
        knurl_error_clear(error);
        return KNURL_AGAIN;
    }
    count = length;
    if (count > r->chunk)
        count = r->chunk;
    if (count > capacity - KNURL_RESPONSE_CHUNK_OVERHEAD)
        count = capacity - KNURL_RESPONSE_CHUNK_OVERHEAD;
    copy = malloc(count + KNURL_RESPONSE_CHUNK_OVERHEAD);
    if (copy == NULL)
        return knurl_fail(error, KNURL_NOMEM, "response_write",
                          "cannot allocate bounded chunk");
    prefix = r->chunked ? (size_t)sprintf((char *)copy, "%lx\r\n",
                                          (unsigned long)count)
                        : 0;
    memcpy(copy + prefix, data, count);
    if (r->chunked)
        memcpy(copy + prefix + count, "\r\n", 2);
    status = enqueue(r, copy, prefix + count + (r->chunked ? 2 : 0), prefix,
                     count, error);
    if (status != KNURL_OK) {
        free(copy);
        return status;
    }
    (void)add_size(r->accepted, count, &r->accepted);
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
    if (r->producer)
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
        (count != 0 && !r->trailers_enabled))
        return knurl_fail(error, KNURL_INVALID, "response_end",
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
            return knurl_fail(error, KNURL_INVALID, "response_end",
                              "invalid trailer text");
        if (trailers[i].name.length + trailers[i].value.length + 4 >
            r->header_bound - size)
            return knurl_fail(error, KNURL_LIMIT, "response_end",
                              "trailers exceed finite header bound");
        size += trailers[i].name.length + trailers[i].value.length + 4;
    }
    if (size > r->bound)
        return knurl_fail(error, KNURL_LIMIT, "response_end",
                          "trailers cannot fit output queue");
    text = malloc(size + 1);
    if (text == NULL)
        return knurl_fail(error, KNURL_NOMEM, "response_end",
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
    if (r->chunked) {
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
    if (r->phase != KNURL_RESPONSE_ACTIVE &&
        r->phase != KNURL_RESPONSE_FINISHING)
        return knurl_fail(error, KNURL_STATE, "response_end",
                          "response has not begun");
    status = make_ending(r, trailers, count, &ending, &length, error);
    if (status != KNURL_OK)
        return status;
    if (r->phase == KNURL_RESPONSE_FINISHING && r->ending != NULL) {
        status =
            length == r->ending_length && memcmp(ending, r->ending, length) == 0
                ? KNURL_OK
                : KNURL_STATE;
        free(ending);
        if (status != KNURL_OK)
            return knurl_fail(error, status, "response_end",
                              "end was accepted with different trailers");
        knurl_error_clear(error);
        return KNURL_OK;
    }
    if (!r->suppressed && r->has_length &&
        compare(r->accepted, r->length) != 0) {
        free(ending);
        return knurl_response_abort(r, KNURL_IO, error) == KNURL_OK
                   ? knurl_fail(error, KNURL_IO, "response_end",
                                "response ended short of its declared length")
                   : KNURL_STATE;
    }
    if (length != 0) {
        if (r->pending_head != NULL) {
            free(ending);
            r->blocked = 1;
            r->writable_need = 1;
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
                r->blocked = 1;
            return status;
        }
    }
    r->ending = ending;
    r->ending_length = length;
    r->phase = KNURL_RESPONSE_FINISHING;
    r->schedule(r);
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
    if (r->producer)
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
    if (r->request->websocket != NULL)
        return knurl_fail(
            error, KNURL_STATE, "response_replace",
            "accepted WebSocket owns the handshake; abort to cancel");
    if (r->outcome.committed ||
        (r->inflight && (r->head == NULL || !r->head->interim)))
        return knurl_fail(error, KNURL_STATE, "response_replace",
                          "transport has committed or holds an active buffer");
    status = knurl_reply_new(&headers, error);
    if (status != KNURL_OK)
        return status;
    {
        struct knurl_response_segment **slot;
        struct knurl_response_segment *segment;
        slot = &r->head;
        r->tail = NULL;
        while ((segment = *slot) != NULL) {
            if (segment->interim) {
                r->tail = segment;
                slot = &segment->next;
            } else {
                *slot = segment->next;
                r->queued -= segment->length;
                free(segment->data);
                free(segment);
            }
        }
    }
    clear_producer(r, KNURL_CANCELLED);
    free(r->pending_head);
    r->pending_head = NULL;
    r->pending_head_length = r->pending_head_position = 0;
    (void)knurl_reply_destroy(r->headers, NULL);
    r->headers = headers;
    free(r->trailer_names);
    free(r->ending);
    r->trailer_names = NULL;
    r->ending = NULL;
    r->ending_length = 0;
    r->complete_reply = 0;
    r->has_length = r->trailers_enabled = r->chunked = r->suppressed =
        r->blocked = 0;
    r->explicit_length_header = 0;
    r->length.high = r->length.low = r->accepted.high = r->accepted.low = 0;
    r->outcome.status = 200;
    r->outcome.cause = KNURL_OK;
    r->phase = KNURL_RESPONSE_BUILDING;
    r->schedule(r);
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
    r->outcome.cause = cause;
    r->phase = KNURL_RESPONSE_ABORTED;
    r->close_connection = 1;
    r->schedule(r);
    knurl_error_clear(error);
    return KNURL_OK;
}

static knurl_status available_producer(knurl_response *r, knurl_error *error)
{
    knurl_status status;
    status = building(r, error);
    if (status != KNURL_OK)
        return status;
    if (r->producer)
        return knurl_fail(error, KNURL_STATE, "response_producer",
                          "a producer is already installed");
    return KNURL_OK;
}

static void source_ready(void *data)
{
    knurl_source *source = data;
    knurl_response *r = source->owner;
    if (r != NULL && r->source == source && !r->terminal_notified) {
        r->paused = 0;
        r->schedule(r);
    }
}
knurl_status knurl_response_producer(knurl_response *r, knurl_source *source,
                                     knurl_error *error)
{
    knurl_status status;
    status = available_producer(r, error);
    if (status != KNURL_OK)
        return status;
    if (source == NULL || source->borrowed || source->owner != NULL ||
        source->busy || source->failure != KNURL_OK)
        return knurl_fail(error, KNURL_STATE, "response_producer",
                          "source is unavailable");
    if (r->has_length &&
        (!source->config.has_length ||
         compare(r->length,
                 difference(source->config.length, source->position)) != 0))
        return knurl_fail(error, KNURL_INVALID, "response_producer",
                          "source and response lengths differ");
    if (r->trailers_enabled && source->config.has_length)
        return knurl_fail(error, KNURL_INVALID, "response_producer",
                          "known-length source conflicts with trailers");
    r->source = source;
    source->owner = r;
    source->loop = knurl_request_loop(r->request);
    source->resume = source_ready;
    source->resume_data = source;
    r->producer = 1;
    if (source->config.has_length) {
        r->has_length = 1;
        r->length = difference(source->config.length, source->position);
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
    if (r->source == NULL || r->eof)
        return knurl_fail(error, KNURL_STATE, "response_resume",
                          "no live source producer");
    return knurl_source_resume(r->source, error);
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
    if (r->trailers_enabled ||
        (r->has_length && compare(r->length, length) != 0))
        return knurl_fail(error, KNURL_INVALID, "response_file",
                          "file length conflicts with framing");
    r->file = fd;
    r->file_offset = offset;
    r->file_remaining = length;
    r->length = length;
    r->has_length = r->producer = 1;
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
    if (r->has_length || r->trailers_enabled || r->headers->header_count != 0)
        return knurl_fail(error, KNURL_STATE, "response_bytes",
                          "complete reply requires an empty declaration");
    /* Bound the copied convenience. Larger existing values use the staged
     * facade, which transfers its owned payload without a second whole copy. */
    if (bytes.data == NULL && bytes.length != 0)
        return knurl_fail(error, KNURL_INVALID, "response_bytes",
                          "provide valid bytes");
    suppressed = r->request->method == KNURL_HTTP_HEAD || code == 204 ||
                 code == 205 || code == 304;
    if (!suppressed && bytes.length > r->bound)
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
        (r->request->method == KNURL_HTTP_HEAD || code == 304)) {
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
        r->complete_reply = 1;
        r->phase = KNURL_RESPONSE_FINISHING;
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
    saved = r->headers;
    saved_status = r->outcome.status;
    saved_length = r->length;
    saved_has_length = r->has_length;
    saved_explicit_length = r->explicit_length_header;
    saved_close = r->close_connection;
    status = knurl_reply_new(&r->headers, error);
    if (status != KNURL_OK) {
        r->headers = saved;
        return status;
    }
    r->outcome.status = reply->selected ? reply->status : 204;
    r->explicit_length_header = 0;
    for (i = 0; status == KNURL_OK && i < reply->header_count; ++i) {
        if (knurl_text_equal_case(reply->headers[i].name.data,
                                  "content-length")) {
            if (r->outcome.status == 304 ||
                (r->request->method == KNURL_HTTP_HEAD &&
                 !status_bodyless(r->outcome.status))) {
                status = header(r, reply->headers[i].name.data,
                                reply->headers[i].value.data, 1, 1, error);
                if (status == KNURL_OK)
                    r->explicit_length_header = 1;
            }
        } else if (!knurl_text_equal_case(reply->headers[i].name.data,
                                          "transfer-encoding")) {
            status = header(r, reply->headers[i].name.data,
                            reply->headers[i].value.data,
                            !knurl_text_equal_case(reply->headers[i].name.data,
                                                   "set-cookie"),
                            1, error);
        }
    }
    if (status == KNURL_OK && reply->content_type != NULL)
        status = header(r, "content-type", reply->content_type, 1, 1, error);
    if (status == KNURL_OK) {
        if (reply->kind == KNURL_REPLY_SOURCE) {
            reply->source->owner = NULL;
            if (reply->source->config.reset_before_reply &&
                reply->source->config.reset != NULL && !bodyless(r)) {
                r->busy = reply->busy = 1;
                status = knurl_source_reset(reply->source, error);
                r->busy = reply->busy = 0;
            }
            if (status == KNURL_OK)
                status = knurl_response_producer(r, reply->source, error);
            if (status != KNURL_OK)
                reply->source->owner = reply;
        } else if (reply->kind == KNURL_REPLY_FILE) {
            knurl_u64 zero;
            zero.high = zero.low = 0;
            if (!status_bodyless(r->outcome.status)) {
                status = knurl_reply_open_file(reply, error);
                if (status == KNURL_EOF) {
                    r->outcome.status = 404;
                    r->has_length = r->producer = 1;
                    r->length = zero;
                    status = KNURL_OK;
                } else if (status == KNURL_OK) {
                    if (reply->file_length.high || reply->file_length.low) {
                        conditional = knurl_response_file_metadata(
                                          r, reply->file_modified, modified,
                                          sizeof(modified)) == 304;
                        if (!conditional && modified[0] != '\0')
                            status = header(r, "last-modified", modified, 1, 1,
                                            error);
                    }
                    if (conditional) {
                        r->outcome.status = 304;
                        r->has_length = r->producer = 1;
                        r->length = zero;
                    } else if (status == KNURL_OK)
                        status = knurl_response_file_take(
                            r, reply->file, zero, reply->file_length, error);
                }
            }
        } else {
            r->has_length = !status_bodyless(r->outcome.status);
            (void)knurl_u64_from_size(reply->body_length, &r->length, NULL);
        }
    }
    if (status_bodyless(r->outcome.status) && !conditional)
        r->has_length = 0;
    if (status == KNURL_OK && r->outcome.status >= 400 &&
        reply->kind != KNURL_REPLY_SOURCE && reply->body_length == 0 &&
        (reply->kind != KNURL_REPLY_FILE ||
         (r->file_remaining.high == 0 && r->file_remaining.low == 0))) {
        status = knurl_response_pretty(r->outcome.status, &pretty, error);
        if (status == KNURL_OK && pretty.data != NULL) {
            r->has_length = 1;
            status = knurl_u64_from_size(pretty.length, &r->length, error);
        }
    }
    if (status == KNURL_OK)
        status = knurl_response_begin(r, error);
    if (status != KNURL_OK) {
        knurl_owned_bytes_cleanup(&pretty);
        if (r->source != NULL) {
            r->source->owner = reply;
            r->source->resume = NULL;
            r->source->resume_data = NULL;
            r->source->loop = NULL;
            r->source = NULL;
        }
        r->file = -1;
        r->producer = r->chunked = r->suppressed = 0;
        r->has_length = saved_has_length;
        r->explicit_length_header = saved_explicit_length;
        r->length = saved_length;
        r->close_connection = saved_close;
        (void)knurl_reply_destroy(r->headers, NULL);
        r->headers = saved;
        r->outcome.status = saved_status;
        return status;
    }
    (void)knurl_reply_destroy(saved, NULL);
    if (reply->kind == KNURL_REPLY_SOURCE)
        reply->source = NULL;
    else if (reply->kind == KNURL_REPLY_FILE && r->file == reply->file &&
             r->file != -1)
        reply->file = -1;
    else if (pretty.data == NULL) {
        r->memory = reply->body;
        r->memory_length = reply->body_length;
        reply->body = NULL;
        reply->body_length = 0;
        r->producer = 1;
    }
    if (pretty.data != NULL) {
        r->memory = pretty.data;
        r->memory_length = pretty.length;
        r->producer = 1;
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
    if (!r->outcome.committed && !r->inflight) {
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
    r->busy = 1;
    knurl_reply_retire(r->request->reply);
    r->busy = 0;
    while (r->retired != NULL) {
        retired = r->retired;
        r->retired = retired->retired_next;
        r->busy = 1;
        knurl_source_release(retired, retired->failure);
        r->busy = 0;
    }
    if (r->pending_head != NULL && r->phase != KNURL_RESPONSE_ABORTED) {
        status = flush_head(r, NULL);
        if (status != KNURL_OK && status != KNURL_AGAIN) {
            knurl_response_fail(r, status);
            return;
        }
        if (r->pending_head != NULL)
            return;
    }
    if (r->blocked &&
        (r->phase == KNURL_RESPONSE_ACTIVE ||
         r->phase == KNURL_RESPONSE_BUILDING) &&
        r->bound - r->queued >= r->writable_need) {
        r->blocked = 0;
        if (r->writable != NULL)
            r->writable(r, r->userdata);
        if ((r->phase == KNURL_RESPONSE_ACTIVE ||
             r->phase == KNURL_RESPONSE_BUILDING) &&
            r->request->plan.on_writable != NULL)
            r->request->plan.on_writable(r->request, r->request->plan.userdata);
    }
    if (r->phase != KNURL_RESPONSE_ACTIVE &&
        !(r->phase == KNURL_RESPONSE_FINISHING && r->complete_reply &&
          r->ending == NULL))
        return;
    if (r->suppressed) {
        r->eof = 1;
        (void)end_response(r, NULL, 0, NULL);
        return;
    }
    if (!r->producer || r->paused)
        return;
    if (r->eof) {
        status = end_response(r, NULL, 0, NULL);
        if (status != KNURL_OK && status != KNURL_AGAIN)
            knurl_response_fail(r, status);
        return;
    }
    capacity = r->bound - r->queued;
    if (capacity <= KNURL_RESPONSE_CHUNK_OVERHEAD)
        return;
    capacity -= KNURL_RESPONSE_CHUNK_OVERHEAD;
    if (capacity > r->chunk)
        capacity = r->chunk;
    buffer = NULL;
    count = 0;
    status = KNURL_EOF;
    if (r->source != NULL ||
        (r->file != -1 && (r->file_remaining.high || r->file_remaining.low))) {
        buffer = malloc(capacity);
        if (buffer == NULL) {
            knurl_response_fail(r, KNURL_NOMEM);
            return;
        }
    }
    if (r->source != NULL) {
        r->busy = 1;
        if (r->source->config.execution == KNURL_SOURCE_NATIVE_READER)
            status = knurl_source_native_pull(r->source, r, buffer, capacity,
                                              &count, NULL);
        else {
            r->source->resume_requested = 0;
            status =
                knurl_source_pull(r->source, r, buffer, capacity, &count, NULL);
        }
        r->busy = 0;
    } else if (r->file != -1 &&
               (r->file_remaining.high || r->file_remaining.low)) {
        if (knurl_u64_to_size(r->file_remaining, &remaining, NULL) ==
                KNURL_OK &&
            capacity > remaining)
            capacity = remaining;
        (void)knurl_file_offset(r->file_offset, &offset);
        do {
            result = pread(r->file, buffer, capacity, offset);
        } while (result == -1 && errno == EINTR);
        status = result > 0 ? KNURL_OK : KNURL_IO;
        if (result > 0) {
            count = (size_t)result;
            (void)add_size(r->file_offset, count, &r->file_offset);
            (void)knurl_u64_from_size(count, &amount, NULL);
            r->file_remaining = difference(r->file_remaining, amount);
        }
    } else if (r->memory_position < r->memory_length) {
        count = r->memory_length - r->memory_position;
        if (count > capacity)
            count = capacity;
        status = KNURL_OK;
    }
    if (status == KNURL_OK) {
        status = write_body(
            r, buffer != NULL ? buffer : r->memory + r->memory_position, count,
            &accepted, NULL);
        if (status == KNURL_OK && accepted != count)
            status = KNURL_IO;
        if (buffer == NULL)
            r->memory_position += accepted;
    } else if (status == KNURL_AGAIN) {
        r->paused = !(r->source != NULL &&
                      r->source->config.execution == KNURL_SOURCE_OWNER_LOOP &&
                      r->source->resume_requested);
        if (!r->paused)
            r->schedule(r);
    } else if (status == KNURL_EOF) {
        r->eof = 1;
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
    segment = r->head;
    if (!segment->interim && (count != 0 || retry) && !r->outcome.committed) {
        r->outcome.committed = 1;
        knurl_policy_observe(r->request, KNURL_OBSERVE_RESPONSE_COMMITTED, NULL,
                             r->outcome.status, KNURL_OK);
    }
    if (add_size(r->outcome.wire_bytes, count, &r->outcome.wire_bytes) !=
        KNURL_OK) {
        (void)knurl_response_abort(r, KNURL_OVERFLOW, NULL);
        return;
    }
    if (segment->interim &&
        add_size(r->outcome.interim_wire_bytes, count,
                 &r->outcome.interim_wire_bytes) != KNURL_OK) {
        (void)knurl_response_abort(r, KNURL_OVERFLOW, NULL);
        return;
    }
    if (count != 0 && r->progress != NULL)
        r->progress(r->progress_data, count);
    start = offset > segment->body_start ? offset : segment->body_start;
    end = offset + count < segment->body_start + segment->body_length
              ? offset + count
              : segment->body_start + segment->body_length;
    if (end > start && add_size(r->outcome.body_bytes, end - start,
                                &r->outcome.body_bytes) != KNURL_OK)
        (void)knurl_response_abort(r, KNURL_OVERFLOW, NULL);
}

void knurl_response_shift(knurl_response *r)
{
    struct knurl_response_segment *segment;
    segment = r->head;
    r->head = segment->next;
    if (r->head == NULL)
        r->tail = NULL;
    r->queued -= segment->length;
    free(segment->data);
    free(segment);
}

void knurl_response_terminal(knurl_response *r)
{
    knurl_u64 now;
    if (r->terminal_notified)
        return;
    r->terminal_notified = 1;
    if (knurl_loop_monotonic(&now, NULL) == KNURL_OK &&
        compare(now, r->started) >= 0)
        r->outcome.duration_ms = difference(now, r->started);
    if (r->phase != KNURL_RESPONSE_ABORTED)
        r->phase = KNURL_RESPONSE_COMPLETE;
    r->busy = 1;
    knurl_reply_retire(r->request->reply);
    /* Failed/unselected staged sources were never transferred to r->source.
     * Close them while request metadata/application associations still live. */
    if (r->request->reply != NULL && r->request->reply->source != NULL) {
        knurl_source *source = r->request->reply->source;
        r->request->reply->source = NULL;
        knurl_source_release(source, source->failure != KNURL_OK
                                         ? source->failure
                                         : KNURL_CANCELLED);
    }
    clear_producer(r, r->outcome.cause != KNURL_OK
                          ? r->outcome.cause
                          : (r->suppressed ? KNURL_CANCELLED : KNURL_OK));
    while (r->retired != NULL) {
        knurl_source *source;
        source = r->retired;
        r->retired = source->retired_next;
        knurl_source_release(source, source->failure);
    }
    if (r->closed != NULL)
        r->closed(r, &r->outcome, r->userdata);
    r->busy = 0;
}

void knurl_response_release(knurl_response *r)
{
    if (r == NULL)
        return;
    knurl_response_terminal(r);
    clear_queue(r);
    (void)knurl_reply_destroy(r->headers, NULL);
    free(r->ending);
    free(r->trailer_names);
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
    if (r->producer || r->has_length || r->trailers_enabled)
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
    saved = r->headers;
    old_status = r->outcome.status;
    old_close = r->close_connection;
    if (status == KNURL_OK) {
        r->headers = copy;
        r->outcome.status = 101;
        r->close_connection = 0;
        status = knurl_response_begin(r, error);
    }
    if (status != KNURL_OK) {
        r->headers = saved;
        r->outcome.status = old_status;
        r->close_connection = old_close;
        r->suppressed = r->chunked = 0;
        free(ending);
        (void)knurl_reply_destroy(copy, NULL);
        return status;
    }
    (void)knurl_reply_destroy(saved, NULL);
    r->ending = ending;
    r->ending_length = 0;
    r->eof = 1;
    r->phase = KNURL_RESPONSE_FINISHING;
    knurl_error_clear(error);
    return KNURL_OK;
}

#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "body_internal.h"
#include "request_internal.h"
#include "loop_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void knurl_body_policy_init_materialized(knurl_body_policy *policy)
{
    knurl_body_policy_init(policy);
    if (policy != NULL)
        policy->mode = KNURL_BODY_MATERIALIZED;
}

void knurl_body_policy_init_stream(knurl_body_policy *policy)
{
    knurl_body_policy_init(policy);
    if (policy != NULL)
        policy->mode = KNURL_BODY_STREAM;
}

knurl_u64 knurl_body_policy_limit(const knurl_body_policy *policy,
                                  knurl_u64 fallback)
{
    if (policy->max_bytes.high || policy->max_bytes.low)
        return policy->max_bytes;
    if (policy->mode == KNURL_BODY_STREAMING_UPLOAD) {
        fallback.high = 0;
        fallback.low = 3221225472UL;
    }
    return fallback;
}

knurl_status knurl_body_create(const knurl_body_policy *policy,
                               const char *directory, knurl_body **out,
                               knurl_error *error)
{
    knurl_body *body;
    size_t limit;
    size_t memory_limit;
    int fits;
    *out = NULL;
    if (policy == NULL || !knurl_u64_valid(policy->max_bytes) ||
        (policy->mode != KNURL_BODY_NONE && policy->mode != KNURL_BODY_JSON &&
         policy->mode != KNURL_BODY_STREAMING_UPLOAD &&
         policy->mode != KNURL_BODY_BUFFERED &&
         policy->mode != KNURL_BODY_MATERIALIZED &&
         policy->mode != KNURL_BODY_STREAM))
        return knurl_fail(error, KNURL_INVALID, "body_plan",
                          "select NONE, JSON, BUFFERED, MATERIALIZED or "
                          "STREAM with a valid limit");
    limit = 0;
    fits = knurl_u64_to_size(policy->max_bytes, &limit, NULL) == KNURL_OK;
    memory_limit = policy->memory_buffer_limit_bytes;
    if (memory_limit == 0)
        memory_limit = fits && limit < 262144UL ? limit : 262144UL;
    if (policy->mode != KNURL_BODY_NONE && policy->mode != KNURL_BODY_STREAM &&
        policy->mode != KNURL_BODY_STREAMING_UPLOAD && fits &&
        memory_limit > limit)
        return knurl_fail(error, KNURL_INVALID, "body_plan",
                          "body memory threshold must not exceed its limit");
    if (policy->mode == KNURL_BODY_STREAMING_UPLOAD &&
        policy->disk_spool_disabled && (!fits || memory_limit < limit))
        return knurl_fail(
            error, KNURL_INVALID, "body_plan",
            "upload policy cannot disable spooling unless fully buffered");
    body = calloc(1, sizeof(*body));
    if (body == NULL)
        return knurl_fail(error, KNURL_NOMEM, "body_plan",
                          "cannot allocate body storage");
    body->policy = *policy;
    body->fd = -1;
    body->memory_limit = memory_limit;
    if (directory != NULL) {
        body->directory = knurl_copy_text(directory);
        if (body->directory == NULL) {
            free(body);
            return knurl_fail(error, KNURL_NOMEM, "body_plan",
                              "cannot copy spill directory");
        }
    }
    *out = body;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_body_append(knurl_body *body, const void *data,
                               size_t length, knurl_error *error)
{
    knurl_u64 amount;
    knurl_u64 next;
    knurl_status status;
    unsigned char *memory;
    size_t size;
    size_t previous;
    size_t capacity;
    int order;
    status = knurl_u64_from_size(length, &amount, error);
    if (status == KNURL_OK)
        status = knurl_u64_add(body->length, amount, &next, error);
    if (status != KNURL_OK)
        return status;
    (void)knurl_u64_compare(next, body->policy.max_bytes, &order, NULL);
    if (order > 0)
        return knurl_fail(error, KNURL_LIMIT, "body_receive",
                          "body exceeds its admitted limit");
    if (body->policy.mode == KNURL_BODY_STREAM) {
        body->length = next;
        return KNURL_OK;
    }
    if (length == 0)
        return KNURL_OK;
    if (body->fd == -1 && (knurl_u64_to_size(next, &size, NULL) != KNURL_OK ||
                           size > body->memory_limit)) {
        if (body->policy.disk_spool_disabled)
            return knurl_fail(error, KNURL_LIMIT, "body_receive",
                              "body exceeds its memory budget");
        status = knurl_private_file(
            body->directory != NULL ? body->directory : "/tmp", "knurl-body",
            &body->fd, &body->path, error);
        if (status != KNURL_OK)
            return status;
        if (knurl_u64_to_size(body->length, &previous, NULL) != KNURL_OK)
            return knurl_fail(error, KNURL_OVERFLOW, "body_receive",
                              "memory length cannot fit size_t");
        status = knurl_file_write(body->fd, body->memory, previous, error);
        if (status != KNURL_OK)
            return status;
        free(body->memory);
        body->memory = NULL;
        body->capacity = 0;
    }
    if (body->fd != -1) {
        status = knurl_file_write(body->fd, data, length, error);
        if (status != KNURL_OK)
            return status;
    } else {
        (void)knurl_u64_to_size(body->length, &previous, NULL);
        if (size > body->capacity) {
            capacity = body->capacity == 0 ? 1024 : body->capacity;
            if (capacity > body->memory_limit)
                capacity = body->memory_limit;
            while (capacity < size) {
                if (capacity > body->memory_limit / 2) {
                    capacity = body->memory_limit;
                    break;
                }
                capacity *= 2;
            }
            memory = realloc(body->memory, capacity);
            if (memory == NULL)
                return knurl_fail(error, KNURL_NOMEM, "body_receive",
                                  "cannot grow bounded body memory");
            body->memory = memory;
            body->capacity = capacity;
        }
        memcpy(body->memory + previous, data, length);
    }
    body->length = next;
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_body_release(knurl_body *body)
{
    if (body == NULL)
        return;
    if (body->fd != -1)
        (void)close(body->fd);
    if (body->path != NULL)
        (void)unlink(body->path);
    free(body->path);
    free(body->directory);
    free(body->memory);
    free(body);
}

static knurl_status readable(const knurl_body *body, knurl_error *error)
{
    if (body == NULL || !body->complete || body->failure != KNURL_OK ||
        body->policy.mode == KNURL_BODY_STREAM)
        return knurl_fail(
            error, KNURL_STATE, "body_read",
            "only successfully completed materialized bodies have readers");
    return KNURL_OK;
}

knurl_body *knurl_request_body(knurl_request *request)
{
    return request != NULL && request->body_store != NULL &&
                   request->body_store->complete &&
                   request->body_store->failure == KNURL_OK &&
                   request->body_store->policy.mode != KNURL_BODY_NONE &&
                   request->body_store->policy.mode != KNURL_BODY_STREAM
               ? request->body_store
               : NULL;
}

knurl_body_storage knurl_body_kind(const knurl_body *body)
{
    return body == NULL
               ? KNURL_BODY_STORAGE_NONE
               : (body->fd != -1 ? KNURL_BODY_FILE : KNURL_BODY_MEMORY);
}

knurl_status knurl_body_length(const knurl_body *body, knurl_u64 *out,
                               knurl_error *error)
{
    knurl_status status;
    if (out != NULL)
        out->high = out->low = 0;
    status = readable(body, error);
    if (status != KNURL_OK)
        return status;
    if (out == NULL)
        return knurl_fail(error, KNURL_INVALID, "body_length",
                          "length output is required");
    *out = body->length;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_body_bytes(const knurl_body *body, knurl_bytes *out,
                              knurl_error *error)
{
    knurl_status status;
    if (out != NULL) {
        out->data = NULL;
        out->length = 0;
    }
    status = readable(body, error);
    if (status != KNURL_OK)
        return status;
    if (body->fd != -1)
        return knurl_fail(error, KNURL_STATE, "body_bytes",
                          "body is file-backed; use its bounded reader");
    if (out == NULL)
        return knurl_fail(error, KNURL_INVALID, "body_bytes",
                          "byte view output is required");
    out->data = body->memory;
    (void)knurl_u64_to_size(body->length, &out->length, NULL);
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_body_file(const knurl_body *body, int *fd, const char **path,
                             knurl_error *error)
{
    knurl_status status;
    if (fd != NULL)
        *fd = -1;
    if (path != NULL)
        *path = NULL;
    status = readable(body, error);
    if (status != KNURL_OK)
        return status;
    if (fd == NULL || path == NULL)
        return knurl_fail(error, KNURL_INVALID, "body_file",
                          "descriptor and path outputs are required");
    if (body->fd == -1)
        return knurl_fail(error, KNURL_STATE, "body_file",
                          "body is memory-backed");
    *fd = body->fd;
    *path = body->path;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_body_read(knurl_body *body, void *buffer, size_t capacity,
                             size_t *produced, knurl_error *error)
{
    knurl_status status;
    knurl_u64 amount;
    knurl_u64 remaining;
    size_t size;
    size_t position;
    off_t offset;
    ssize_t count;
    if (produced != NULL)
        *produced = 0;
    status = readable(body, error);
    if (status != KNURL_OK)
        return status;
    if (buffer == NULL || capacity == 0 || produced == NULL)
        return knurl_fail(error, KNURL_INVALID, "body_read",
                          "provide a buffer, positive capacity and byte count");
    remaining.high = body->length.high - body->position.high -
                     (body->length.low < body->position.low ? 1 : 0);
    remaining.low = (body->length.low - body->position.low) & 0xffffffffUL;
    knurl_error_clear(error);
    if (remaining.high == 0 && remaining.low == 0)
        return KNURL_EOF;
    if (knurl_u64_to_size(remaining, &size, NULL) == KNURL_OK &&
        capacity > size)
        capacity = size;
    if (body->fd != -1) {
        if (!knurl_file_offset(body->position, &offset))
            return knurl_fail(error, KNURL_OVERFLOW, "body_read",
                              "body read offset cannot fit off_t");
        do {
            count = pread(body->fd, buffer, capacity, offset);
        } while (count == -1 && errno == EINTR);
        if (count <= 0)
            return knurl_fail(error, KNURL_IO, "body_read",
                              "file body ended short or could not be read");
        size = (size_t)count;
    } else {
        (void)knurl_u64_to_size(body->position, &position, NULL);
        memcpy(buffer, body->memory + position, capacity);
        size = capacity;
    }
    (void)knurl_u64_from_size(size, &amount, NULL);
    (void)knurl_u64_add(body->position, amount, &body->position, NULL);
    *produced = size;
    return KNURL_OK;
}

knurl_status knurl_body_rewind(knurl_body *body, knurl_error *error)
{
    knurl_status status;
    status = readable(body, error);
    if (status != KNURL_OK)
        return status;
    body->position.high = body->position.low = 0;
    knurl_error_clear(error);
    return KNURL_OK;
}

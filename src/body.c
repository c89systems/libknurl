#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "body_internal.h"
#include <knurl/body.h>
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
    body = calloc(1, sizeof(struct knurl_body_impl));
    if (body == NULL)
        return knurl_fail(error, KNURL_NOMEM, "body_plan",
                          "cannot allocate body storage");
    knurl_body_receiver_init(body);
    body->impl->policy = *policy;
    body->impl->fd = -1;
    body->impl->memory_limit = memory_limit;
    if (directory != NULL) {
        body->impl->directory = knurl_copy_text(directory);
        if (body->impl->directory == NULL) {
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
        status = knurl_u64_add(body->impl->length, amount, &next, error);
    if (status != KNURL_OK)
        return status;
    (void)knurl_u64_compare(next, body->impl->policy.max_bytes, &order, NULL);
    if (order > 0)
        return knurl_fail(error, KNURL_LIMIT, "body_receive",
                          "body exceeds its admitted limit");
    if (body->impl->policy.mode == KNURL_BODY_STREAM) {
        body->impl->length = next;
        return KNURL_OK;
    }
    if (length == 0)
        return KNURL_OK;
    if (body->impl->fd == -1 &&
        (knurl_u64_to_size(next, &size, NULL) != KNURL_OK ||
         size > body->impl->memory_limit)) {
        if (body->impl->policy.disk_spool_disabled)
            return knurl_fail(error, KNURL_LIMIT, "body_receive",
                              "body exceeds its memory budget");
        status = knurl_private_file(
            body->impl->directory != NULL ? body->impl->directory : "/tmp",
            "knurl-body", &body->impl->fd, &body->impl->path, error);
        if (status != KNURL_OK)
            return status;
        if (knurl_u64_to_size(body->impl->length, &previous, NULL) != KNURL_OK)
            return knurl_fail(error, KNURL_OVERFLOW, "body_receive",
                              "memory length cannot fit size_t");
        status = knurl_file_write(body->impl->fd, body->impl->memory, previous,
                                  error);
        if (status != KNURL_OK)
            return status;
        free(body->impl->memory);
        body->impl->memory = NULL;
        body->impl->capacity = 0;
    }
    if (body->impl->fd != -1) {
        status = knurl_file_write(body->impl->fd, data, length, error);
        if (status != KNURL_OK)
            return status;
    } else {
        (void)knurl_u64_to_size(body->impl->length, &previous, NULL);
        if (size > body->impl->capacity) {
            capacity = body->impl->capacity == 0 ? 1024 : body->impl->capacity;
            if (capacity > body->impl->memory_limit)
                capacity = body->impl->memory_limit;
            while (capacity < size) {
                if (capacity > body->impl->memory_limit / 2) {
                    capacity = body->impl->memory_limit;
                    break;
                }
                capacity *= 2;
            }
            memory = realloc(body->impl->memory, capacity);
            if (memory == NULL)
                return knurl_fail(error, KNURL_NOMEM, "body_receive",
                                  "cannot grow bounded body memory");
            body->impl->memory = memory;
            body->impl->capacity = capacity;
        }
        memcpy(body->impl->memory + previous, data, length);
    }
    body->impl->length = next;
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_body_release(knurl_body *body)
{
    if (body == NULL)
        return;
    if (body->impl->fd != -1)
        (void)close(body->impl->fd);
    if (body->impl->path != NULL)
        (void)unlink(body->impl->path);
    free(body->impl->path);
    free(body->impl->directory);
    free(body->impl->memory);
    free(body);
}

static knurl_status readable(const knurl_body *body, knurl_error *error)
{
    if (body == NULL || !body->impl->complete ||
        body->impl->failure != KNURL_OK ||
        body->impl->policy.mode == KNURL_BODY_STREAM)
        return knurl_fail(
            error, KNURL_STATE, "body_read",
            "only successfully completed materialized bodies have readers");
    return KNURL_OK;
}

knurl_body *knurl_request_body(knurl_request *request)
{
    return request != NULL && request->impl->body_store != NULL &&
                   request->impl->body_store->impl->complete &&
                   request->impl->body_store->impl->failure == KNURL_OK &&
                   request->impl->body_store->impl->policy.mode !=
                       KNURL_BODY_NONE &&
                   request->impl->body_store->impl->policy.mode !=
                       KNURL_BODY_STREAM
               ? request->impl->body_store
               : NULL;
}

knurl_body_storage knurl_body_kind(const knurl_body *body)
{
    return body == NULL
               ? KNURL_BODY_STORAGE_NONE
               : (body->impl->fd != -1 ? KNURL_BODY_FILE : KNURL_BODY_MEMORY);
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
    *out = body->impl->length;
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
    if (body->impl->fd != -1)
        return knurl_fail(error, KNURL_STATE, "body_bytes",
                          "body is file-backed; use its bounded reader");
    if (out == NULL)
        return knurl_fail(error, KNURL_INVALID, "body_bytes",
                          "byte view output is required");
    out->data = body->impl->memory;
    (void)knurl_u64_to_size(body->impl->length, &out->length, NULL);
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
    if (body->impl->fd == -1)
        return knurl_fail(error, KNURL_STATE, "body_file",
                          "body is memory-backed");
    *fd = body->impl->fd;
    *path = body->impl->path;
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
    remaining.high =
        body->impl->length.high - body->impl->position.high -
        (body->impl->length.low < body->impl->position.low ? 1 : 0);
    remaining.low =
        (body->impl->length.low - body->impl->position.low) & 0xffffffffUL;
    knurl_error_clear(error);
    if (remaining.high == 0 && remaining.low == 0)
        return KNURL_EOF;
    if (knurl_u64_to_size(remaining, &size, NULL) == KNURL_OK &&
        capacity > size)
        capacity = size;
    if (body->impl->fd != -1) {
        if (!knurl_file_offset(body->impl->position, &offset))
            return knurl_fail(error, KNURL_OVERFLOW, "body_read",
                              "body read offset cannot fit off_t");
        do {
            count = pread(body->impl->fd, buffer, capacity, offset);
        } while (count == -1 && errno == EINTR);
        if (count <= 0)
            return knurl_fail(error, KNURL_IO, "body_read",
                              "file body ended short or could not be read");
        size = (size_t)count;
    } else {
        (void)knurl_u64_to_size(body->impl->position, &position, NULL);
        memcpy(buffer, body->impl->memory + position, capacity);
        size = capacity;
    }
    (void)knurl_u64_from_size(size, &amount, NULL);
    (void)knurl_u64_add(body->impl->position, amount, &body->impl->position,
                        NULL);
    *produced = size;
    return KNURL_OK;
}

knurl_status knurl_body_rewind(knurl_body *body, knurl_error *error)
{
    knurl_status status;
    status = readable(body, error);
    if (status != KNURL_OK)
        return status;
    body->impl->position.high = body->impl->position.low = 0;
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_body_receiver_init(knurl_body *receiver)
{
    static const knurl_body methods = {knurl_body_kind,
                                       knurl_body_length,
                                       knurl_body_bytes,
                                       knurl_body_file,
                                       knurl_body_read,
                                       knurl_body_rewind,
                                       NULL};
    *receiver = methods;
    receiver->impl = (struct knurl_body_impl *)receiver;
}

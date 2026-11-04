#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "body_internal.h"
#include "request_internal.h"
#include "loop_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void rotmos_body_policy_init_materialized(rotmos_body_policy *policy)
{
    rotmos_body_policy_init(policy);
    if (policy != NULL)
        policy->mode = ROTMOS_BODY_MATERIALIZED;
}

void rotmos_body_policy_init_stream(rotmos_body_policy *policy)
{
    rotmos_body_policy_init(policy);
    if (policy != NULL)
        policy->mode = ROTMOS_BODY_STREAM;
}

rotmos_u64 rotmos_body_policy_limit(const rotmos_body_policy *policy,
                                    rotmos_u64 fallback)
{
    if (policy->max_bytes.high || policy->max_bytes.low)
        return policy->max_bytes;
    if (policy->mode == ROTMOS_BODY_STREAMING_UPLOAD) {
        fallback.high = 0;
        fallback.low = 3221225472UL;
    }
    return fallback;
}

rotmos_status rotmos_body_create(const rotmos_body_policy *policy,
                                 const char *directory, rotmos_body **out,
                                 rotmos_error *error)
{
    rotmos_body *body;
    size_t limit;
    size_t memory_limit;
    int fits;
    *out = NULL;
    if (policy == NULL || !rotmos_u64_valid(policy->max_bytes) ||
        (policy->mode != ROTMOS_BODY_NONE && policy->mode != ROTMOS_BODY_JSON &&
         policy->mode != ROTMOS_BODY_STREAMING_UPLOAD &&
         policy->mode != ROTMOS_BODY_BUFFERED &&
         policy->mode != ROTMOS_BODY_MATERIALIZED &&
         policy->mode != ROTMOS_BODY_STREAM))
        return rotmos_fail(error, ROTMOS_INVALID, "body_plan",
                           "select NONE, JSON, BUFFERED, MATERIALIZED or "
                           "STREAM with a valid limit");
    limit = 0;
    fits = rotmos_u64_to_size(policy->max_bytes, &limit, NULL) == ROTMOS_OK;
    memory_limit = policy->memory_buffer_limit_bytes;
    if (memory_limit == 0)
        memory_limit = fits && limit < 262144UL ? limit : 262144UL;
    if (policy->mode != ROTMOS_BODY_NONE &&
        policy->mode != ROTMOS_BODY_STREAM &&
        policy->mode != ROTMOS_BODY_STREAMING_UPLOAD && fits &&
        memory_limit > limit)
        return rotmos_fail(error, ROTMOS_INVALID, "body_plan",
                           "body memory threshold must not exceed its limit");
    if (policy->mode == ROTMOS_BODY_STREAMING_UPLOAD &&
        policy->disk_spool_disabled && (!fits || memory_limit < limit))
        return rotmos_fail(
            error, ROTMOS_INVALID, "body_plan",
            "upload policy cannot disable spooling unless fully buffered");
    body = calloc(1, sizeof(*body));
    if (body == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "body_plan",
                           "cannot allocate body storage");
    body->policy = *policy;
    body->fd = -1;
    body->memory_limit = memory_limit;
    if (directory != NULL) {
        body->directory = rotmos_copy_text(directory);
        if (body->directory == NULL) {
            free(body);
            return rotmos_fail(error, ROTMOS_NOMEM, "body_plan",
                               "cannot copy spill directory");
        }
    }
    *out = body;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_body_append(rotmos_body *body, const void *data,
                                 size_t length, rotmos_error *error)
{
    rotmos_u64 amount;
    rotmos_u64 next;
    rotmos_status status;
    unsigned char *memory;
    size_t size;
    size_t previous;
    size_t capacity;
    int order;
    status = rotmos_u64_from_size(length, &amount, error);
    if (status == ROTMOS_OK)
        status = rotmos_u64_add(body->length, amount, &next, error);
    if (status != ROTMOS_OK)
        return status;
    (void)rotmos_u64_compare(next, body->policy.max_bytes, &order, NULL);
    if (order > 0)
        return rotmos_fail(error, ROTMOS_LIMIT, "body_receive",
                           "body exceeds its admitted limit");
    if (body->policy.mode == ROTMOS_BODY_STREAM) {
        body->length = next;
        return ROTMOS_OK;
    }
    if (length == 0)
        return ROTMOS_OK;
    if (body->fd == -1 && (rotmos_u64_to_size(next, &size, NULL) != ROTMOS_OK ||
                           size > body->memory_limit)) {
        if (body->policy.disk_spool_disabled)
            return rotmos_fail(error, ROTMOS_LIMIT, "body_receive",
                               "body exceeds its memory budget");
        status = rotmos_private_file(body->directory, "rotmos-body", &body->fd,
                                     &body->path, error);
        if (status != ROTMOS_OK)
            return status;
        if (rotmos_u64_to_size(body->length, &previous, NULL) != ROTMOS_OK)
            return rotmos_fail(error, ROTMOS_OVERFLOW, "body_receive",
                               "memory length cannot fit size_t");
        status = rotmos_file_write(body->fd, body->memory, previous, error);
        if (status != ROTMOS_OK)
            return status;
        free(body->memory);
        body->memory = NULL;
        body->capacity = 0;
    }
    if (body->fd != -1) {
        status = rotmos_file_write(body->fd, data, length, error);
        if (status != ROTMOS_OK)
            return status;
    } else {
        (void)rotmos_u64_to_size(body->length, &previous, NULL);
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
                return rotmos_fail(error, ROTMOS_NOMEM, "body_receive",
                                   "cannot grow bounded body memory");
            body->memory = memory;
            body->capacity = capacity;
        }
        memcpy(body->memory + previous, data, length);
    }
    body->length = next;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

void rotmos_body_release(rotmos_body *body)
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

static rotmos_status readable(const rotmos_body *body, rotmos_error *error)
{
    if (body == NULL || !body->complete || body->failure != ROTMOS_OK ||
        body->policy.mode == ROTMOS_BODY_STREAM)
        return rotmos_fail(
            error, ROTMOS_STATE, "body_read",
            "only successfully completed materialized bodies have readers");
    return ROTMOS_OK;
}

rotmos_body *rotmos_request_body(rotmos_request *request)
{
    return request != NULL && request->body_store != NULL &&
                   request->body_store->complete &&
                   request->body_store->failure == ROTMOS_OK &&
                   request->body_store->policy.mode != ROTMOS_BODY_STREAM
               ? request->body_store
               : NULL;
}

rotmos_body_storage rotmos_body_kind(const rotmos_body *body)
{
    return body == NULL
               ? ROTMOS_BODY_STORAGE_NONE
               : (body->fd != -1 ? ROTMOS_BODY_FILE : ROTMOS_BODY_MEMORY);
}

rotmos_status rotmos_body_length(const rotmos_body *body, rotmos_u64 *out,
                                 rotmos_error *error)
{
    rotmos_status status;
    if (out != NULL)
        out->high = out->low = 0;
    status = readable(body, error);
    if (status != ROTMOS_OK)
        return status;
    if (out == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "body_length",
                           "length output is required");
    *out = body->length;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_body_bytes(const rotmos_body *body, rotmos_bytes *out,
                                rotmos_error *error)
{
    rotmos_status status;
    if (out != NULL) {
        out->data = NULL;
        out->length = 0;
    }
    status = readable(body, error);
    if (status != ROTMOS_OK)
        return status;
    if (body->fd != -1)
        return rotmos_fail(error, ROTMOS_STATE, "body_bytes",
                           "body is file-backed; use its bounded reader");
    if (out == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "body_bytes",
                           "byte view output is required");
    out->data = body->memory;
    (void)rotmos_u64_to_size(body->length, &out->length, NULL);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_body_file(const rotmos_body *body, int *fd,
                               const char **path, rotmos_error *error)
{
    rotmos_status status;
    if (fd != NULL)
        *fd = -1;
    if (path != NULL)
        *path = NULL;
    status = readable(body, error);
    if (status != ROTMOS_OK)
        return status;
    if (fd == NULL || path == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "body_file",
                           "descriptor and path outputs are required");
    if (body->fd == -1)
        return rotmos_fail(error, ROTMOS_STATE, "body_file",
                           "body is memory-backed");
    *fd = body->fd;
    *path = body->path;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_body_read(rotmos_body *body, void *buffer, size_t capacity,
                               size_t *produced, rotmos_error *error)
{
    rotmos_status status;
    rotmos_u64 amount;
    rotmos_u64 remaining;
    size_t size;
    size_t position;
    off_t offset;
    ssize_t count;
    if (produced != NULL)
        *produced = 0;
    status = readable(body, error);
    if (status != ROTMOS_OK)
        return status;
    if (buffer == NULL || capacity == 0 || produced == NULL)
        return rotmos_fail(
            error, ROTMOS_INVALID, "body_read",
            "provide a buffer, positive capacity and byte count");
    remaining.high = body->length.high - body->position.high -
                     (body->length.low < body->position.low ? 1 : 0);
    remaining.low = (body->length.low - body->position.low) & 0xffffffffUL;
    rotmos_error_clear(error);
    if (remaining.high == 0 && remaining.low == 0)
        return ROTMOS_EOF;
    if (rotmos_u64_to_size(remaining, &size, NULL) == ROTMOS_OK &&
        capacity > size)
        capacity = size;
    if (body->fd != -1) {
        if (!rotmos_file_offset(body->position, &offset))
            return rotmos_fail(error, ROTMOS_OVERFLOW, "body_read",
                               "body read offset cannot fit off_t");
        do {
            count = pread(body->fd, buffer, capacity, offset);
        } while (count == -1 && errno == EINTR);
        if (count <= 0)
            return rotmos_fail(error, ROTMOS_IO, "body_read",
                               "file body ended short or could not be read");
        size = (size_t)count;
    } else {
        (void)rotmos_u64_to_size(body->position, &position, NULL);
        memcpy(buffer, body->memory + position, capacity);
        size = capacity;
    }
    (void)rotmos_u64_from_size(size, &amount, NULL);
    (void)rotmos_u64_add(body->position, amount, &body->position, NULL);
    *produced = size;
    return ROTMOS_OK;
}

rotmos_status rotmos_body_rewind(rotmos_body *body, rotmos_error *error)
{
    rotmos_status status;
    status = readable(body, error);
    if (status != ROTMOS_OK)
        return status;
    body->position.high = body->position.low = 0;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

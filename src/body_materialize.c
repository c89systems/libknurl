#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "body_internal.h"
#include "request_internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void knurl_body_materialize_config_init(knurl_body_materialize_config *config)
{
    if (config == NULL)
        return;
    config->buffer = NULL;
    config->capacity = 0;
    config->memory_limit_bytes = 262144UL;
    config->directory = NULL;
    config->prefix = NULL;
}

void knurl_body_result_init(knurl_body_result *result)
{
    if (result == NULL)
        return;
    result->kind = KNURL_BODY_STORAGE_NONE;
    result->length.high = result->length.low = 0;
    result->memory.data = NULL;
    result->memory.length = 0;
    result->owns_memory = 0;
    result->path = NULL;
}

void knurl_body_result_cleanup(knurl_body_result *result)
{
    if (result == NULL)
        return;
    if (result->owns_memory)
        free((void *)result->memory.data);
    free(result->path);
    knurl_body_result_init(result);
}

knurl_status
knurl_request_body_materialize(knurl_request *request,
                               const knurl_body_materialize_config *config,
                               knurl_body_result *out, knurl_error *error)
{
    knurl_body_materialize_config defaults;
    knurl_body_result result;
    knurl_body *body;
    knurl_status status;
    unsigned char *memory;
    unsigned char *chunk;
    size_t length;
    size_t used;
    size_t count;
    size_t memory_limit;
    const char *directory;
    int fits;
    int fd;
    if (out != NULL)
        knurl_body_result_init(out);
    if (request == NULL || out == NULL)
        return knurl_fail(error, KNURL_INVALID, "body_materialize",
                          "provide a request and empty result output");
    knurl_body_materialize_config_init(&defaults);
    defaults.memory_limit_bytes = 0;
    if (config == NULL)
        config = &defaults;
    body = knurl_request_body(request);
    status = knurl_body_rewind(body, error);
    if (status != KNURL_OK)
        return status;
    knurl_body_result_init(&result);
    result.kind = KNURL_BODY_MEMORY;
    result.length = body->length;
    fits = knurl_u64_to_size(body->length, &length, NULL) == KNURL_OK;
    memory = NULL;
    chunk = NULL;
    fd = -1;
    used = 0;
    memory_limit = config->memory_limit_bytes;
    if (config->buffer != NULL && config->capacity != 0 &&
        (memory_limit == 0 || memory_limit > config->capacity))
        memory_limit = config->capacity;
    if (memory_limit == 0)
        memory_limit = body->memory_limit;
    directory = config->directory != NULL && config->directory[0] != '\0'
                    ? config->directory
                    : "/tmp";
    if (fits && length == 0) {
        if (config->buffer != NULL && config->capacity != 0)
            result.memory.data = config->buffer;
        *out = result;
        knurl_error_clear(error);
        return KNURL_OK;
    }
    if (fits && config->buffer != NULL && length <= config->capacity &&
        length <= memory_limit) {
        memory = config->buffer;
    } else if (fits && length <= memory_limit) {
        memory = malloc(length);
        if (memory == NULL)
            return knurl_fail(error, KNURL_NOMEM, "body_materialize",
                              "cannot allocate explicit body copy");
        result.owns_memory = 1;
    } else {
        status = knurl_private_file(directory, config->prefix, &fd,
                                    &result.path, error);
        if (status != KNURL_OK)
            return status;
        result.kind = KNURL_BODY_FILE;
        chunk = malloc(8192);
        if (chunk == NULL)
            status = knurl_fail(error, KNURL_NOMEM, "body_materialize",
                                "cannot allocate copy chunk");
    }
    while (status == KNURL_OK) {
        count = 0;
        if (memory != NULL) {
            if (used == length)
                break;
            status = knurl_body_read(body, memory + used, length - used, &count,
                                     error);
        } else {
            status = knurl_body_read(body, chunk, 8192, &count, error);
        }
        if (status == KNURL_EOF) {
            status = KNURL_OK;
            break;
        }
        if (status != KNURL_OK)
            break;
        if (memory != NULL)
            used += count;
        else
            status = knurl_file_write(fd, chunk, count, error);
    }
    if (fd != -1 && close(fd) != 0 && status == KNURL_OK)
        status = knurl_fail(error, KNURL_IO, "body_materialize",
                            "cannot close returned body file");
    free(chunk);
    result.memory.data = memory;
    result.memory.length = memory != NULL ? length : 0;
    if (status == KNURL_OK)
        status = knurl_body_rewind(body, error);
    if (status != KNURL_OK) {
        if (result.path != NULL)
            (void)unlink(result.path);
        knurl_body_result_cleanup(&result);
        return status;
    }
    *out = result;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_request_body_copy(knurl_request *request, void *buffer,
                                     size_t capacity, knurl_body_result *out,
                                     knurl_error *error)
{
    knurl_body_materialize_config config;
    knurl_body_materialize_config_init(&config);
    config.buffer = buffer;
    config.capacity = capacity;
    config.memory_limit_bytes = (size_t)-1;
    return knurl_request_body_materialize(request, &config, out, error);
}

knurl_status
knurl_request_body_spill(knurl_request *request,
                         const knurl_body_materialize_config *config,
                         knurl_body_result *out, knurl_error *error)
{
    return knurl_request_body_materialize(request, config, out, error);
}

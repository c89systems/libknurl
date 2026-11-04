#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "body_internal.h"
#include "request_internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void rotmos_body_materialize_config_init(rotmos_body_materialize_config *config)
{
    if (config == NULL)
        return;
    config->buffer = NULL;
    config->capacity = 0;
    config->memory_limit_bytes = 262144UL;
    config->directory = NULL;
    config->prefix = NULL;
}

void rotmos_body_result_init(rotmos_body_result *result)
{
    if (result == NULL)
        return;
    result->kind = ROTMOS_BODY_STORAGE_NONE;
    result->length.high = result->length.low = 0;
    result->memory.data = NULL;
    result->memory.length = 0;
    result->owns_memory = 0;
    result->path = NULL;
}

void rotmos_body_result_cleanup(rotmos_body_result *result)
{
    if (result == NULL)
        return;
    if (result->owns_memory)
        free((void *)result->memory.data);
    free(result->path);
    rotmos_body_result_init(result);
}

rotmos_status
rotmos_request_body_materialize(rotmos_request *request,
                                const rotmos_body_materialize_config *config,
                                rotmos_body_result *out, rotmos_error *error)
{
    rotmos_body_materialize_config defaults;
    rotmos_body_result result;
    rotmos_body *body;
    rotmos_status status;
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
        rotmos_body_result_init(out);
    if (request == NULL || out == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "body_materialize",
                           "provide a request and empty result output");
    rotmos_body_materialize_config_init(&defaults);
    defaults.memory_limit_bytes = 0;
    if (config == NULL)
        config = &defaults;
    body = rotmos_request_body(request);
    status = rotmos_body_rewind(body, error);
    if (status != ROTMOS_OK)
        return status;
    rotmos_body_result_init(&result);
    result.kind = ROTMOS_BODY_MEMORY;
    result.length = body->length;
    fits = rotmos_u64_to_size(body->length, &length, NULL) == ROTMOS_OK;
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
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    if (fits && config->buffer != NULL && length <= config->capacity &&
        length <= memory_limit) {
        memory = config->buffer;
    } else if (fits && length <= memory_limit) {
        memory = malloc(length);
        if (memory == NULL)
            return rotmos_fail(error, ROTMOS_NOMEM, "body_materialize",
                               "cannot allocate explicit body copy");
        result.owns_memory = 1;
    } else {
        status = rotmos_private_file(directory, config->prefix, &fd,
                                     &result.path, error);
        if (status != ROTMOS_OK)
            return status;
        result.kind = ROTMOS_BODY_FILE;
        chunk = malloc(8192);
        if (chunk == NULL)
            status = rotmos_fail(error, ROTMOS_NOMEM, "body_materialize",
                                 "cannot allocate copy chunk");
    }
    while (status == ROTMOS_OK) {
        count = 0;
        if (memory != NULL) {
            if (used == length)
                break;
            status = rotmos_body_read(body, memory + used, length - used,
                                      &count, error);
        } else {
            status = rotmos_body_read(body, chunk, 8192, &count, error);
        }
        if (status == ROTMOS_EOF) {
            status = ROTMOS_OK;
            break;
        }
        if (status != ROTMOS_OK)
            break;
        if (memory != NULL)
            used += count;
        else
            status = rotmos_file_write(fd, chunk, count, error);
    }
    if (fd != -1 && close(fd) != 0 && status == ROTMOS_OK)
        status = rotmos_fail(error, ROTMOS_IO, "body_materialize",
                             "cannot close returned body file");
    free(chunk);
    result.memory.data = memory;
    result.memory.length = memory != NULL ? length : 0;
    if (status == ROTMOS_OK)
        status = rotmos_body_rewind(body, error);
    if (status != ROTMOS_OK) {
        if (result.path != NULL)
            (void)unlink(result.path);
        rotmos_body_result_cleanup(&result);
        return status;
    }
    *out = result;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_request_body_copy(rotmos_request *request, void *buffer,
                                       size_t capacity, rotmos_body_result *out,
                                       rotmos_error *error)
{
    rotmos_body_materialize_config config;
    rotmos_body_materialize_config_init(&config);
    config.buffer = buffer;
    config.capacity = capacity;
    config.memory_limit_bytes = (size_t)-1;
    return rotmos_request_body_materialize(request, &config, out, error);
}

rotmos_status
rotmos_request_body_spill(rotmos_request *request,
                          const rotmos_body_materialize_config *config,
                          rotmos_body_result *out, rotmos_error *error)
{
    return rotmos_request_body_materialize(request, config, out, error);
}

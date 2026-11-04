#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "upload_internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void rotmos_upload_file_config_init(rotmos_upload_file_config *config)
{
    if (config != NULL) {
        memset(config, 0, sizeof(*config));
        rotmos_body_policy_init_upload(&config->body);
    }
}
static rotmos_status file_error(rotmos_error *error, const char *message)
{
    int code = errno;
    rotmos_status status =
        rotmos_fail(error, ROTMOS_IO, "upload_file", message);
    if (error != NULL) {
        error->source = ROTMOS_ERROR_SYSTEM;
        error->native_code = code;
    }
    return status;
}
static rotmos_status write_file(const rotmos_upload_metadata *metadata,
                                rotmos_source *reader, rotmos_reply *reply,
                                void *data, rotmos_error *error)
{
    FILE *file;
    unsigned char bytes[2048];
    rotmos_u64 total = {0}, amount;
    char count[21];
    size_t n, formatted;
    rotmos_status status;
    (void)metadata;
    if (rotmos_source_cancelled(reader))
        return ROTMOS_CANCELLED;
    file = fopen(data, "wb");
    if (file == NULL)
        return file_error(error, "opening upload destination");
    for (;;) {
        status = rotmos_source_read(reader, bytes, sizeof(bytes), &n, error);
        if (status == ROTMOS_EOF) {
            status = ROTMOS_OK;
            break;
        }
        if (status != ROTMOS_OK)
            break;
        if (fwrite(bytes, 1, n, file) != n) {
            status = file_error(error, "writing upload destination");
            break;
        }
        status = rotmos_u64_from_size(n, &amount, error);
        if (status == ROTMOS_OK)
            status = rotmos_u64_add(total, amount, &total, error);
        if (status != ROTMOS_OK)
            break;
    }
    if (status == ROTMOS_OK && fflush(file) != 0)
        status = file_error(error, "flushing upload destination");
    if (fclose(file) != 0 && status == ROTMOS_OK)
        status = file_error(error, "closing upload destination");
    if (status == ROTMOS_OK)
        status = rotmos_u64_format_decimal(total, count, sizeof(count),
                                           &formatted, error);
    if (status == ROTMOS_OK)
        status = rotmos_reply_text(reply, 200, "text/plain", count, error);
    return status;
}
static rotmos_status add(rotmos_router *router, rotmos_route_group *group,
                         const rotmos_upload_file_config *config,
                         rotmos_route **out, rotmos_error *error)
{
    rotmos_upload_reader_config reader;
    rotmos_status status;
    char *path;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || config->file_path == NULL ||
        config->file_path[0] == 0)
        return rotmos_fail(error, ROTMOS_INVALID, "upload_file",
                           "provide an explicit destination path");
    path = rotmos_copy_text(config->file_path);
    if (path == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "upload_file",
                           "copying destination path");
    rotmos_upload_reader_config_init(&reader);
    reader.method = config->method;
    reader.methods = config->methods;
    reader.path = config->path;
    reader.path_kind = config->path_kind;
    reader.body = config->body;
    reader.hooks = config->hooks;
    reader.buffer_bytes = config->body.memory_buffer_limit_bytes;
    reader.handler = write_file;
    reader.userdata = path;
    status = rotmos_upload_reader_add(router, group, &reader, free, out, error);
    if (status != ROTMOS_OK)
        free(path);
    return status;
}
rotmos_status rotmos_router_upload_file(rotmos_router *router,
                                        const rotmos_upload_file_config *config,
                                        rotmos_route **out, rotmos_error *error)
{
    return add(router, NULL, config, out, error);
}
rotmos_status
rotmos_route_group_upload_file(rotmos_route_group *group,
                               const rotmos_upload_file_config *config,
                               rotmos_route **out, rotmos_error *error)
{
    if (group == NULL) {
        if (out != NULL)
            *out = NULL;
        return rotmos_fail(error, ROTMOS_INVALID, "upload_file",
                           "group is required");
    }
    return add(NULL, group, config, out, error);
}

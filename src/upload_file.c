#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "upload_internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void knurl_upload_file_config_init(knurl_upload_file_config *config)
{
    if (config != NULL) {
        memset(config, 0, sizeof(*config));
        knurl_body_policy_init_upload(&config->body);
    }
}
static knurl_status file_error(knurl_error *error, const char *message)
{
    int code = errno;
    knurl_status status = knurl_fail(error, KNURL_IO, "upload_file", message);
    if (error != NULL) {
        error->source = KNURL_ERROR_SYSTEM;
        error->native_code = code;
    }
    return status;
}
static knurl_status write_file(const knurl_upload_metadata *metadata,
                               knurl_source *reader, knurl_reply *reply,
                               void *data, knurl_error *error)
{
    FILE *file;
    unsigned char bytes[2048];
    knurl_u64 total = {0}, amount;
    char count[21];
    size_t n, formatted;
    knurl_status status;
    (void)metadata;
    if (knurl_source_cancelled(reader))
        return KNURL_CANCELLED;
    file = fopen(data, "wb");
    if (file == NULL)
        return file_error(error, "opening upload destination");
    for (;;) {
        status = knurl_source_read(reader, bytes, sizeof(bytes), &n, error);
        if (status == KNURL_EOF) {
            status = KNURL_OK;
            break;
        }
        if (status != KNURL_OK)
            break;
        if (fwrite(bytes, 1, n, file) != n) {
            status = file_error(error, "writing upload destination");
            break;
        }
        status = knurl_u64_from_size(n, &amount, error);
        if (status == KNURL_OK)
            status = knurl_u64_add(total, amount, &total, error);
        if (status != KNURL_OK)
            break;
    }
    if (status == KNURL_OK && fflush(file) != 0)
        status = file_error(error, "flushing upload destination");
    if (fclose(file) != 0 && status == KNURL_OK)
        status = file_error(error, "closing upload destination");
    if (status == KNURL_OK)
        status = knurl_u64_format_decimal(total, count, sizeof(count),
                                          &formatted, error);
    if (status == KNURL_OK)
        status = knurl_reply_text(reply, 200, "text/plain", count, error);
    return status;
}
static knurl_status add(knurl_router *router, knurl_route_group *group,
                        const knurl_upload_file_config *config,
                        knurl_route **out, knurl_error *error)
{
    knurl_upload_reader_config reader;
    knurl_status status;
    char *path;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || config->file_path == NULL ||
        config->file_path[0] == 0)
        return knurl_fail(error, KNURL_INVALID, "upload_file",
                          "provide an explicit destination path");
    path = knurl_copy_text(config->file_path);
    if (path == NULL)
        return knurl_fail(error, KNURL_NOMEM, "upload_file",
                          "copying destination path");
    knurl_upload_reader_config_init(&reader);
    reader.method = config->method;
    reader.methods = config->methods;
    reader.path = config->path;
    reader.path_kind = config->path_kind;
    reader.body = config->body;
    reader.hooks = config->hooks;
    reader.acl = config->acl;
    reader.buffer_bytes = config->body.memory_buffer_limit_bytes;
    reader.handler = write_file;
    reader.userdata = path;
    status =
        knurl_upload_reader_add(router, group, &reader, NULL, free, out, error);
    if (status != KNURL_OK)
        free(path);
    return status;
}
knurl_status knurl_router_upload_file(knurl_router *router,
                                      const knurl_upload_file_config *config,
                                      knurl_route **out, knurl_error *error)
{
    return add(router, NULL, config, out, error);
}
knurl_status
knurl_route_group_upload_file(knurl_route_group *group,
                              const knurl_upload_file_config *config,
                              knurl_route **out, knurl_error *error)
{
    if (group == NULL) {
        if (out != NULL)
            *out = NULL;
        return knurl_fail(error, KNURL_INVALID, "upload_file",
                          "group is required");
    }
    return add(NULL, group, config, out, error);
}

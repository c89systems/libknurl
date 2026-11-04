#include "json_internal.h"
#include "internal.h"
#include "reply_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

rotmos_status rotmos_json_error(lonejson_status code,
                                const lonejson_error *native,
                                rotmos_error *error)
{
    rotmos_status status;
    if (code == LONEJSON_STATUS_OK) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    status = ROTMOS_INVALID;
    if (code == LONEJSON_STATUS_ALLOCATION_FAILED)
        status = ROTMOS_NOMEM;
    else if (code == LONEJSON_STATUS_IO_ERROR)
        status = ROTMOS_IO;
    else if (code == LONEJSON_STATUS_UNSUPPORTED)
        status = ROTMOS_NOT_SUPPORTED;
    else if (code == LONEJSON_STATUS_OVERFLOW ||
             code == LONEJSON_STATUS_TRUNCATED)
        status = ROTMOS_LIMIT;
    else if (code == LONEJSON_STATUS_INTERNAL_ERROR)
        status = ROTMOS_BACKEND;
    (void)rotmos_fail(error, status, "lonejson",
                      native != NULL && native->message[0] != 0
                          ? native->message
                          : lonejson_status_string(code));
    if (error != NULL) {
        error->source = ROTMOS_ERROR_LONEJSON;
        error->native_code = (long)code;
        if (native != NULL)
            (void)snprintf(error->detail, sizeof(error->detail),
                           "line %lu, column %lu, offset %lu, errno %d",
                           (unsigned long)native->line,
                           (unsigned long)native->column,
                           (unsigned long)native->offset, native->system_errno);
    }
    return status;
}

rotmos_status rotmos_json_schema_check(const lonejson_schema_view *s,
                                       rotmos_error *error)
{
    if (s == NULL || s->size != sizeof(*s) ||
        s->abi_version != LONEJSON_VIEW_ABI_VERSION || s->map == NULL ||
        s->record_size == 0 || s->record_size > 10485760UL ||
        s->record_size != s->map->struct_size)
        return rotmos_fail(error, ROTMOS_INVALID, "json_schema",
                           "provide a public schema view matching its record "
                           "size (1..10 MiB)");
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
rotmos_status rotmos_json_schema_from_map(lonejson *runtime,
                                          const lonejson_map *map,
                                          lonejson_schema_view *out,
                                          rotmos_error *error)
{
    if (out == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "json_schema",
                           "schema output is required");
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    out->abi_version = LONEJSON_VIEW_ABI_VERSION;
    out->runtime = runtime;
    out->map = map;
    out->record_size = map != NULL ? map->struct_size : 0;
    return rotmos_json_schema_check(out, error);
}
rotmos_status rotmos_json_runtime(const lonejson_schema_view *schema,
                                  lonejson **out, int *owned,
                                  rotmos_error *error)
{
    lonejson_error native;
    *owned = 0;
    *out = schema != NULL ? schema->runtime : NULL;
    if (*out == NULL) {
        *out = lonejson_new(NULL, &native);
        if (*out == NULL)
            return rotmos_json_error(native.code, &native, error);
        *owned = 1;
    }
    return ROTMOS_OK;
}

struct json_reader {
    rotmos_body *body;
    rotmos_error failure;
};
static lonejson_read_result body_read(void *data, unsigned char *bytes,
                                      size_t capacity)
{
    struct json_reader *r;
    lonejson_read_result result;
    rotmos_status status;
    r = data;
    memset(&result, 0, sizeof(result));
    status = rotmos_body_read(r->body, bytes, capacity, &result.bytes_read,
                              &r->failure);
    if (status == ROTMOS_EOF)
        result.eof = 1;
    else if (status != ROTMOS_OK)
        result.error_code = EIO;
    return result;
}
static rotmos_status parse_body(rotmos_request *request,
                                const lonejson_schema_view *schema,
                                lonejson *runtime, void *record,
                                rotmos_error *error)
{
    struct json_reader reader;
    lonejson_schema_view borrowed;
    lonejson_error native;
    lonejson_status code;
    rotmos_status status;
    rotmos_status rewind_status;
    rotmos_error rewind_error;
    int owned;
    if (schema != NULL) {
        status = rotmos_json_schema_check(schema, error);
        if (status != ROTMOS_OK)
            return status;
        if (record == NULL)
            return rotmos_fail(error, ROTMOS_INVALID, "json_parse",
                               "initialized record storage is required");
    }
    memset(&reader, 0, sizeof(reader));
    reader.body = rotmos_request_body(request);
    if (reader.body == NULL ||
        rotmos_body_kind(reader.body) == ROTMOS_BODY_STORAGE_NONE)
        return rotmos_fail(
            error, ROTMOS_STATE, "json_parse",
            "JSON helpers require a completed BUFFERED or MATERIALIZED body");
    status = rotmos_body_rewind(reader.body, error);
    if (status != ROTMOS_OK)
        return status;
    memset(&borrowed, 0, sizeof(borrowed));
    borrowed.runtime = runtime;
    status = rotmos_json_runtime(schema != NULL ? schema : &borrowed, &runtime,
                                 &owned, error);
    if (status != ROTMOS_OK)
        return status;
    if (schema != NULL)
        code = lonejson_parse_reader(runtime, schema->map, record, body_read,
                                     &reader, &native);
    else
        code = lonejson_validate_reader(runtime, body_read, &reader, &native);
    status = rotmos_json_error(code, &native, error);
    if (status == ROTMOS_LIMIT) {
        status = ROTMOS_INVALID;
        if (error != NULL)
            error->status = status;
    }
    if (reader.failure.status != ROTMOS_OK) {
        status = reader.failure.status;
        if (error != NULL)
            *error = reader.failure;
    }
    rewind_status = rotmos_body_rewind(reader.body, &rewind_error);
    if (status == ROTMOS_OK && rewind_status != ROTMOS_OK) {
        status = rewind_status;
        if (error != NULL)
            *error = rewind_error;
    }
    if (owned)
        lonejson_free(runtime);
    return status;
}
rotmos_status rotmos_request_json_into(rotmos_request *request,
                                       const lonejson_schema_view *schema,
                                       void *record, rotmos_error *error)
{
    if (schema == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "json_parse",
                           "schema is required");
    return parse_body(request, schema, NULL, record, error);
}
rotmos_status rotmos_request_json_validate(rotmos_request *request,
                                           lonejson *runtime,
                                           rotmos_error *error)
{
    return parse_body(request, NULL, runtime, NULL, error);
}

struct json_source {
    lonejson_generator generator;
    lonejson *runtime;
    int owned;
    int eof;
    rotmos_source_close_fn release;
    void *userdata;
};
void rotmos_json_source_config_init(rotmos_json_source_config *config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    config->record.size = sizeof(config->record);
    config->record.abi_version = LONEJSON_VIEW_ABI_VERSION;
    config->record.schema.size = sizeof(config->record.schema);
    config->record.schema.abi_version = LONEJSON_VIEW_ABI_VERSION;
}
static rotmos_status json_read(void *buffer, size_t capacity, size_t *produced,
                               void *data, rotmos_error *error)
{
    struct json_source *s;
    lonejson_status code;
    int eof;
    s = data;
    *produced = 0;
    if (s->eof)
        return ROTMOS_EOF;
    code = lonejson_generator_read(&s->generator, buffer, capacity, produced,
                                   &eof);
    if (code != LONEJSON_STATUS_OK) {
        *produced = 0;
        return rotmos_json_error(code, &s->generator.error, error);
    }
    s->eof = eof;
    rotmos_error_clear(error);
    return *produced != 0 ? ROTMOS_OK : (eof ? ROTMOS_EOF : ROTMOS_AGAIN);
}
static void json_close(void *data, rotmos_status outcome)
{
    struct json_source *s;
    s = data;
    lonejson_generator_cleanup(&s->generator);
    if (s->owned)
        lonejson_free(s->runtime);
    if (s->release != NULL)
        s->release(s->userdata, outcome);
    free(s);
}
static rotmos_status make_source(const rotmos_json_source_config *config,
                                 rotmos_source **out,
                                 struct json_source **state,
                                 rotmos_error *error)
{
    struct json_source *s;
    rotmos_source_config source;
    rotmos_status status;
    lonejson_status code;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || out == NULL ||
        config->record.size != sizeof(config->record) ||
        config->record.abi_version != LONEJSON_VIEW_ABI_VERSION ||
        config->record.record == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "json_source",
                           "provide public schema and record views");
    status = rotmos_json_schema_check(&config->record.schema, error);
    if (status != ROTMOS_OK)
        return status;
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "json_source",
                           "cannot allocate JSON generator");
    status = rotmos_json_runtime(&config->record.schema, &s->runtime, &s->owned,
                                 error);
    if (status != ROTMOS_OK) {
        free(s);
        return status;
    }
    code = lonejson_generator_init(s->runtime, &s->generator,
                                   config->record.schema.map,
                                   config->record.record);
    if (code != LONEJSON_STATUS_OK) {
        status = rotmos_json_error(code, &s->generator.error, error);
        json_close(s, status);
        return status;
    }
    rotmos_source_config_init(&source);
    source.read = json_read;
    source.close = json_close;
    source.userdata = s;
    status = rotmos_source_new(&source, out, error);
    if (status != ROTMOS_OK) {
        json_close(s, status);
        return status;
    }
    *state = s;
    return ROTMOS_OK;
}
rotmos_status rotmos_source_json(const rotmos_json_source_config *config,
                                 rotmos_source **out, rotmos_error *error)
{
    struct json_source *state;
    rotmos_status status;
    status = make_source(config, out, &state, error);
    if (status == ROTMOS_OK) {
        state->release = config->release;
        state->userdata = config->userdata;
    }
    return status;
}
rotmos_status rotmos_reply_json(rotmos_reply *reply, int code,
                                const rotmos_json_source_config *config,
                                rotmos_error *error)
{
    struct json_source *state;
    rotmos_source *source;
    rotmos_status status;
    status = make_source(config, &source, &state, error);
    if (status != ROTMOS_OK)
        return status;
    status = rotmos_reply_source_take(reply, code, "application/json", source,
                                      error);
    if (status == ROTMOS_OK) {
        state->release = config->release;
        state->userdata = config->userdata;
    } else
        (void)rotmos_source_destroy(source, NULL);
    return status;
}

rotmos_status
rotmos_json_serialize_materialized(const lonejson_schema_view *schema,
                                   const void *record, size_t max_bytes,
                                   rotmos_owned_bytes *out, rotmos_error *error)
{
    struct rotmos_json_buffer sink;
    lonejson *runtime;
    lonejson_status code;
    lonejson_error native;
    rotmos_status status;
    int owned;
    if (out != NULL)
        rotmos_owned_bytes_init(out);
    if (out == NULL || record == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "json_materialize",
                           "provide record and empty output");
    status = rotmos_json_schema_check(schema, error);
    if (status != ROTMOS_OK)
        return status;
    status = rotmos_json_runtime(schema, &runtime, &owned, error);
    if (status != ROTMOS_OK)
        return status;
    memset(&sink, 0, sizeof(sink));
    sink.bound = max_bytes != 0 ? max_bytes : 10485760UL;
    code = lonejson_serialize_sink(runtime, schema->map, record,
                                   rotmos_json_buffer_write, &sink, &native);
    status = rotmos_json_error(code, &native, error);
    if (sink.failure != ROTMOS_OK)
        status =
            rotmos_fail(error, sink.failure, "json_materialize",
                        "JSON output exceeds its bound or allocation failed");
    if (owned)
        lonejson_free(runtime);
    if (status == ROTMOS_OK)
        *out = sink.bytes;
    else
        rotmos_owned_bytes_cleanup(&sink.bytes);
    return status;
}
rotmos_status rotmos_reply_json_materialized(rotmos_reply *reply, int code,
                                             const lonejson_schema_view *schema,
                                             const void *record,
                                             size_t max_bytes,
                                             rotmos_error *error)
{
    rotmos_owned_bytes bytes;
    rotmos_bytes empty = {NULL, 0};
    rotmos_status status;
    status = rotmos_json_serialize_materialized(schema, record, max_bytes,
                                                &bytes, error);
    if (status != ROTMOS_OK)
        return status;
    status = rotmos_reply_bytes(reply, code, "application/json", empty, error);
    if (status == ROTMOS_OK) {
        reply->body = bytes.data;
        reply->body_length = bytes.length;
    } else
        rotmos_owned_bytes_cleanup(&bytes);
    return status;
}

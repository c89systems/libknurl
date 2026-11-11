#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "json_internal.h"
#include "internal.h"
#include "reply_internal.h"
#include "file_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

knurl_status knurl_json_error(lonejson_status code,
                              const lonejson_error *native, knurl_error *error)
{
    knurl_status status;
    if (code == LONEJSON_STATUS_OK) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    status = KNURL_INVALID;
    if (code == LONEJSON_STATUS_ALLOCATION_FAILED)
        status = KNURL_NOMEM;
    else if (code == LONEJSON_STATUS_IO_ERROR)
        status = KNURL_IO;
    else if (code == LONEJSON_STATUS_UNSUPPORTED)
        status = KNURL_NOT_SUPPORTED;
    else if (code == LONEJSON_STATUS_OVERFLOW ||
             code == LONEJSON_STATUS_TRUNCATED)
        status = KNURL_LIMIT;
    else if (code == LONEJSON_STATUS_INTERNAL_ERROR)
        status = KNURL_BACKEND;
    (void)knurl_fail(error, status, "lonejson",
                     native != NULL && native->message[0] != 0
                         ? native->message
                         : lonejson_status_string(code));
    if (error != NULL) {
        error->source = KNURL_ERROR_LONEJSON;
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

knurl_status knurl_json_schema_check(const lonejson_schema_view *s,
                                     knurl_error *error)
{
    if (s == NULL || s->size != sizeof(*s) ||
        s->abi_version != LONEJSON_VIEW_ABI_VERSION || s->map == NULL ||
        s->record_size == 0 || s->record_size != s->map->struct_size)
        return knurl_fail(error, KNURL_INVALID, "json_schema",
                          "provide a public schema view matching its record "
                          "size");
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_json_schema_from_map(lonejson *runtime,
                                        const lonejson_map *map,
                                        lonejson_schema_view *out,
                                        knurl_error *error)
{
    if (out == NULL)
        return knurl_fail(error, KNURL_INVALID, "json_schema",
                          "schema output is required");
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    out->abi_version = LONEJSON_VIEW_ABI_VERSION;
    out->runtime = runtime;
    out->map = map;
    out->record_size = map != NULL ? map->struct_size : 0;
    return knurl_json_schema_check(out, error);
}
knurl_status knurl_json_record_check(const lonejson_schema_view *schema,
                                     const lonejson_record_view *view,
                                     knurl_error *error)
{
    if (view->size != sizeof(*view) ||
        view->abi_version != LONEJSON_VIEW_ABI_VERSION ||
        view->record == NULL ||
        knurl_json_schema_check(&view->schema, error) != KNURL_OK ||
        view->schema.map != schema->map || view->schema.runtime == NULL ||
        (schema->runtime != NULL && view->schema.runtime != schema->runtime))
        return knurl_fail(
            error, KNURL_INVALID, "typed_record",
            "provider must supply initialized storage matching its schema");
    return KNURL_OK;
}
knurl_status knurl_json_runtime(const lonejson_schema_view *schema,
                                lonejson **out, int *owned, knurl_error *error)
{
    lonejson_error native;
    *owned = 0;
    *out = schema != NULL ? schema->runtime : NULL;
    if (*out == NULL) {
        *out = lonejson_new(NULL, &native);
        if (*out == NULL) {
            knurl_status status = knurl_json_error(native.code, &native, error);
            if (error != NULL)
                strcpy(error->operation, "json_runtime");
            return status;
        }
        *owned = 1;
    }
    return KNURL_OK;
}

knurl_status knurl_json_validate_cstr(const char *json, knurl_error *error)
{
    lonejson_error native;
    lonejson_status code;
    lonejson *runtime;
    knurl_status status;
    int owned;
    char message[384];
    if (json == NULL)
        return knurl_fail(error, KNURL_INVALID, "json_validate",
                          "json is required");
    status = knurl_json_runtime(NULL, &runtime, &owned, error);
    if (status != KNURL_OK)
        return status;
    code = lonejson_validate_cstr(runtime, json, &native);
    lonejson_free(runtime);
    if (code == LONEJSON_STATUS_OK) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    (void)snprintf(message, sizeof(message),
                   "invalid json at line %lu column %lu: %s",
                   (unsigned long)native.line, (unsigned long)native.column,
                   native.message);
    status = knurl_fail(error, KNURL_INVALID, "json_validate", message);
    if (error != NULL) {
        error->source = KNURL_ERROR_LONEJSON;
        error->native_code = (long)code;
    }
    return status;
}

struct json_reader {
    knurl_body *body;
    knurl_source *source;
    knurl_error failure;
};
static lonejson_read_result body_read(void *data, unsigned char *bytes,
                                      size_t capacity)
{
    struct json_reader *r;
    lonejson_read_result result;
    knurl_status status;
    r = data;
    memset(&result, 0, sizeof(result));
    status = r->source != NULL
                 ? knurl_source_read(r->source, bytes, capacity,
                                     &result.bytes_read, &r->failure)
                 : knurl_body_read(r->body, bytes, capacity, &result.bytes_read,
                                   &r->failure);
    if (status == KNURL_AGAIN)
        status = knurl_fail(&r->failure, KNURL_STATE, "json_parse",
                            "synchronous parsing needs a blocking reader");
    if (status == KNURL_EOF)
        result.eof = 1;
    else if (status != KNURL_OK) {
        if (r->failure.status != status)
            (void)knurl_fail(&r->failure, status, "json_parse",
                             "JSON input failed");
        result.error_code = EIO;
    }
    return result;
}
static knurl_status parse_reader(struct json_reader *reader,
                                 const lonejson_schema_view *schema,
                                 lonejson *runtime, void *record,
                                 knurl_error *error)
{
    lonejson_schema_view borrowed;
    lonejson_error native;
    lonejson_status code;
    knurl_status status;
    int owned;
    memset(&borrowed, 0, sizeof(borrowed));
    borrowed.runtime = runtime;
    status = knurl_json_runtime(schema != NULL ? schema : &borrowed, &runtime,
                                &owned, error);
    if (status != KNURL_OK)
        return status;
    if (schema != NULL)
        code = lonejson_parse_reader(runtime, schema->map, record, body_read,
                                     reader, &native);
    else
        code = lonejson_validate_reader(runtime, body_read, reader, &native);
    status = knurl_json_error(code, &native, error);
    if (reader->failure.status != KNURL_OK) {
        status = reader->failure.status;
        if (error != NULL)
            *error = reader->failure;
    }
    if (owned)
        lonejson_free(runtime);
    return status;
}
knurl_status knurl_json_parse_source(knurl_source *source,
                                     const lonejson_schema_view *schema,
                                     void *record, knurl_error *error)
{
    struct json_reader reader;
    if (source == NULL || record == NULL)
        return knurl_fail(error, KNURL_INVALID, "json_parse",
                          "provide source and initialized record");
    if (knurl_json_schema_check(schema, error) != KNURL_OK)
        return KNURL_INVALID;
    memset(&reader, 0, sizeof(reader));
    reader.source = source;
    return parse_reader(&reader, schema, NULL, record, error);
}
static knurl_status parse_body(knurl_request *request,
                               const lonejson_schema_view *schema,
                               lonejson *runtime, void *record,
                               knurl_error *error)
{
    struct json_reader reader;
    knurl_status status;
    knurl_status rewind_status;
    knurl_error rewind_error;
    if (schema != NULL) {
        status = knurl_json_schema_check(schema, error);
        if (status != KNURL_OK)
            return status;
        if (record == NULL)
            return knurl_fail(error, KNURL_INVALID, "json_parse",
                              "initialized record storage is required");
    }
    memset(&reader, 0, sizeof(reader));
    reader.body = knurl_request_body(request);
    if (reader.body == NULL ||
        knurl_body_kind(reader.body) == KNURL_BODY_STORAGE_NONE)
        return knurl_fail(
            error, KNURL_STATE, "json_parse",
            "JSON helpers require a completed BUFFERED or MATERIALIZED body");
    status = knurl_body_rewind(reader.body, error);
    if (status != KNURL_OK)
        return status;
    status = parse_reader(&reader, schema, runtime, record, error);
    if (status == KNURL_LIMIT) {
        status = KNURL_INVALID;
        if (error != NULL)
            error->status = status;
    }
    if (reader.failure.status != KNURL_OK) {
        status = reader.failure.status;
        if (error != NULL)
            *error = reader.failure;
    }
    rewind_status = knurl_body_rewind(reader.body, &rewind_error);
    if (status == KNURL_OK && rewind_status != KNURL_OK) {
        status = rewind_status;
        if (error != NULL)
            *error = rewind_error;
    }
    return status;
}
knurl_status knurl_request_json_into(knurl_request *request,
                                     const lonejson_schema_view *schema,
                                     void *record, knurl_error *error)
{
    if (schema == NULL)
        return knurl_fail(error, KNURL_INVALID, "json_parse",
                          "schema is required");
    return parse_body(request, schema, NULL, record, error);
}
knurl_status knurl_request_json_validate(knurl_request *request,
                                         lonejson *runtime, knurl_error *error)
{
    return parse_body(request, NULL, runtime, NULL, error);
}

struct json_source {
    lonejson_generator generator;
    lonejson *runtime;
    int owned;
    int eof;
    knurl_source_close_fn release;
    void *userdata;
};
void knurl_json_source_config_init(knurl_json_source_config *config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    config->record.size = sizeof(config->record);
    config->record.abi_version = LONEJSON_VIEW_ABI_VERSION;
    config->record.schema.size = sizeof(config->record.schema);
    config->record.schema.abi_version = LONEJSON_VIEW_ABI_VERSION;
}
static knurl_status json_read(void *buffer, size_t capacity, size_t *produced,
                              void *data, knurl_error *error)
{
    struct json_source *s;
    lonejson_status code;
    int eof;
    s = data;
    *produced = 0;
    if (s->eof)
        return KNURL_EOF;
    code = lonejson_generator_read(&s->generator, buffer, capacity, produced,
                                   &eof);
    if (code != LONEJSON_STATUS_OK) {
        *produced = 0;
        return knurl_json_error(code, &s->generator.error, error);
    }
    s->eof = eof;
    knurl_error_clear(error);
    return *produced != 0 ? KNURL_OK : (eof ? KNURL_EOF : KNURL_AGAIN);
}
static void json_close(void *data, knurl_status outcome)
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
static knurl_status make_source(const knurl_json_source_config *config,
                                knurl_source **out, struct json_source **state,
                                knurl_error *error)
{
    struct json_source *s;
    knurl_source_config source;
    knurl_status status;
    lonejson_status code;
    *state = NULL;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || out == NULL ||
        config->record.size != sizeof(config->record) ||
        config->record.abi_version != LONEJSON_VIEW_ABI_VERSION ||
        config->record.record == NULL)
        return knurl_fail(error, KNURL_INVALID, "json_source",
                          "provide public schema and record views");
    status = knurl_json_schema_check(&config->record.schema, error);
    if (status != KNURL_OK)
        return status;
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return knurl_fail(error, KNURL_NOMEM, "json_source",
                          "cannot allocate JSON generator");
    status = knurl_json_runtime(&config->record.schema, &s->runtime, &s->owned,
                                error);
    if (status != KNURL_OK) {
        free(s);
        return status;
    }
    code = lonejson_generator_init(s->runtime, &s->generator,
                                   config->record.schema.map,
                                   config->record.record);
    if (code != LONEJSON_STATUS_OK) {
        status = knurl_json_error(code, &s->generator.error, error);
        json_close(s, status);
        return status;
    }
    knurl_source_config_init(&source);
    source.read = json_read;
    source.close = json_close;
    source.userdata = s;
    status = knurl_source_new(&source, out, error);
    if (status != KNURL_OK) {
        json_close(s, status);
        return status;
    }
    *state = s;
    return KNURL_OK;
}
knurl_status knurl_source_json(const knurl_json_source_config *config,
                               knurl_source **out, knurl_error *error)
{
    struct json_source *state;
    knurl_status status;
    status = make_source(config, out, &state, error);
    if (status == KNURL_OK) {
        state->release = config->release;
        state->userdata = config->userdata;
    }
    return status;
}
knurl_status knurl_reply_json(knurl_reply *reply, int code,
                              const knurl_json_source_config *config,
                              knurl_error *error)
{
    struct json_source *state;
    knurl_source *source;
    knurl_status status;
    status = make_source(config, &source, &state, error);
    if (status != KNURL_OK)
        return status;
    status =
        knurl_reply_source_take(reply, code, "application/json", source, error);
    if (status == KNURL_OK) {
        state->release = config->release;
        state->userdata = config->userdata;
    } else
        (void)knurl_source_destroy(source, NULL);
    return status;
}

knurl_status
knurl_json_serialize_materialized(const lonejson_schema_view *schema,
                                  const void *record, size_t max_bytes,
                                  knurl_owned_bytes *out, knurl_error *error)
{
    struct knurl_json_buffer sink;
    lonejson *runtime;
    lonejson_status code;
    lonejson_error native;
    knurl_status status;
    int owned;
    if (out != NULL)
        knurl_owned_bytes_init(out);
    if (out == NULL || record == NULL)
        return knurl_fail(error, KNURL_INVALID, "json_materialize",
                          "provide record and empty output");
    status = knurl_json_schema_check(schema, error);
    if (status != KNURL_OK)
        return status;
    status = knurl_json_runtime(schema, &runtime, &owned, error);
    if (status != KNURL_OK)
        return status;
    memset(&sink, 0, sizeof(sink));
    sink.bound = max_bytes != 0 ? max_bytes : (size_t)-1;
    code = lonejson_serialize_sink(runtime, schema->map, record,
                                   knurl_json_buffer_write, &sink, &native);
    status = knurl_json_error(code, &native, error);
    if (sink.failure != KNURL_OK)
        status =
            knurl_fail(error, sink.failure, "json_materialize",
                       "JSON output exceeds its bound or allocation failed");
    if (owned)
        lonejson_free(runtime);
    if (status == KNURL_OK)
        *out = sink.bytes;
    else
        knurl_owned_bytes_cleanup(&sink.bytes);
    return status;
}
knurl_status knurl_reply_json_materialized(knurl_reply *reply, int code,
                                           const lonejson_schema_view *schema,
                                           const void *record, size_t max_bytes,
                                           knurl_error *error)
{
    knurl_owned_bytes bytes;
    knurl_bytes empty = {NULL, 0};
    knurl_status status;
    status = knurl_json_serialize_materialized(schema, record, max_bytes,
                                               &bytes, error);
    if (status != KNURL_OK)
        return status;
    status = knurl_reply_bytes(reply, code, "application/json", empty, error);
    if (status == KNURL_OK) {
        reply->impl->body = bytes.data;
        reply->impl->body_length = bytes.length;
    } else
        knurl_owned_bytes_cleanup(&bytes);
    return status;
}

struct json_sink {
    knurl_json_sink_fn write;
    void *userdata;
    knurl_error failure;
};
static lonejson_status sink_write(void *data, const void *bytes, size_t count,
                                  lonejson_error *error)
{
    struct json_sink *sink = data;
    knurl_status status;
    const unsigned char *cursor = bytes;
    size_t part;
    (void)error;
    if (sink->failure.status != KNURL_OK)
        return LONEJSON_STATUS_IO_ERROR;
    while (count != 0) {
        part = count > 4096 ? 4096 : count;
        status = sink->write(cursor, part, sink->userdata, &sink->failure);
        if (status == KNURL_AGAIN || status == KNURL_EOF || status < KNURL_OK ||
            status > KNURL_BACKEND)
            status = knurl_fail(&sink->failure, KNURL_INVALID, "json_sink",
                                "synchronous sink must accept or fail");
        if (status != KNURL_OK) {
            if (sink->failure.status != status)
                (void)knurl_fail(&sink->failure, status, "json_sink",
                                 "JSON sink rejected output");
            return LONEJSON_STATUS_IO_ERROR;
        }
        cursor += part;
        count -= part;
    }
    return LONEJSON_STATUS_OK;
}
knurl_status knurl_json_write_sink(const lonejson_schema_view *schema,
                                   const void *record, knurl_json_sink_fn write,
                                   void *userdata, knurl_error *error)
{
    struct json_sink sink;
    lonejson *runtime;
    lonejson_error native;
    lonejson_status code;
    knurl_status status;
    int owned;
    if (record == NULL || write == NULL)
        return knurl_fail(error, KNURL_INVALID, "json_sink",
                          "provide record and sink callback");
    status = knurl_json_schema_check(schema, error);
    if (status != KNURL_OK)
        return status;
    status = knurl_json_runtime(schema, &runtime, &owned, error);
    if (status != KNURL_OK)
        return status;
    memset(&sink, 0, sizeof(sink));
    sink.write = write;
    sink.userdata = userdata;
    code = lonejson_serialize_sink(runtime, schema->map, record, sink_write,
                                   &sink, &native);
    if (owned)
        lonejson_free(runtime);
    if (sink.failure.status != KNURL_OK) {
        if (error != NULL)
            *error = sink.failure;
        return sink.failure.status;
    }
    return knurl_json_error(code, &native, error);
}
knurl_status knurl_json_array_rewrite_source(
    knurl_source *source, const char *selector, lonejson *runtime,
    const lonejson_array_rewrite_options *options, knurl_json_sink_fn write,
    void *userdata, knurl_error *error)
{
    struct json_reader reader;
    struct json_sink sink;
    lonejson_schema_view schema;
    lonejson_error native;
    lonejson_status code;
    knurl_status status;
    int owned;
    if (source == NULL || options == NULL || write == NULL)
        return knurl_fail(error, KNURL_INVALID, "json_rewrite",
                          "provide source, rewrite options and sink");
    memset(&schema, 0, sizeof(schema));
    schema.runtime = runtime;
    status = knurl_json_runtime(&schema, &runtime, &owned, error);
    if (status != KNURL_OK)
        return status;
    memset(&reader, 0, sizeof(reader));
    reader.source = source;
    memset(&sink, 0, sizeof(sink));
    sink.write = write;
    sink.userdata = userdata;
    code = lonejson_array_rewrite_reader(runtime, selector, body_read, &reader,
                                         sink_write, &sink, options, &native);
    if (owned)
        lonejson_free(runtime);
    status = knurl_json_error(code, &native, error);
    if (reader.failure.status != KNURL_OK) {
        status = reader.failure.status;
        if (error != NULL)
            *error = reader.failure;
    } else if (sink.failure.status != KNURL_OK) {
        status = sink.failure.status;
        if (error != NULL)
            *error = sink.failure;
    }
    return status;
}
struct json_file {
    const lonejson_schema_view *schema;
    const void *record;
    int fd;
};
static knurl_status json_file_sink(const void *bytes, size_t count, void *data,
                                   knurl_error *error)
{
    struct json_file *s = data;
    return knurl_file_write(s->fd, bytes, count, error);
}
static knurl_status json_file_write(int fd, void *data, knurl_error *error)
{
    struct json_file *s = data;
    s->fd = fd;
    return knurl_json_write_sink(s->schema, s->record, json_file_sink, s,
                                 error);
}
knurl_status knurl_reply_json_spooled(knurl_reply *reply, int code,
                                      const lonejson_schema_view *schema,
                                      const void *record, const char *directory,
                                      knurl_error *error)
{
    struct json_file state;
    knurl_status status;
    if (record == NULL)
        return knurl_fail(error, KNURL_INVALID, "reply_json_spooled",
                          "record is required");
    status = knurl_json_schema_check(schema, error);
    if (status != KNURL_OK)
        return status;
    memset(&state, 0, sizeof(state));
    state.schema = schema;
    state.record = record;
    return knurl_reply_generate_file(reply, code, "application/json", directory,
                                     json_file_write, &state, error);
}

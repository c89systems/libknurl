#include "json_internal.h"
#include "reply_internal.h"
#include <stdlib.h>
#include <string.h>
void knurl_rest_reply_config_init(knurl_rest_reply_config *config)
{
    if (config != NULL)
        memset(config, 0, sizeof(*config));
}
static knurl_status select_bytes(knurl_reply *reply,
                                 struct knurl_json_buffer *sink,
                                 const knurl_rest_reply_config *config,
                                 knurl_error *error)
{
    knurl_bytes empty = {0};
    knurl_status status = knurl_reply_bytes(
        reply, config->status != 0 ? config->status : 200,
        config->content_type != NULL ? config->content_type
                                     : "application/json; charset=utf-8",
        empty, error);
    if (status == KNURL_OK) {
        reply->impl->body = sink->bytes.data;
        reply->impl->body_length = sink->bytes.length;
        knurl_owned_bytes_init(&sink->bytes);
    }
    return status;
}
knurl_status knurl_reply_rest_json(knurl_reply *reply,
                                   const lonejson_json_value *value,
                                   const knurl_rest_reply_config *config,
                                   knurl_error *error)
{
    struct knurl_json_buffer sink;
    knurl_rest_reply_config defaults;
    lonejson_writer *writer;
    lonejson *runtime;
    lonejson_error native;
    lonejson_status code;
    knurl_status status;
    if (value == NULL)
        return knurl_fail(error, KNURL_INVALID, "rest_json",
                          "JSON value is required");
    knurl_rest_reply_config_init(&defaults);
    if (config == NULL)
        config = &defaults;
    memset(&sink, 0, sizeof(sink));
    sink.bound = config->max_bytes != 0 ? config->max_bytes : (size_t)-1;
    runtime = lonejson_new(NULL, &native);
    if (runtime == NULL)
        return knurl_json_error(native.code, &native, error);
    writer = calloc(1, sizeof(*writer));
    if (writer == NULL) {
        lonejson_free(runtime);
        return knurl_fail(error, KNURL_NOMEM, "rest_json",
                          "allocating JSON writer");
    }
    code = lonejson_writer_init_sink(runtime, writer, knurl_json_buffer_write,
                                     &sink, &native);
    if (code == LONEJSON_STATUS_OK)
        code = lonejson_writer_json_value(writer, value, &native);
    if (code == LONEJSON_STATUS_OK)
        code = lonejson_writer_finish(writer, &native);
    lonejson_writer_cleanup(writer);
    free(writer);
    lonejson_free(runtime);
    if (code == LONEJSON_STATUS_OK && !config->newline_disabled)
        code = knurl_json_buffer_write(&sink, "\n", 1, &native);
    status =
        sink.failure != KNURL_OK
            ? knurl_fail(error, sink.failure, "rest_json",
                         "JSON output exceeds its bound or allocation failed")
            : knurl_json_error(code, &native, error);
    if (status == KNURL_OK)
        status = select_bytes(reply, &sink, config, error);
    knurl_owned_bytes_cleanup(&sink.bytes);
    return status;
}
knurl_status knurl_reply_error_json(knurl_reply *reply, int status,
                                    const char *kind, const knurl_error *cause,
                                    knurl_error *error)
{
    struct knurl_json_buffer sink;
    knurl_rest_reply_config config;
    lonejson_writer *w;
    lonejson *runtime;
    lonejson_error native;
    lonejson_status code;
    knurl_status result;
    const char *message = cause != NULL ? cause->message : "request failed";
    int reason = cause != NULL ? (int)cause->status : (int)KNURL_STATE;
    if (kind == NULL)
        kind = "error";
    if (status == 0)
        status = reason == KNURL_INVALID         ? 400
                 : reason == KNURL_CONFLICT      ? 409
                 : reason == KNURL_NOT_SUPPORTED ? 501
                 : reason == KNURL_TIMEOUT       ? 504
                                                 : 500;
    memset(&sink, 0, sizeof(sink));
    sink.bound = 8192;
    runtime = lonejson_new(NULL, &native);
    if (runtime == NULL)
        return knurl_json_error(native.code, &native, error);
    w = calloc(1, sizeof(*w));
    if (w == NULL) {
        lonejson_free(runtime);
        return knurl_fail(error, KNURL_NOMEM, "rest_error",
                          "allocating JSON writer");
    }
    code = lonejson_writer_init_sink(runtime, w, knurl_json_buffer_write, &sink,
                                     &native);
#define EMIT(call)                                                             \
    do {                                                                       \
        if (code == LONEJSON_STATUS_OK)                                        \
            code = (call);                                                     \
    } while (0)
    EMIT(lonejson_writer_begin_object(w, &native));
    EMIT(lonejson_writer_key(w, "ok", 2, &native));
    EMIT(lonejson_writer_bool(w, 0, &native));
    EMIT(lonejson_writer_key(w, "error", 5, &native));
    EMIT(lonejson_writer_begin_object(w, &native));
    EMIT(lonejson_writer_key(w, "kind", 4, &native));
    EMIT(lonejson_writer_string(w, kind, strlen(kind), &native));
    EMIT(lonejson_writer_key(w, "message", 7, &native));
    EMIT(lonejson_writer_string(w, message, strlen(message), &native));
    EMIT(lonejson_writer_key(w, "status", 6, &native));
    EMIT(lonejson_writer_i64(w, reason, &native));
    EMIT(lonejson_writer_key(w, "source_code", 11, &native));
    EMIT(lonejson_writer_i64(w, cause != NULL ? (int)cause->source : 0,
                             &native));
    EMIT(lonejson_writer_end_object(w, &native));
    EMIT(lonejson_writer_end_object(w, &native));
    EMIT(lonejson_writer_finish(w, &native));
#undef EMIT
    lonejson_writer_cleanup(w);
    free(w);
    lonejson_free(runtime);
    if (code == LONEJSON_STATUS_OK)
        code = knurl_json_buffer_write(&sink, "\n", 1, &native);
    result = sink.failure != KNURL_OK
                 ? knurl_fail(error, sink.failure, "rest_error",
                              "building error envelope")
                 : knurl_json_error(code, &native, error);
    knurl_rest_reply_config_init(&config);
    config.status = status;
    if (result == KNURL_OK)
        result = select_bytes(reply, &sink, &config, error);
    knurl_owned_bytes_cleanup(&sink.bytes);
    return result;
}

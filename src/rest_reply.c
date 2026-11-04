#include "json_internal.h"
#include "reply_internal.h"
#include <stdlib.h>
#include <string.h>
void rotmos_rest_reply_config_init(rotmos_rest_reply_config *config)
{
    if (config != NULL)
        memset(config, 0, sizeof(*config));
}
static rotmos_status select_bytes(rotmos_reply *reply,
                                  struct rotmos_json_buffer *sink,
                                  const rotmos_rest_reply_config *config,
                                  rotmos_error *error)
{
    rotmos_bytes empty = {0};
    rotmos_status status = rotmos_reply_bytes(
        reply, config->status != 0 ? config->status : 200,
        config->content_type != NULL ? config->content_type
                                     : "application/json; charset=utf-8",
        empty, error);
    if (status == ROTMOS_OK) {
        reply->body = sink->bytes.data;
        reply->body_length = sink->bytes.length;
        rotmos_owned_bytes_init(&sink->bytes);
    }
    return status;
}
rotmos_status rotmos_reply_rest_json(rotmos_reply *reply,
                                     const lonejson_json_value *value,
                                     const rotmos_rest_reply_config *config,
                                     rotmos_error *error)
{
    struct rotmos_json_buffer sink;
    rotmos_rest_reply_config defaults;
    lonejson_writer *writer;
    lonejson *runtime;
    lonejson_error native;
    lonejson_status code;
    rotmos_status status;
    if (value == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "rest_json",
                           "JSON value is required");
    rotmos_rest_reply_config_init(&defaults);
    if (config == NULL)
        config = &defaults;
    memset(&sink, 0, sizeof(sink));
    sink.bound = config->max_bytes != 0 ? config->max_bytes : 10485760UL;
    runtime = lonejson_new(NULL, &native);
    if (runtime == NULL)
        return rotmos_json_error(native.code, &native, error);
    writer = calloc(1, sizeof(*writer));
    if (writer == NULL) {
        lonejson_free(runtime);
        return rotmos_fail(error, ROTMOS_NOMEM, "rest_json",
                           "allocating JSON writer");
    }
    code = lonejson_writer_init_sink(runtime, writer, rotmos_json_buffer_write,
                                     &sink, &native);
    if (code == LONEJSON_STATUS_OK)
        code = lonejson_writer_json_value(writer, value, &native);
    if (code == LONEJSON_STATUS_OK)
        code = lonejson_writer_finish(writer, &native);
    lonejson_writer_cleanup(writer);
    free(writer);
    lonejson_free(runtime);
    if (code == LONEJSON_STATUS_OK && !config->newline_disabled)
        code = rotmos_json_buffer_write(&sink, "\n", 1, &native);
    status =
        sink.failure != ROTMOS_OK
            ? rotmos_fail(error, sink.failure, "rest_json",
                          "JSON output exceeds its bound or allocation failed")
            : rotmos_json_error(code, &native, error);
    if (status == ROTMOS_OK)
        status = select_bytes(reply, &sink, config, error);
    rotmos_owned_bytes_cleanup(&sink.bytes);
    return status;
}
rotmos_status rotmos_reply_error_json(rotmos_reply *reply, int status,
                                      const char *kind,
                                      const rotmos_error *cause,
                                      rotmos_error *error)
{
    struct rotmos_json_buffer sink;
    rotmos_rest_reply_config config;
    lonejson_writer *w;
    lonejson *runtime;
    lonejson_error native;
    lonejson_status code;
    rotmos_status result;
    const char *message = cause != NULL ? cause->message : "request failed";
    int reason = cause != NULL ? (int)cause->status : (int)ROTMOS_STATE;
    if (kind == NULL)
        kind = "error";
    if (status == 0)
        status = reason == ROTMOS_INVALID         ? 400
                 : reason == ROTMOS_CONFLICT      ? 409
                 : reason == ROTMOS_NOT_SUPPORTED ? 501
                 : reason == ROTMOS_TIMEOUT       ? 504
                                                  : 500;
    memset(&sink, 0, sizeof(sink));
    sink.bound = 8192;
    runtime = lonejson_new(NULL, &native);
    if (runtime == NULL)
        return rotmos_json_error(native.code, &native, error);
    w = calloc(1, sizeof(*w));
    if (w == NULL) {
        lonejson_free(runtime);
        return rotmos_fail(error, ROTMOS_NOMEM, "rest_error",
                           "allocating JSON writer");
    }
    code = lonejson_writer_init_sink(runtime, w, rotmos_json_buffer_write,
                                     &sink, &native);
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
        code = rotmos_json_buffer_write(&sink, "\n", 1, &native);
    result = sink.failure != ROTMOS_OK
                 ? rotmos_fail(error, sink.failure, "rest_error",
                               "building error envelope")
                 : rotmos_json_error(code, &native, error);
    rotmos_rest_reply_config_init(&config);
    config.status = status;
    if (result == ROTMOS_OK)
        result = select_bytes(reply, &sink, &config, error);
    rotmos_owned_bytes_cleanup(&sink.bytes);
    return result;
}

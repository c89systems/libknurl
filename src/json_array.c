#include "json_internal.h"
#include "internal.h"
#include <stdlib.h>
#include <string.h>
struct rotmos_json_array {
    lonejson_record_view view;
    lonejson_array_stream *parser;
    rotmos_json_array_item_fn item;
    void *userdata;
    char *selector;
    rotmos_error failure;
    int owned;
    int busy;
    int finished;
};
void rotmos_json_array_config_init(rotmos_json_array_config *config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    config->schema.size = sizeof(config->schema);
    config->schema.abi_version = LONEJSON_VIEW_ABI_VERSION;
}
rotmos_status rotmos_json_array_new(const rotmos_json_array_config *config,
                                    rotmos_json_array **out,
                                    rotmos_error *error)
{
    rotmos_json_array *s;
    rotmos_status status;
    lonejson_error native;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || out == NULL || config->item == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "json_array",
                           "provide schema, accepting callback and output");
    status = rotmos_json_schema_check(&config->schema, error);
    if (status != ROTMOS_OK)
        return status;
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "json_array",
                           "cannot allocate selected-array parser");
    s->view.size = sizeof(s->view);
    s->view.abi_version = LONEJSON_VIEW_ABI_VERSION;
    s->view.schema = config->schema;
    s->item = config->item;
    s->userdata = config->userdata;
    s->selector =
        rotmos_copy_text(config->selector != NULL ? config->selector : "");
    status = rotmos_json_runtime(&config->schema, &s->view.schema.runtime,
                                 &s->owned, error);
    if (status == ROTMOS_OK) {
        s->view.record = calloc(1, s->view.schema.record_size);
        if (s->selector == NULL || s->view.record == NULL)
            status =
                rotmos_fail(error, ROTMOS_NOMEM, "json_array",
                            "cannot allocate selected-array record/selector");
    }
    if (status == ROTMOS_OK) {
        lonejson_init(s->view.schema.runtime, s->view.schema.map,
                      s->view.record);
        s->parser = lonejson_array_stream_open_push(s->view.schema.runtime,
                                                    s->selector, &native);
        if (s->parser == NULL)
            status = rotmos_json_error(native.code, &native, error);
    }
    if (status != ROTMOS_OK) {
        (void)rotmos_json_array_destroy(s, NULL);
        return status;
    }
    *out = s;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
static lonejson_status item_ready(void *data, void *record)
{
    rotmos_json_array *s;
    rotmos_error error;
    rotmos_status status;
    s = data;
    if (s->failure.status != ROTMOS_OK || record != s->view.record)
        return LONEJSON_STATUS_CALLBACK_FAILED;
    rotmos_error_clear(&error);
    status = s->item(&s->view, s->userdata, &error);
    if (s->failure.status != ROTMOS_OK)
        return LONEJSON_STATUS_CALLBACK_FAILED;
    if (status != ROTMOS_OK) {
        if (status <= ROTMOS_EOF || status > ROTMOS_BACKEND)
            status = ROTMOS_INVALID;
        if (error.status != status)
            (void)rotmos_fail(&error, status, "json_array",
                              "selected-array callback failed; callbacks "
                              "cannot pause a chunk");
        s->failure = error;
        return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    return LONEJSON_STATUS_OK;
}
static rotmos_status process(rotmos_json_array *s, const void *bytes,
                             size_t length, int finish, rotmos_error *error)
{
    lonejson_error native;
    lonejson_status code;
    rotmos_status status;
    if (s == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "json_array",
                           "parser is required");
    if (s->busy) {
        (void)rotmos_fail(
            &s->failure, ROTMOS_STATE, "json_array",
            "cannot reenter or destroy an active selected-array parser");
        if (error != NULL)
            *error = s->failure;
        return ROTMOS_STATE;
    }
    if (s->failure.status != ROTMOS_OK) {
        if (error != NULL)
            *error = s->failure;
        return s->failure.status;
    }
    if (s->finished) {
        if (finish) {
            rotmos_error_clear(error);
            return ROTMOS_OK;
        }
        return rotmos_fail(error, ROTMOS_STATE, "json_array",
                           "parser is finished");
    }
    if (length > 65536UL || (bytes == NULL && length != 0))
        return rotmos_fail(error, ROTMOS_INVALID, "json_array",
                           "feed at most 64 KiB of valid input");
    s->busy = 1;
    code = finish ? lonejson_array_stream_finish(s->parser, s->view.schema.map,
                                                 s->view.record, item_ready, s,
                                                 &native)
                  : lonejson_array_stream_push(s->parser, s->view.schema.map,
                                               s->view.record, bytes, length,
                                               item_ready, s, &native);
    s->busy = 0;
    if (s->failure.status == ROTMOS_OK && code != LONEJSON_STATUS_OK)
        (void)rotmos_json_error(code, &native, &s->failure);
    status = s->failure.status;
    if (status != ROTMOS_OK) {
        if (error != NULL)
            *error = s->failure;
        return status;
    }
    if (finish)
        s->finished = 1;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
rotmos_status rotmos_json_array_feed(rotmos_json_array *s, const void *bytes,
                                     size_t length, rotmos_error *error)
{
    return process(s, bytes, length, 0, error);
}
rotmos_status rotmos_json_array_finish(rotmos_json_array *s,
                                       rotmos_error *error)
{
    return process(s, NULL, 0, 1, error);
}
rotmos_status rotmos_json_array_destroy(rotmos_json_array *s,
                                        rotmos_error *error)
{
    if (s != NULL) {
        if (s->busy) {
            (void)rotmos_fail(&s->failure, ROTMOS_STATE, "json_array",
                              "cannot destroy an active selected-array parser");
            if (error != NULL)
                *error = s->failure;
            return ROTMOS_STATE;
        }
        s->busy = 1;
        if (s->parser != NULL)
            lonejson_array_stream_close(s->parser);
        if (s->view.record != NULL) {
            lonejson_cleanup(s->view.schema.map, s->view.record);
            free(s->view.record);
        }
        if (s->owned)
            lonejson_free(s->view.schema.runtime);
        free(s->selector);
        free(s);
    }
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

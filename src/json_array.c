#include "json_internal.h"
#include "internal.h"
#include <errno.h>
#include <string.h>

struct array_input {
    rotmos_source *source;
    rotmos_body *body;
    rotmos_error failure;
};
static lonejson_read_result read_input(void *data, unsigned char *bytes,
                                       size_t capacity)
{
    struct array_input *input = data;
    lonejson_read_result result;
    rotmos_status status;
    memset(&result, 0, sizeof(result));
    status = input->source != NULL
                 ? rotmos_source_read(input->source, bytes, capacity,
                                      &result.bytes_read, &input->failure)
                 : rotmos_body_read(input->body, bytes, capacity,
                                    &result.bytes_read, &input->failure);
    if (status == ROTMOS_AGAIN)
        status = rotmos_fail(&input->failure, ROTMOS_STATE, "json_array",
                             "synchronous iteration needs a blocking reader");
    if (status == ROTMOS_EOF)
        result.eof = 1;
    else if (status != ROTMOS_OK) {
        if (input->failure.status != status)
            (void)rotmos_fail(&input->failure, status, "json_array",
                              "array input failed");
        result.error_code = EIO;
    }
    return result;
}
static rotmos_status each(struct array_input *input, const char *selector,
                          const lonejson_schema_view *schema, void *item,
                          rotmos_json_array_item_fn callback, void *userdata,
                          rotmos_error *error)
{
    lonejson_array_stream *stream;
    lonejson_array_stream_result result;
    lonejson_record_view view;
    lonejson_error native;
    rotmos_status status;
    int owned;
    size_t index = 0;
    if (item == NULL || callback == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "json_array",
                           "provide item storage and callback");
    status = rotmos_json_schema_check(schema, error);
    if (status != ROTMOS_OK)
        return status;
    memset(&view, 0, sizeof(view));
    view.size = sizeof(view);
    view.abi_version = LONEJSON_VIEW_ABI_VERSION;
    view.schema = *schema;
    view.record = item;
    status = rotmos_json_runtime(schema, &view.schema.runtime, &owned, error);
    if (status != ROTMOS_OK)
        return status;
    stream = lonejson_array_stream_open_reader(view.schema.runtime, selector,
                                               read_input, input, &native);
    if (stream == NULL)
        status = rotmos_json_error(native.code, &native, error);
    else {
        for (;;) {
            lonejson_init(view.schema.runtime, schema->map, item);
            result =
                lonejson_array_stream_next(stream, schema->map, item, &native);
            if (result == LONEJSON_ARRAY_STREAM_ITEM) {
                rotmos_error_clear(error);
                status = callback(index++, &view, userdata, error);
                if (status != ROTMOS_OK &&
                    (error == NULL || error->status != status))
                    (void)rotmos_fail(error, status, "json_array",
                                      "selected-array callback stopped");
            } else if (result == LONEJSON_ARRAY_STREAM_EOF)
                status = ROTMOS_OK;
            else
                status = rotmos_json_error(native.code, &native, error);
            lonejson_cleanup(schema->map, item);
            if (status != ROTMOS_OK || result != LONEJSON_ARRAY_STREAM_ITEM)
                break;
        }
        lonejson_array_stream_close(stream);
    }
    if (owned)
        lonejson_free(view.schema.runtime);
    if (input->failure.status != ROTMOS_OK) {
        status = input->failure.status;
        if (error != NULL)
            *error = input->failure;
    }
    if (status == ROTMOS_OK)
        rotmos_error_clear(error);
    return status;
}
rotmos_status rotmos_json_array_each_source(rotmos_source *source,
                                            const char *selector,
                                            const lonejson_schema_view *schema,
                                            void *item,
                                            rotmos_json_array_item_fn callback,
                                            void *userdata, rotmos_error *error)
{
    struct array_input input;
    if (source == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "json_array",
                           "source is required");
    memset(&input, 0, sizeof(input));
    input.source = source;
    return each(&input, selector, schema, item, callback, userdata, error);
}
rotmos_status
rotmos_request_json_array_each(rotmos_request *request, const char *selector,
                               const lonejson_schema_view *schema, void *item,
                               rotmos_json_array_item_fn callback,
                               void *userdata, rotmos_error *error)
{
    struct array_input input;
    rotmos_status status, rewind;
    rotmos_error rewind_error;
    memset(&input, 0, sizeof(input));
    input.body = rotmos_request_body(request);
    status = rotmos_body_rewind(input.body, error);
    if (status != ROTMOS_OK)
        return status;
    status = each(&input, selector, schema, item, callback, userdata, error);
    rewind = rotmos_body_rewind(input.body, &rewind_error);
    if (status == ROTMOS_OK && rewind != ROTMOS_OK) {
        status = rewind;
        if (error != NULL)
            *error = rewind_error;
    }
    return status;
}

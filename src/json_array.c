#include "json_internal.h"
#include "internal.h"
#include <errno.h>
#include <string.h>

struct array_input {
    knurl_source *source;
    knurl_body *body;
    knurl_error failure;
};
static lonejson_read_result read_input(void *data, unsigned char *bytes,
                                       size_t capacity)
{
    struct array_input *input = data;
    lonejson_read_result result;
    knurl_status status;
    memset(&result, 0, sizeof(result));
    status = input->source != NULL
                 ? knurl_source_read(input->source, bytes, capacity,
                                     &result.bytes_read, &input->failure)
                 : knurl_body_read(input->body, bytes, capacity,
                                   &result.bytes_read, &input->failure);
    if (status == KNURL_AGAIN)
        status = knurl_fail(&input->failure, KNURL_STATE, "json_array",
                            "synchronous iteration needs a blocking reader");
    if (status == KNURL_EOF)
        result.eof = 1;
    else if (status != KNURL_OK) {
        if (input->failure.status != status)
            (void)knurl_fail(&input->failure, status, "json_array",
                             "array input failed");
        result.error_code = EIO;
    }
    return result;
}
static knurl_status each(struct array_input *input, const char *selector,
                         const lonejson_schema_view *schema, void *item,
                         knurl_json_array_item_fn callback, void *userdata,
                         knurl_error *error)
{
    lonejson_array_stream *stream;
    lonejson_array_stream_result result;
    lonejson_record_view view;
    lonejson_error native;
    lonejson_status code;
    knurl_status status;
    int owned;
    size_t index = 0;
    if (item == NULL || callback == NULL)
        return knurl_fail(error, KNURL_INVALID, "json_array",
                          "provide item storage and callback");
    status = knurl_json_schema_check(schema, error);
    if (status != KNURL_OK)
        return status;
    memset(&view, 0, sizeof(view));
    view.size = sizeof(view);
    view.abi_version = LONEJSON_VIEW_ABI_VERSION;
    view.schema = *schema;
    view.record = item;
    owned = schema->runtime == NULL;
    if (owned) {
        view.schema.runtime = lonejson_new(NULL, &native);
        if (view.schema.runtime == NULL)
            return knurl_json_error(native.code, &native, error);
    }
    lonejson_error_init(&native);
    stream = lonejson_array_stream_open_reader(view.schema.runtime, selector,
                                               read_input, input, &native);
    if (stream == NULL)
        status = knurl_json_error(native.code, &native, error);
    else {
        /* Storage belongs to the caller and is reset here. Select this for
         * the stream without changing a borrowed runtime's clearing policy. */
        if (lonejson_array_stream_set_record_reuse(stream, 1, &native) !=
            LONEJSON_STATUS_OK)
            status = knurl_json_error(native.code, &native, error);
        else
            for (;;) {
                code = lonejson_reset(view.schema.runtime, schema->map, item,
                                      &native);
                if (code != LONEJSON_STATUS_OK) {
                    status = knurl_json_error(code, &native, error);
                    break;
                }
                result = lonejson_array_stream_next(stream, schema->map, item,
                                                    &native);
                if (result == LONEJSON_ARRAY_STREAM_ITEM) {
                    knurl_error_clear(error);
                    status = callback(index++, &view, userdata, error);
                    if (status != KNURL_OK &&
                        (error == NULL || error->status != status))
                        (void)knurl_fail(error, status, "json_array",
                                         "selected-array callback stopped");
                } else if (result == LONEJSON_ARRAY_STREAM_EOF)
                    status = KNURL_OK;
                else
                    status = knurl_json_error(native.code, &native, error);
                code = lonejson_reset(view.schema.runtime, schema->map, item,
                                      &native);
                if (status == KNURL_OK)
                    status = knurl_json_error(code, &native, error);
                if (status != KNURL_OK || result != LONEJSON_ARRAY_STREAM_ITEM)
                    break;
            }
        lonejson_array_stream_close(stream);
    }
    if (owned)
        lonejson_free(view.schema.runtime);
    if (input->failure.status != KNURL_OK) {
        status = input->failure.status;
        if (error != NULL)
            *error = input->failure;
    }
    if (status == KNURL_OK)
        knurl_error_clear(error);
    return status;
}
knurl_status knurl_json_array_each_source(knurl_source *source,
                                          const char *selector,
                                          const lonejson_schema_view *schema,
                                          void *item,
                                          knurl_json_array_item_fn callback,
                                          void *userdata, knurl_error *error)
{
    struct array_input input;
    if (source == NULL)
        return knurl_fail(error, KNURL_INVALID, "json_array",
                          "source is required");
    memset(&input, 0, sizeof(input));
    input.source = source;
    return each(&input, selector, schema, item, callback, userdata, error);
}
knurl_status knurl_request_json_array_each(knurl_request *request,
                                           const char *selector,
                                           const lonejson_schema_view *schema,
                                           void *item,
                                           knurl_json_array_item_fn callback,
                                           void *userdata, knurl_error *error)
{
    struct array_input input;
    knurl_status status, rewind;
    knurl_error rewind_error;
    memset(&input, 0, sizeof(input));
    input.body = knurl_request_body(request);
    status = knurl_body_rewind(input.body, error);
    if (status != KNURL_OK)
        return status;
    status = each(&input, selector, schema, item, callback, userdata, error);
    rewind = knurl_body_rewind(input.body, &rewind_error);
    if (status == KNURL_OK && rewind != KNURL_OK) {
        status = rewind;
        if (error != NULL)
            *error = rewind_error;
    }
    return status;
}

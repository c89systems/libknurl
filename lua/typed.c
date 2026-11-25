#include "binding.h"
#include <lonejson_lua.h>
#include <errno.h>

#define TYPED_REPLY_MAX_BYTES (8U * 1024U * 1024U)

static int xml_options(lua_State *L, int index, knurl_xml_config *xml,
                       knurl_error *error)
{
    static const char *const fields[] = {
        "schema",           "record",    "root_element",          "text_key",
        "attribute_prefix", "trim_text", "skip_unknown_disabled", "max_depth",
        "max_text_bytes"};
    knurl_xml_config_init(xml);
    if (!rl_fields(L, index, fields, RL_COUNT(fields), error))
        return 0;
    if (!rl_field_string(L, index, "root_element", &xml->root_element) ||
        !rl_field_string(L, index, "text_key", &xml->text_key) ||
        !rl_field_string(L, index, "attribute_prefix",
                         &xml->attribute_prefix) ||
        !rl_field_bool(L, index, "trim_text", &xml->trim_text) ||
        !rl_field_bool(L, index, "skip_unknown_disabled",
                       &xml->skip_unknown_disabled) ||
        !rl_field_size(L, index, "max_depth", &xml->max_depth) ||
        !rl_field_size(L, index, "max_text_bytes", &xml->max_text_bytes)) {
        rl_fail(error, KNURL_INVALID, "invalid XML options");
        return 0;
    }
    return 1;
}

int rl_schema(lua_State *L, int index, lonejson_schema_view *view,
              knurl_error *error)
{
    lonejson_error native;
    memset(view, 0, sizeof(*view));
    view->size = sizeof(*view);
    view->abi_version = LONEJSON_VIEW_ABI_VERSION;
    if (lonejson_lua_check_schema(L, index, view, &native) !=
        LONEJSON_STATUS_OK) {
        rl_fail(error, KNURL_INVALID, "expected a LoneJSON schema");
        return 0;
    }
    return 1;
}

int rl_typed_options(lua_State *L, int json, int xml, knurl_xml_config *options,
                     knurl_error *error)
{
    lonejson_schema_view schema;
    int ok;
    if (!lua_isnil(L, json) && !lua_isnil(L, xml)) {
        rl_fail(error, KNURL_INVALID, "choose request_json or request_xml");
        return 0;
    }
    if (lua_isnil(L, xml))
        return lua_isnil(L, json) || rl_schema(L, json, &schema, error);
    if (!xml_options(L, xml, options, error))
        return 0;
    rl_field(L, xml, "schema");
    ok = rl_schema(L, -1, &schema, error);
    lua_pop(L, 1);
    return ok;
}

struct json_input {
    knurl_source *source;
    knurl_status failure;
    knurl_error error;
};

static lonejson_read_result read_json(void *data, unsigned char *buffer,
                                      size_t capacity)
{
    struct json_input *input = data;
    lonejson_read_result result = lonejson_default_read_result();
    knurl_status status = knurl_source_read(input->source, buffer, capacity,
                                            &result.bytes_read, &input->error);
    if (status == KNURL_EOF)
        result.eof = 1;
    else if (status != KNURL_OK) {
        input->failure = status;
        result.error_code = EIO;
    }
    return result;
}

static knurl_status parse_json(lua_State *L, int schema, knurl_source *source,
                               knurl_error *error)
{
    struct json_input input;
    lonejson_error native;
    lonejson_status result;
    knurl_status status;
    memset(&input, 0, sizeof(input));
    input.source = source;
    lonejson_error_init(&native);
    result = lonejson_lua_parse_reader_to_record(L, schema, read_json, &input,
                                                 NULL, &native);
    if (input.failure != KNURL_OK) {
        *error = input.error;
        return input.failure;
    }
    if (result == LONEJSON_STATUS_OK)
        return KNURL_OK;
    status = result == LONEJSON_STATUS_ALLOCATION_FAILED ? KNURL_NOMEM
                                                         : KNURL_INVALID;
    rl_fail(error, status,
            native.message[0] ? native.message
                              : lonejson_status_string(result));
    error->source = KNURL_ERROR_LONEJSON;
    error->native_code = (long)result;
    snprintf(error->detail, sizeof(error->detail),
             "line %lu, column %lu, offset %lu", (unsigned long)native.line,
             (unsigned long)native.column, (unsigned long)native.offset);
    return status;
}

knurl_status rl_typed_input(lua_State *L, int schema, int xml,
                            const knurl_xml_config *options,
                            knurl_source *source, knurl_error *error)
{
    lonejson_record_view record;
    lonejson_error native;
    if (!xml)
        return parse_json(L, schema, source, error);
    memset(&record, 0, sizeof(record));
    record.size = sizeof(record);
    record.abi_version = LONEJSON_VIEW_ABI_VERSION;
    if (lonejson_lua_new_record(L, schema, NULL, &record, &native) !=
        LONEJSON_STATUS_OK)
        return rl_fail(error, KNURL_NOMEM,
                       "constructing a typed request record");
    return knurl_xml_parse_source(source, &record.schema, options,
                                  record.record, error);
}

static int check_record(lua_State *L, int index, lonejson_record_view *record)
{
    lonejson_error native;
    memset(record, 0, sizeof(*record));
    record->size = sizeof(*record);
    record->abi_version = LONEJSON_VIEW_ABI_VERSION;
    return lonejson_lua_check_record(L, index, NULL, record, &native) ==
           LONEJSON_STATUS_OK;
}

int rl_json_record_reply(lua_State *L, int index, knurl_reply *reply, int code,
                         const char *type, knurl_status *status,
                         knurl_error *error)
{
    lonejson_record_view record;
    knurl_owned_bytes out;
    knurl_bytes bytes;
    if (!check_record(L, index, &record))
        return 0;
    knurl_owned_bytes_init(&out);
    *status = knurl_json_serialize_materialized(
        &record.schema, record.record, TYPED_REPLY_MAX_BYTES, &out, error);
    if (*status == KNURL_OK) {
        bytes.data = out.data;
        bytes.length = out.length;
        *status = knurl_reply_bytes(
            reply, code, type ? type : "application/json", bytes, error);
    }
    knurl_owned_bytes_cleanup(&out);
    return 1;
}

knurl_status rl_xml_reply(lua_State *L, int index, knurl_reply *reply, int code,
                          const char *type, knurl_error *error)
{
    knurl_xml_config xml;
    lonejson_record_view record;
    knurl_owned_bytes out;
    knurl_bytes bytes;
    knurl_status status;
    index = lua_absindex(L, index);
    knurl_xml_config_init(&xml);
    if (lua_istable(L, index)) {
        if (!xml_options(L, index, &xml, error))
            return error->status;
        rl_field(L, index, "record");
        index = lua_absindex(L, -1);
    }
    if (!check_record(L, index, &record))
        return rl_fail(error, KNURL_INVALID,
                       "XML reply requires a LoneJSON record");
    knurl_owned_bytes_init(&out);
    status = knurl_xml_serialize_materialized(
        &record, &xml, TYPED_REPLY_MAX_BYTES, &out, error);
    if (status == KNURL_OK) {
        bytes.data = out.data;
        bytes.length = out.length;
        status = knurl_reply_bytes(reply, code, type ? type : "application/xml",
                                   bytes, error);
    }
    knurl_owned_bytes_cleanup(&out);
    return status;
}

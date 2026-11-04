#include "binding.h"
#include <rotmos/lonejson.h>
#include <lonejson_lua.h>
#include <errno.h>

#define RL_CODEC "rotmos.codec_owner"
struct codec {
    lonejson_schema_view schema;
    lonejson_record_view record;
    rotmos_xml_config xml;
    rotmos_owned_bytes bytes;
    rotmos_source *input;
    size_t max_bytes;
    int private_record;
};
static void record_init(lonejson_record_view *view)
{
    memset(view, 0, sizeof(*view));
    view->size = sizeof(*view);
    view->abi_version = LONEJSON_VIEW_ABI_VERSION;
    view->schema.size = sizeof(view->schema);
    view->schema.abi_version = LONEJSON_VIEW_ABI_VERSION;
}
static rotmos_status json_error(rotmos_error *error,
                                const lonejson_error *cause)
{
    rotmos_status status = ROTMOS_INVALID;
    if (cause->code == LONEJSON_STATUS_ALLOCATION_FAILED)
        status = ROTMOS_NOMEM;
    else if (cause->code == LONEJSON_STATUS_IO_ERROR)
        status = ROTMOS_IO;
    else if (cause->code == LONEJSON_STATUS_OVERFLOW ||
             cause->code == LONEJSON_STATUS_TRUNCATED)
        status = ROTMOS_LIMIT;
    else if (cause->code == LONEJSON_STATUS_UNSUPPORTED)
        status = ROTMOS_NOT_SUPPORTED;
    else if (cause->code == LONEJSON_STATUS_INTERNAL_ERROR)
        status = ROTMOS_BACKEND;
    rl_fail(error, status,
            cause->message[0] ? cause->message
                              : "invalid LoneJSON schema/record");
    error->source = ROTMOS_ERROR_LONEJSON;
    error->native_code = (long)cause->code;
    snprintf(error->detail, sizeof(error->detail),
             "line %lu, column %lu, offset %lu, errno %d",
             (unsigned long)cause->line, (unsigned long)cause->column,
             (unsigned long)cause->offset, cause->system_errno);
    return error->status;
}
static void cleanup(lua_State *L, int owner, struct codec *c)
{
    rotmos_owned_bytes_cleanup(&c->bytes);
    rotmos_source_destroy(c->input, NULL);
    c->input = NULL;
    if (c->private_record) {
        lua_getiuservalue(L, owner, 2);
        (void)lonejson_lua_clear_record(L, -1, NULL);
        lua_pop(L, 1);
        c->private_record = 0;
    }
}
static int codec_close(lua_State *L)
{
    struct codec *c = luaL_testudata(L, 1, RL_CODEC);
    if (c != NULL)
        cleanup(L, 1, c);
    return 0;
}
static int xml_options(lua_State *L, int options, rotmos_xml_config *xml)
{
    int skip = 1, strict = 0;
    if (!rl_field_string(L, options, "root_element", &xml->root_element) ||
        !rl_field_string(L, options, "text_key", &xml->text_key) ||
        !rl_field_string(L, options, "attribute_prefix",
                         &xml->attribute_prefix) ||
        !rl_field_bool(L, options, "trim_text", &xml->trim_text) ||
        !rl_field_bool(L, options, "skip_unknown", &skip) ||
        !rl_field_bool(L, options, "strict_unknown", &strict) ||
        !rl_field_size(L, options, "max_depth", &xml->max_depth) ||
        !rl_field_size(L, options, "max_text_bytes", &xml->max_text_bytes))
        return 0;
    xml->skip_unknown_disabled = !skip;
    rl_field(L, options, "strict_unknown");
    if (!lua_isnil(L, -1))
        xml->skip_unknown_disabled = strict;
    lua_pop(L, 1);
    return 1;
}
static struct codec *prepare(lua_State *L, int options, int xml, int mode,
                             rotmos_error *error)
{
    static const char *const names[] = {"schema",
                                        "record",
                                        "value",
                                        "table",
                                        "max_bytes",
                                        "root_element",
                                        "text_key",
                                        "attribute_prefix",
                                        "trim_text",
                                        "skip_unknown",
                                        "strict_unknown",
                                        "max_depth",
                                        "max_text_bytes",
                                        "xml",
                                        "json",
                                        "data",
                                        "path"};
    struct codec *c;
    lonejson_error cause;
    lonejson_status status;
    size_t i;
    int owner, snapshot, schema, value, record = 0;
    options = lua_absindex(L, options);
    if (!lua_istable(L, options) ||
        !rl_fields(L, options, names, sizeof(names) / sizeof(names[0]),
                   error)) {
        if (!lua_istable(L, options))
            rl_fail(error, ROTMOS_INVALID, "codec options table required");
        return NULL;
    }
    c = lua_newuserdatauv(L, sizeof(*c), 3);
    memset(c, 0, sizeof(*c));
    c->schema.size = sizeof(c->schema);
    c->schema.abi_version = LONEJSON_VIEW_ABI_VERSION;
    record_init(&c->record);
    rotmos_xml_config_init(&c->xml);
    luaL_setmetatable(L, RL_CODEC);
    owner = lua_absindex(L, -1);
    lua_newtable(L);
    snapshot = lua_absindex(L, -1);
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if ((!xml && i >= 5 && i < 13) || (mode < 2 && i >= 13))
            continue;
        lua_getfield(L, options, names[i]);
        lua_setfield(L, snapshot, names[i]);
    }
    lua_pushvalue(L, snapshot);
    lua_setiuservalue(L, owner, 3);
    rl_field(L, snapshot, "schema");
    schema = lua_absindex(L, -1);
    lua_pushvalue(L, schema);
    lua_setiuservalue(L, owner, 1);
    lonejson_error_init(&cause);
    status = lonejson_lua_check_schema(L, schema, &c->schema, &cause);
    if (status != LONEJSON_STATUS_OK) {
        json_error(error, &cause);
        return NULL;
    }
    if (!rl_field_size(L, snapshot, "max_bytes", &c->max_bytes))
        goto invalid;
    if (xml) {
        if (!xml_options(L, snapshot, &c->xml))
            goto invalid;
    } else {
        for (i = 5; i < 13; ++i) {
            rl_field(L, options, names[i]);
            if (!lua_isnil(L, -1))
                goto invalid;
            lua_pop(L, 1);
        }
    }
    if (mode >= 2) {
        for (i = 1; i <= 4; ++i) {
            rl_field(L, snapshot, names[i]);
            if (!lua_isnil(L, -1))
                goto invalid;
            lua_pop(L, 1);
        }
        if (mode == 3) {
            for (i = 13; i < sizeof(names) / sizeof(names[0]); ++i) {
                rl_field(L, snapshot, names[i]);
                if (!lua_isnil(L, -1))
                    goto invalid;
                lua_pop(L, 1);
            }
        }
        lua_settop(L, owner);
        return c;
    }
    for (i = 13; i < sizeof(names) / sizeof(names[0]); ++i) {
        rl_field(L, options, names[i]);
        if (!lua_isnil(L, -1))
            goto invalid;
        lua_pop(L, 1);
    }
    rl_field(L, snapshot, "record");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        rl_field(L, snapshot, "value");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            rl_field(L, snapshot, "table");
        }
    }
    value = lua_absindex(L, -1);
    if (!mode && lua_type(L, value) == LUA_TUSERDATA) {
        status =
            lonejson_lua_check_record(L, value, &c->schema, &c->record, &cause);
        record = value;
    } else {
        if (!lua_istable(L, value)) {
            if (mode)
                luaL_error(
                    L,
                    "callback writers require value tables; externally mutable "
                    "records "
                    "have no public mutation lease (use serialize to buffer)");
            rl_fail(error, ROTMOS_INVALID,
                    "codec value table or record required");
            return NULL;
        }
        status =
            lonejson_lua_new_record(L, schema, &record, &c->record, &cause);
        if (status == LONEJSON_STATUS_OK) {
            lua_pushvalue(L, record);
            lua_setiuservalue(L, owner, 2);
            c->private_record = 1;
            lua_getfield(L, schema, "assign");
            lua_pushvalue(L, schema);
            lua_pushvalue(L, record);
            lua_pushvalue(L, value);
            if (lua_pcall(L, 3, 1, 0) != LUA_OK) {
                /* Assignment misuse has the same Lua error contract as Vectis.
                 * Clear private fields before propagating that error. */
                (void)lonejson_lua_clear_record(L, record, NULL);
                c->private_record = 0;
                lua_error(L);
                return NULL;
            }
            lua_pop(L, 1);
            record_init(&c->record);
            status = lonejson_lua_check_record(L, record, &c->schema,
                                               &c->record, &cause);
        }
    }
    if (status != LONEJSON_STATUS_OK) {
        json_error(error, &cause);
        return NULL;
    }
    lua_pushvalue(L, record);
    lua_setiuservalue(L, owner, 2);
    lua_settop(L, owner);
    return c;
invalid:
    rl_fail(error, ROTMOS_INVALID, "invalid codec option");
    return NULL;
}
static int serialize(lua_State *L, int xml)
{
    struct codec *c;
    rotmos_error error;
    rotmos_status status;
    int owner;
    c = prepare(L, 1, xml, 0, &error);
    if (c == NULL)
        return rl_error(L, &error);
    owner = lua_absindex(L, -1);
    status =
        xml ? rotmos_xml_serialize_materialized(&c->record, &c->xml,
                                                c->max_bytes, &c->bytes, &error)
            : rotmos_json_serialize_materialized(&c->schema, c->record.record,
                                                 c->max_bytes, &c->bytes,
                                                 &error);
    if (status != ROTMOS_OK)
        return rl_error(L, &error);
    lua_pushlstring(L, c->bytes.data == NULL ? "" : c->bytes.data,
                    c->bytes.length);
    rotmos_owned_bytes_cleanup(&c->bytes);
    if (c->private_record) {
        lua_getiuservalue(L, owner, 2);
        (void)lonejson_lua_clear_record(L, -1, NULL);
        lua_pop(L, 1);
        c->private_record = 0;
    }
    return 1;
}
static rotmos_status codec_source(lua_State *L, int index, int xml,
                                  rotmos_error *error)
{
    struct codec *c;
    struct rl_source *s;
    rotmos_status status;
    rotmos_json_source_config json;
    rotmos_xml_source_config config;
    c = prepare(L, index, xml, 1, error);
    if (c == NULL)
        return error->status;
    if (c->max_bytes != 0)
        return rl_fail(error, ROTMOS_INVALID,
                       "max_bytes is for materialized serialization");
    s = rl_source_native_new(L, -1, codec_close, error);
    if (s == NULL)
        return error->status;
    if (xml) {
        rotmos_xml_source_config_init(&config);
        config.record = &c->record;
        config.xml = c->xml;
        status = rotmos_source_xml(&config, &s->delegate, error);
    } else {
        rotmos_json_source_config_init(&json);
        json.record = c->record;
        status = rotmos_source_json(&json, &s->delegate, error);
    }
    if (status != ROTMOS_OK) {
        lua_pushvalue(L, -1);
        s->root = luaL_ref(L, LUA_REGISTRYINDEX);
        rotmos_source_destroy(s->native, NULL);
        return status;
    }
    return ROTMOS_OK;
}
static int source(lua_State *L, int xml)
{
    rotmos_error error;
    if (codec_source(L, 1, xml, &error) != ROTMOS_OK)
        return rl_error(L, &error);
    return 1;
}
rotmos_status rl_codec_reply(lua_State *L, int index, int xml, int materialized,
                             rotmos_reply *reply, int code, const char *type,
                             rotmos_error *error)
{
    struct codec *c;
    rotmos_bytes bytes;
    int owner;
    rotmos_status status;
    if (type == NULL)
        type = xml ? "application/xml" : "application/json";
    if (materialized) {
        c = prepare(L, index, xml, 0, error);
        if (c == NULL)
            return error->status;
        owner = lua_absindex(L, -1);
        status = xml ? rotmos_xml_serialize_materialized(
                           &c->record, &c->xml, c->max_bytes, &c->bytes, error)
                     : rotmos_json_serialize_materialized(
                           &c->schema, c->record.record, c->max_bytes,
                           &c->bytes, error);
        if (status == ROTMOS_OK) {
            bytes.data = c->bytes.data;
            bytes.length = c->bytes.length;
            status = rotmos_reply_bytes(reply, code, type, bytes, error);
        }
        cleanup(L, owner, c);
        return status;
    }
    status = codec_source(L, index, xml, error);
    if (status != ROTMOS_OK)
        return status;
    return rl_source_take(L, -1, reply, code, type, error);
}
struct sink_call {
    const void *bytes;
    size_t length;
    rotmos_status status;
    rotmos_error *error;
};
static int sink_trampoline(lua_State *L)
{
    struct sink_call *c = lua_touserdata(L, 1);
    lua_pushvalue(L, 2);
    lua_pushlstring(L, c->bytes, c->length);
    if (lua_pcall(L, 1, 2, 0) != LUA_OK) {
        c->status = rl_callback_error(L, -1, c->error, "writer sink failed");
        return 0;
    }
    if ((lua_isboolean(L, -2) && !lua_toboolean(L, -2)) || !lua_isnil(L, -1))
        c->status =
            rl_callback_error(L, -1, c->error, "writer sink rejected bytes");
    return 0;
}
static int write(lua_State *L, int xml)
{
    struct rl_source *s;
    struct sink_call call;
    rotmos_error error;
    rotmos_status status;
    rotmos_u64 written, amount;
    unsigned char *buffer;
    size_t count;
    if (!lua_isfunction(L, 2) && !lua_istable(L, 2))
        return rl_error(
            L,
            (rl_fail(&error, ROTMOS_INVALID, "writer sink required"), &error));
    if (lua_istable(L, 2)) {
        /* Vectis snapshots a table sink's function before output. Mutating the
         * table during a callback cannot replace the active sink. */
        lua_getfield(L, 2, "write");
        if (!lua_isfunction(L, -1))
            return rl_error(L, (rl_fail(&error, ROTMOS_INVALID,
                                        "writer sink function required"),
                                &error));
        lua_replace(L, 2);
    }
    if (source(L, xml) != 1)
        return 2;
    s = luaL_testudata(L, -1, RL_SOURCE);
    buffer = lua_newuserdatauv(L, 4096, 0);
    /* Native close runs outside a Source method frame here. Pin the actual
     * Source before callbacks so cleanup never mistakes options for userdata.
     */
    lua_pushvalue(L, -2);
    s->root = luaL_ref(L, LUA_REGISTRYINDEX);
    memset(&call, 0, sizeof(call));
    call.bytes = buffer;
    call.error = &error;
    written.high = written.low = 0;
    for (;;) {
        status = rotmos_source_read(s->native, buffer, 4096, &count, &error);
        if (status == ROTMOS_EOF) {
            status = ROTMOS_OK;
            break;
        }
        if (status != ROTMOS_OK)
            break;
        call.length = count;
        status = rl_protect_value(L, sink_trampoline, &call, 2, &error);
        if (status == ROTMOS_OK)
            status = call.status;
        if (status != ROTMOS_OK)
            break;
        rotmos_u64_from_size(count, &amount, NULL);
        status = rotmos_u64_add(written, amount, &written, &error);
        if (status != ROTMOS_OK)
            break;
    }
    rotmos_source_destroy(s->native, NULL);
    if (status != ROTMOS_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    rl_push_u64(L, written);
    return 2;
}
struct input_reader {
    rotmos_source *source;
    rotmos_body *body;
    rotmos_error failure;
};
static lonejson_read_result input_read(void *data, unsigned char *buffer,
                                       size_t capacity)
{
    struct input_reader *reader = data;
    lonejson_read_result result;
    rotmos_status status;
    memset(&result, 0, sizeof(result));
    status = reader->body != NULL
                 ? rotmos_body_read(reader->body, buffer, capacity,
                                    &result.bytes_read, &reader->failure)
                 : rotmos_source_read(reader->source, buffer, capacity,
                                      &result.bytes_read, &reader->failure);
    if (status == ROTMOS_EOF)
        result.eof = 1;
    else if (status != ROTMOS_OK)
        result.error_code = EIO;
    return result;
}
static rotmos_status parse_json(lua_State *L, int schema,
                                struct input_reader *reader, int *record,
                                rotmos_error *error)
{
    lonejson_error cause;
    lonejson_status status;
    *record = 0;
    status = lonejson_lua_parse_reader_to_record(L, schema, input_read, reader,
                                                 record, &cause);
    if (reader->failure.status != ROTMOS_OK) {
        *error = reader->failure;
        return error->status;
    }
    if (status != LONEJSON_STATUS_OK) {
        if (*record != 0)
            (void)lonejson_lua_clear_record(L, *record, NULL);
        return json_error(error, &cause);
    }
    return ROTMOS_OK;
}
static rotmos_status new_record(lua_State *L, int owner, struct codec *c,
                                rotmos_error *error)
{
    lonejson_error cause;
    lonejson_status status;
    int index;
    lua_getiuservalue(L, owner, 1);
    status = lonejson_lua_new_record(L, -1, &index, &c->record, &cause);
    if (status != LONEJSON_STATUS_OK)
        return json_error(error, &cause);
    lua_pushvalue(L, index);
    lua_setiuservalue(L, owner, 2);
    c->private_record = 1;
    return ROTMOS_OK;
}
static int parse(lua_State *L, int xml, int table)
{
    struct codec *c;
    struct input_reader reader;
    rotmos_error error;
    rotmos_status status;
    rotmos_bytes bytes;
    const char *path = NULL;
    int owner, options, record = 0, has_data;
    c = prepare(L, 1, xml, 2, &error);
    if (c == NULL)
        return rl_error(L, &error);
    owner = lua_absindex(L, -1);
    lua_getiuservalue(L, owner, 3);
    options = lua_absindex(L, -1);
    rl_field(L, options, xml ? "json" : "xml");
    if (!lua_isnil(L, -1))
        goto invalid;
    lua_pop(L, 1);
    rl_field(L, options, xml ? "xml" : "json");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        rl_field(L, options, "data");
    }
    has_data = !lua_isnil(L, -1);
    if (has_data && lua_type(L, -1) != LUA_TSTRING)
        goto invalid;
    bytes.data = lua_tolstring(L, -1, &bytes.length);
    if (!rl_field_string(L, options, "path", &path) ||
        has_data == (path != NULL))
        goto invalid;
    status = path != NULL ? rotmos_source_file(path, &c->input, &error)
                          : rotmos_source_bytes(bytes, &c->input, &error);
    if (status != ROTMOS_OK)
        return rl_error(L, &error);
    if (xml) {
        status = new_record(L, owner, c, &error);
        if (status == ROTMOS_OK)
            status = rotmos_xml_parse_source(c->input, &c->schema, &c->xml,
                                             c->record.record, &error);
    } else {
        memset(&reader, 0, sizeof(reader));
        reader.source = c->input;
        lua_getiuservalue(L, owner, 1);
        status = parse_json(L, lua_absindex(L, -1), &reader, &record, &error);
        if (status == ROTMOS_OK) {
            lua_pushvalue(L, record);
            lua_setiuservalue(L, owner, 2);
            c->private_record = 1;
        }
    }
    rotmos_source_destroy(c->input, NULL);
    c->input = NULL;
    if (status != ROTMOS_OK) {
        cleanup(L, owner, c);
        return rl_error(L, &error);
    }
    lua_getiuservalue(L, owner, 2);
    if (table) {
        if (lonejson_lua_record_to_table(L, -1) == 0)
            return rl_error(
                L, (rl_fail(&error, ROTMOS_INVALID, "record conversion failed"),
                    &error));
        cleanup(L, owner, c);
    } else
        c->private_record =
            0; /* Returned userdata owns its independent record. */
    return 1;
invalid:
    return rl_error(L, (rl_fail(&error, ROTMOS_INVALID,
                                "provide exactly one data string or path"),
                        &error));
}
int rl_request_json_record(lua_State *L)
{
    struct rl_request *request;
    struct input_reader reader;
    lonejson_schema_view schema;
    lonejson_error cause;
    rotmos_error error, rewind_error;
    rotmos_status status, rewind_status;
    int record;
    if (!rl_request_check(L, 1, &request, &error))
        return rl_error(L, &error);
    memset(&schema, 0, sizeof(schema));
    schema.size = sizeof(schema);
    schema.abi_version = LONEJSON_VIEW_ABI_VERSION;
    if (lonejson_lua_check_schema(L, 2, &schema, &cause) != LONEJSON_STATUS_OK)
        return rl_error(L, (json_error(&error, &cause), &error));
    memset(&reader, 0, sizeof(reader));
    reader.body = rotmos_request_body(request->native);
    if (rotmos_body_rewind(reader.body, &error) != ROTMOS_OK)
        return rl_error(L, &error);
    status = parse_json(L, 2, &reader, &record, &error);
    rewind_status = rotmos_body_rewind(reader.body, &rewind_error);
    if (status == ROTMOS_OK && rewind_status != ROTMOS_OK) {
        status = rewind_status;
        error = rewind_error;
    }
    if (status == ROTMOS_LIMIT)
        error.status = status = ROTMOS_INVALID;
    if (status != ROTMOS_OK)
        return rl_error(L, &error);
    return 1;
}
int rl_request_xml_record(lua_State *L)
{
    struct rl_request *request;
    struct codec *c;
    rotmos_error error;
    rotmos_status status;
    int owner;
    if (!rl_request_check(L, 1, &request, &error))
        return rl_error(L, &error);
    c = prepare(L, 2, 1, 3, &error);
    if (c == NULL)
        return rl_error(L, &error);
    owner = lua_absindex(L, -1);
    status = new_record(L, owner, c, &error);
    if (status == ROTMOS_OK)
        status = rotmos_request_xml_into(request->native, &c->schema, &c->xml,
                                         c->record.record, &error);
    if (status != ROTMOS_OK) {
        cleanup(L, owner, c);
        return rl_error(L, &error);
    }
    c->private_record = 0;
    lua_getiuservalue(L, owner, 2);
    return 1;
}
struct record_owner {
    lua_State *L;
    int root;
};
struct record_call {
    struct rl_rule *rule;
    struct rl_request *request;
    rotmos_record_storage *storage;
    struct record_owner *owner;
    int output;
    rotmos_status status;
    rotmos_error *error;
};
static int record_acquire_call(lua_State *L)
{
    struct record_call *call = lua_touserdata(L, 1);
    lonejson_error cause;
    lonejson_status status;
    int record;
    rl_rule_options(L, call->rule);
    rl_field(L, -1, call->output ? "output_schema" : "input_schema");
    record_init(&call->storage->view);
    status =
        lonejson_lua_new_record(L, -1, &record, &call->storage->view, &cause);
    if (status != LONEJSON_STATUS_OK) {
        call->status = json_error(call->error, &cause);
        return 0;
    }
    lua_pushvalue(L, record);
    call->owner->root = luaL_ref(L, LUA_REGISTRYINDEX);
    call->request->typed_records[call->output] = call->owner->root;
    return 0;
}
static rotmos_status record_acquire(rotmos_request *request, int output,
                                    const lonejson_schema_view *schema,
                                    rotmos_record_storage *storage, void *data,
                                    rotmos_error *error)
{
    struct record_call call;
    rotmos_status status;
    (void)schema;
    memset(&call, 0, sizeof(call));
    call.rule = data;
    call.request = rotmos_request_userdata(request);
    call.output = output;
    call.storage = storage;
    call.error = error;
    if (call.request == NULL)
        return rl_fail(error, ROTMOS_STATE, "Lua request association missing");
    call.owner = calloc(1, sizeof(*call.owner));
    if (call.owner == NULL)
        return rl_fail(error, ROTMOS_NOMEM, "allocating Lua record owner");
    call.owner->L = call.rule->server->L;
    call.owner->root = LUA_NOREF;
    storage->owner = call.owner;
    status = rl_protect(call.owner->L, record_acquire_call, &call, error);
    return status == ROTMOS_OK ? call.status : status;
}
static void record_release(rotmos_record_storage *storage, void *data)
{
    struct record_owner *owner = storage->owner;
    (void)data;
    if (owner != NULL) {
        if (owner->root != LUA_NOREF)
            luaL_unref(owner->L, LUA_REGISTRYINDEX, owner->root);
        free(owner);
    }
}
struct typed_call {
    struct rl_rule *rule;
    rotmos_request *request;
    rotmos_reply *reply;
    const lonejson_record_view *input, *output;
    rotmos_status status;
    rotmos_error *error;
};
static int typed_handler_call(lua_State *L)
{
    struct typed_call *call = lua_touserdata(L, 1);
    struct rl_request *request = rotmos_request_userdata(call->request);
    rl_rule_options(L, call->rule);
    rl_field(L, -1, "handler");
    rl_request_push(L, request);
    if (call->input != NULL)
        lua_rawgeti(L, LUA_REGISTRYINDEX, request->typed_records[0]);
    else
        lua_pushnil(L);
    if (call->rule->codec_kind == 3) {
        if (call->output != NULL)
            lua_rawgeti(L, LUA_REGISTRYINDEX, request->typed_records[1]);
        else
            lua_pushnil(L);
    }
    lua_call(L, call->rule->codec_kind == 3 ? 3 : 2, 2);
    if (!lua_isnil(L, -1))
        call->status = rl_callback_error(L, -1, call->error,
                                         "typed route callback failed");
    else if (call->rule->codec_kind != 3)
        call->status = rl_reply(L, -2, call->reply, call->request, call->error);
    request->native_reply = 1;
    return 0;
}
static rotmos_status typed_handler(rotmos_request *request,
                                   const lonejson_record_view *input,
                                   const lonejson_record_view *output,
                                   rotmos_reply *reply, void *data,
                                   rotmos_error *error)
{
    struct typed_call call;
    rotmos_status status;
    memset(&call, 0, sizeof(call));
    call.rule = data;
    call.request = request;
    call.reply = reply;
    call.input = input;
    call.output = output;
    call.error = error;
    status = rl_protect(call.rule->server->L, typed_handler_call, &call, error);
    return status == ROTMOS_OK ? call.status : status;
}
static rotmos_status json_handler(rotmos_request *request,
                                  const lonejson_record_view *input,
                                  const lonejson_record_view *output,
                                  void *data, rotmos_error *error)
{
    return typed_handler(request, input, output, NULL, data, error);
}
static rotmos_status general_handler(rotmos_request *request,
                                     const lonejson_record_view *input,
                                     rotmos_reply *reply, void *data,
                                     rotmos_error *error)
{
    return typed_handler(request, input, NULL, reply, data, error);
}
static int route_schema(lua_State *L, const char *name,
                        lonejson_schema_view *view, rotmos_error *error)
{
    lonejson_error cause;
    int valid = 1;
    memset(view, 0, sizeof(*view));
    view->size = sizeof(*view);
    view->abi_version = LONEJSON_VIEW_ABI_VERSION;
    rl_field(L, 2, name);
    if (!lua_isnil(L, -1) &&
        lonejson_lua_check_schema(L, -1, view, &cause) != LONEJSON_STATUS_OK) {
        json_error(error, &cause);
        valid = 0;
    }
    lua_pop(L, 1);
    return valid;
}
rotmos_status rl_codec_route_register(lua_State *L, struct rl_rule *rule,
                                      struct rl_rule *parent,
                                      const rotmos_route_config *config,
                                      int kind, rotmos_error *error)
{
    static const rotmos_record_provider provider = {record_acquire,
                                                    record_release};
    lonejson_schema_view input, output;
    rotmos_json_route_config json;
    rotmos_json_typed_route_config typed;
    rotmos_xml_route_config xml;
    size_t bound = 0, buffer_bytes = 0;
    static const char *const fields[] = {
        "root_element", "text_key",       "attribute_prefix", "trim_text",
        "skip_unknown", "strict_unknown", "max_depth",        "max_text_bytes"};
    static const char *const specialized[] = {
        "output_schema", "max_output_bytes", "xml", "buffer_bytes"};
    size_t i;
    int valid;
    for (i = 0; i < sizeof(specialized) / sizeof(specialized[0]); ++i) {
        rl_field(L, 2, specialized[i]);
        valid = lua_isnil(L, -1) || (i < 2 ? kind == 3 : kind == 5);
        lua_pop(L, 1);
        if (!valid)
            return rl_fail(error, ROTMOS_INVALID,
                           "option does not apply to this typed route");
    }
    if (!route_schema(L, "input_schema", &input, error) ||
        !route_schema(L, "output_schema", &output, error))
        return error->status;
    if (!rl_field_size(L, 2, "max_output_bytes", &bound) ||
        !rl_field_size(L, 2, "buffer_bytes", &buffer_bytes) ||
        (kind != 5 && buffer_bytes != 0))
        return rl_fail(error, ROTMOS_INVALID, "invalid typed route bound");
    rule->native_reply = 1;
    rule->codec_kind = kind;
    if (kind == 5) {
        rotmos_xml_route_config_init(&xml);
        xml.method = config->method;
        xml.methods = config->methods;
        xml.path = config->path;
        xml.path_kind = config->path_kind;
        xml.body = config->body;
        xml.hooks = config->hooks;
        xml.userdata = rule;
        xml.records = &provider;
        xml.input = &input;
        xml.buffer_bytes = buffer_bytes;
        xml.handler = general_handler;
        rl_field(L, 2, "xml");
        valid = lua_isnil(L, -1) ||
                (lua_istable(L, -1) &&
                 rl_fields(L, -1, fields, sizeof(fields) / sizeof(fields[0]),
                           error) &&
                 xml_options(L, lua_absindex(L, -1), &xml.xml));
        lua_pop(L, 1);
        if (!valid)
            return rl_fail(error, ROTMOS_INVALID, "invalid XML route options");
        return parent != NULL ? rotmos_route_group_xml_route(parent->group,
                                                             &xml, NULL, error)
                              : rotmos_router_xml_route(
                                    rotmos_server_router(rule->server->native),
                                    &xml, NULL, error);
    }
    if (kind == 4) {
        rotmos_json_typed_route_config_init(&typed);
        typed.method = config->method;
        typed.methods = config->methods;
        typed.path = config->path;
        typed.path_kind = config->path_kind;
        typed.body = config->body;
        typed.hooks = config->hooks;
        typed.userdata = rule;
        typed.records = &provider;
        typed.input = input;
        typed.handler = general_handler;
        return parent != NULL ? rotmos_route_group_json_typed_route(
                                    parent->group, &typed, NULL, error)
                              : rotmos_router_json_typed_route(
                                    rotmos_server_router(rule->server->native),
                                    &typed, NULL, error);
    }
    rotmos_json_route_config_init(&json);
    json.method = config->method;
    json.methods = config->methods;
    json.path = config->path;
    json.path_kind = config->path_kind;
    json.body = config->body;
    json.hooks = config->hooks;
    json.userdata = rule;
    json.records = &provider;
    json.input = input;
    json.output = output;
    json.max_output_bytes = bound;
    json.handler = json_handler;
    return parent != NULL ? rotmos_route_group_json_route(parent->group, &json,
                                                          NULL, error)
                          : rotmos_router_json_route(
                                rotmos_server_router(rule->server->native),
                                &json, NULL, error);
}
struct array_call {
    lua_State *L;
    int context;
    size_t index, count;
    rotmos_status status;
    rotmos_error *error;
};
static int array_item_call(lua_State *L)
{
    struct array_call *call = lua_touserdata(L, 1);
    lua_rawgeti(L, 2, 1);
    lua_rawgeti(L, 2, 2);
    if (lonejson_lua_record_to_table(L, -1) == 0)
        return luaL_error(L, "selected-array record conversion failed");
    lua_remove(L, -2);
    rl_push_size(L, call->index);
    lua_call(L, 2, 2);
    call->status = lua_isnil(L, -1)
                       ? ROTMOS_OK
                       : rl_callback_error(L, -1, call->error,
                                           "selected-array callback failed");
    return 0;
}
static rotmos_status array_item(size_t index, const lonejson_record_view *view,
                                void *data, rotmos_error *error)
{
    struct array_call *call = data;
    rotmos_status status;
    (void)view;
    call->index = index;
    call->error = error;
    status =
        rl_protect_value(call->L, array_item_call, call, call->context, error);
    if (status == ROTMOS_OK)
        status = call->status;
    if (status == ROTMOS_OK)
        ++call->count;
    return status;
}
static int array_each(lua_State *L, int request_mode)
{
    static const char *const fields[] = {"schema", "selector", "callback",
                                         "source", "data",     "path"};
    struct rl_request *request = NULL;
    struct rl_source *borrowed = NULL;
    struct codec *c;
    struct array_call call;
    rotmos_source *source = NULL;
    rotmos_bytes bytes;
    rotmos_error error;
    rotmos_status status;
    const char *selector = NULL, *path = NULL;
    int options = request_mode ? 2 : 1, owner, context, choices = 0, ref;
    if (request_mode && !rl_request_check(L, 1, &request, &error))
        return rl_error(L, &error);
    if (!lua_istable(L, options) ||
        !rl_fields(L, options, fields, request_mode ? 3 : 6, &error))
        return rl_error(L, (rl_fail(&error, ROTMOS_INVALID,
                                    "selected-array declaration required"),
                            &error));
    lua_newtable(L);
    rl_field(L, options, "schema");
    lua_setfield(L, -2, "schema");
    c = prepare(L, -1, 0, 3, &error);
    if (c == NULL)
        return rl_error(L, &error);
    owner = lua_absindex(L, -1);
    status = new_record(L, owner, c, &error);
    if (status != ROTMOS_OK)
        return rl_error(L, &error);
    lua_newtable(L);
    context = lua_absindex(L, -1);
    rl_field(L, options, "callback");
    if (!lua_isfunction(L, -1))
        goto invalid;
    lua_rawseti(L, context, 1);
    lua_getiuservalue(L, owner, 2);
    lua_rawseti(L, context, 2);
    if (!rl_field_string(L, options, "selector", &selector))
        goto invalid;
    memset(&call, 0, sizeof(call));
    call.L = L;
    call.context = context;
    if (request_mode)
        status = rotmos_request_json_array_each(request->native, selector,
                                                &c->schema, c->record.record,
                                                array_item, &call, &error);
    else {
        rl_field(L, options, "source");
        if (!lua_isnil(L, -1)) {
            ++choices;
            borrowed = luaL_testudata(L, -1, RL_SOURCE);
            if (borrowed == NULL || borrowed->native == NULL ||
                !borrowed->owned || borrowed->active ||
                borrowed->root != LUA_NOREF)
                goto invalid;
            source = borrowed->native;
        }
        ref = lua_absindex(L, -1);
        rl_field(L, options, "data");
        bytes.data = NULL;
        bytes.length = 0;
        if (!lua_isnil(L, -1)) {
            ++choices;
            if (lua_type(L, -1) != LUA_TSTRING)
                goto invalid;
            bytes.data = lua_tolstring(L, -1, &bytes.length);
        }
        if (!rl_field_string(L, options, "path", &path))
            goto invalid;
        choices += path != NULL;
        if (choices != 1)
            goto invalid;
        if (borrowed != NULL) {
            lua_pushvalue(L, ref);
            borrowed->root = luaL_ref(L, LUA_REGISTRYINDEX);
            borrowed->L = L;
            borrowed->owned = 0;
            status = ROTMOS_OK;
        } else
            status = path != NULL
                         ? rotmos_source_file(path, &c->input, &error)
                         : rotmos_source_bytes(bytes, &c->input, &error);
        if (status == ROTMOS_OK)
            status = rotmos_json_array_each_source(
                borrowed != NULL ? source : c->input, selector, &c->schema,
                c->record.record, array_item, &call, &error);
        if (borrowed != NULL) {
            borrowed->owned = 1;
            luaL_unref(L, LUA_REGISTRYINDEX, borrowed->root);
            borrowed->root = LUA_NOREF;
        }
    }
    cleanup(L, owner, c);
    if (status != ROTMOS_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    rl_push_size(L, call.count);
    return 2;
invalid:
    return rl_error(
        L, (rl_fail(&error, ROTMOS_INVALID,
                    "provide schema, callback and one source/data/path"),
            &error));
}
static int json_array_each(lua_State *L) { return array_each(L, 0); }
int rl_request_json_array_each(lua_State *L) { return array_each(L, 1); }
static int json_parse(lua_State *L) { return parse(L, 0, 1); }
static int json_parse_record(lua_State *L) { return parse(L, 0, 0); }
static int xml_parse(lua_State *L) { return parse(L, 1, 1); }
static int xml_parse_record(lua_State *L) { return parse(L, 1, 0); }
static int json_serialize(lua_State *L) { return serialize(L, 0); }
static int xml_serialize(lua_State *L) { return serialize(L, 1); }
static int json_source(lua_State *L) { return source(L, 0); }
static int xml_source(lua_State *L) { return source(L, 1); }
static int json_write(lua_State *L) { return write(L, 0); }
static int xml_write(lua_State *L) { return write(L, 1); }
void rl_codecs_open(lua_State *L)
{
    static const luaL_Reg json[] = {{"serialize", json_serialize},
                                    {"array_each", json_array_each},
                                    {"parse", json_parse},
                                    {"parse_record", json_parse_record},
                                    {"source", json_source},
                                    {"write", json_write},
                                    {NULL, NULL}};
    static const luaL_Reg xml[] = {{"serialize", xml_serialize},
                                   {"parse", xml_parse},
                                   {"parse_record", xml_parse_record},
                                   {"source", xml_source},
                                   {"write", xml_write},
                                   {NULL, NULL}};
    luaL_newmetatable(L, RL_CODEC);
    lua_pushcfunction(L, codec_close);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    luaL_newlib(L, json);
    lua_setfield(L, -2, "json");
    luaL_newlib(L, xml);
    rl_xml_objects_open(L);
    lua_setfield(L, -2, "xml");
}

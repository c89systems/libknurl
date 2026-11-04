#include "binding.h"
#include <rotmos/lonejson.h>
#include <lonejson_lua.h>

#define RL_CODEC "rotmos.codec_owner"
struct codec {
    lonejson_schema_view schema;
    lonejson_record_view record;
    rotmos_xml_config xml;
    rotmos_owned_bytes bytes;
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
    rl_fail(error,
            cause->code == LONEJSON_STATUS_ALLOCATION_FAILED ? ROTMOS_NOMEM
                                                             : ROTMOS_INVALID,
            cause->message[0] ? cause->message
                              : "invalid LoneJSON schema/record");
    error->source = ROTMOS_ERROR_LONEJSON;
    error->native_code = (long)cause->code;
    return error->status;
}
static int codec_close(lua_State *L)
{
    struct codec *c = luaL_testudata(L, 1, RL_CODEC);
    if (c == NULL)
        return 0;
    rotmos_owned_bytes_cleanup(&c->bytes);
    if (c->private_record) {
        lua_getiuservalue(L, 1, 2);
        (void)lonejson_lua_clear_record(L, -1, NULL);
        lua_pop(L, 1);
        c->private_record = 0;
    }
    return 0;
}
static struct codec *prepare(lua_State *L, int options, int xml, int callback,
                             rotmos_error *error)
{
    static const char *const names[] = {
        "schema",        "record",       "value",          "table",
        "max_bytes",     "root_element", "text_key",       "attribute_prefix",
        "trim_text",     "skip_unknown", "strict_unknown", "max_depth",
        "max_text_bytes"};
    struct codec *c;
    lonejson_error cause;
    lonejson_status status;
    size_t i;
    int owner, snapshot, schema, value, record = 0, skip = 1, strict = 0;
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
        if (!xml && i >= 5)
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
        if (!rl_field_string(L, snapshot, "root_element",
                             &c->xml.root_element) ||
            !rl_field_string(L, snapshot, "text_key", &c->xml.text_key) ||
            !rl_field_string(L, snapshot, "attribute_prefix",
                             &c->xml.attribute_prefix) ||
            !rl_field_bool(L, snapshot, "trim_text", &c->xml.trim_text) ||
            !rl_field_bool(L, snapshot, "skip_unknown", &skip) ||
            !rl_field_bool(L, snapshot, "strict_unknown", &strict) ||
            !rl_field_size(L, snapshot, "max_depth", &c->xml.max_depth) ||
            !rl_field_size(L, snapshot, "max_text_bytes",
                           &c->xml.max_text_bytes))
            goto invalid;
        c->xml.skip_unknown_disabled = !skip || strict;
    } else {
        for (i = 5; i < sizeof(names) / sizeof(names[0]); ++i) {
            rl_field(L, options, names[i]);
            if (!lua_isnil(L, -1))
                goto invalid;
            lua_pop(L, 1);
        }
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
    if (!callback && lua_type(L, value) == LUA_TUSERDATA) {
        status =
            lonejson_lua_check_record(L, value, &c->schema, &c->record, &cause);
        record = value;
    } else {
        if (!lua_istable(L, value)) {
            if (callback)
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
static int source(lua_State *L, int xml)
{
    struct codec *c;
    struct rl_source *s;
    rotmos_error error;
    rotmos_status status;
    rotmos_json_source_config json;
    rotmos_xml_source_config config;
    c = prepare(L, 1, xml, 1, &error);
    if (c == NULL)
        return rl_error(L, &error);
    if (c->max_bytes != 0)
        return rl_error(L,
                        (rl_fail(&error, ROTMOS_INVALID,
                                 "max_bytes is for materialized serialization"),
                         &error));
    s = rl_source_native_new(L, -1, codec_close, &error);
    if (s == NULL)
        return rl_error(L, &error);
    if (xml) {
        rotmos_xml_source_config_init(&config);
        config.record = &c->record;
        config.xml = c->xml;
        status = rotmos_source_xml(&config, &s->delegate, &error);
    } else {
        rotmos_json_source_config_init(&json);
        json.record = c->record;
        status = rotmos_source_json(&json, &s->delegate, &error);
    }
    if (status != ROTMOS_OK) {
        lua_pushvalue(L, -1);
        s->root = luaL_ref(L, LUA_REGISTRYINDEX);
        rotmos_source_destroy(s->native, NULL);
        return rl_error(L, &error);
    }
    return 1;
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
static int json_serialize(lua_State *L) { return serialize(L, 0); }
static int xml_serialize(lua_State *L) { return serialize(L, 1); }
static int json_source(lua_State *L) { return source(L, 0); }
static int xml_source(lua_State *L) { return source(L, 1); }
static int json_write(lua_State *L) { return write(L, 0); }
static int xml_write(lua_State *L) { return write(L, 1); }
void rl_codecs_open(lua_State *L)
{
    static const luaL_Reg json[] = {{"serialize", json_serialize},
                                    {"source", json_source},
                                    {"write", json_write},
                                    {NULL, NULL}};
    static const luaL_Reg xml[] = {{"serialize", xml_serialize},
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
    lua_setfield(L, -2, "xml");
}

#include "curl_internal.h"

knurl_status rl_curl_json_config(lua_State *L, struct rl_curl_call *c,
                                 knurl_error *error)
{
    static const char *const names[] = {"request_schema", "response_schema"};
    lonejson_schema_view schema;
    lonejson_record_view record;
    lonejson_error cause;
    lonejson_status status;
    int i, top = lua_gettop(L), index, owner, value;
    for (i = 0; i < 2; ++i) {
        rl_field(L, 1, names[i]);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        index = lua_absindex(L, -1);
        memset(&schema, 0, sizeof(schema));
        schema.size = sizeof(schema);
        schema.abi_version = LONEJSON_VIEW_ABI_VERSION;
        memset(&record, 0, sizeof(record));
        record.size = sizeof(record);
        record.abi_version = LONEJSON_VIEW_ABI_VERSION;
        record.schema.size = sizeof(record.schema);
        record.schema.abi_version = LONEJSON_VIEW_ABI_VERSION;
        lonejson_error_init(&cause);
        status = lonejson_lua_check_schema(L, index, &schema, &cause);
        if (status == LONEJSON_STATUS_OK)
            status = lonejson_lua_new_record(L, index, &owner, &record, &cause);
        if (status != LONEJSON_STATUS_OK)
            return rl_fail(error, KNURL_INVALID, cause.message);
        lua_pushvalue(L, owner);
        c->records[i] = luaL_ref(L, LUA_REGISTRYINDEX);
        if (i == 0) {
            rl_field(L, 1, "body_json");
            value = lua_absindex(L, -1);
            if (!lua_istable(L, value))
                return rl_fail(
                    error, KNURL_INVALID,
                    "streaming JSON upload requires a schema and value table");
            lua_getfield(L, index, "assign");
            lua_pushvalue(L, index);
            lua_pushvalue(L, owner);
            lua_pushvalue(L, value);
            lua_call(L, 3, 1);
            lua_pop(L, 1);
            status = lonejson_curl_upload_init(&c->upload, schema.runtime,
                                               schema.map, record.record);
            if (status != LONEJSON_STATUS_OK)
                return rl_fail(error, KNURL_INVALID,
                               "cannot initialize incremental JSON upload");
            c->json_read = 1;
            if (curl_easy_setopt(c->easy, CURLOPT_SEEKFUNCTION,
                                 lonejson_curl_seek_callback) != CURLE_OK ||
                curl_easy_setopt(c->easy, CURLOPT_SEEKDATA, &c->upload) !=
                    CURLE_OK)
                return rl_fail(error, KNURL_BACKEND,
                               "configuring incremental JSON upload replay");
        } else {
            status = lonejson_curl_parse_init(&c->parser, schema.runtime,
                                              schema.map, record.record);
            if (status != LONEJSON_STATUS_OK)
                return rl_fail(
                    error, KNURL_INVALID,
                    "cannot initialize incremental JSON response parser");
            c->json_write = 1;
        }
        lua_settop(L, top);
    }
    return KNURL_OK;
}
void rl_curl_json_result(lua_State *L, struct rl_curl_call *c)
{
    int result = lua_absindex(L, -1);
    lonejson_status status;
    if (!c->json_write || c->code != CURLE_OK)
        return;
    status = lonejson_curl_parse_finish(&c->parser);
    if (status == LONEJSON_STATUS_OK) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, c->records[1]);
        if (lonejson_lua_record_to_table(L, -1) != 0) {
            lua_pushvalue(L, -1);
            lua_setfield(L, result, "json");
            lua_setfield(L, result, "response_json");
            lua_pop(L, 1);
            return;
        }
        lua_pop(L, 1);
    }
    lua_pushboolean(L, 0);
    lua_setfield(L, result, "ok");
    lua_pushstring(L, c->parser.error.message[0]
                          ? c->parser.error.message
                          : "JSON response conversion failed");
    lua_pushvalue(L, -1);
    lua_setfield(L, result, "json_error");
    lua_setfield(L, result, "error");
}

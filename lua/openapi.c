#include "binding.h"

#define RL_OPENAPI_BYTES "knurl.openapi_bytes"

static int owned_bytes_gc(lua_State *L)
{
    knurl_owned_bytes *bytes = luaL_checkudata(L, 1, RL_OPENAPI_BYTES);
    knurl_owned_bytes_cleanup(bytes);
    return 0;
}

static int generate(lua_State *L)
{
    static const char *const fields[] = {"title", "version", "format",
                                         "max_bytes"};
    struct rl_server *server = luaL_testudata(L, 1, RL_SERVER);
    knurl_openapi_document config;
    knurl_openapi_format format;
    knurl_owned_bytes *out;
    knurl_error error;
    knurl_status status;
    const char *encoding = "json";
    if (server == NULL || server->native == NULL || server->running)
        return rl_result(
            L, rl_fail(&error, KNURL_STATE, "inactive server required"),
            &error);
    knurl_openapi_document_init(&config);
    if (!lua_isnoneornil(L, 2)) {
        if (!rl_fields(L, 2, fields, RL_COUNT(fields), &error))
            return rl_error(L, &error);
        if (!rl_field_string(L, 2, "title", &config.title) ||
            !rl_field_string(L, 2, "version", &config.version) ||
            !rl_field_size(L, 2, "max_bytes", &config.max_bytes) ||
            !rl_field_string(L, 2, "format", &encoding) ||
            (strcmp(encoding, "json") && strcmp(encoding, "yaml")))
            return rl_result(L,
                             rl_fail(&error, KNURL_INVALID,
                                     "invalid OpenAPI document options"),
                             &error);
    }
    format =
        !strcmp(encoding, "json") ? KNURL_OPENAPI_JSON : KNURL_OPENAPI_YAML;
    out = lua_newuserdatauv(L, sizeof(*out), 0);
    knurl_owned_bytes_init(out);
    luaL_setmetatable(L, RL_OPENAPI_BYTES);
    status = knurl_router_openapi(knurl_server_router(server->native), &config,
                                  format, out, &error);
    if (status != KNURL_OK)
        return rl_error(L, &error);
    lua_pushlstring(L, out->data, out->length);
    knurl_owned_bytes_cleanup(out);
    return 1;
}

static int schema(lua_State *L, int index, knurl_openapi_schema *out,
                  int anchors, knurl_error *error)
{
    lonejson_schema_view view;
    if (lua_isnil(L, index))
        return 1;
    if (!rl_schema(L, index, &view, error))
        return 0;
    *out = knurl_openapi_lonejson_schema(NULL, view.map);
    lua_pushvalue(L, index);
    lua_rawseti(L, anchors, (lua_Integer)lua_rawlen(L, anchors) + 1);
    return 1;
}

static int responses(lua_State *L, int index, knurl_openapi_route_doc *doc,
                     int anchors, knurl_error *error)
{
    static const char *const fields[] = {"description", "schema"};
    size_t count = 0, code;
    knurl_openapi_response *response;
    if (lua_isnil(L, index))
        return 1;
    if (!lua_istable(L, index)) {
        rl_fail(error, KNURL_INVALID,
                "responses must map HTTP status codes to declarations");
        return 0;
    }
    lua_pushnil(L);
    while (lua_next(L, index)) {
        if (!lua_isinteger(L, -2) || !rl_size(L, -2, &code) || code < 100 ||
            code > 599 || !lua_istable(L, -1)) {
            rl_fail(error, KNURL_INVALID,
                    "responses must map status codes 100..599 to tables");
            return 0;
        }
        ++count;
        lua_pop(L, 1);
    }
    doc->responses = lua_newuserdatauv(L, count * sizeof(*doc->responses), 0);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        int item = lua_absindex(L, -1);
        response = &doc->responses[doc->response_count++];
        memset(response, 0, sizeof(*response));
        response->status_code = (int)lua_tointeger(L, -2);
        if (!rl_fields(L, item, fields, RL_COUNT(fields), error))
            return 0;
        if (!rl_field_string(L, item, "description", &response->description)) {
            rl_fail(error, KNURL_INVALID,
                    "invalid OpenAPI response description");
            return 0;
        }
        rl_field(L, item, "schema");
        if (!schema(L, -1, &response->schema, anchors, error))
            return 0;
        lua_pop(L, 2);
    }
    return 1;
}

static int annotate(lua_State *L)
{
    static const char *const fields[] = {
        "path",         "method", "methods",        "summary",
        "operation_id", "tags",   "request_schema", "responses"};
    struct rl_server *server = luaL_testudata(L, 1, RL_SERVER);
    knurl_openapi_route_doc doc;
    knurl_http_methods methods;
    const char *path = NULL;
    knurl_error error;
    knurl_status status;
    int anchors, callbacks, ref;
    if (server == NULL || server->native == NULL || server->running)
        return rl_result(
            L, rl_fail(&error, KNURL_STATE, "inactive server required"),
            &error);
    if (!rl_fields(L, 2, fields, RL_COUNT(fields), &error))
        return rl_error(L, &error);
    knurl_openapi_route_doc_init(&doc);
    if (!rl_field_string(L, 2, "path", &path) || path == NULL ||
        !rl_methods(L, 2, KNURL_HTTP_METHODS_GET, &methods) ||
        !rl_field_string(L, 2, "summary", &doc.summary) ||
        !rl_field_string(L, 2, "operation_id", &doc.operation_id))
        return rl_result(
            L, rl_fail(&error, KNURL_INVALID, "invalid OpenAPI operation"),
            &error);
    if (!rl_string_array(L, 2, "tags", &doc.tags, &doc.tag_count, &error))
        return rl_error(L, &error);
    lua_getiuservalue(L, 1, 1);
    callbacks = lua_absindex(L, -1);
    lua_newtable(L);
    anchors = lua_absindex(L, -1);
    rl_field(L, 2, "request_schema");
    if (!schema(L, -1, &doc.request_schema, anchors, &error))
        return rl_error(L, &error);
    rl_field(L, 2, "responses");
    if (!responses(L, lua_absindex(L, -1), &doc, anchors, &error))
        return rl_error(L, &error);
    lua_pushvalue(L, anchors);
    ref = luaL_ref(L, callbacks);
    status = knurl_router_openapi_doc(knurl_server_router(server->native),
                                      methods, path, &doc, &error);
    if (status != KNURL_OK)
        luaL_unref(L, callbacks, ref);
    return rl_result(L, status, &error);
}

void rl_openapi_open(lua_State *L)
{
    lua_pushcfunction(L, generate);
    lua_setfield(L, -2, "openapi");
    lua_pushcfunction(L, annotate);
    lua_setfield(L, -2, "openapi_doc");
    if (luaL_newmetatable(L, RL_OPENAPI_BYTES)) {
        lua_pushcfunction(L, owned_bytes_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
}

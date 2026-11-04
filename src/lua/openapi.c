/* Vectis's route annotations, using public schema views and router metadata. */
#include "binding.h"
#include <rotmos/openapi.h>
#include <lonejson_lua.h>

#define RL_OPENAPI "rotmos.openapi_builder"
struct openapi_builder {
    rotmos_openapi_route_doc doc;
    const char **tags;
    rotmos_owned_bytes bytes;
    int root, committed;
};
static int builder_close(lua_State *L)
{
    struct openapi_builder *b = luaL_testudata(L, 1, RL_OPENAPI);
    if (b == NULL)
        return 0;
    if (b->root != LUA_NOREF && !b->committed) {
        lua_getiuservalue(L, 1, 2);
        lua_getiuservalue(L, -1, 4);
        if (lua_istable(L, -1))
            luaL_unref(L, -1, b->root);
        lua_pop(L, 2);
    }
    b->root = LUA_NOREF;
    rotmos_openapi_route_doc_cleanup(&b->doc);
    rotmos_owned_bytes_cleanup(&b->bytes);
    free(b->tags);
    b->tags = NULL;
    return 0;
}
static struct openapi_builder *builder_new(lua_State *L)
{
    struct openapi_builder *b = lua_newuserdatauv(L, sizeof(*b), 3);
    int owner = lua_absindex(L, -1);
    memset(b, 0, sizeof(*b));
    b->root = LUA_NOREF;
    rotmos_openapi_route_doc_init(&b->doc);
    rotmos_owned_bytes_init(&b->bytes);
    luaL_setmetatable(L, RL_OPENAPI);
    lua_toclose(L, owner);
    lua_newtable(L);
    lua_setiuservalue(L, owner, 1);
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, owner, 2);
    lua_newtable(L);
    lua_setiuservalue(L, owner, 3);
    return b;
}
/* Private snapshots pin strings even if collection runs user finalizers which
 * mutate the original declaration while a later Lua allocation is in flight. */
static int snapshot(lua_State *L, int owner, int index)
{
    int result;
    rl_table_copy(L, index);
    result = lua_absindex(L, -1);
    lua_getiuservalue(L, owner, 3);
    lua_pushvalue(L, result);
    lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
    lua_pop(L, 1);
    return result;
}
static int string_alias(lua_State *L, int index, const char *key,
                        const char *alias, const char **out)
{
    const char *other = NULL;
    if (!rl_field_string(L, index, key, out) ||
        !rl_field_string(L, index, alias, &other))
        return 0;
    if (*out == NULL)
        *out = other;
    return 1;
}
static int schema(lua_State *L, int owner, int index,
                  rotmos_openapi_schema *out, rotmos_error *error)
{
    lonejson_schema_view view;
    lonejson_error cause;
    const char *name = NULL;
    int value;
    if (!string_alias(L, index, "name", "schema_name", &name) || name == NULL ||
        name[0] == 0) {
        rl_fail(error, ROTMOS_INVALID, "OpenAPI schema name is required");
        return 0;
    }
    rl_field(L, index, "schema");
    value = lua_absindex(L, -1);
    memset(&view, 0, sizeof(view));
    view.size = sizeof(view);
    view.abi_version = LONEJSON_VIEW_ABI_VERSION;
    lonejson_error_init(&cause);
    if (lonejson_lua_check_schema(L, value, &view, &cause) !=
        LONEJSON_STATUS_OK) {
        rl_fail(error, ROTMOS_INVALID, cause.message);
        error->source = ROTMOS_ERROR_LONEJSON;
        lua_pop(L, 1);
        return 0;
    }
    lua_getiuservalue(L, owner, 1);
    lua_pushvalue(L, value);
    lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
    lua_pop(L, 2);
    *out = rotmos_openapi_lonejson_schema(name, view.map);
    return 1;
}
static int parse_doc(lua_State *L, int owner, int declaration,
                     struct openapi_builder *b, rotmos_error *error)
{
    size_t count, i, status;
    int index, list;
    rotmos_openapi_response *response;
    if (!rl_field_string(L, declaration, "summary", &b->doc.summary) ||
        !string_alias(L, declaration, "operation_id", "operationId",
                      &b->doc.operation_id))
        goto invalid;
    rl_field(L, declaration, "tags");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1))
            goto invalid;
        list = snapshot(L, owner, -1);
        count = lua_rawlen(L, list);
        if (count > (size_t)-1 / sizeof(*b->tags))
            goto overflow;
        b->tags = count ? calloc(count, sizeof(*b->tags)) : NULL;
        if (count && b->tags == NULL)
            goto nomem;
        b->doc.tags = b->tags;
        b->doc.tag_count = count;
        for (i = 0; i < count; ++i) {
            size_t length;
            lua_rawgeti(L, list, (lua_Integer)i + 1);
            if (lua_type(L, -1) != LUA_TSTRING)
                goto invalid;
            b->tags[i] = lua_tolstring(L, -1, &length);
            if (memchr(b->tags[i], 0, length) != NULL)
                goto invalid;
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    rl_field(L, declaration, "request");
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1))
            goto invalid;
        index = snapshot(L, owner, -1);
        if (!schema(L, owner, index, &b->doc.request_schema, error))
            return 0;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    rl_field(L, declaration, "responses");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 1;
    }
    if (!lua_istable(L, -1))
        goto invalid;
    list = snapshot(L, owner, -1);
    count = lua_rawlen(L, list);
    if (!count)
        goto invalid;
    if (count > (size_t)-1 / sizeof(*response))
        goto overflow;
    b->doc.responses = calloc(count, sizeof(*response));
    if (b->doc.responses == NULL)
        goto nomem;
    b->doc.response_count = b->doc.response_capacity = count;
    for (i = 0; i < count; ++i) {
        lua_rawgeti(L, list, (lua_Integer)i + 1);
        if (!lua_istable(L, -1))
            goto invalid;
        index = snapshot(L, owner, -1);
        status = 0;
        response = &b->doc.responses[i];
        if (!rl_field_size(L, index, "status_code", &status) ||
            !rl_field_size(L, index, "status", &status) || status < 100 ||
            status > 599 ||
            !string_alias(L, index, "description", "desc",
                          &response->description))
            goto invalid;
        response->status_code = (int)status;
        rl_field(L, index, "schema");
        if (!lua_isnil(L, -1) &&
            !schema(L, owner, index, &response->schema, error))
            return 0;
        lua_pop(L, 3);
    }
    lua_pop(L, 2);
    return 1;
invalid:
    rl_fail(error, ROTMOS_INVALID, "invalid OpenAPI route metadata");
    return 0;
overflow:
    rl_fail(error, ROTMOS_OVERFLOW, "OpenAPI metadata vector overflow");
    return 0;
nomem:
    rl_fail(error, ROTMOS_NOMEM, "allocating OpenAPI metadata vector");
    return 0;
}
int rl_openapi_doc(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    struct openapi_builder *b;
    rotmos_http_methods methods;
    rotmos_error error;
    const char *path = NULL;
    int owner, declaration;
    static const char *const fields[] = {
        "path",        "method", "methods", "summary",  "operation_id",
        "operationId", "tags",   "request", "responses"};
    if (s == NULL || s->native == NULL)
        return rl_error(
            L, (rl_fail(&error, ROTMOS_STATE, "server is closed"), &error));
    if (!lua_istable(L, 2))
        return rl_error(L, (rl_fail(&error, ROTMOS_INVALID,
                                    "OpenAPI route declaration required"),
                            &error));
    if (!rl_fields(L, 2, fields, sizeof(fields) / sizeof(fields[0]), &error))
        return rl_error(L, &error);
    b = builder_new(L);
    owner = lua_absindex(L, -1);
    declaration = snapshot(L, owner, 2);
    if (!rl_field_string(L, declaration, "path", &path) || path == NULL ||
        !rl_methods(L, declaration, ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_GET),
                    &methods))
        return rl_error(L, (rl_fail(&error, ROTMOS_INVALID,
                                    "OpenAPI route path and methods required"),
                            &error));
    if (!parse_doc(L, owner, declaration, b, &error))
        return rl_error(L, &error);
    /* Retain public schema owners before the C router borrows their maps. No
     * Lua allocation occurs after a successful native attachment. */
    lua_getiuservalue(L, 1, 4);
    lua_getiuservalue(L, owner, 1);
    b->root = luaL_ref(L, -2);
    lua_pop(L, 1);
    if (rotmos_router_openapi_doc(rotmos_server_router(s->native), methods,
                                  path, &b->doc, &error) != ROTMOS_OK)
        return rl_error(L, &error);
    b->committed = 1;
    lua_pushboolean(L, 1);
    return 1;
}
int rl_openapi(lua_State *L)
{
    struct rl_server *s = rl_server(L, 1);
    struct openapi_builder *b;
    rotmos_openapi_document document;
    rotmos_openapi_format format = ROTMOS_OPENAPI_JSON;
    rotmos_error error;
    const char *name = NULL;
    int owner, declaration;
    static const char *const fields[] = {"title", "version", "format",
                                         "max_bytes"};
    if (s == NULL || s->native == NULL)
        return rl_error(
            L, (rl_fail(&error, ROTMOS_STATE, "server is closed"), &error));
    lua_settop(L, 2);
    if (lua_isnil(L, 2)) {
        lua_newtable(L);
        lua_replace(L, 2);
    }
    if (!lua_istable(L, 2))
        return rl_error(L, (rl_fail(&error, ROTMOS_INVALID,
                                    "OpenAPI document options required"),
                            &error));
    if (!rl_fields(L, 2, fields, sizeof(fields) / sizeof(fields[0]), &error))
        return rl_error(L, &error);
    b = builder_new(L);
    owner = lua_absindex(L, -1);
    declaration = snapshot(L, owner, 2);
    rotmos_openapi_document_init(&document);
    if (!rl_field_string(L, declaration, "title", &document.title) ||
        !rl_field_string(L, declaration, "version", &document.version) ||
        !rl_field_string(L, declaration, "format", &name) ||
        !rl_field_size(L, declaration, "max_bytes", &document.max_bytes))
        goto invalid;
    if (name != NULL) {
        if (!strcmp(name, "yaml") || !strcmp(name, "yml"))
            format = ROTMOS_OPENAPI_YAML;
        else if (strcmp(name, "json"))
            goto invalid;
    }
    if (rotmos_router_openapi(rotmos_server_router(s->native), &document,
                              format, &b->bytes, &error) != ROTMOS_OK)
        return rl_error(L, &error);
    lua_pushlstring(L, b->bytes.data, b->bytes.length);
    return 1;
invalid:
    return rl_error(
        L, (rl_fail(&error, ROTMOS_INVALID, "invalid OpenAPI document options"),
            &error));
}
void rl_openapi_open(lua_State *L)
{
    luaL_newmetatable(L, RL_OPENAPI);
    lua_pushcfunction(L, builder_close);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, builder_close);
    lua_setfield(L, -2, "__close");
    lua_pop(L, 1);
}

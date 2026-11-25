#include "binding.h"

int rl_string_array(lua_State *L, int table, const char *key,
                    const char *const **out, size_t *count, knurl_error *error)
{
    const char **items;
    size_t n, i, length, array_key;
    int list, top = lua_gettop(L);
    rl_field(L, table, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 1;
    }
    if (!lua_istable(L, -1) || (n = lua_rawlen(L, -1)) > 1024)
        goto invalid;
    list = lua_absindex(L, -1);
    lua_pushnil(L);
    while (lua_next(L, list)) {
        if (!lua_isinteger(L, -2) || !rl_size(L, -2, &array_key) ||
            array_key == 0 || array_key > n)
            goto invalid;
        lua_pop(L, 1);
    }
    items = lua_newuserdatauv(L, n * sizeof(*items), 0);
    for (i = 0; i < n; ++i) {
        lua_rawgeti(L, list, (lua_Integer)i + 1);
        if (lua_type(L, -1) != LUA_TSTRING)
            goto invalid;
        items[i] = lua_tolstring(L, -1, &length);
        if (memchr(items[i], 0, length))
            goto invalid;
        lua_pop(L, 1);
    }
    *out = items;
    *count = n;
    return 1;
invalid:
    lua_settop(L, top);
    rl_fail(error, KNURL_INVALID,
            "expected a dense array of at most 1024 strings without NUL");
    snprintf(error->detail, sizeof(error->detail), "%s", key);
    return 0;
}

/* A Lua-owned declaration array stays anchored until Core copies its strings.
 * Validation of extensions/media types belongs to the public C configuration.
 */
int rl_mime_types(lua_State *L, int index, const knurl_mime_type **out,
                  size_t *count, knurl_error *error)
{
    knurl_mime_type *items;
    size_t n = 0, length, i = 0;
    const char *text;
    index = lua_absindex(L, index);
    *out = NULL;
    *count = 0;
    if (lua_isnil(L, index)) {
        lua_pushnil(L);
        return 1;
    }
    if (!lua_istable(L, index))
        goto invalid;
    lua_pushnil(L);
    while (lua_next(L, index)) {
        if (lua_type(L, -2) != LUA_TSTRING || lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 2);
            goto invalid;
        }
        text = lua_tolstring(L, -2, &length);
        if (memchr(text, 0, length) != NULL) {
            lua_pop(L, 2);
            goto invalid;
        }
        text = lua_tolstring(L, -1, &length);
        if (memchr(text, 0, length) != NULL) {
            lua_pop(L, 2);
            goto invalid;
        }
        ++n;
        lua_pop(L, 1);
    }
    if (n > (size_t)-1 / sizeof(*items))
        goto invalid;
    items = lua_newuserdatauv(L, n * sizeof(*items), 0);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        items[i].extension = lua_tostring(L, -2);
        items[i++].content_type = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    *out = items;
    *count = n;
    return 1;
invalid:
    rl_fail(error, KNURL_INVALID,
            "mime_types must map extension strings to Content-Type strings");
    return 0;
}

knurl_status rl_fail(knurl_error *error, knurl_status status,
                     const char *message)
{
    knurl_error_clear(error);
    if (error != NULL) {
        error->status = status;
        error->source = KNURL_ERROR_CORE;
        snprintf(error->operation, sizeof(error->operation), "%s", "lua");
        snprintf(error->message, sizeof(error->message), "%s", message);
    }
    return status;
}
void rl_push_error(lua_State *L, const knurl_error *error)
{
    lua_createtable(L, 0, 6);
    lua_pushinteger(L, error->status);
    lua_setfield(L, -2, "status");
    lua_pushstring(L, knurl_status_string(error->status));
    lua_setfield(L, -2, "kind");
    lua_pushinteger(L, error->source);
    lua_setfield(L, -2, "source");
    lua_pushinteger(L, error->native_code);
    lua_setfield(L, -2, "native_code");
    lua_pushstring(L, error->operation);
    lua_setfield(L, -2, "operation");
    lua_pushstring(L, error->message);
    lua_setfield(L, -2, "message");
    lua_pushstring(L, error->detail);
    lua_setfield(L, -2, "detail");
}
int rl_error(lua_State *L, const knurl_error *error)
{
    lua_pushnil(L);
    rl_push_error(L, error);
    return 2;
}
int rl_result(lua_State *L, knurl_status status, const knurl_error *error)
{
    knurl_error fallback;
    if (status != KNURL_OK) {
        if (error == NULL || error->status != status) {
            rl_fail(&fallback, status, knurl_status_string(status));
            error = &fallback;
        }
        return rl_error(L, error);
    }
    lua_pushboolean(L, 1);
    return 1;
}
void rl_field(lua_State *L, int index, const char *name)
{
    index = lua_absindex(L, index);
    lua_pushstring(L, name);
    lua_rawget(L, index);
}
int rl_fields(lua_State *L, int index, const char *const *fields, size_t count,
              knurl_error *error)
{
    size_t i, length;
    const char *name, *callback;
    index = lua_absindex(L, index);
    knurl_error_clear(error);
    if (!lua_istable(L, index)) {
        rl_fail(error, KNURL_INVALID, "configuration table required");
        return 0;
    }
    lua_pushnil(L);
    while (lua_next(L, index)) {
        name = lua_type(L, -2) == LUA_TSTRING ? lua_tolstring(L, -2, &length)
                                              : NULL;
        for (i = 0; name != NULL && i < count; ++i)
            if (strlen(fields[i]) == length && !memcmp(name, fields[i], length))
                break;
        if (name == NULL || i == count) {
            rl_fail(error, KNURL_INVALID,
                    "unknown or unbound configuration field");
            if (error != NULL && name != NULL)
                snprintf(error->detail, sizeof(error->detail), "%s", name);
            lua_pop(L, 2);
            return 0;
        }
        if (!strcmp(name, "callbacks")) {
            callback = NULL;
            if (lua_istable(L, -1)) {
                lua_pushnil(L);
                if (!lua_next(L, -2)) {
                    lua_pop(L, 1);
                    continue;
                }
                callback =
                    lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "?";
            }
            rl_fail(error, KNURL_INVALID,
                    "request-time Lua callbacks are unavailable");
            snprintf(error->detail, sizeof(error->detail), "%s%s%s", name,
                     callback != NULL ? "." : "",
                     callback != NULL ? callback : "");
            lua_pop(L, callback != NULL ? 4 : 2);
            return 0;
        }
        lua_pop(L, 1);
    }
    return 1;
}
static int rl_integer(lua_State *L, int index, lua_Integer *out)
{
    int valid;
    /* Same exact conversion as Vectis's luaL_checkinteger, returning the
     * binding's ordinary error result instead of throwing across C owners. */
    *out = lua_tointegerx(L, index, &valid);
    return valid;
}
int rl_size(lua_State *L, int index, size_t *out)
{
    lua_Integer value;
    lua_Unsigned n;
    if (!rl_integer(L, index, &value) || value < 0)
        return 0;
    n = (lua_Unsigned)value;
    if (n > (lua_Unsigned)(size_t)-1)
        return 0;
    *out = (size_t)n;
    return 1;
}
int rl_field_size(lua_State *L, int index, const char *name, size_t *out)
{
    int ok;
    rl_field(L, index, name);
    ok = lua_isnil(L, -1) || rl_size(L, -1, out);
    lua_pop(L, 1);
    return ok;
}
int rl_body_limit(lua_State *L, int index, const struct rl_server *server,
                  knurl_u64 *out, knurl_error *error)
{
    size_t limit = 0;
    int order;
    if (!rl_field_size(L, index, "max_body_bytes", &limit) ||
        knurl_u64_from_size(limit, out, error) != KNURL_OK ||
        knurl_u64_compare(*out, server->hard_body_bytes, &order, error) !=
            KNURL_OK ||
        order > 0) {
        rl_fail(error, KNURL_INVALID,
                "max_body_bytes must be within the server body limit");
        return 0;
    }
    return 1;
}
int rl_field_string(lua_State *L, int index, const char *name, const char **out)
{
    int ok = 1;
    size_t length;
    const char *value;
    rl_field(L, index, name);
    if (!lua_isnil(L, -1)) {
        ok = lua_type(L, -1) == LUA_TSTRING;
        if (ok) {
            value = lua_tolstring(L, -1, &length);
            ok = memchr(value, 0, length) == NULL;
            if (ok)
                *out = value;
        }
    }
    lua_pop(L, 1);
    return ok;
}
int rl_field_bool(lua_State *L, int index, const char *name, int *out)
{
    int ok;
    rl_field(L, index, name);
    ok = lua_isnil(L, -1) || lua_isboolean(L, -1);
    if (lua_isboolean(L, -1))
        *out = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return ok;
}
static int rl_method_value(lua_State *L, int index, knurl_http_method *out)
{
    static const char *names[] = {"ANY",      "GET",    "POST", "PUT",
                                  "PATCH",    "DELETE", "HEAD", "OPTIONS",
                                  "PROPFIND", "MKCOL",  "COPY", "MOVE"};
    const char *text;
    size_t i, j, length, value;
    if (lua_type(L, index) == LUA_TNUMBER) {
        if (!rl_size(L, index, &value) || value > KNURL_HTTP_MOVE)
            return 0;
        *out = (knurl_http_method)value;
        return 1;
    }
    if (lua_type(L, index) != LUA_TSTRING)
        return 0;
    text = lua_tolstring(L, index, &length);
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strlen(names[i]) != length)
            continue;
        for (j = 0; j < length; ++j) {
            int c = (unsigned char)text[j];
            if (c >= 'a' && c <= 'z')
                c -= 'a' - 'A';
            if (c != names[i][j])
                break;
        }
        if (j == length) {
            *out = (knurl_http_method)i;
            return 1;
        }
    }
    return 0;
}
int rl_methods(lua_State *L, int index, knurl_http_methods defaults,
               knurl_http_methods *out)
{
    size_t i, n;
    knurl_http_method method;
    index = lua_absindex(L, index);
    *out = defaults;
    rl_field(L, index, "methods");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        rl_field(L, index, "method");
        if (!lua_isnil(L, -1) && !rl_method_value(L, -1, &method))
            return 0;
    } else {
        rl_field(L, index, "method");
        if (!lua_isnil(L, -1))
            return 0;
        lua_pop(L, 1);
    }
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 1;
    }
    *out = 0;
    if (!lua_istable(L, -1)) {
        if (!rl_method_value(L, -1, &method))
            return 0;
        *out = method == KNURL_HTTP_ANY ? KNURL_HTTP_METHODS_ALL
                                        : KNURL_HTTP_METHOD_MASK(method);
    } else {
        size_t key;
        n = lua_rawlen(L, -1);
        if (n == 0)
            return 0;
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (!lua_isinteger(L, -2) || !rl_size(L, -2, &key) || key == 0 ||
                key > n)
                return 0;
            lua_pop(L, 1);
        }
        for (i = 0; i < n; ++i) {
            lua_rawgeti(L, -1, (lua_Integer)i + 1);
            if (!rl_method_value(L, -1, &method))
                return 0;
            *out |= method == KNURL_HTTP_ANY ? KNURL_HTTP_METHODS_ALL
                                             : KNURL_HTTP_METHOD_MASK(method);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return 1;
}
int rl_route_path(lua_State *L, int index, knurl_route_config *config)
{
    const char *kind = NULL;
    if (!rl_field_string(L, index, "path", &config->path) ||
        config->path == NULL || !rl_field_string(L, index, "path_kind", &kind))
        return 0;
    if (config->path[0] == '^')
        config->path_kind = KNURL_PATH_REGEX;
    else if (strchr(config->path, ':') != NULL)
        config->path_kind = KNURL_PATH_PARAMS;
    if (kind == NULL)
        return 1;
    if (!strcmp(kind, "params"))
        config->path_kind = KNURL_PATH_PARAMS;
    else if (!strcmp(kind, "regex"))
        config->path_kind = KNURL_PATH_REGEX;
    else if (!strcmp(kind, "literal"))
        config->path_kind = KNURL_PATH_LITERAL;
    else
        return 0;
    return 1;
}

#include "binding.h"
static char again_key, eof_key;
rotmos_status rl_fail(rotmos_error *error, rotmos_status status,
                      const char *message)
{
    rotmos_error_clear(error);
    if (error != NULL) {
        error->status = status;
        error->source = ROTMOS_ERROR_CORE;
        snprintf(error->operation, sizeof(error->operation), "%s", "lua");
        snprintf(error->message, sizeof(error->message), "%s", message);
    }
    return status;
}
void rl_push_error(lua_State *L, const rotmos_error *error)
{
    lua_createtable(L, 0, 6);
    lua_pushinteger(L, error->status);
    lua_setfield(L, -2, "status");
    lua_pushstring(L, rotmos_status_string(error->status));
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
int rl_error(lua_State *L, const rotmos_error *error)
{
    lua_pushnil(L);
    rl_push_error(L, error);
    return 2;
}
rotmos_status rl_callback_error(lua_State *L, int index, rotmos_error *error,
                                const char *fallback)
{
    rotmos_error local;
    lua_Integer value;
    static const char *names[] = {"operation", "message", "detail"};
    char *destinations[3];
    size_t sizes[3], i;
    if (error == NULL)
        error = &local;
    rl_fail(error, ROTMOS_STATE, fallback);
    index = lua_absindex(L, index);
    if (lua_type(L, index) == LUA_TSTRING) {
        snprintf(error->message, sizeof(error->message), "%s",
                 lua_tostring(L, index));
        return error->status;
    }
    if (!lua_istable(L, index))
        return error->status;
    rl_field(L, index, "status");
    if (lua_isinteger(L, -1)) {
        value = lua_tointeger(L, -1);
        if (value >= ROTMOS_INVALID && value <= ROTMOS_BACKEND)
            error->status = (rotmos_status)value;
    }
    lua_pop(L, 1);
    rl_field(L, index, "source");
    if (lua_isinteger(L, -1)) {
        value = lua_tointeger(L, -1);
        if (value >= ROTMOS_ERROR_NONE && value <= ROTMOS_ERROR_XML)
            error->source = (rotmos_error_source)value;
    }
    lua_pop(L, 1);
    rl_field(L, index, "native_code");
    if (lua_isinteger(L, -1)) {
        value = lua_tointeger(L, -1);
        if (value >= LONG_MIN && value <= LONG_MAX)
            error->native_code = (long)value;
    }
    lua_pop(L, 1);
    destinations[0] = error->operation;
    sizes[0] = sizeof(error->operation);
    destinations[1] = error->message;
    sizes[1] = sizeof(error->message);
    destinations[2] = error->detail;
    sizes[2] = sizeof(error->detail);
    for (i = 0; i < 3; ++i) {
        rl_field(L, index, names[i]);
        if (lua_type(L, -1) == LUA_TSTRING)
            snprintf(destinations[i], sizes[i], "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    return error->status;
}
rotmos_status rl_protect_value(lua_State *L, lua_CFunction function, void *data,
                               int value, rotmos_error *error)
{
    int top = lua_gettop(L), status;
    const char *message;
    if (!lua_checkstack(L, 8))
        return rl_fail(error, ROTMOS_NOMEM,
                       "Lua callback stack allocation failed");
    lua_pushcfunction(L, function);
    lua_pushlightuserdata(L, data);
    if (value != 0)
        lua_pushvalue(L, value);
    status = lua_pcall(L, value != 0 ? 2 : 1, 0, 0);
    if (status != LUA_OK) {
        message = lua_type(L, -1) == LUA_TSTRING
                      ? lua_tostring(L, -1)
                      : "Lua callback failed with a non-string error";
        rl_fail(error, status == LUA_ERRMEM ? ROTMOS_NOMEM : ROTMOS_STATE,
                message);
    }
    lua_settop(L, top);
    return status == LUA_OK       ? ROTMOS_OK
           : status == LUA_ERRMEM ? ROTMOS_NOMEM
                                  : ROTMOS_STATE;
}
rotmos_status rl_protect(lua_State *L, lua_CFunction function, void *data,
                         rotmos_error *error)
{
    return rl_protect_value(L, function, data, 0, error);
}
void rl_table_copy(lua_State *L, int index)
{
    int copy;
    index = lua_absindex(L, index);
    lua_newtable(L);
    copy = lua_absindex(L, -1);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        lua_pushvalue(L, -2);
        lua_pushvalue(L, -2);
        lua_rawset(L, copy);
        lua_pop(L, 1);
    }
}
void rl_field(lua_State *L, int index, const char *name)
{
    index = lua_absindex(L, index);
    lua_pushstring(L, name);
    lua_rawget(L, index);
}
int rl_fields(lua_State *L, int index, const char *const *fields, size_t count,
              rotmos_error *error)
{
    size_t i;
    const char *name;
    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        name = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : NULL;
        for (i = 0; name != NULL && i < count; ++i)
            if (!strcmp(name, fields[i]))
                break;
        if (name == NULL || i == count) {
            rl_fail(error, ROTMOS_INVALID,
                    "unknown or unbound configuration field");
            if (error != NULL && name != NULL)
                snprintf(error->detail, sizeof(error->detail), "%s", name);
            lua_pop(L, 2);
            return 0;
        }
        lua_pop(L, 1);
    }
    return 1;
}
int rl_size(lua_State *L, int index, size_t *out)
{
    lua_Unsigned n;
    if (!lua_isinteger(L, index) || lua_tointeger(L, index) < 0)
        return 0;
    n = (lua_Unsigned)lua_tointeger(L, index);
    if (n > (lua_Unsigned)(size_t)-1)
        return 0;
    *out = (size_t)n;
    return 1;
}
int rl_u64(lua_State *L, int index, rotmos_u64 *out)
{
    rotmos_u64 *value = luaL_testudata(L, index, RL_COUNT);
    lua_Unsigned n;
    if (value != NULL) {
        *out = *value;
        return 1;
    }
    if (!lua_isinteger(L, index) || lua_tointeger(L, index) < 0)
        return 0;
    n = (lua_Unsigned)lua_tointeger(L, index);
    out->high = (unsigned long)(n >> 32);
    out->low = (unsigned long)(n & 0xffffffffUL);
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
void rl_push_u64(lua_State *L, rotmos_u64 value)
{
    rotmos_u64 *out;
    if (value.high <= 0x7fffffffUL) {
        lua_Unsigned n =
            ((lua_Unsigned)value.high << 32) | (lua_Unsigned)value.low;
        lua_pushinteger(L, (lua_Integer)n);
        return;
    }
    out = lua_newuserdatauv(L, sizeof(*out), 0);
    *out = value;
    luaL_setmetatable(L, RL_COUNT);
}
void rl_push_size(lua_State *L, size_t value)
{
    rotmos_u64 out;
    (void)rotmos_u64_from_size(value, &out, NULL);
    rl_push_u64(L, out);
}
static int count_string(lua_State *L)
{
    rotmos_u64 *value = luaL_testudata(L, 1, RL_COUNT);
    char bytes[21];
    size_t n;
    if (value == NULL) {
        lua_pushnil(L);
        return 1;
    }
    rotmos_u64_format_decimal(*value, bytes, sizeof(bytes), &n, NULL);
    lua_pushlstring(L, bytes, n);
    return 1;
}
static int count_index(lua_State *L)
{
    rotmos_u64 *value = luaL_testudata(L, 1, RL_COUNT);
    const char *key = lua_tostring(L, 2);
    if (value != NULL && key != NULL && !strcmp(key, "high"))
        lua_pushinteger(L, (lua_Integer)value->high);
    else if (value != NULL && key != NULL && !strcmp(key, "low"))
        lua_pushinteger(L, (lua_Integer)value->low);
    else
        lua_pushnil(L);
    return 1;
}
static int count_new(lua_State *L)
{
    rotmos_u64 value;
    rotmos_error error;
    rotmos_status status;
    size_t a, b;
    const char *text;
    if (lua_type(L, 1) == LUA_TSTRING) {
        text = lua_tolstring(L, 1, &a);
        status = rotmos_u64_from_decimal(text, a, &value, &error);
    } else if (rl_size(L, 1, &a) && rl_size(L, 2, &b) && a <= 0xffffffffUL &&
               b <= 0xffffffffUL)
        status = rotmos_u64_from_words((unsigned long)a, (unsigned long)b,
                                       &value, &error);
    else
        status = rl_fail(&error, ROTMOS_INVALID,
                         "count requires decimal text or high/low words");
    if (status != ROTMOS_OK)
        return rl_error(L, &error);
    rl_push_u64(L, value);
    return 1;
}
static int count_compare(lua_State *L, int operation)
{
    rotmos_u64 a, b;
    rotmos_error error;
    int comparison;
    if (!rl_u64(L, 1, &a) || !rl_u64(L, 2, &b))
        return rl_error(L, (rl_fail(&error, ROTMOS_INVALID,
                                    "exact unsigned counts required"),
                            &error));
    if (rotmos_u64_compare(a, b, &comparison, &error) != ROTMOS_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, operation == 0   ? comparison == 0
                       : operation == 1 ? comparison < 0
                                        : comparison <= 0);
    return 1;
}
static int count_equal(lua_State *L) { return count_compare(L, 0); }
static int count_less(lua_State *L) { return count_compare(L, 1); }
static int count_less_equal(lua_State *L) { return count_compare(L, 2); }
void rl_sentinel(lua_State *L, int code)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX,
                code == ROTMOS_AGAIN ? (void *)&again_key : (void *)&eof_key);
}
int rl_is_sentinel(lua_State *L, int index, int code)
{
    int yes;
    index = lua_absindex(L, index);
    rl_sentinel(L, code);
    yes = lua_rawequal(L, index, -1);
    lua_pop(L, 1);
    return yes;
}
void rl_values_open(lua_State *L)
{
    luaL_newmetatable(L, RL_COUNT);
    lua_pushcfunction(L, count_string);
    lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, count_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, count_equal);
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, count_less);
    lua_setfield(L, -2, "__lt");
    lua_pushcfunction(L, count_less_equal);
    lua_setfield(L, -2, "__le");
    lua_pop(L, 1);
    lua_pushcfunction(L, count_new);
    lua_setfield(L, -2, "count");
    lua_rawgetp(L, LUA_REGISTRYINDEX, &again_key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newuserdatauv(L, 1, 0);
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, &again_key);
    }
    lua_setfield(L, -2, "AGAIN");
    lua_rawgetp(L, LUA_REGISTRYINDEX, &eof_key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newuserdatauv(L, 1, 0);
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, &eof_key);
    }
    lua_setfield(L, -2, "EOF");
}

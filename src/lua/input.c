#include "binding.h"
#include <knurl/input.h>
#include <limits.h>

static int failure(lua_State *L, knurl_error *error, const char *message)
{
    (void)rl_fail(error, KNURL_INVALID, message);
    return rl_error(L, error);
}
static int bytes(lua_State *L, int index, knurl_bytes *out)
{
    if (lua_type(L, index) != LUA_TSTRING)
        return 0;
    out->data = lua_tolstring(L, index, &out->length);
    return 1;
}
static int long_arg(lua_State *L, int index, long fallback, long *out)
{
    lua_Integer value;
    *out = fallback;
    if (lua_isnoneornil(L, index))
        return 1;
    if (!rl_integer(L, index, &value) || value < (lua_Integer)LONG_MIN ||
        value > (lua_Integer)LONG_MAX)
        return 0;
    *out = (long)value;
    return 1;
}
static int count_arg(lua_State *L, int index, knurl_u64 fallback,
                     knurl_u64 *out)
{
    *out = fallback;
    return lua_isnoneornil(L, index) || rl_u64(L, index, out);
}
static int size_arg(lua_State *L, int index, size_t fallback, size_t *out)
{
    *out = fallback;
    return lua_isnoneornil(L, index) || rl_size(L, index, out);
}
static int numeric(lua_State *L, int kind, int request, int path)
{
    knurl_bytes input;
    struct rl_request *r = NULL;
    knurl_error error;
    knurl_status status;
    knurl_u64 zero = {0, 0}, maximum = {0xffffffffUL, 0xffffffffUL};
    knurl_u64 low, high, result;
    long minimum, limit, integer;
    int boolean;
    int start = request ? 3 : 2;
    const char *name = NULL;
    if (!bytes(L, request ? 2 : 1, &input))
        return failure(L, &error,
                       request ? "parameter name must be a string"
                               : "input must be a string");
    if (request) {
        if (memchr(input.data, 0, input.length) != NULL)
            return failure(L, &error, "parameter name contains NUL");
        if (!rl_request_check(L, 1, &r, &error))
            return rl_error(L, &error);
        name = input.data;
    }
    if (kind == 0) {
        if (!long_arg(L, start, LONG_MIN, &minimum) ||
            !long_arg(L, start + 1, LONG_MAX, &limit))
            return failure(L, &error, "bounds must fit C long exactly");
        status =
            request
                ? (path ? knurl_request_param_long : knurl_request_query_long)(
                      r->native, name, minimum, limit, &integer, &error)
                : knurl_input_long(input, minimum, limit, &integer, &error);
        if (status == KNURL_OK)
            lua_pushinteger(L, (lua_Integer)integer);
    } else if (kind == 1) {
        if (!count_arg(L, start, zero, &low) ||
            !count_arg(L, start + 1, maximum, &high))
            return failure(L, &error, "bounds must be exact unsigned counts");
        status = request ? (path ? knurl_request_param_u64
                                 : knurl_request_query_u64)(
                               r->native, name, low, high, &result, &error)
                         : knurl_input_u64(input, low, high, &result, &error);
        if (status == KNURL_OK)
            rl_push_u64(L, result);
    } else {
        status = request ? (path ? knurl_request_param_boolean
                                 : knurl_request_query_boolean)(
                               r->native, name, &boolean, &error)
                         : knurl_input_boolean(input, &boolean, &error);
        if (status == KNURL_OK)
            lua_pushboolean(L, boolean);
    }
    if (status == KNURL_EOF) {
        (void)rl_fail(&error, KNURL_EOF, "parameter is absent");
        return rl_error(L, &error);
    }
    return status == KNURL_OK ? 1 : rl_error(L, &error);
}
static int owned_close(lua_State *L)
{
    knurl_owned_bytes *owned = lua_touserdata(L, 1);
    if (owned != NULL)
        knurl_owned_bytes_cleanup(owned);
    return 0;
}
static int input_call(lua_State *L)
{
    int kind = (int)lua_tointeger(L, lua_upvalueindex(1));
    knurl_bytes input, allowed, *choices;
    knurl_owned_bytes *owned;
    knurl_error error;
    knurl_status status;
    size_t minimum, maximum, count, i;
    int controls;
    if (kind < 3)
        return numeric(L, kind, 0, 0);
    if (!bytes(L, 1, &input))
        return failure(L, &error, "input must be a string");
    if (kind == 3 || kind == 4) {
        if (!size_arg(L, kind == 3 ? 2 : 3, 0, &minimum) ||
            !size_arg(L, kind == 3 ? 3 : 4, 1048576UL, &maximum))
            return failure(L, &error,
                           "byte bounds must be nonnegative exact sizes");
        if (kind == 3) {
            if (!lua_isnoneornil(L, 4) && lua_type(L, 4) != LUA_TBOOLEAN)
                return failure(L, &error, "reject_controls must be a boolean");
            controls = lua_toboolean(L, 4);
            status =
                knurl_input_text(input, minimum, maximum, controls, &error);
        } else {
            if (!bytes(L, 2, &allowed))
                return failure(L, &error, "allowlist must be a string");
            status =
                knurl_input_allowlist(input, allowed, minimum, maximum, &error);
        }
    } else if (kind == 5) {
        if (!lua_istable(L, 2))
            return failure(L, &error, "choices must be a string array");
        count = lua_rawlen(L, 2);
        if (count > (size_t)-1 / sizeof(*choices))
            return failure(L, &error, "choice array is too large");
        choices = lua_newuserdatauv(L, count * sizeof(*choices), 0);
        for (i = 0; i < count; ++i) {
            lua_rawgeti(L, 2, (lua_Integer)i + 1);
            if (!bytes(L, -1, &choices[i]))
                return failure(L, &error, "every choice must be a string");
            lua_pop(L, 1);
        }
        status = knurl_input_choice(input, choices, count, &error);
    } else if (kind == 6)
        status = knurl_input_email(input, &error);
    else if (kind == 7)
        status = knurl_input_http_url(input, &error);
    else if (kind == 8)
        status = knurl_input_filename(input, &error);
    else {
        if (!size_arg(L, 2, 0, &maximum))
            return failure(L, &error, "output bound must be an exact size");
        owned = lua_newuserdatauv(L, sizeof(*owned), 0);
        knurl_owned_bytes_init(owned);
        luaL_setmetatable(L, "knurl.input.bytes");
        status = knurl_input_html_escape(input, maximum, owned, &error);
        if (status != KNURL_OK)
            return rl_error(L, &error);
        lua_pushlstring(L, owned->data, owned->length);
        knurl_owned_bytes_cleanup(owned);
        return 1;
    }
    if (status != KNURL_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    return 1;
}
static int request_call(lua_State *L)
{
    return numeric(L, (int)lua_tointeger(L, lua_upvalueindex(1)), 1,
                   (int)lua_tointeger(L, lua_upvalueindex(2)));
}
void rl_input_requests_open(lua_State *L)
{
    static const char *const names[] = {"query_long",    "query_u64",
                                        "query_boolean", "param_long",
                                        "param_u64",     "param_boolean"};
    int i;
    for (i = 0; i < 6; ++i) {
        lua_pushinteger(L, i % 3);
        lua_pushinteger(L, i / 3);
        lua_pushcclosure(L, request_call, 2);
        lua_setfield(L, -2, names[i]);
    }
}
void rl_input_open(lua_State *L)
{
    static const char *const names[] = {
        "parse_long", "parse_u64", "boolean",  "text",     "allowlist",
        "choice",     "email",     "http_url", "filename", "html_escape"};
    int i;
    luaL_newmetatable(L, "knurl.input.bytes");
    lua_pushcfunction(L, owned_close);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    lua_newtable(L);
    for (i = 0; i < 10; ++i) {
        lua_pushinteger(L, i);
        lua_pushcclosure(L, input_call, 1);
        lua_setfield(L, -2, names[i]);
    }
    lua_setfield(L, -2, "input");
}

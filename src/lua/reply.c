#include "binding.h"
void rl_reply_view_push(lua_State *L, struct rl_call *call)
{
    struct rl_reply_view *view = lua_newuserdatauv(L, sizeof(*view), 0);
    view->native = call->reply;
    view->request = call->native;
    luaL_setmetatable(L, RL_REPLY);
    /* Request rooting survives an exception/OOM inside the after callback.
     * The outer native frame can always invalidate this borrowed view. */
    rl_request_push(L, call->request);
    lua_pushvalue(L, -2);
    lua_setiuservalue(L, -2, 6);
    lua_pop(L, 1);
    call->reply_view = view;
}
static struct rl_reply_view *check(lua_State *L, knurl_error *error)
{
    struct rl_reply_view *view = luaL_testudata(L, 1, RL_REPLY);
    if (view == NULL || view->native == NULL) {
        rl_fail(error, KNURL_STATE, "reply callback scope has closed");
        return NULL;
    }
    return view;
}
static int snapshot(lua_State *L)
{
    static const char *const kinds[] = {"empty", "bytes", "file", "source"};
    struct rl_reply_view *view;
    knurl_error error;
    knurl_bytes bytes;
    knurl_header header;
    size_t i, n;
    view = check(L, &error);
    if (view == NULL)
        return rl_error(L, &error);
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, knurl_reply_status_code(view->native));
    lua_setfield(L, -2, "status_code");
    lua_pushstring(L, kinds[knurl_reply_payload_kind(view->native)]);
    lua_setfield(L, -2, "kind");
    bytes = knurl_reply_content_type(view->native);
    if (bytes.data != NULL) {
        lua_pushlstring(L, bytes.data, bytes.length);
        lua_setfield(L, -2, "content_type");
    }
    if (knurl_reply_payload_kind(view->native) == KNURL_REPLY_BYTES) {
        bytes = knurl_reply_body(view->native);
        lua_pushlstring(L, bytes.data == NULL ? "" : bytes.data, bytes.length);
        lua_setfield(L, -2, "body");
    }
    n = knurl_reply_header_count(view->native);
    lua_createtable(L, (int)n, 0);
    for (i = 0; i < n; ++i) {
        knurl_reply_header_at(view->native, i, &header, NULL);
        lua_createtable(L, 0, 2);
        lua_pushlstring(L, header.name.data, header.name.length);
        lua_setfield(L, -2, "name");
        lua_pushlstring(L, header.value.data, header.value.length);
        lua_setfield(L, -2, "value");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "headers");
    return 1;
}
static int replace(lua_State *L)
{
    struct rl_reply_view *view;
    knurl_error error;
    view = check(L, &error);
    if (view == NULL ||
        rl_reply(L, 2, view->native, view->request, &error) != KNURL_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    return 1;
}
static int header_edit(lua_State *L, int add)
{
    struct rl_reply_view *view;
    knurl_error error;
    knurl_status status;
    const char *name, *value;
    size_t n, m;
    view = check(L, &error);
    if (view == NULL)
        return rl_error(L, &error);
    if (lua_type(L, 2) != LUA_TSTRING || lua_type(L, 3) != LUA_TSTRING)
        goto invalid;
    name = lua_tolstring(L, 2, &n);
    value = lua_tolstring(L, 3, &m);
    if (memchr(name, 0, n) || memchr(value, 0, m))
        goto invalid;
    status = add ? knurl_reply_header_add(view->native, name, value, &error)
                 : knurl_reply_header_set(view->native, name, value, &error);
    if (status != KNURL_OK)
        return rl_error(L, &error);
    lua_pushboolean(L, 1);
    return 1;
invalid:
    return rl_error(L, (rl_fail(&error, KNURL_INVALID,
                                "header name/value must be NUL-free strings"),
                        &error));
}
static int header_add(lua_State *L) { return header_edit(L, 1); }
static int header_set(lua_State *L) { return header_edit(L, 0); }
void rl_replies_open(lua_State *L)
{
    static const luaL_Reg methods[] = {{"snapshot", snapshot},
                                       {"replace", replace},
                                       {"header_add", header_add},
                                       {"header_set", header_set},
                                       {NULL, NULL}};
    luaL_newmetatable(L, RL_REPLY);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, methods, 0);
    lua_pop(L, 1);
}

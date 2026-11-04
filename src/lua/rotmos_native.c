#include "binding.h"
#include "facade.inc"
static char module_key, facade_key;
static int facade_loader(lua_State *L)
{
    if (luaL_loadbufferx(L, (const char *)rotmos_facade, sizeof(rotmos_facade),
                         "@rotmos.lua", "t") != LUA_OK)
        return lua_error(L);
    lua_call(L, 0, 1);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &facade_key);
    return 1;
}
int luaopen_rotmos_native(lua_State *L)
{
    static const char *names[] = {"ANY",      "GET",    "POST", "PUT",
                                  "PATCH",    "DELETE", "HEAD", "OPTIONS",
                                  "PROPFIND", "MKCOL",  "COPY", "MOVE"};
    int i;
    static const char *statuses[] = {
        "OK",    "AGAIN",         "EOF",       "INVALID",  "NOMEM",
        "STATE", "CONFLICT",      "LIMIT",     "OVERFLOW", "TIMEOUT",
        "IO",    "NOT_SUPPORTED", "CANCELLED", "BACKEND"};
    luaL_checkversion(L);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &module_key);
    if (!lua_isnil(L, -1))
        return 1;
    lua_pop(L, 1);
    lua_newtable(L);
    rl_values_open(L);
    rl_servers_open(L);
    rl_requests_open(L);
    rl_sources_open(L);
    rl_codecs_open(L);
    rl_rest_open(L);
    rl_openapi_open(L);
    rl_policy_open(L);
    rl_bodies_open(L);
    rl_replies_open(L);
    rl_loops_open(L);
    rl_websockets_open(L);
    lua_pushstring(L, rotmos_version());
    lua_setfield(L, -2, "version");
    lua_newtable(L);
    for (i = 0; i < 12; ++i) {
        lua_pushinteger(L, i);
        lua_setfield(L, -2, names[i]);
    }
    lua_setfield(L, -2, "methods");
    lua_newtable(L);
    for (i = 0; i < (int)(sizeof(statuses) / sizeof(statuses[0])); ++i) {
        lua_pushinteger(L, i);
        lua_setfield(L, -2, statuses[i]);
    }
    lua_setfield(L, -2, "status");
    lua_pushinteger(L, ROTMOS_BODY_NONE);
    lua_setfield(L, -2, "BODY_NONE");
    lua_pushinteger(L, ROTMOS_BODY_BUFFERED);
    lua_setfield(L, -2, "BODY_BUFFERED");
    lua_pushinteger(L, ROTMOS_BODY_MATERIALIZED);
    lua_setfield(L, -2, "BODY_MATERIALIZED");
    lua_pushinteger(L, ROTMOS_BODY_STREAMING_UPLOAD);
    lua_setfield(L, -2, "BODY_STREAMING_UPLOAD");
    lua_pushinteger(L, ROTMOS_BODY_STREAM);
    lua_setfield(L, -2, "BODY_STREAM");
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &module_key);
    return 1;
}
struct preload {
    rotmos_status status;
    rotmos_error *error;
};
static int preload_call(lua_State *L)
{
    struct preload *p = lua_touserdata(L, 1);
    int package, preload, loaded, i;
    const char *names[] = {"rotmos.native", "rotmos"};
    lua_CFunction loaders[] = {luaopen_rotmos_native, facade_loader};
    void *keys[] = {&module_key, &facade_key};
    lua_getglobal(L, "package");
    if (!lua_istable(L, -1)) {
        p->status = rl_fail(p->error, ROTMOS_STATE,
                            "rotmos preload requires the host package library");
        return 0;
    }
    package = lua_absindex(L, -1);
    lua_getfield(L, package, "preload");
    if (!lua_istable(L, -1)) {
        p->status = rl_fail(p->error, ROTMOS_STATE,
                            "package.preload table is required");
        return 0;
    }
    preload = lua_absindex(L, -1);
    lua_getfield(L, package, "loaded");
    if (!lua_istable(L, -1)) {
        p->status =
            rl_fail(p->error, ROTMOS_STATE, "package.loaded table is required");
        return 0;
    }
    loaded = lua_absindex(L, -1);
    for (i = 0; i < 2; ++i) {
        lua_getfield(L, preload, names[i]);
        if (!lua_isnil(L, -1) && lua_tocfunction(L, -1) != loaders[i]) {
            p->status = rl_fail(
                p->error, ROTMOS_CONFLICT,
                "rotmos loader conflicts with an existing registration");
            return 0;
        }
        lua_pop(L, 1);
        lua_getfield(L, loaded, names[i]);
        if (!lua_isnil(L, -1)) {
            lua_rawgetp(L, LUA_REGISTRYINDEX, keys[i]);
            if (!lua_rawequal(L, -1, -2)) {
                p->status = rl_fail(
                    p->error, ROTMOS_CONFLICT,
                    "rotmos module conflicts with an existing loaded module");
                return 0;
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    {
        static const char *base[] = {"type",         "pairs",    "ipairs",
                                     "setmetatable", "tostring", "error"};
        for (i = 0; i < (int)(sizeof(base) / sizeof(base[0])); ++i) {
            lua_getglobal(L, base[i]);
            if (!lua_isfunction(L, -1)) {
                p->status =
                    rl_fail(p->error, ROTMOS_STATE,
                            "rotmos facade requires the host base library");
                return 0;
            }
            lua_pop(L, 1);
        }
    }
    lua_getglobal(L, "string");
    if (!lua_istable(L, -1)) {
        p->status = rl_fail(p->error, ROTMOS_STATE,
                            "rotmos facade requires the host string library");
        return 0;
    }
    {
        static const char *functions[] = {"lower", "sub"};
        for (i = 0; i < 2; ++i) {
            lua_getfield(L, -1, functions[i]);
            if (!lua_isfunction(L, -1)) {
                p->status = rl_fail(
                    p->error, ROTMOS_STATE,
                    "rotmos facade requires string.lower and string.sub");
                return 0;
            }
            lua_pop(L, 1);
        }
    }
    for (i = 0; i < 2; ++i) {
        lua_pushcfunction(L, loaders[i]);
        lua_setfield(L, preload, names[i]);
    }
    return 0;
}
rotmos_status rotmos_lua_preload(lua_State *L, rotmos_error *error)
{
    struct preload context;
    rotmos_error local;
    rotmos_status status;
    if (error == NULL)
        error = &local;
    if (L == NULL)
        return rl_fail(error, ROTMOS_INVALID, "host Lua state is required");
    context.status = ROTMOS_OK;
    context.error = error;
    status = rl_protect(L, preload_call, &context, error);
    if (status == ROTMOS_OK)
        status = context.status;
    if (status == ROTMOS_OK)
        rotmos_error_clear(error);
    return status;
}

#include "binding.h"
#define RL_ASSET_PROVIDER "knurl.asset_provider"
#define RL_STORAGE_PROVIDER "knurl.storage_provider"
struct provider_value {
    void *native;
};
struct provider_call {
    void *native;
    const char *kind;
};
static int push_provider(lua_State *L)
{
    struct provider_call *call = lua_touserdata(L, 1);
    struct provider_value *value;
    (void)luaopen_knurl_native(L);
    luaL_newmetatable(L, call->kind);
    lua_pushliteral(L, "host-owned knurl provider");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
    value = lua_newuserdatauv(L, sizeof(*value), 1);
    value->native = call->native;
    luaL_setmetatable(L, call->kind);
    lua_pushvalue(L, 2);
    lua_setiuservalue(L, -2, 1);
    return 1;
}
static knurl_status provider(lua_State *L, void *native, const char *kind,
                             int owner, knurl_error *error)
{
    struct provider_call call;
    int top, result;
    const char *message;
    if (L == NULL || native == NULL ||
        (owner != 0 && lua_type(L, owner) == LUA_TNONE))
        return rl_fail(error, KNURL_INVALID,
                       "provide a VM, provider and valid optional owner");
    if (owner != 0)
        owner = lua_absindex(L, owner);
    top = lua_gettop(L);
    if (!lua_checkstack(L, 4))
        return rl_fail(error, KNURL_NOMEM,
                       "reserving provider conversion stack");
    call.native = native;
    call.kind = kind;
    lua_pushcfunction(L, push_provider);
    lua_pushlightuserdata(L, &call);
    if (owner != 0)
        lua_pushvalue(L, owner);
    else
        lua_pushnil(L);
    result = lua_pcall(L, 2, 1, 0);
    if (result != LUA_OK) {
        message = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1)
                                                 : "provider conversion failed";
        rl_fail(error, result == LUA_ERRMEM ? KNURL_NOMEM : KNURL_STATE,
                message);
        lua_settop(L, top);
        return result == LUA_ERRMEM ? KNURL_NOMEM : KNURL_STATE;
    }
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_lua_push_asset_provider(lua_State *L,
                                           knurl_asset_provider *native,
                                           int owner, knurl_error *error)
{
    return provider(L, native, RL_ASSET_PROVIDER, owner, error);
}
knurl_status knurl_lua_push_storage_provider(lua_State *L,
                                             knurl_storage_provider *native,
                                             int owner, knurl_error *error)
{
    return provider(L, native, RL_STORAGE_PROVIDER, owner, error);
}
knurl_asset_provider *rl_asset_provider(lua_State *L, int index)
{
    struct provider_value *value = luaL_testudata(L, index, RL_ASSET_PROVIDER);
    return value != NULL ? value->native : NULL;
}
knurl_storage_provider *rl_storage_provider(lua_State *L, int index)
{
    struct provider_value *value =
        luaL_testudata(L, index, RL_STORAGE_PROVIDER);
    return value != NULL ? value->native : NULL;
}

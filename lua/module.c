#include "binding.h"

static int load_smtp(lua_State *L)
{
    const char *key;
    size_t length;
    if (lua_type(L, 2) != LUA_TSTRING)
        return 0;
    key = lua_tolstring(L, 2, &length);
    if (length != 4 || memcmp(key, "smtp", 4) != 0)
        return 0;
    lua_getglobal(L, "require");
    lua_pushliteral(L, "knurl.smtp");
    lua_call(L, 1, 1);
    lua_pushvalue(L, 2);
    lua_pushvalue(L, -2);
    lua_rawset(L, 1);
    return 1;
}

int luaopen_knurl(lua_State *L)
{
    luaL_checkversion(L);
    lua_newtable(L);
    rl_server_open(L);
    rl_acl_open(L);
    rl_curl_core(L);
    lua_setfield(L, -2, "curl");
    lua_newtable(L);
    lua_pushcfunction(L, load_smtp);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    return 1;
}

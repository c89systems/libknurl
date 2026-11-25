#include "binding.h"

int luaopen_knurl(lua_State *L)
{
    luaL_checkversion(L);
    lua_newtable(L);
    rl_server_open(L);
    rl_acl_open(L);
    rl_curl_core(L);
    lua_setfield(L, -2, "curl");
    return 1;
}

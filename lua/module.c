#include "binding.h"

int luaopen_knurl(lua_State *L)
{
    lua_newtable(L);
    rl_server_open(L);
    rl_acl_open(L);
    return 1;
}

#define _POSIX_C_SOURCE 200809L
#include <knurl/lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <lonejson_lua.h>
#include <pslog_lua.h>
#include <pid0/pid0.h>
#include <stdio.h>
#include <string.h>
#include "examples.h"
int luaopen_lonejson_core(lua_State *L);
int luaopen_pslog_core(lua_State *L);

static int json_loader(lua_State *L)
{
#include "lonejson_facade.inc"
    if (luaL_loadbufferx(L, (const char *)knurl_facade, sizeof(knurl_facade),
                         "@embedded/lonejson.lua", "t") != LUA_OK)
        return lua_error(L);
    lua_call(L, 0, 1);
    return 1;
}
static int log_loader(lua_State *L)
{
#include "pslog_facade.inc"
    if (luaL_loadbufferx(L, (const char *)knurl_facade, sizeof(knurl_facade),
                         "@embedded/pslog.lua", "t") != LUA_OK)
        return lua_error(L);
    lua_call(L, 0, 1);
    return 1;
}
#define knurl_facade startup
#include "startup.inc"
#undef knurl_facade
struct invocation {
    const char *file;
    int check;
    char **argv;
};
static int execute(lua_State *L)
{
    struct invocation *call = lua_touserdata(L, 1);
    knurl_error error;
    luaL_openlibs(L);
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    lua_pushcfunction(L, luaopen_lonejson_core);
    lua_setfield(L, -2, "lonejson.core");
    lua_pushcfunction(L, json_loader);
    lua_setfield(L, -2, "lonejson");
    lua_pushcfunction(L, luaopen_pslog_core);
    lua_setfield(L, -2, "pslog.core");
    lua_pushcfunction(L, log_loader);
    lua_setfield(L, -2, "pslog");
    lua_pop(L, 2);
    if (knurl_lua_preload(L, &error) != KNURL_OK)
        return luaL_error(L, "%s", error.message);
    lua_createtable(L, 0, 2);
    lua_pushstring(L, call->file);
    lua_rawseti(L, -2, 0);
    lua_pushstring(L, call->argv[0]);
    lua_rawseti(L, -2, -1);
    lua_setglobal(L, "arg");
    if (luaL_loadbufferx(L, (const char *)startup, sizeof(startup),
                         "@embedded/startup.lua", "t") != LUA_OK)
        return lua_error(L);
    lua_pushstring(L, call->file);
    lua_pushboolean(L, call->check);
    lua_call(L, 2, 0);
    return 0;
}
static void usage(FILE *out)
{
    fputs(
        "Usage: knurl FILE.lua\n"
        "       knurl --check FILE.lua\n"
        "       knurl --dump-examples [DIRECTORY]\n"
        "       knurl --help | --version\n\n"
        "Run a returned Lua server in the foreground. --check executes the\n"
        "configuration and validates declarations without starting listeners.\n"
        "--dump-examples writes four examples without replacing existing "
        "files.\n",
        out);
}
static int submain(int argc, char **argv)
{
    struct invocation call;
    lua_State *L;
    int status;
    memset(&call, 0, sizeof(call));
    call.argv = argv;
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("knurl " KNURL_EXECUTABLE_VERSION);
        return 0;
    }
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "--dump-examples") == 0)
        return knurl_dump_examples(argc == 3 ? argv[2] : ".");
    if (argc == 3 && strcmp(argv[1], "--check") == 0) {
        call.check = 1;
        call.file = argv[2];
    } else if (argc == 2 && argv[1][0] != '-')
        call.file = argv[1];
    else {
        usage(stderr);
        return 2;
    }
    L = luaL_newstate();
    if (L == NULL) {
        fputs("knurl: unable to allocate Lua runtime\n", stderr);
        return 1;
    }
    lua_pushcfunction(L, execute);
    lua_pushlightuserdata(L, &call);
    status = lua_pcall(L, 1, 0, 0);
    if (status != LUA_OK)
        fprintf(stderr, "knurl: %s\n",
                lua_tostring(L, -1) ? lua_tostring(L, -1)
                                    : "Lua execution failed");
    lua_close(L);
    return status == LUA_OK ? 0 : 1;
}
int main(int argc, char **argv) { return pid0_run(submain, argc, argv); }

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <knurl/common.h>
#include <stdio.h>
#include <string.h>

int luaopen_knurl(lua_State *L);
int luaopen_pslog_core(lua_State *L);
int luaopen_lonejson_core(lua_State *L);
int rl_curl_core(lua_State *L);

#include "pslog_facade.inc"
#include "lonejson_facade.inc"
#include "curl_facade.inc"
#include "smtp_facade.inc"

struct command {
    int argc;
    char **argv;
};

static int load_facade(lua_State *L, const unsigned char *source, size_t length,
                       const char *name)
{
    if (luaL_loadbufferx(L, (const char *)source, length, name, "t") != LUA_OK)
        return lua_error(L);
    lua_call(L, 0, 1);
    return 1;
}

static int load_pslog(lua_State *L)
{
    return load_facade(L, pslog_facade, sizeof(pslog_facade),
                       "@embedded/pslog.lua");
}

static int load_lonejson(lua_State *L)
{
    return load_facade(L, lonejson_facade, sizeof(lonejson_facade),
                       "@embedded/lonejson.lua");
}

static int load_curl(lua_State *L)
{
    return load_facade(L, curl_facade, sizeof(curl_facade),
                       "@embedded/curl.lua");
}

static int load_smtp(lua_State *L)
{
    return load_facade(L, smtp_facade, sizeof(smtp_facade),
                       "@embedded/knurl/smtp.lua");
}

static int execute(lua_State *L)
{
    static const luaL_Reg modules[] = {{"pslog.core", luaopen_pslog_core},
                                       {"pslog", load_pslog},
                                       {"lonejson.core", luaopen_lonejson_core},
                                       {"lonejson", load_lonejson},
                                       {"curl.core", rl_curl_core},
                                       {"curl", load_curl},
                                       {"knurl.smtp", load_smtp},
                                       {NULL, NULL}};
    const struct command *command = lua_touserdata(L, 1);
    int i;
    luaL_openlibs(L);
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    luaL_setfuncs(L, modules, 0);
    lua_pop(L, 2);
    luaL_requiref(L, "knurl", luaopen_knurl, 0);
    lua_pop(L, 1);

    lua_createtable(L, command->argc - 1, 1);
    for (i = 0; i < command->argc; ++i) {
        lua_pushstring(L, command->argv[i]);
        lua_rawseti(L, -2, i - 1);
    }
    lua_setglobal(L, "arg");
    if (luaL_loadfilex(L, command->argv[1], "t") != LUA_OK)
        return lua_error(L);
    if (!lua_checkstack(L, command->argc - 2))
        return luaL_error(L, "too many arguments");
    for (i = 2; i < command->argc; ++i)
        lua_pushstring(L, command->argv[i]);
    lua_call(L, command->argc - 2, 0);
    return 0;
}

static void usage(FILE *out)
{
    fputs("Usage: knurl FILE.lua [ARG ...]\n"
          "       knurl --help | --version\n",
          out);
}

int main(int argc, char **argv)
{
    struct command command;
    lua_State *L;
    const char *message;
    int status;

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("knurl %s\n", knurl_version());
        return 0;
    }
    if (argc < 2 || argv[1][0] == '-') {
        usage(stderr);
        return 2;
    }
    L = luaL_newstate();
    if (L == NULL) {
        fputs("knurl: cannot create Lua state\n", stderr);
        return 1;
    }
    command.argc = argc;
    command.argv = argv;
    lua_pushcfunction(L, execute);
    lua_pushlightuserdata(L, &command);
    status = lua_pcall(L, 1, 0, 0);
    if (status != LUA_OK) {
        message = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : NULL;
        fprintf(stderr, "knurl: %s\n",
                message != NULL ? message : "Lua script failed");
    }
    lua_close(L);
    return status == LUA_OK ? 0 : 1;
}

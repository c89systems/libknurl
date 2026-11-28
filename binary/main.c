#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <knurl/common.h>
#include <knurl/lua_source.h>
#include <stdio.h>
#include <string.h>
#include "auth.h"

int luaopen_knurl(lua_State *L);
int luaopen_pslog_core(lua_State *L);
int luaopen_lonejson_core(lua_State *L);
#include "pslog_facade.inc"
#include "lonejson_facade.inc"
#include "knurl_docs.inc"

static const knurl_lua_source dependencies[] = {
    {"lonejson/init.lua",
     {(const char *)lonejson_facade, sizeof(lonejson_facade)}},
    {"pslog/init.lua", {(const char *)pslog_facade, sizeof(pslog_facade)}}};

struct command {
    const char *program;
    int argc;
    char **argv;
};

static int load_facade(lua_State *L, const knurl_lua_source *file)
{
    const char *name = lua_pushfstring(L, "@embedded/%s", file->path);
    if (luaL_loadbufferx(L, file->source.data, file->source.length, name,
                         "t") != LUA_OK)
        return lua_error(L);
    lua_call(L, 0, 1);
    return 1;
}

static int load_source(lua_State *L, const char *path)
{
    size_t i;
    for (i = 0; i < knurl_lua_source_count(); ++i) {
        const knurl_lua_source *file = knurl_lua_source_at(i);
        if (strcmp(path, file->path) == 0)
            return load_facade(L, file);
    }
    for (i = 0; i < sizeof(dependencies) / sizeof(*dependencies); ++i)
        if (strcmp(path, dependencies[i].path) == 0)
            return load_facade(L, &dependencies[i]);
    return luaL_error(L, "missing Lua source: %s", path);
}

static int load_pslog(lua_State *L) { return load_source(L, "pslog/init.lua"); }
static int load_lonejson(lua_State *L)
{
    return load_source(L, "lonejson/init.lua");
}
static int load_curl(lua_State *L) { return load_source(L, "curl/init.lua"); }
static int load_curl_core(lua_State *L)
{
    return load_source(L, "curl/core.lua");
}
static int load_smtp(lua_State *L) { return load_source(L, "knurl/smtp.lua"); }

static int execute(lua_State *L)
{
    static const luaL_Reg modules[] = {{"pslog.core", luaopen_pslog_core},
                                       {"pslog", load_pslog},
                                       {"lonejson.core", luaopen_lonejson_core},
                                       {"lonejson", load_lonejson},
                                       {"curl.core", load_curl_core},
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
    lua_pushstring(L, command->program);
    lua_rawseti(L, -2, -1);
    for (i = 0; i < command->argc; ++i) {
        lua_pushstring(L, command->argv[i]);
        lua_rawseti(L, -2, i);
    }
    lua_setglobal(L, "arg");
    if (luaL_loadfilex(L, command->argv[0], "t") != LUA_OK)
        return lua_error(L);
    if (!lua_checkstack(L, command->argc - 1))
        return luaL_error(L, "too many arguments");
    for (i = 1; i < command->argc; ++i)
        lua_pushstring(L, command->argv[i]);
    lua_call(L, command->argc - 1, 0);
    return 0;
}

static void usage(FILE *out)
{
    fputs("Usage: knurl [OPTION] [ARGUMENT]...\n"
          "Run a Lua script or select an action.\n"
          "\n"
          "Options:\n"
          "  -a, --action=ACTION  Select an action (default: run).\n"
          "  -h, --help           Display this help and exit.\n"
          "      --version        Print version information and exit.\n",
          out);
    fputs(
        "\nActions:\n"
        "  run [--] FILE [ARG]...  Run a Lua script with the given arguments.\n"
        "  auth COMMAND [ARG]...   Manage authentication state. See -a auth "
        "--help.\n"
        "  docs                    Print the Markdown reference.\n"
        "  source                  Print the bundled Lua module sources.\n"
        "\n"
        "Use -- before a script filename that begins with a dash.\n",
        out);
}

static int run(const char *program, int argc, char **argv)
{
    struct command command;
    lua_State *L;
    const char *message;
    int status;

    L = luaL_newstate();
    if (L == NULL) {
        fputs("knurl: cannot create Lua state\n", stderr);
        return 1;
    }
    command.program = program;
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

static int print_source(const knurl_lua_source *file)
{
    return printf("-- ===== %s =====\n", file->path) >= 0 &&
           fwrite(file->source.data, 1, file->source.length, stdout) ==
               file->source.length &&
           fputc('\n', stdout) != EOF;
}

static int print_sources(void)
{
    size_t i;
    for (i = 0; i < knurl_lua_source_count(); ++i)
        if (!print_source(knurl_lua_source_at(i)))
            return 1;
    for (i = 0; i < sizeof(dependencies) / sizeof(*dependencies); ++i)
        if (!print_source(&dependencies[i]))
            return 1;
    return fflush(stdout) == EOF ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *program = argv[0], *action = "run";
    --argc;
    ++argv;
    if (argc == 1 &&
        (strcmp(argv[0], "-h") == 0 || strcmp(argv[0], "--help") == 0)) {
        puts("knurl and libknurl Copyright (c) 2026 C89 Systems AB "
             "https://c89.systems\n");
        usage(stdout);
        return 0;
    }
    if (argc == 1 && strcmp(argv[0], "--version") == 0) {
        printf("knurl %s\n", knurl_version());
        return 0;
    }
    if (argc >= 2 &&
        (strcmp(argv[0], "-a") == 0 || strcmp(argv[0], "--action") == 0)) {
        action = argv[1];
        argc -= 2;
        argv += 2;
    } else if (argc != 0 && strncmp(argv[0], "--action=", 9) == 0) {
        action = argv[0] + 9;
        --argc;
        ++argv;
    }
    if (strcmp(action, "auth") == 0)
        return knurl_auth_cli(argc, argv);
    if (argc == 0 && strcmp(action, "docs") == 0)
        return fwrite(knurl_docs, 1, sizeof(knurl_docs), stdout) ==
                           sizeof(knurl_docs) &&
                       fflush(stdout) != EOF
                   ? 0
                   : 1;
    if (argc == 0 && strcmp(action, "source") == 0)
        return print_sources();
    if (strcmp(action, "run") == 0 && argc != 0) {
        if (argv[0][0] != '-')
            return run(program, argc, argv);
        if (argc >= 2 && strcmp(argv[0], "--") == 0)
            return run(program, argc - 1, argv + 1);
    }
    usage(stderr);
    return 2;
}

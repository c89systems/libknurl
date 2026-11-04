/* Local-CA fixture for the host VM; no binding or core-private layouts. */
#define _POSIX_C_SOURCE 200809L
#include <rotmos/lua.h>
#include <lua.h>
#include <lauxlib.h>
#include "../acme-ca.h"
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define OWNER "rotmos.test.acme"
struct owner {
    struct test_ca *ca;
    pid_t parent, client;
};
static struct owner *checked(lua_State *L)
{
    struct owner *o = luaL_checkudata(L, 1, OWNER);
    luaL_argcheck(L, o->ca != NULL, 1, "CA fixture has closed");
    return o;
}
static int close_owner(lua_State *L)
{
    struct owner *o = luaL_checkudata(L, 1, OWNER);
    if (getpid() == o->parent) {
        if (o->client > 0) {
            (void)kill(o->client, SIGKILL);
            (void)waitpid(o->client, NULL, 0);
            o->client = 0;
        }
        test_ca_free(o->ca);
        o->ca = NULL;
    }
    return 0;
}
static int new_owner(lua_State *L)
{
    const char *root = luaL_checkstring(L, 1);
    struct owner *o = lua_newuserdatauv(L, sizeof(*o), 0);
    memset(o, 0, sizeof(*o));
    o->parent = getpid();
    luaL_setmetatable(L, OWNER);
    o->ca = test_ca_new(root, 0);
    if (o->ca == NULL)
        return luaL_error(L, "cannot prepare local CA");
    return 1;
}
static int start(lua_State *L)
{
    struct owner *o = checked(L);
    luaL_argcheck(L, o->ca->pid == 0, 1, "CA already started");
    if (!test_ca_start(o->ca))
        return luaL_error(L, "cannot start local CA");
    return 0;
}
static int directory(lua_State *L)
{
    lua_pushfstring(L, "%s/directory", checked(L)->ca->base);
    return 1;
}
static int process(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)getpid());
    return 1;
}
static int restore(lua_State *L)
{
    struct test_ca *ca = checked(L)->ca;
    const char *root = luaL_checkstring(L, 2);
    size_t size = strlen(root) + 32;
    char *path = malloc(size);
    int okay;
    if (path == NULL)
        return luaL_error(L, "cannot allocate fixture path");
    snprintf(path, size, "%s/certificates", root);
    okay = mkdir(path, 0700) == 0 || errno == EEXIST;
    snprintf(path, size, "%s/certificates/localhost", root);
    okay = okay && (mkdir(path, 0700) == 0 || errno == EEXIST);
    free(path);
    if (!okay || !test_ca_write(root, "account-key.pem", ca->account_pem) ||
        !test_ca_write(root, "certificates/localhost/key.pem", ca->domain_pem))
        return luaL_error(L, "cannot hydrate fixture keys");
    return 0;
}
static int ports(lua_State *L)
{
    struct test_ca *ca = checked(L)->ca;
    lua_Integer a = luaL_checkinteger(L, 2), b = luaL_checkinteger(L, 3);
    luaL_argcheck(L, a > 0 && a <= 65535 && b > 0 && b <= 65535, 2,
                  "listener ports required");
    ca->report->ports[0] = (unsigned short)a;
    ca->report->ports[1] = (unsigned short)b;
    __atomic_store_n(&ca->report->ports_ready, 1, __ATOMIC_RELEASE);
    return 0;
}
static int report(lua_State *L)
{
    struct test_ca_report *r = checked(L)->ca->report;
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, r->issued);
    lua_setfield(L, -2, "issued");
    lua_pushinteger(L, r->orders);
    lua_setfield(L, -2, "orders");
    lua_pushinteger(L, r->challenges);
    lua_setfield(L, -2, "challenges");
    lua_pushboolean(L, r->failed);
    lua_setfield(L, -2, "failed");
    return 1;
}
static int client(lua_State *L)
{
    struct owner *o = checked(L);
    int fd = (int)luaL_checkinteger(L, 2);
    size_t i;
    int attempt, okay = 1;
    struct timespec pause;
    char note;
    luaL_argcheck(L, o->client == 0, 1, "client already started");
    o->client = fork();
    if (o->client < 0)
        return luaL_error(L, "cannot fork certificate client");
    if (o->client != 0)
        return 0;
    (void)signal(SIGPIPE, SIG_IGN);
    pause.tv_sec = 0;
    pause.tv_nsec = 2000000;
    for (i = 0; i < 2; ++i) {
        for (attempt = 0; attempt < 100; ++attempt) {
            if (test_ca_client(o->ca, o->ca->report->ports[i], 1, 200, 0))
                break;
            (void)nanosleep(&pause, NULL);
        }
        if (attempt == 100)
            okay = 0;
    }
    note = okay ? 'C' : 'F';
    if (write(fd, &note, 1) != 1)
        okay = 0;
    _exit(okay ? 0 : 1);
}
static int join(lua_State *L)
{
    struct owner *o = checked(L);
    int status;
    if (o->client <= 0 || waitpid(o->client, &status, 0) != o->client ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return luaL_error(L, "certificate client failed");
    o->client = 0;
    return 0;
}
ROTMOS_API int luaopen_rotmos_acme_fixture(lua_State *L);
int luaopen_rotmos_acme_fixture(lua_State *L)
{
    static const luaL_Reg methods[] = {
        {"start", start},     {"directory", directory}, {"pid", process},
        {"restore", restore}, {"ports", ports},         {"report", report},
        {"client", client},   {"join", join},           {"close", close_owner},
        {NULL, NULL}};
    luaL_newmetatable(L, OWNER);
    lua_pushcfunction(L, close_owner);
    lua_setfield(L, -2, "__gc");
    luaL_newlib(L, methods);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushcfunction(L, new_owner);
    lua_setfield(L, -2, "new");
    return 1;
}

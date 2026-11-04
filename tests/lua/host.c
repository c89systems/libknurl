#define _POSIX_C_SOURCE 200809L
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#if defined(ROTMOS_PRELOAD_TEST)
#include <rotmos/lua.h>
#endif
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static pid_t child;
struct allocator {
    size_t after;
    int failures;
};
static struct allocator allocation;
static void *allocate(void *data, void *pointer, size_t old_size,
                      size_t new_size)
{
    struct allocator *a = data;
    if (new_size == 0) {
        free(pointer);
        return NULL;
    }
    if (a->failures && (pointer == NULL || new_size > old_size)) {
        if (a->after)
            --a->after;
        else {
            --a->failures;
            return NULL;
        }
    }
    return realloc(pointer, new_size);
}
static int fail_next(lua_State *L)
{
    (void)L;
    allocation.after = 0;
    allocation.failures = 2; /* Lua retries after its emergency collection. */
    return 0;
}
static int fail_call(lua_State *L)
{
    int top = lua_gettop(L), status;
    lua_Integer after = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    allocation.after = (size_t)after;
    allocation.failures = 2;
    status = lua_pcall(L, 0, LUA_MULTRET, 0);
    allocation.failures = 0;
    lua_pushboolean(L, status == LUA_OK);
    lua_insert(L, top + 1);
    return lua_gettop(L) - top;
}
static int exchange(unsigned short port, const char *method, const char *path,
                    const void *body, size_t length, int status,
                    const void *expected, size_t expected_length)
{
    int fd;
    struct sockaddr_in address;
    struct timeval timeout;
    char head[1024], prefix[32], *reply, *payload, *cursor, *end, *decoded;
    size_t used = 0, n = 0, chunk;
    ssize_t count;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return 1;
    }
    snprintf(head, sizeof(head),
             "%s %s HTTP/1.1\r\nHost: localhost\r\nX-Test: value"
             "\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
             method, path, (unsigned long)length);
    if (write(fd, head, strlen(head)) != (ssize_t)strlen(head) ||
        (length && write(fd, body, length) != (ssize_t)length)) {
        close(fd);
        return 1;
    }
    reply = malloc(65536);
    decoded = malloc(65536);
    if (reply == NULL || decoded == NULL) {
        free(reply);
        free(decoded);
        close(fd);
        return 1;
    }
    while (used < 65535 && (count = read(fd, reply + used, 65535 - used)) > 0)
        used += (size_t)count;
    close(fd);
    reply[used] = 0;
    snprintf(prefix, sizeof(prefix), "HTTP/1.1 %d ", status);
    payload = strstr(reply, "\r\n\r\n");
    if (strncmp(reply, prefix, strlen(prefix)) || payload == NULL) {
        fprintf(stderr, "%s: %s\n", path, reply);
        free(reply);
        free(decoded);
        return 1;
    }
    payload += 4;
    if (strstr(reply, "transfer-encoding: chunked")) {
        cursor = payload;
        while (cursor < reply + used) {
            chunk = strtoul(cursor, &end, 16);
            if (end == cursor || strncmp(end, "\r\n", 2)) {
                n = 65536;
                break;
            }
            cursor = end + 2;
            if (chunk == 0)
                break;
            if (chunk > 65535 - n || cursor + chunk + 2 > reply + used) {
                n = 65536;
                break;
            }
            memcpy(decoded + n, cursor, chunk);
            n += chunk;
            cursor += chunk + 2;
        }
    } else {
        n = used - (size_t)(payload - reply);
        memcpy(decoded, payload, n);
    }
    if (n != expected_length || memcmp(decoded, expected, expected_length)) {
        fprintf(stderr, "%s body length %lu expected %lu\n", path,
                (unsigned long)n, (unsigned long)expected_length);
        free(reply);
        free(decoded);
        return 1;
    }
    free(reply);
    free(decoded);
    return 0;
}
static int drive(unsigned short port)
{
    char *stream = malloc(1003);
    int failed = 0;
    if (stream == NULL)
        return 1;
    memset(stream, 's', 1000);
    memcpy(stream + 1000, "123", 3);
#define REQ(m, p, b, l, s, e, n)                                               \
    do {                                                                       \
        if (exchange(port, m, p, b, l, s, e, n)) {                             \
            failed = 1;                                                        \
            goto done;                                                         \
        }                                                                      \
    } while (0)
    REQ("GET", "/text", "", 0, 200, "hello", 5);
    REQ("GET", "/nil", "", 0, 204, "", 0);
    REQ("POST", "/echo", "A\0B", 3, 201, "A\0B", 3);
    REQ("GET", "/group/value/42?x=&x=two", "", 0, 200, "42", 2);
    REQ("GET", "/before", "", 0, 403, "blocked", 7);
    REQ("GET", "/after", "", 0, 202, "changed", 7);
    REQ("GET", "/throw", "", 0, 500, "", 0);
    REQ("GET", "/stream", "", 0, 200, stream, 1003);
    REQ("GET", "/source-error", "", 0, 500, "", 0);
    REQ("GET", "/getter-error", "", 0, 500, "", 0);
    REQ("GET", "/oom-reply", "", 0, 500, "", 0);
    REQ("GET", "/oom-source", "", 0, 500, "", 0);
    REQ("GET", "/discard", "", 0, 203, "replacement", 11);
    REQ("GET", "/table-group/entry", "", 0, 200, "default", 7);
    REQ("POST", "/table-group/other", "", 0, 200, "override", 8);
    REQ("GET", "/verify", "", 0, 200, "verified", 8);
    REQ("GET", "/stop", "", 0, 200, "stopped", 7);
done:
    free(stream);
    return failed;
}
static int spawn(lua_State *L)
{
    int status;
    unsigned short port = (unsigned short)lua_tointeger(L, 1);
    child = fork();
    if (child < 0)
        return luaL_error(L, "fork failed");
    if (child == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        status = drive(port);
        if (status)
            kill(getppid(), SIGTERM);
        _exit(status);
    }
    lua_pushboolean(L, 1);
    return 1;
}
static int join(lua_State *L)
{
    int status;
    if (child <= 0 || waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return luaL_error(L, "HTTP driver failed");
    child = 0;
    lua_pushboolean(L, 1);
    return 1;
}
#if defined(ROTMOS_PRELOAD_TEST)
static int conflict(lua_State *L)
{
    (void)L;
    return 0;
}
#endif
int main(int argc, char **argv)
{
    lua_State *L;
    int status;
#if defined(ROTMOS_PRELOAD_TEST)
    rotmos_error error;
#endif
    if (argc != 3)
        return 2;
    L = lua_newstate(allocate, &allocation, 0);
    if (L == NULL)
        return 2;
#if defined(ROTMOS_PRELOAD_TEST)
    if (rotmos_lua_preload(L, &error) != ROTMOS_STATE)
        return 3;
#endif
    luaL_openlibs(L);
#if defined(ROTMOS_PRELOAD_TEST)
    lua_getglobal(L, "string");
    lua_pushnil(L);
    lua_setglobal(L, "string");
    if (rotmos_lua_preload(L, &error) != ROTMOS_STATE)
        return 6;
    lua_setglobal(L, "string");
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    lua_pushcfunction(L, conflict);
    lua_setfield(L, -2, "rotmos");
    lua_settop(L, 0);
    if (rotmos_lua_preload(L, &error) != ROTMOS_CONFLICT)
        return 4;
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    lua_pushnil(L);
    lua_setfield(L, -2, "rotmos");
    lua_settop(L, 0);
    if (rotmos_lua_preload(L, &error) != ROTMOS_OK ||
        rotmos_lua_preload(L, &error) != ROTMOS_OK)
        return 5;
#else
    lua_getglobal(L, "package");
    lua_pushfstring(L, "%s/share/lua/5.5/?.lua", argv[2]);
    lua_setfield(L, -2, "path");
    lua_pushfstring(L, "%s/lib/lua/5.5/?.so", argv[2]);
    lua_setfield(L, -2, "cpath");
    lua_pop(L, 1);
#endif
    lua_newtable(L);
    lua_pushcfunction(L, spawn);
    lua_setfield(L, -2, "spawn");
    lua_pushcfunction(L, join);
    lua_setfield(L, -2, "join");
    lua_pushcfunction(L, fail_next);
    lua_setfield(L, -2, "fail_next");
    lua_pushcfunction(L, fail_call);
    lua_setfield(L, -2, "fail_call");
    lua_setglobal(L, "fixture");
    lua_pushstring(L, "preserved");
    lua_setglobal(L, "host_marker");
    status = luaL_loadfilex(L, argv[1], "t");
    if (status == LUA_OK)
        status = lua_pcall(L, 0, 0, 0);
    if (status != LUA_OK)
        fprintf(stderr, "Lua fixture: %s\n", lua_tostring(L, -1));
    lua_close(L);
    return status != LUA_OK;
}

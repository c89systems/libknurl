#define _POSIX_C_SOURCE 200809L
#include "../acl-wire.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int entered[2], release_app[2];

struct lua_allocator {
    lua_Alloc allocate;
    void *userdata;
    pid_t owner;
};

static void *frontend_allocate(void *data, void *ptr, size_t before,
                               size_t after)
{
    struct lua_allocator *allocator = data;
    if (getpid() != allocator->owner) {
        static const char message[] = "Kore worker touched the Lua allocator\n";
        (void)write(STDERR_FILENO, message, sizeof(message) - 1);
        _exit(91);
    }
    return allocator->allocate(allocator->userdata, ptr, before, after);
}

static int listen_local(unsigned short *port)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&address, length) != 0 ||
        getsockname(fd, (struct sockaddr *)&address, &length) != 0 ||
        listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(address.sin_port);
    return fd;
}

static int start_request(unsigned short port, const char *path)
{
    struct sockaddr_in address;
    struct timeval timeout = {3, 0};
    char head[256];
    int fd = socket(AF_INET, SOCK_STREAM, 0), length;
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
        length = snprintf(
            head, sizeof(head),
            "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
            path);
        if (length > 0 && (size_t)length < sizeof(head) &&
            write(fd, head, (size_t)length) == length)
            return fd;
    }
    close(fd);
    return -1;
}

static int read_response(int fd, char *buffer, size_t capacity)
{
    size_t count = 0;
    ssize_t length;
    if (fd < 0)
        return -1;
    while (count < capacity - 1) {
        length = read(fd, buffer + count, capacity - 1 - count);
        if (length < 0 && errno == EINTR)
            continue;
        if (length < 0)
            return -1;
        if (length == 0)
            break;
        count += (size_t)length;
    }
    buffer[count] = '\0';
    return (int)count;
}

static int block_application(lua_State *L)
{
    char byte;
    if (write(entered[1], "x", 1) != 1 || read(release_app[0], &byte, 1) != 1)
        return luaL_error(L, "test application wait failed");
    return 0;
}

static int upstream(int listener)
{
    static const char program[] =
        "return function(path) "
        "if path == '/curl-module' then return '{\"message\":\"loaded\"}' end "
        "if path == '/app' or path == '/timeout' then block_application() end "
        "return 'lua:' .. path end";
    lua_State *L = luaL_newstate();
    char head[2048], response[256], path[128];
    size_t i, used, body_length;
    ssize_t n;
    int fd, length;
    const char *body;

    if (L == NULL)
        return 1;
    close(entered[0]);
    close(release_app[1]);
    lua_pushcfunction(L, block_application);
    lua_setglobal(L, "block_application");
    if (luaL_loadstring(L, program) != LUA_OK ||
        lua_pcall(L, 0, 1, 0) != LUA_OK)
        return 1;
    for (i = 0; i < 4; ++i) {
        fd = accept(listener, NULL, NULL);
        if (fd < 0)
            return 1;
        used = 0;
        head[0] = '\0';
        while (strstr(head, "\r\n\r\n") == NULL) {
            n = read(fd, head + used, sizeof(head) - used - 1);
            if (n <= 0)
                return 1;
            used += (size_t)n;
            head[used] = '\0';
        }
        if (sscanf(head, "GET %127s", path) != 1)
            return 1;
        lua_pushvalue(L, 1);
        lua_pushstring(L, path);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK)
            return 1;
        body = lua_tolstring(L, -1, &body_length);
        length = snprintf(response, sizeof(response),
                          "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n"
                          "Connection: close\r\n\r\n%s",
                          (unsigned long)body_length, body);
        if (length <= 0 || (size_t)length >= sizeof(response))
            return 1;
        /* The timeout case can close the connection before the handler returns.
         */
        (void)write(fd, response, (size_t)length);
        close(fd);
        lua_pop(L, 1);
    }
    lua_close(L);
    close(listener);
    return 0;
}

static int frontend(char **args, unsigned short port, unsigned short app_port)
{
    lua_State *L = luaL_newstate();
    struct lua_allocator allocator;
    char target[64];
    int status;
    if (L == NULL)
        return 1;
    allocator.owner = getpid();
    allocator.allocate = lua_getallocf(L, &allocator.userdata);
    lua_setallocf(L, frontend_allocate, &allocator);
    luaL_openlibs(L);
    status = luaL_loadfile(L, args[2]);
    if (status == LUA_OK) {
        snprintf(target, sizeof(target), "http://127.0.0.1:%u",
                 (unsigned int)app_port);
        lua_pushstring(L, args[1]);
        lua_pushinteger(L, port);
        lua_pushstring(L, target);
        lua_pushstring(L, args[3]);
        lua_pushstring(L, args[4]);
        status = lua_pcall(L, 5, 0, 0);
    }
    if (status != LUA_OK)
        fprintf(stderr, "Lua frontend: %s\n", lua_tostring(L, -1));
    lua_close(L);
    return status != LUA_OK;
}

static int wait_entry(void)
{
    struct pollfd event;
    char byte;
    event.fd = entered[0];
    event.events = POLLIN;
    return poll(&event, 1, 3000) == 1 && (event.revents & POLLIN) &&
           read(entered[0], &byte, 1) == 1;
}

static int exchange(unsigned short port, const char *path, int status,
                    const char *body)
{
    int actual = knurl_test_acl_exchange(port, "GET", path, NULL, body);
    if (actual == status)
        return 1;
    fprintf(stderr, "%s: expected %d, received %d or unexpected body\n", path,
            status, actual);
    return 0;
}

static int smoke(unsigned short port)
{
    char raw[2048], *payload;
    int fd, i, length;
    struct timespec tick = {0, 20000000};
    for (i = 0; i < 150; ++i) {
        fd = start_request(port, "/health");
        if (fd >= 0) {
            length = read_response(fd, raw, sizeof(raw));
            close(fd);
            if (length >= 5 && strstr(raw, "HTTP/1.1 200") != NULL &&
                !strcmp(raw + length - 5, "ready"))
                break;
        }
        nanosleep(&tick, NULL);
    }
    if (i == 150) {
        fprintf(stderr, "frontend never became ready\n");
        return 0;
    }
    fd = start_request(port, "/redirect");
    length = read_response(fd, raw, sizeof(raw));
    close(fd);
    if (length < 0 || strstr(raw, "HTTP/1.1 307") == NULL ||
        strstr(raw, "\r\nLocation: /health\r\n") == NULL) {
        fprintf(stderr, "/redirect: unexpected status or Location\n");
        return 0;
    }
    fd = start_request(port, "/bytes");
    length = read_response(fd, raw, sizeof(raw));
    close(fd);
    payload = length > 0 ? strstr(raw, "\r\n\r\n") : NULL;
    if (payload == NULL || raw + length - payload != 7 ||
        memcmp(payload + 4, "A\0B", 3)) {
        fprintf(stderr, "/bytes: unexpected binary response\n");
        return 0;
    }
    fd = start_request(port, "/proxy");
    length = read_response(fd, raw, sizeof(raw));
    close(fd);
    if (length < 0 || strstr(raw, "HTTP/1.1 200") == NULL ||
        strstr(raw, "\r\nlua:/proxy\r\n") == NULL) {
        fprintf(stderr, "/proxy: unexpected response: %s\n",
                length < 0 ? "(read error)" : raw);
        return 0;
    }
    return exchange(port, "/json", 200, "{\"native\":true}") &&
           exchange(port, "/missing", 404, NULL) &&
           exchange(port, "/deny/text", 403, NULL) &&
           exchange(port, "/native/acl", 200, "allow") &&
           exchange(port, "/directory/asset.txt", 200, "native static\n") &&
           knurl_test_acl_exchange(port, "POST", "/upload",
                                   "Content-Length: 3\r\n\r\nabc",
                                   NULL) == 200 &&
           knurl_test_acl_exchange(port, "POST", "/upload",
                                   "Content-Length: 9\r\n\r\nabcdefghi",
                                   NULL) == 413 &&
           exchange(port, "/uploaded", 200, "abc") &&
           knurl_test_acl_exchange(port, "POST", "/upload-default",
                                   "Content-Length: 3\r\n\r\ndef",
                                   NULL) == 200 &&
           exchange(port, "/uploaded", 200, "def");
}

static int isolation(unsigned short port)
{
    char raw[2048];
    struct pollfd pending;
    int fd = start_request(port, "/app");
    int ok = 0;
    if (fd < 0)
        return 0;
    pending.fd = fd;
    pending.events = POLLIN;
    if (wait_entry() && exchange(port, "/health", 200, "ready") &&
        exchange(port, "/native/static", 200, "native static\n") &&
        poll(&pending, 1, 0) == 0 && write(release_app[1], "x", 1) == 1 &&
        read_response(fd, raw, sizeof(raw)) > 0 &&
        strstr(raw, "HTTP/1.1 200") != NULL &&
        strstr(raw, "\r\nlua:/app\r\n") != NULL)
        ok = 1;
    close(fd);
    if (!ok)
        return 0;

    fd = start_request(port, "/timeout");
    if (fd < 0)
        return 0;
    ok = wait_entry() && read_response(fd, raw, sizeof(raw)) > 0 &&
         strstr(raw, "HTTP/1.1 504") != NULL &&
         write(release_app[1], "x", 1) == 1 &&
         exchange(port, "/health", 200, "ready");
    close(fd);
    return ok;
}

int main(int argc, char **argv)
{
    unsigned short port, app_port;
    int fd, listener, status = 0, ok;
    pid_t app, front;
    if (argc != 5 || pipe(entered) != 0 || pipe(release_app) != 0)
        return 1;
    signal(SIGPIPE, SIG_IGN);
    fd = listen_local(&port);
    if (fd < 0)
        return 1;
    close(fd);
    listener = listen_local(&app_port);
    if (listener < 0)
        return 1;
    app = fork();
    if (app == 0)
        _exit(upstream(listener));
    close(listener);
    close(entered[1]);
    close(release_app[0]);
    if (app < 0)
        return 1;
    front = fork();
    if (front == 0) {
        close(entered[0]);
        close(release_app[1]);
        _exit(frontend(argv, port, app_port));
    }
    ok = front > 0 && smoke(port);
    if (!ok)
        fprintf(stderr, "configuration route smoke checks failed\n");
    else if (!(ok = isolation(port)))
        fprintf(stderr, "application isolation or timeout check failed\n");
    close(release_app[1]);
    close(entered[0]);
    if (!ok)
        (void)kill(app, SIGTERM);
    if (waitpid(app, &status, 0) != app || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
        ok = 0;
    if (front > 0) {
        (void)kill(front, SIGTERM);
        if (waitpid(front, &status, 0) != front || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0)
            ok = 0;
    }
    return !ok;
}

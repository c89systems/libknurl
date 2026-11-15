#define _POSIX_C_SOURCE 200809L
#include "curl-fixture.h"
#include "events.h"
#include <lauxlib.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t origin, client, owner;
static int gate[2] = {-1, -1}, notes[2] = {-1, -1};
#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "curl fixture line %d: %s\n", __LINE__, #x);       \
            if (owner > 0 && getpid() != owner)                                \
                (void)kill(owner, SIGTERM);                                    \
            _exit(1);                                                          \
        }                                                                      \
    } while (0)
static char note(int fd)
{
    struct pollfd p;
    char value = 0;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    CHECK(poll(&p, 1, 3000) == 1 && read(fd, &value, 1) == 1);
    return value;
}
static void send_bytes(int fd, const void *bytes, size_t length)
{
    const unsigned char *p = bytes;
    ssize_t n;
    while (length) {
        n = write(fd, p, length);
        if (n < 0 && errno == EINTR)
            continue;
        CHECK(n > 0);
        p += n;
        length -= (size_t)n;
    }
}
static void serve(int listener)
{
    int fd;
    char request[8192], response[512];
    const char *body;
    size_t used;
    ssize_t n;
    struct linger reset;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, SIG_DFL);
    for (;;) {
        fd = accept(listener, NULL, NULL);
        CHECK(fd >= 0);
        used = 0;
        do {
            n = read(fd, request + used, sizeof(request) - used - 1);
            CHECK(n > 0);
            used += (size_t)n;
            request[used] = 0;
        } while (!strstr(request, "\r\n\r\n"));
        body = "hello";
        if (strstr(request, " /held ")) {
            CHECK(write(notes[1], "h", 1) == 1);
            CHECK(note(gate[0]) == 'g');
            body = "held";
        } else if (strstr(request, " /stalled ")) {
            CHECK(write(notes[1], "s", 1) == 1);
            do {
                n = read(fd, request, sizeof(request));
            } while (n > 0 || (n < 0 && errno == EINTR));
            CHECK(write(notes[1], "x", 1) == 1);
            close(fd);
            continue;
        } else if (strstr(request, " /json-upload ")) {
            while (!strstr(request, "0\r\n\r\n")) {
                n = read(fd, request + used, sizeof(request) - used - 1);
                CHECK(n > 0);
                used += (size_t)n;
                request[used] = 0;
            }
            CHECK(strstr(request, "uploaded"));
            body = "{\"message\":\"uploaded\"}";
        } else if (strstr(request, " /json "))
            body = "{\"message\":\"hello\"}";
        else if (strstr(request, " /stream ")) {
            const char *head =
                "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: "
                "close\r\n\r\n3\r\none\r\n";
            send_bytes(fd, head, strlen(head));
            CHECK(note(gate[0]) == 'g');
            send_bytes(fd, "3\r\ntwo\r\n0\r\n\r\n", 13);
            close(fd);
            continue;
        } else if (strstr(request, " /file-upload ")) {
            while (!strstr(strstr(request, "\r\n\r\n") + 4, "onetwo")) {
                n = read(fd, request + used, sizeof(request) - used - 1);
                CHECK(n > 0);
                used += (size_t)n;
                request[used] = 0;
            }
            body = "read";
        } else if (strstr(request, " /echo ")) {
            while (!strstr(request, "0\r\n\r\n")) {
                n = read(fd, request + used, sizeof(request) - used - 1);
                CHECK(n > 0);
                used += (size_t)n;
                request[used] = 0;
            }
            CHECK(strstr(request, "one") && strstr(request, "two"));
            body = "read";
        } else if (strstr(request, " /reset ")) {
            memset(&reset, 0, sizeof(reset));
            reset.l_onoff = 1;
            CHECK(setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset,
                             sizeof(reset)) == 0);
            close(fd);
            continue;
        }
        snprintf(response, sizeof(response),
                 "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\nContent-Type: "
                 "application/json\r\nConnection: close\r\n\r\n%s",
                 (unsigned long)strlen(body), body);
        send_bytes(fd, response, strlen(response));
        close(fd);
    }
}
static int setup(lua_State *L)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int fd;
    owner = getpid();
    if (origin || pipe(gate) || pipe(notes))
        return luaL_error(L, "curl fixture setup failed");
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return luaL_error(L, "curl origin socket failed");
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) ||
        listen(fd, 8) || getsockname(fd, (struct sockaddr *)&address, &length))
        return luaL_error(L, "curl origin bind failed");
    origin = fork();
    if (origin < 0)
        return luaL_error(L, "curl origin fork failed");
    if (origin == 0)
        serve(fd);
    close(fd);
    lua_pushfstring(L, "http://127.0.0.1:%d", ntohs(address.sin_port));
    return 1;
}
static int connect_to(unsigned short port)
{
    struct sockaddr_in address;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    return fd;
}
static void response(int fd, const char *body)
{
    char buffer[4096];
    size_t used = 0;
    ssize_t n;
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    do {
        CHECK(poll(&p, 1, 3000) == 1);
        n = read(fd, buffer + used, sizeof(buffer) - used - 1);
        CHECK(n >= 0);
        used += (size_t)n;
        buffer[used] = 0;
    } while (n > 0);
    CHECK(strstr(buffer, "HTTP/1.1 200 ") && strstr(buffer, "\r\n\r\n") &&
          strstr(buffer, body));
    close(fd);
}
static int spawn(lua_State *L)
{
    int batch[16];
    char first, second;
    struct linger reset;
    unsigned short port = (unsigned short)luaL_checkinteger(L, 1);
    static const char *const paths[] = {
        "/json",           "/json-map",     "/write",       "/read",
        "/failed",         "/sleep",        "/after-sleep", "/json-upload",
        "/callback-error", "/callback-oom", "/adapter",     "/files"};
    const char *request =
        "GET /slow HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    size_t i;
    int fd;
    client = fork();
    if (client < 0)
        return luaL_error(L, "curl client fork failed");
    if (client != 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    signal(SIGTERM, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    fd = connect_to(port);
    send_bytes(fd, request, strlen(request));
    CHECK(note(notes[0]) == 'h');
    CHECK(knurl_lua_test_exchange(port, "GET", "/fast", "", 0, 200, "fast",
                                  4) == 0);
    CHECK(write(gate[1], "g", 1) == 1);
    response(fd, "held");
    request =
        "GET /pair HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    for (i = 0; i < 2; ++i) {
        batch[i] = connect_to(port);
        send_bytes(batch[i], request, strlen(request));
    }
    CHECK(note(notes[0]) == 'p' && note(notes[0]) == 'p');
    CHECK(knurl_lua_test_exchange(port, "GET", "/pair-release", "", 0, 200,
                                  "ok", 2) == 0);
    for (i = 0; i < 2; ++i)
        response(batch[i], "ok");
    fd = connect_to(port);
    request =
        "GET /stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send_bytes(fd, request, strlen(request));
    CHECK(note(notes[0]) == 'w');
    CHECK(knurl_lua_test_exchange(port, "GET", "/stream-progress", "", 0, 200,
                                  "ok", 2) == 0);
    CHECK(write(gate[1], "g", 1) == 1);
    response(fd, "ok");
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
        CHECK(knurl_lua_test_exchange(port, "GET", paths[i], "", 0, 200, "ok",
                                      2) == 0);
    CHECK(knurl_lua_test_exchange(port, "GET", "/timeout", "", 0, 200, "ok",
                                  2) == 0);
    fd = connect_to(port);
    request =
        "GET /cancel HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send_bytes(fd, request, strlen(request));
    CHECK(note(notes[0]) == 'n');
    CHECK(note(notes[0]) == 's');
    memset(&reset, 0, sizeof(reset));
    reset.l_onoff = 1;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
    close(fd);
    first = note(notes[0]);
    second = note(notes[0]);
    CHECK((first == 'c' && second == 'x') || (first == 'x' && second == 'c'));
    for (i = 0; i < 16; ++i) {
        batch[i] = connect_to(port);
        request = "GET /batch HTTP/1.1\r\nHost: localhost\r\nConnection: "
                  "close\r\n\r\n";
        send_bytes(batch[i], request, strlen(request));
        CHECK(note(notes[0]) == 'b');
    }
    CHECK(knurl_lua_test_exchange(port, "GET", "/limit", "", 0, 200, "ok", 2) ==
          0);
    for (i = 0; i < 16; ++i) {
        CHECK(setsockopt(batch[i], SOL_SOCKET, SO_LINGER, &reset,
                         sizeof(reset)) == 0);
        close(batch[i]);
    }
    for (i = 0; i < 16; ++i)
        CHECK(note(notes[0]) == 'c');
    CHECK(knurl_lua_test_exchange(port, "GET", "/batch-release", "", 0, 200,
                                  "ok", 2) == 0);
    CHECK(knurl_lua_test_exchange(port, "GET", "/stop", "", 0, 200, "ok", 2) ==
          0);
    _exit(0);
}
static int cancelled(lua_State *L)
{
    (void)L;
    CHECK(write(notes[1], "c", 1) == 1);
    return 0;
}
static int started(lua_State *L)
{
    (void)L;
    CHECK(write(notes[1], "n", 1) == 1);
    return 0;
}
static int batch_started(lua_State *L)
{
    (void)L;
    CHECK(write(notes[1], "b", 1) == 1);
    return 0;
}
static int streamed(lua_State *L)
{
    (void)L;
    CHECK(write(notes[1], "w", 1) == 1);
    return 0;
}
static int pair_started(lua_State *L)
{
    (void)L;
    CHECK(write(notes[1], "p", 1) == 1);
    return 0;
}
static int join(lua_State *L)
{
    int status;
    pid_t result;
    do {
        result = waitpid(client, &status, 0);
    } while (result < 0 && errno == EINTR);
    client = 0;
    if (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status))
        return luaL_error(L, "curl HTTP client failed");
    lua_pushboolean(L, 1);
    return 1;
}
void knurl_test_curl_cleanup(void)
{
    int status, i;
    if (client > 0) {
        kill(client, SIGKILL);
        while (waitpid(client, &status, 0) < 0 && errno == EINTR) {
        }
        client = 0;
    }
    if (origin > 0) {
        kill(origin, SIGTERM);
        while (waitpid(origin, &status, 0) < 0 && errno == EINTR) {
        }
        origin = 0;
    }
    for (i = 0; i < 2; ++i) {
        if (gate[i] >= 0) {
            close(gate[i]);
            gate[i] = -1;
        }
        if (notes[i] >= 0) {
            close(notes[i]);
            notes[i] = -1;
        }
    }
}
void knurl_test_curl_register(lua_State *L)
{
    static const luaL_Reg methods[] = {{"curl_setup", setup},
                                       {"spawn_curl", spawn},
                                       {"curl_join", join},
                                       {"curl_cancelled", cancelled},
                                       {"curl_started", started},
                                       {"curl_batch", batch_started},
                                       {"curl_streamed", streamed},
                                       {"curl_pair_started", pair_started},
                                       {NULL, NULL}};
    luaL_setfuncs(L, methods, 0);
}

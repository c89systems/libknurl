#define _POSIX_C_SOURCE 200809L
#include <lua.h>
#include "events.h"
#include <lauxlib.h>
#include <lualib.h>
#if defined(ROTMOS_PRELOAD_TEST)
#include <rotmos/lua.h>
#endif
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <signal.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static pid_t child;
static pid_t proxy_origin;
static int proxy_shutdown = -1;
static int event_pipe[2] = {-1, -1};
static int progress_read = -1, progress_write = -1;
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
static int encrypted;
static int omit_length;
static const char *connection_value = "close";
static const char *required_header;
static const char *request_headers = "", *required_body;
static const char *forwarded_address = "198.51.100.7";
static SSL_CTX *client_context;
int rotmos_lua_test_exchange(unsigned short port, const char *method,
                             const char *path, const void *body, size_t length,
                             int status, const void *expected,
                             size_t expected_length)
{
    int fd = -1, failed = 1;
    SSL *ssl = NULL;
    struct sockaddr_in address;
    struct timeval timeout;
    char head[1024], framing[64], prefix[32], *reply = NULL, *payload, *cursor,
                                              *end, *decoded = NULL;
    size_t used = 0, n = 0, chunk;
    const size_t capacity = 262144;
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
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
        goto done;
    if (encrypted) {
        ssl = SSL_new(client_context);
        if (ssl == NULL || !SSL_set_fd(ssl, fd) ||
            !SSL_set_tlsext_host_name(ssl, "localhost") ||
            !SSL_set1_host(ssl, "localhost") || SSL_connect(ssl) != 1)
            goto done;
    }
    framing[0] = 0;
    if (!omit_length)
        snprintf(framing, sizeof(framing), "Content-Length: %lu\r\n",
                 (unsigned long)length);
    snprintf(head, sizeof(head),
             "%s %s HTTP/1.1\r\nHost: localhost\r\nX-Test: value\r\n"
             "Authorization: Bearer fixture-token\r\nX-Forwarded-For: %s\r\n"
             "%s%sConnection: %s\r\n\r\n",
             method, path, forwarded_address, request_headers, framing,
             connection_value);
    count = ssl ? SSL_write(ssl, head, (int)strlen(head))
                : write(fd, head, strlen(head));
    if (count != (ssize_t)strlen(head))
        goto done;
    if (length) {
        if (progress_read >= 0 &&
            (!strcmp(path, "/upload/live") || !strcmp(path, "/proxy/echo"))) {
            struct pollfd ready;
            char byte;
            if (write(fd, body, 3) != 3)
                goto done;
            ready.fd = progress_read;
            ready.events = POLLIN;
            ready.revents = 0;
            if (poll(&ready, 1, 3000) != 1 ||
                read(progress_read, &byte, 1) != 1 || byte != 'p')
                goto done;
            body = (const char *)body + 3;
            length -= 3;
        }
        count =
            ssl ? SSL_write(ssl, body, (int)length) : write(fd, body, length);
        if (count != (ssize_t)length)
            goto done;
    }
    reply = malloc(capacity);
    decoded = malloc(capacity);
    if (reply == NULL || decoded == NULL)
        goto done;
    while (used < capacity - 1) {
        count = ssl ? SSL_read(ssl, reply + used, (int)(capacity - 1 - used))
                    : read(fd, reply + used, capacity - 1 - used);
        if (count <= 0)
            break;
        used += (size_t)count;
    }
    reply[used] = 0;
    snprintf(prefix, sizeof(prefix), "HTTP/1.1 %d ", status);
    payload = strstr(reply, "\r\n\r\n");
    if (strncmp(reply, prefix, strlen(prefix)) || payload == NULL ||
        (required_header && strstr(reply, required_header) == NULL)) {
        fprintf(stderr, "%s: %s\n", path, reply);
        goto done;
    }
    payload += 4;
    if (strstr(reply, "transfer-encoding: chunked")) {
        cursor = payload;
        while (cursor < reply + used) {
            chunk = strtoul(cursor, &end, 16);
            if (end == cursor || strncmp(end, "\r\n", 2))
                goto done;
            cursor = end + 2;
            if (chunk == 0)
                break;
            if (chunk > capacity - 1 - n || cursor + chunk + 2 > reply + used)
                goto done;
            memcpy(decoded + n, cursor, chunk);
            n += chunk;
            cursor += chunk + 2;
        }
    } else {
        n = used - (size_t)(payload - reply);
        memcpy(decoded, payload, n);
    }
    decoded[n] = 0;
    if ((expected != NULL && (n != expected_length ||
                              memcmp(decoded, expected, expected_length))) ||
        (required_body != NULL && strstr(decoded, required_body) == NULL)) {
        fprintf(stderr, "%s body length %lu expected %lu\n", path,
                (unsigned long)n, (unsigned long)expected_length);
        goto done;
    }
    failed = 0;
done:
    SSL_free(ssl);
    if (fd >= 0)
        close(fd);
    free(reply);
    free(decoded);
    return failed;
}
static int drive(unsigned short port)
{
    char *stream = malloc(1003);
    char *spooled = malloc(20004);
    int failed = 0;
    if (stream == NULL || spooled == NULL) {
        free(stream);
        free(spooled);
        return 1;
    }
    memset(stream, 's', 1000);
    memcpy(stream + 1000, "123", 3);
    memset(spooled, 'p', 20000);
    spooled[20000] = 0;
    memcpy(spooled + 20001, "123", 3);
#define REQ(m, p, b, l, s, e, n)                                               \
    do {                                                                       \
        if (rotmos_lua_test_exchange(port, m, p, b, l, s, e, n)) {             \
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
    required_body = "expected protected handler error";
    REQ("GET", "/throw", "", 0, 500, NULL, 0);
    required_body = NULL;
    REQ("GET", "/stream", "", 0, 200, stream, 1003);
    REQ("GET", "/source-error", "", 0, 500, "", 0);
    required_header = "content-length: 20004\r\n";
    REQ("GET", "/spooled", "", 0, 200, spooled, 20004);
    REQ("HEAD", "/spooled", "", 0, 200, "", 0);
    required_header = NULL;
    required_body = "expected spool read failure";
    REQ("GET", "/spooled-error", "", 0, 500, NULL, 0);
    required_body = "memory";
    REQ("GET", "/spooled-oom", "", 0, 500, NULL, 0);
    required_body = NULL;
    REQ("GET", "/spooled-close-error", "", 0, 200, "", 0);
    REQ("GET", "/spooled-strict", "", 0, 200, "strict", 6);
    required_body = "paused source";
    REQ("GET", "/spooled-paused", "", 0, 501, NULL, 0);
    required_body = NULL;
    REQ("GET", "/spooled-discard", "", 0, 200, "discarded", 9);
    required_body = "expected getter failure";
    REQ("GET", "/getter-error", "", 0, 500, NULL, 0);
    required_body = "memory";
    REQ("GET", "/oom-reply", "", 0, 500, NULL, 0);
    REQ("GET", "/oom-source", "", 0, 500, NULL, 0);
    required_body = NULL;
    REQ("GET", "/discard", "", 0, 203, "replacement", 11);
    REQ("GET", "/table-group/entry", "", 0, 200, "default", 7);
    REQ("POST", "/table-group/other", "", 0, 200, "override", 8);
    REQ("GET", "/verify", "", 0, 200, "verified", 8);
    REQ("GET", "/stop", "", 0, 200, "stopped", 7);
done:
    free(stream);
    free(spooled);
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
static int spawn_logging(lua_State *L)
{
    unsigned short port = (unsigned short)lua_tointeger(L, 1);
    int failed;
    child = fork();
    if (child < 0)
        return luaL_error(L, "fork failed");
    if (child == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        failed =
            rotmos_lua_test_exchange(port, "GET", "/ok", "", 0, 202, "ok", 2) ||
            rotmos_lua_test_exchange(port, "GET", "/bad", "", 0, 404, "missing",
                                     7) ||
            rotmos_lua_test_exchange(port, "GET", "/fault", "", 0, 500,
                                     "failed", 6) ||
            rotmos_lua_test_exchange(port, "GET", "/stop", "", 0, 200,
                                     "stopped", 7);
        if (failed)
            kill(getppid(), SIGTERM);
        _exit(failed);
    }
    lua_pushboolean(L, 1);
    return 1;
}
static int drive_providers(unsigned short port)
{
    char *large;
    int failed;
    if (rotmos_lua_test_exchange(port, "GET", "/pack/asset", "", 0, 200,
                                 "packed", 6))
        return 1;
    request_headers = "Range: bytes=2-4\r\n";
    required_header = "content-range: bytes 2-4/6";
    failed = rotmos_lua_test_exchange(port, "GET", "/pack/asset", "", 0, 206,
                                      "cke", 3);
    request_headers = "If-None-Match: \"pack\"\r\n";
    required_header = NULL;
    if (failed ||
        rotmos_lua_test_exchange(port, "GET", "/pack/asset", "", 0, 304, "", 0))
        return 1;
    request_headers = "";
    if (rotmos_lua_test_exchange(port, "HEAD", "/pack/large", "", 0, 200, "",
                                 0) ||
        rotmos_lua_test_exchange(port, "GET", "/group/pack/asset", "", 0, 200,
                                 "packed", 6) ||
        rotmos_lua_test_exchange(port, "GET", "/pack/binary", "", 0, 200,
                                 "A\0B", 3) ||
        rotmos_lua_test_exchange(port, "GET", "/dav/asset", "", 0, 200,
                                 "packed", 6))
        return 1;
    request_headers = "Depth: 1\r\n";
    required_body = "asset";
    failed = rotmos_lua_test_exchange(port, "PROPFIND", "/dav/", "", 0, 207,
                                      NULL, 0);
    request_headers = "";
    required_body = NULL;
    if (failed || rotmos_lua_test_exchange(port, "PUT", "/dav/asset", "new", 3,
                                           405, "", 0))
        return 1;
    large = malloc(131072);
    if (large == NULL)
        return 1;
    memset(large, 'p', 131072);
    failed = rotmos_lua_test_exchange(port, "GET", "/pack/large", "", 0, 200,
                                      large, 131072);
    free(large);
    return failed || rotmos_lua_test_exchange(port, "GET", "/stop", "", 0, 200,
                                              "stopped", 7);
}
static int spawn_providers(lua_State *L)
{
    unsigned short port = (unsigned short)lua_tointeger(L, 1);
    int failed;
    child = fork();
    if (child < 0)
        return luaL_error(L, "fork failed");
    if (child == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        failed = drive_providers(port);
        if (failed)
            kill(getppid(), SIGTERM);
        _exit(failed);
    }
    lua_pushboolean(L, 1);
    return 1;
}
static int spawn_codecs(lua_State *L)
{
    int failed;
    unsigned short port = (unsigned short)luaL_checkinteger(L, 1);
    child = fork();
    if (child < 0)
        return luaL_error(L, "fork failed");
    if (child == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        failed =
            rotmos_lua_test_exchange(port, "GET", "/json", "", 0, 200,
                                     "{\"text\":\"wire\",\"@id\":null}", 26);
        if (!failed)
            failed = rotmos_lua_test_exchange(port, "HEAD", "/json", "", 0, 200,
                                              "", 0);
        if (!failed)
            failed = rotmos_lua_test_exchange(port, "GET", "/xml", "", 0, 200,
                                              "<writer>wire</writer>", 21);
        if (!failed)
            failed = rotmos_lua_test_exchange(port, "GET", "/stop", "", 0, 200,
                                              "ok", 2);
        if (failed)
            kill(getppid(), SIGTERM);
        _exit(failed);
    }
    lua_pushboolean(L, 1);
    return 1;
}
static int spawn_typed(lua_State *L)
{
    static const struct {
        const char *method, *path, *input;
        int status;
        const char *output;
    } cases[] = {
        {"POST", "/fixed", "{\"text\":\"one\"}", 202, "{\"text\":\"one\"}"},
        {"POST", "/group/typed", "{\"text\":\"existing\"}", 409,
         "{\"code\":\"existing\"}"},
        {"POST", "/echo", "{\"text\":\"direct\"}", 200,
         "{\"text\":\"direct\"}"},
        {"GET", "/retained", "", 200, "ok"},
        {"POST", "/array",
         "{\"items\":[{\"text\":\"one\"},{\"text\":\"two\"}]}", 200, "array"},
        {"POST", "/xml", "<doc id=\"7\">xml</doc>", 200,
         "<doc id=\"7\">xml</doc>"},
        {"POST", "/xml-live", "<doc id=\"7\">xml</doc>", 200,
         "<doc id=\"7\">xml</doc>"},
        {"POST", "/malformed", "{\"text\":", 400, NULL},
        {"POST", "/malformed", "{}", 400, NULL},
        {"POST", "/denied", "{", 403, NULL},
        {"POST", "/blocked", "{", 403, "blocked"},
        {"POST", "/empty", "", 204, ""},
        {"POST", "/captured", "{\"payload\":{\"n\":1}}", 200,
         "{\"payload\":{\"n\":1}}"},
        {"GET", "/discard", "", 200, "discarded"},
        {"GET", "/bad-codec", "", 500, NULL}};
    unsigned short port = (unsigned short)luaL_checkinteger(L, 1);
    size_t i, length = 65536 + 11;
    char *large;
    int failed = 0;
    child = fork();
    if (child < 0)
        return luaL_error(L, "fork failed");
    if (child == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        for (i = 0; !failed && i < sizeof(cases) / sizeof(cases[0]); ++i) {
            required_header =
                strcmp(cases[i].path, "/echo") == 0
                    ? "content-type: application/vnd.rotmos+json\r\n"
                    : NULL;
            failed = rotmos_lua_test_exchange(
                port, cases[i].method, cases[i].path, cases[i].input,
                strlen(cases[i].input), cases[i].status, cases[i].output,
                cases[i].output != NULL ? strlen(cases[i].output) : 0);
        }
        required_header = NULL;
        large = malloc(length + 1);
        if (large == NULL)
            failed = 1;
        else {
            memcpy(large, "{\"text\":\"", 9);
            memset(large + 9, 'x', 65536);
            memcpy(large + 65536 + 9, "\"}", 3);
            if (!failed)
                failed = rotmos_lua_test_exchange(port, "POST", "/large", large,
                                                  length, 200, large, length);
            free(large);
        }
        if (!failed)
            failed = rotmos_lua_test_exchange(port, "GET", "/stop", "", 0, 200,
                                              "ok", 2);
        if (failed)
            kill(getppid(), SIGTERM);
        _exit(failed);
    }
    lua_pushboolean(L, 1);
    return 1;
}
static int spawn_rest(lua_State *L)
{
    static const struct {
        const char *method, *path, *input;
        int status;
        const char *output;
    } cases[] = {{"POST", "/echo", "{\"n\":2}", 202, "{\"n\":2}\n"},
                 {"POST", "/echo", "{", 400, NULL},
                 {"POST", "/validate", "{}", 400, NULL},
                 {"POST", "/validate-reply", "{}", 422, "validation"},
                 {"POST", "/validate-throw", "{}", 400, NULL},
                 {"GET", "/throw", "", 500, NULL},
                 {"GET", "/recover", "", 418, "recovered"},
                 {"POST", "/raw", "not JSON", 201, "not JSON"},
                 {"GET", "/empty", "", 204, ""},
                 {"POST", "/null", "null", 200, "true\n"},
                 {"GET", "/source", "", 200, "live"},
                 {"POST", "/api/one", "{\"n\":1}", 201, "{\"n\":1}"},
                 {"POST", "/native/r", "{}", 200, "{}\n"},
                 {"GET", "/bounded", "", 500, NULL},
                 {"POST", "/blocked", "{", 401, "blocked"},
                 {"POST", "/denied", "{", 403, NULL},
                 {"POST", "/limited", "{\"n\":123456}", 413, NULL},
                 {"GET", "/docs", "", 200, NULL},
                 {"GET", "/stop", "", 200, "ok"}};
    unsigned short port = (unsigned short)luaL_checkinteger(L, 1);
    size_t i;
    int failed = 0;
    child = fork();
    if (child < 0)
        return luaL_error(L, "fork failed");
    if (child == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        for (i = 0; !failed && i < sizeof(cases) / sizeof(cases[0]); ++i)
            failed = rotmos_lua_test_exchange(
                port, cases[i].method, cases[i].path, cases[i].input,
                strlen(cases[i].input), cases[i].status, cases[i].output,
                cases[i].output != NULL ? strlen(cases[i].output) : 0);
        if (failed)
            kill(getppid(), SIGTERM);
        _exit(failed);
    }
    lua_pushboolean(L, 1);
    return 1;
}
static int closed_exchange(unsigned short port, const char *path)
{
    struct sockaddr_in address;
    struct timeval timeout;
    int fd, valid;
    ssize_t length;
    char request[256], byte;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 1;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return 1;
    }
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
             path);
    (void)write(fd, request, strlen(request));
    length = read(fd, &byte, 1);
    valid = length == 0 || (length < 0 && errno == ECONNRESET);
    close(fd);
    return !valid;
}
static int spawn_observations(lua_State *L)
{
    static const struct {
        const char *method, *path, *input;
        size_t input_length;
        int status;
        const char *output;
        size_t output_length;
    } cases[] = {{"POST", "/echo", "A\0B", 3, 200, "A\0B", 3},
                 {"GET", "/fallback", "", 0, 200, "fallback", 8},
                 {"GET", "/bad-ip", "", 0, 500, NULL, 0},
                 {"GET", "/ip-nul", "", 0, 500, NULL, 0},
                 {"GET", "/bad-correlation", "", 0, 500, NULL, 0},
                 {"GET", "/long-id", "", 0, 500, NULL, 0},
                 {"GET", "/throw-resolver", "", 0, 500, NULL, 0},
                 {"GET", "/start-fails", "", 0, 500, NULL, 0},
                 {"GET", "/reject", "", 0, 429, "R\0J", 3},
                 {"GET", "/reject-default", "", 0, 403, "", 0},
                 {"GET", "/admit-bad", "", 0, 500, NULL, 0},
                 {"GET", "/admit-source", "", 0, 500, NULL, 0},
                 {"GET", "/admit-getter", "", 0, 500, NULL, 0},
                 {"GET", "/admit-oom", "", 0, 500, NULL, 0},
                 {"POST", "/before", "original", 8, 200, "changed", 7},
                 {"GET", "/event", "", 0, 200, "event", 5},
                 {"GET", "/tls", "", 0, 200, "tls", 3},
                 {"GET", "/stop", "", 0, 200, "ok", 2}};
    unsigned short http = (unsigned short)luaL_checkinteger(L, 1);
    unsigned short tls = (unsigned short)luaL_checkinteger(L, 2);
    int failed;
    size_t i;
    child = fork();
    if (child < 0)
        return luaL_error(L, "fork failed");
    if (child == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        failed = closed_exchange(http, "/connection-denied");
        if (!failed)
            failed = closed_exchange(http, "/close");
        for (i = 0; !failed && i < sizeof(cases) / sizeof(cases[0]); ++i) {
            encrypted = !strcmp(cases[i].path, "/tls");
            required_header = !strcmp(cases[i].path, "/reject")
                                  ? "X-Admission: copied\r\n"
                                  : NULL;
            failed = rotmos_lua_test_exchange(
                encrypted ? tls : http, cases[i].method, cases[i].path,
                cases[i].input, cases[i].input_length, cases[i].status,
                cases[i].output, cases[i].output_length);
        }
        if (failed)
            kill(getppid(), SIGTERM);
        _exit(failed);
    }
    lua_pushboolean(L, 1);
    return 1;
}
static int spawn_single(lua_State *L)
{
    unsigned short port = (unsigned short)luaL_checkinteger(L, 1);
    int tls = lua_toboolean(L, 2);
    int failed;
    child = fork();
    if (child < 0)
        return luaL_error(L, "fork failed");
    if (child == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        encrypted = tls;
        failed =
            rotmos_lua_test_exchange(port, "GET", "/", "", 0, 200, "ok", 2);
        if (failed)
            kill(getppid(), SIGTERM);
        _exit(failed);
    }
    lua_pushboolean(L, 1);
    return 1;
}
static int fd_pair(lua_State *L)
{
    int descriptors[2];
    if (pipe(descriptors))
        return luaL_error(L, "control pipe failed");
    if (fcntl(descriptors[0], F_SETFL, O_NONBLOCK)) {
        close(descriptors[0]);
        close(descriptors[1]);
        return luaL_error(L, "control pipe flags failed");
    }
    lua_pushinteger(L, descriptors[0]);
    lua_pushinteger(L, descriptors[1]);
    return 2;
}
static int fd_read(lua_State *L)
{
    int fd = (int)luaL_checkinteger(L, 1);
    char buffer[64];
    ssize_t length = read(fd, buffer, sizeof(buffer));
    if (length < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            lua_pushnil(L);
            return 1;
        }
        return luaL_error(L, "control pipe read failed");
    }
    lua_pushlstring(L, buffer, (size_t)length);
    return 1;
}
static int fd_write(lua_State *L)
{
    int fd = (int)luaL_checkinteger(L, 1);
    size_t length;
    const char *value = luaL_checklstring(L, 2, &length);
    if (write(fd, value, length) != (ssize_t)length)
        return luaL_error(L, "control pipe write failed");
    return 0;
}
static int fd_close(lua_State *L)
{
    if (close((int)luaL_checkinteger(L, 1)))
        return luaL_error(L, "control pipe close failed");
    return 0;
}
static int fd_closed(lua_State *L)
{
    int fd = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    return 1;
}
static int private_directory(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    if ((mkdir(path, 0700) && errno != EEXIST) || chmod(path, 0700))
        return luaL_error(L, "preparing private directory failed");
    return 0;
}
static int join(lua_State *L)
{
    int status;
    if (child <= 0 || waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return luaL_error(L, "HTTP driver failed");
    child = 0;
    if (progress_write >= 0) {
        close(progress_write);
        progress_write = -1;
    }
    lua_pushboolean(L, 1);
    return 1;
}
static int upload_progress(lua_State *L)
{
    if (progress_write < 0 || write(progress_write, "p", 1) != 1)
        return luaL_error(L, "upload progress acknowledgement failed");
    return 0;
}
static int prepare_body(lua_State *L)
{
    int fds[2];
    if (pipe(fds) != 0)
        return luaL_error(L, "progress pipe failed");
    progress_read = fds[0];
    progress_write = fds[1];
    return 0;
}
static int drive_body(unsigned short port)
{
    static const char *const failures[] = {"open",   "chunk",  "oom",
                                           "finish", "getter", "close"};
    char path[64];
    size_t i;
    int failed;
    failed = rotmos_lua_test_exchange(port, "POST", "/materialized", "A\0BCDE",
                                      6, 200, "A\0BCDE", 6);
    if (!failed)
        failed = rotmos_lua_test_exchange(port, "GET", "/saved", "", 0, 200,
                                          "A\0BCDE", 6);
    if (!failed)
        failed = rotmos_lua_test_exchange(port, "GET", "/check-saved", "", 0,
                                          200, "ok", 2);
    if (!failed)
        failed = rotmos_lua_test_exchange(port, "POST", "/upload/live",
                                          "A\0BCDE", 6, 200, "6", 1);
    if (!failed)
        failed =
            rotmos_lua_test_exchange(port, "POST", "/empty", "", 0, 204, "", 0);
    if (!failed)
        failed = rotmos_lua_test_exchange(port, "POST", "/paused", "A\0BCDE", 6,
                                          200, "6", 1);
    for (i = 0; !failed && i < sizeof(failures) / sizeof(failures[0]); ++i) {
        snprintf(path, sizeof(path), "/failure/%s", failures[i]);
        failed = rotmos_lua_test_exchange(port, "POST", path, "A\0BCDE", 6, 500,
                                          "", 0);
    }
    if (!failed)
        failed = rotmos_lua_test_exchange(port, "POST", "/failure/invalid",
                                          "A\0BCDE", 6, 500, "", 0);
    if (!failed)
        failed =
            rotmos_lua_test_exchange(port, "POST", "/limit", "ABC", 3, 413,
                                     "request body exceeds route limit", 32);
    if (!failed)
        failed = rotmos_lua_test_exchange(port, "POST", "/denied", "ABC", 3,
                                          403, "", 0);
    if (!failed)
        failed =
            rotmos_lua_test_exchange(port, "GET", "/stop", "", 0, 200, "ok", 2);
    return failed;
}
static int spawn_body(lua_State *L)
{
    int status;
    unsigned short port = (unsigned short)lua_tointeger(L, 1);
    child = fork();
    if (child < 0)
        return luaL_error(L, "fork failed");
    if (child == 0) {
        close(progress_write);
        progress_write = -1;
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        status = drive_body(port);
        close(progress_read);
        if (status)
            kill(getppid(), SIGTERM);
        _exit(status);
    }
    close(progress_read);
    progress_read = -1;
    lua_pushboolean(L, 1);
    return 1;
}
static int drive_routes(unsigned short port)
{
    int failed = 0;
#define R(method, path, body, length, status, expected, size)                  \
    if (!failed)                                                               \
    failed = rotmos_lua_test_exchange(port, method, path, body, length,        \
                                      status, expected, size)
    R("GET", "/file", "", 0, 200, "A\0B", 3);
    required_header = "content-length: 3\r\n";
    R("HEAD", "/file", "", 0, 200, "", 0);
    required_header = "content-range: bytes 1-2/3\r\n";
    request_headers = "Range: bytes=1-2\r\n";
    R("GET", "/file", "", 0, 206, "\0B", 2);
    required_header = NULL;
    request_headers = "If-None-Match: *\r\n";
    R("GET", "/file", "", 0, 304, "", 0);
    request_headers = "";
    required_header = "location: /files/\r\n";
    R("GET", "/files", "", 0, 308, "", 0);
    required_header = NULL;
    R("GET", "/files/", "", 0, 200, "index\n", 6);
    R("GET", "/files/missing", "", 0, 404, "", 0);
    R("GET", "/group/file", "", 0, 200, "A\0B", 3);
    R("GET", "/replace", "", 0, 202, "replaced", 8);
    R("GET", "/direct", "", 0, 201, "direct", 6);
    required_body = "expected native after failure";
    R("GET", "/after-error", "", 0, 500, NULL, 0);
    required_body = "memory";
    R("GET", "/after-oom", "", 0, 500, NULL, 0);
    required_body = NULL;
    R("POST", "/upload-file", "A\0B", 3, 200, "3", 1);
    R("POST", "/upload-limit", "four", 4, 413,
      "request body exceeds route limit", 32);
    R("PUT", "/dav/file", "A\0B", 3, 201, "", 0);
    R("GET", "/dav/file", "", 0, 200, "A\0B", 3);
    request_headers = "Depth: 1\r\n";
    required_body = "/dav/file";
    R("PROPFIND", "/dav/", "", 0, 207, NULL, 0);
    required_body = NULL;
    request_headers = "";
    R("GET", "/dav/secret", "", 0, 404, "", 0);
    required_header = "www-authenticate: Basic realm=\"routes\"\r\n";
    R("GET", "/dav/challenge", "", 0, 401, "auth", 4);
    required_header = "location: /login\r\n";
    R("GET", "/dav/redirect", "", 0, 302, "", 0);
    required_header = NULL;
    R("GET", "/dav/error", "", 0, 500, "", 0);
    R("GET", "/dav/oom", "", 0, 500, "", 0);
    R("PUT", "/dav/oversized", "four", 4, 413, "", 0);
    R("DELETE", "/dav/file", "", 0, 204, "", 0);
    R("GET", "/dav/file", "", 0, 404, "", 0);
    R("GET", "/stop", "", 0, 200, "ok", 2);
#undef R
    return failed;
}
static int spawn_routes(lua_State *L)
{
    int status;
    unsigned short port = (unsigned short)lua_tointeger(L, 1);
    child = fork();
    if (child < 0)
        return luaL_error(L, "fork failed");
    if (child == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        status = drive_routes(port);
        if (status)
            kill(getppid(), SIGTERM);
        _exit(status);
    }
    lua_pushboolean(L, 1);
    return 1;
}
static int proxy_socket(int serving, unsigned short *port)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) ||
        getsockname(fd, (struct sockaddr *)&address, &length) ||
        (serving && listen(fd, 8))) {
        close(fd);
        return -1;
    }
    *port = ntohs(address.sin_port);
    return fd;
}
static int origin_send(int fd, SSL *ssl, const void *bytes, size_t length)
{
    ssize_t count;
    const char *cursor = bytes;
    while (length) {
        count = ssl ? SSL_write(ssl, cursor, (int)length)
                    : write(fd, cursor, length);
        if (count <= 0)
            return 0;
        cursor += count;
        length -= (size_t)count;
    }
    return 1;
}
static int origin_request(int fd, SSL_CTX *context)
{
    SSL *ssl = NULL;
    char *buffer = malloc(65536), head[256], *end, *body, *field;
    size_t used = 0, size = 0, received;
    const char *answer = "origin", *type = "text/plain";
    ssize_t count;
    int failed = 1, acknowledged = 0;
    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (buffer == NULL)
        goto done;
    if (context != NULL) {
        ssl = SSL_new(context);
        if (ssl == NULL || !SSL_set_fd(ssl, fd))
            goto done;
        /* Deliberately untrusted/missing-client-certificate routes exercise
         * gateway failure, so a rejected handshake is an expected connection.
         */
        if (SSL_accept(ssl) != 1) {
            failed = 0;
            goto done;
        }
    }
    do {
        count = ssl ? SSL_read(ssl, buffer + used, (int)(65535 - used))
                    : read(fd, buffer + used, 65535 - used);
        if (count <= 0)
            goto done;
        used += (size_t)count;
        buffer[used] = 0;
        end = strstr(buffer, "\r\n\r\n");
    } while (end == NULL && used < 65535);
    if (end == NULL)
        goto done;
    body = end + 4;
    field = strstr(buffer, "Content-Length:");
    if (field == NULL)
        field = strstr(buffer, "content-length:");
    if (field != NULL && field < end)
        size = strtoul(field + 15, NULL, 10);
    if (size > 32768)
        goto done;
    received = used - (size_t)(body - buffer);
    while (received < size) {
        if (strstr(buffer, " /base/echo ") && received >= 3 && !acknowledged) {
            if (write(progress_write, "p", 1) != 1)
                goto done;
            acknowledged = 1;
        }
        count = ssl ? SSL_read(ssl, buffer + used, (int)(65535 - used))
                    : read(fd, buffer + used, 65535 - used);
        if (count <= 0)
            goto done;
        used += (size_t)count;
        received += (size_t)count;
        buffer[used] = 0;
    }
    if (strstr(buffer, " /base/echo ")) {
        if (size != 6 || memcmp(body, "A\0BCDE", 6))
            goto done;
        answer = body;
        size = 6;
    } else if (strstr(buffer, " /alternate/check?q=changed ")) {
        if (strncmp(buffer, "PATCH ", 6) ||
            !strcasestr(buffer, "Host: changed.test:9\r\n") ||
            !strcasestr(buffer, "X-Added: one\r\n") ||
            !strcasestr(buffer, "X-Added: two\r\n") ||
            strcasestr(buffer, "X-Test:"))
            goto done;
        answer = "rewritten";
        size = 9;
    } else if (strstr(buffer, " /base/proxy/sse ")) {
        answer = "data: first\n\ndata: second\n\n";
        size = 27;
        type = "text/event-stream";
    } else if (context != NULL) {
        answer = "tls";
        size = 3;
    } else
        size = strlen(answer);
    snprintf(head, sizeof(head),
             "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: "
             "%lu\r\nX-Origin: selected\r\nX-Remove: yes\r\nConnection: "
             "close\r\n\r\n",
             type, (unsigned long)size);
    if (!origin_send(fd, ssl, head, strlen(head)) ||
        !origin_send(fd, ssl, answer, size))
        goto done;
    failed = 0;
done:
    if (ssl != NULL) {
        (void)SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    close(fd);
    free(buffer);
    return failed;
}
struct origin_ports {
    unsigned short http, tls, unavailable;
};
static int origin_publish(lua_State *L)
{
    struct origin_ports *ports = lua_touserdata(L, 1);
    lua_newtable(L);
    lua_pushfstring(L, "http://127.0.0.1:%d/base", ports->http);
    lua_setfield(L, -2, "http");
    lua_pushfstring(L, "https://localhost:%d/base", ports->tls);
    lua_setfield(L, -2, "tls");
    lua_pushfstring(L, "http://127.0.0.1:%d", ports->unavailable);
    lua_setfield(L, -2, "unavailable");
    return 1;
}
static int proxy_setup(lua_State *L)
{
    struct origin_ports ports;
    struct pollfd ready[3];
    SSL_CTX *context = NULL;
    X509_STORE *store;
    int sockets[3] = {-1, -1, -1}, quit[2] = {-1, -1}, i, fd, failed = 0,
        published = LUA_OK;
    if (client_context == NULL || progress_write < 0)
        goto failed;
    sockets[0] = proxy_socket(1, &ports.http);
    sockets[1] = proxy_socket(1, &ports.tls);
    sockets[2] = proxy_socket(0, &ports.unavailable);
    if (sockets[0] < 0 || sockets[1] < 0 || sockets[2] < 0 || pipe(quit))
        goto failed;
    context = SSL_CTX_new(TLS_server_method());
    store = SSL_CTX_get_cert_store(client_context);
    if (context == NULL ||
        !SSL_CTX_use_certificate(context,
                                 SSL_CTX_get0_certificate(client_context)) ||
        !SSL_CTX_use_PrivateKey(context,
                                SSL_CTX_get0_privatekey(client_context)) ||
        !X509_STORE_up_ref(store))
        goto failed;
    SSL_CTX_set_cert_store(context, store);
    SSL_CTX_set_verify(context,
                       SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    lua_pushcfunction(L, origin_publish);
    lua_pushlightuserdata(L, &ports);
    published = lua_pcall(L, 1, 1, 0);
    if (published != LUA_OK)
        goto failed;
    proxy_origin = fork();
    if (proxy_origin < 0)
        goto failed;
    if (proxy_origin == 0) {
        close(quit[1]);
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        ready[0].fd = sockets[0];
        ready[1].fd = sockets[1];
        ready[2].fd = quit[0];
        for (i = 0; i < 3; ++i)
            ready[i].events = POLLIN;
        while (!failed && poll(ready, 3, -1) > 0) {
            if (ready[2].revents)
                break;
            for (i = 0; i < 2; ++i)
                if (ready[i].revents & POLLIN) {
                    fd = accept(sockets[i], NULL, NULL);
                    if (fd < 0 || origin_request(fd, i ? context : NULL))
                        failed = 1;
                }
        }
        for (i = 0; i < 3; ++i)
            close(sockets[i]);
        close(quit[0]);
        SSL_CTX_free(context);
        _exit(failed);
    }
    proxy_shutdown = quit[1];
    quit[1] = -1;
    for (i = 0; i < 3; ++i)
        close(sockets[i]);
    close(quit[0]);
    SSL_CTX_free(context);
    return 1;
failed:
    for (i = 0; i < 3; ++i)
        if (sockets[i] >= 0)
            close(sockets[i]);
    if (quit[0] >= 0)
        close(quit[0]);
    if (quit[1] >= 0)
        close(quit[1]);
    SSL_CTX_free(context);
    return published != LUA_OK
               ? lua_error(L)
               : luaL_error(L, "proxy origin preparation failed");
}
static int proxy_join(lua_State *L)
{
    int status;
    if (proxy_origin <= 0 || proxy_shutdown < 0 ||
        write(proxy_shutdown, "q", 1) != 1)
        return luaL_error(L, "proxy origin shutdown failed");
    close(proxy_shutdown);
    proxy_shutdown = -1;
    if (waitpid(proxy_origin, &status, 0) != proxy_origin ||
        !WIFEXITED(status) || WEXITSTATUS(status))
        return luaL_error(L, "proxy origin failed");
    proxy_origin = 0;
    lua_pushboolean(L, 1);
    return 1;
}
static int drive_proxy(unsigned short port)
{
    static const char *const local_failures[] = {
        "preflight-error", "preflight-oom", "local-getter", "local-header",
        "local-body"};
    static const char *const invalid_rewrites[] = {"invalid", "nul", "method",
                                                   "type"};
    static const char *const gateway_failures[] = {
        "rewrite-error", "response-error", "response-oom", "response-invalid",
        "response-nul",  "response-type",  "gateway"};
    char path[64];
    size_t i;
    int failed = 0;
#define P(method, path, body, length, status, expected, size)                  \
    if (!failed)                                                               \
    failed = rotmos_lua_test_exchange(port, method, path, body, length,        \
                                      status, expected, size)
    P("GET", "/proxy/plain?client=1", "", 0, 201, "origin", 6);
    P("POST", "/proxy/echo", "A\0BCDE", 6, 201, "A\0BCDE", 6);
    required_header = "X-Proxy: rotmos\r\n";
    P("GET", "/param/id", "", 0, 201, "rewritten", 9);
    required_header = "Content-Type: text/event-stream\r\n";
    P("GET", "/proxy/sse", "", 0, 200, "data: first\n\ndata: second\n\n", 27);
    required_header = NULL;
    P("GET", "/secure", "", 0, 201, "tls", 3);
    P("GET", "/local", "", 0, 403, "A\0B", 3);
    omit_length = 1;
    connection_value = "Upgrade";
    request_headers = "Upgrade: websocket\r\nSec-WebSocket-Version: "
                      "13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
    P("GET", "/proxy/ws", "", 0, 403, "ws", 2);
    omit_length = 0;
    connection_value = "close";
    request_headers = "";
    for (i = 0;
         !failed && i < sizeof(local_failures) / sizeof(local_failures[0]);
         ++i) {
        snprintf(path, sizeof(path), "/%s", local_failures[i]);
        P("GET", path, "", 0, 500, NULL, 0);
    }
    for (i = 0;
         !failed && i < sizeof(gateway_failures) / sizeof(gateway_failures[0]);
         ++i) {
        snprintf(path, sizeof(path), "/%s", gateway_failures[i]);
        P("GET", path, "", 0, 503, "gateway", 7);
    }
    P("GET", "/rewrite-oom", "", 0, 500, "proxy setup or rewrite failed", 29);
    for (i = 0;
         !failed && i < sizeof(invalid_rewrites) / sizeof(invalid_rewrites[0]);
         ++i) {
        snprintf(path, sizeof(path), "/rewrite-%s", invalid_rewrites[i]);
        P("GET", path, "", 0, 400, "proxy setup or rewrite failed", 29);
    }
    P("GET", "/gateway-error", "", 0, 502, "bad gateway", 11);
    P("GET", "/gateway-getter", "", 0, 502, "bad gateway", 11);
    P("GET", "/stop", "", 0, 200, "ok", 2);
#undef P
    return failed;
}
static int spawn_proxy(lua_State *L)
{
    int status;
    unsigned short port = (unsigned short)lua_tointeger(L, 1);
    child = fork();
    if (child < 0)
        return luaL_error(L, "fork failed");
    if (child == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        close(progress_write);
        progress_write = -1;
        status = drive_proxy(port);
        if (status)
            kill(getppid(), SIGTERM);
        _exit(status);
    }
    close(progress_read);
    progress_read = -1;
    lua_pushboolean(L, 1);
    return 1;
}
static int events_pipe(lua_State *L)
{
    if (pipe(event_pipe) || fcntl(event_pipe[0], F_SETFL, O_NONBLOCK))
        return luaL_error(L, "event pipe failed");
    lua_pushinteger(L, event_pipe[0]);
    lua_pushinteger(L, event_pipe[1]);
    return 2;
}
static int events_read(lua_State *L)
{
    char byte;
    if (read(event_pipe[0], &byte, 1) != 1 || byte != 'w')
        return luaL_error(L, "event pipe read failed");
    return 0;
}
static int events_cleanup(lua_State *L)
{
    (void)L;
    close(event_pipe[0]);
    close(event_pipe[1]);
    event_pipe[0] = event_pipe[1] = -1;
    return 0;
}
static int spawn_events(lua_State *L)
{
    int status;
    unsigned short http = (unsigned short)lua_tointeger(L, 1),
                   tls = (unsigned short)lua_tointeger(L, 2);
    child = fork();
    if (child < 0)
        return luaL_error(L, "fork failed");
    if (child == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        status =
            rotmos_lua_test_events(http, tls, client_context, event_pipe[1]);
        if (status)
            kill(getppid(), SIGTERM);
        _exit(status);
    }
    lua_pushboolean(L, 1);
    return 1;
}
static int drive_policy(unsigned short http, unsigned short tls,
                        unsigned short redirect)
{
    int failed;
    encrypted = 1;
    required_header = NULL;
    failed = rotmos_lua_test_exchange(tls, "GET", "/tls", "", 0, 200, "tls", 3);
    encrypted = 0;
    required_header = "location: https://localhost:9443/raw%2Fpath?x=%2F\r\n";
    if (!failed)
        failed = rotmos_lua_test_exchange(redirect, "GET", "/raw%2Fpath?x=%2F",
                                          "", 0, 308, "", 0);
    required_header = "cache-control: no-store\r\n";
    if (!failed)
        failed = rotmos_lua_test_exchange(http, "GET", "/allow", "", 0, 200,
                                          "allowed", 7);
    if (!failed)
        failed = rotmos_lua_test_exchange(http, "GET", "/principal", "", 0, 200,
                                          "principal", 9);
    required_header = "www-authenticate: Basic realm=\"fixture\"\r\n";
    if (!failed)
        failed = rotmos_lua_test_exchange(http, "GET", "/required", "", 0, 401,
                                          "authentication required\n", 24);
    required_header = "location: /login\r\n";
    if (!failed)
        failed = rotmos_lua_test_exchange(http, "GET", "/redirect", "", 0, 302,
                                          "authentication required\n", 24);
    required_header = NULL;
    if (!failed)
        failed = rotmos_lua_test_exchange(http, "GET", "/deny", "", 0, 403,
                                          "A\0B", 3);
    if (!failed)
        failed = rotmos_lua_test_exchange(http, "GET", "/auth-error", "", 0,
                                          500, "", 0);
    if (!failed)
        failed = rotmos_lua_test_exchange(http, "GET", "/auth-invalid", "", 0,
                                          400, "", 0);
    if (!failed)
        failed = rotmos_lua_test_exchange(http, "GET", "/auth-getter", "", 0,
                                          500, "", 0);
    if (!failed)
        failed = rotmos_lua_test_exchange(http, "GET", "/auth-oom", "", 0, 500,
                                          "", 0);
    if (!failed)
        failed = rotmos_lua_test_exchange(http, "GET", "/guard/entry", "", 0,
                                          200, "group", 5);
    if (!failed)
        failed = rotmos_lua_test_exchange(http, "GET", "/event", "", 0, 200,
                                          "event", 5);
    if (!failed)
        failed = rotmos_lua_test_exchange(http, "GET", "/event", "", 0, 200,
                                          "event", 5);
    if (!failed)
        failed = rotmos_lua_test_exchange(http, "GET", "/blocked", "", 0, 403,
                                          "", 0);
    forwarded_address = "203.0.113.9";
    if (!failed)
        failed = rotmos_lua_test_exchange(http, "GET", "/stop", "", 0, 200,
                                          "stopped", 7);
    return failed;
}
static int spawn_policy(lua_State *L)
{
    unsigned short http = (unsigned short)lua_tointeger(L, 1);
    unsigned short tls = (unsigned short)lua_tointeger(L, 2);
    unsigned short redirect = (unsigned short)lua_tointeger(L, 3);
    int status;
    child = fork();
    if (child < 0)
        return luaL_error(L, "fork failed");
    if (child == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        status = drive_policy(http, tls, redirect);
        if (status)
            kill(getppid(), SIGTERM);
        _exit(status);
    }
    lua_pushboolean(L, 1);
    return 1;
}
static int extension(X509 *certificate, X509 *issuer, int nid, const char *text)
{
    X509V3_CTX context;
    X509_EXTENSION *item;
    int ok;
    X509V3_set_ctx(&context, issuer, certificate, NULL, NULL, 0);
    item = X509V3_EXT_conf_nid(NULL, &context, nid, text);
    if (item == NULL)
        return 0;
    ok = X509_add_ext(certificate, item, -1);
    X509_EXTENSION_free(item);
    return ok;
}
static X509 *certificate(EVP_PKEY *key, X509 *ca, EVP_PKEY *signer)
{
    X509 *out = X509_new();
    X509_NAME *name;
    if (out == NULL)
        return NULL;
    name = X509_get_subject_name(out);
    if (!X509_set_version(out, 2) ||
        !ASN1_INTEGER_set(X509_get_serialNumber(out), ca ? 2 : 1) ||
        X509_gmtime_adj(X509_getm_notBefore(out), -60) == NULL ||
        X509_gmtime_adj(X509_getm_notAfter(out), 3600) == NULL ||
        !X509_set_pubkey(out, key) ||
        !X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            (const unsigned char *)(ca ? "localhost" : "Lua fixture CA"), -1,
            -1, 0) ||
        !X509_set_issuer_name(out, ca ? X509_get_subject_name(ca) : name) ||
        !extension(out, ca ? ca : out, NID_basic_constraints,
                   ca ? "critical,CA:FALSE" : "critical,CA:TRUE") ||
        !extension(out, ca ? ca : out, NID_key_usage,
                   ca ? "critical,digitalSignature"
                      : "critical,keyCertSign,cRLSign") ||
        (ca &&
         (!extension(out, ca, NID_ext_key_usage, "serverAuth,clientAuth") ||
          !extension(out, ca, NID_subject_alt_name,
                     "DNS:localhost,DNS:bundle.local"))) ||
        !X509_sign(out, signer, NULL)) {
        X509_free(out);
        return NULL;
    }
    return out;
}
struct tls_material {
    BIO *certificate, *private_key, *ca;
    const char *path;
};
static void pem_field(lua_State *L, const char *name, BIO *pem)
{
    char *bytes;
    long length = BIO_get_mem_data(pem, &bytes);
    lua_pushlstring(L, bytes, (size_t)length);
    lua_setfield(L, -2, name);
}
static int publish_material(lua_State *L)
{
    struct tls_material *material = lua_touserdata(L, 1);
    lua_newtable(L);
    pem_field(L, "certificate", material->certificate);
    pem_field(L, "private_key", material->private_key);
    pem_field(L, "ca", material->ca);
    lua_pushstring(L, material->path);
    lua_setfield(L, -2, "private_key_path");
    return 1;
}
static int tls_setup(lua_State *L)
{
    struct tls_material material;
    EVP_PKEY *key = NULL, *signer = NULL;
    X509 *ca = NULL, *cert = NULL;
    SSL_CTX *context = NULL;
    FILE *file = NULL;
    int fd = -1, ok = 0, status = LUA_OK;
    memset(&material, 0, sizeof(material));
    material.path = luaL_checkstring(L, 1);
    signer = EVP_PKEY_Q_keygen(NULL, NULL, "ED25519");
    key = EVP_PKEY_Q_keygen(NULL, NULL, "ED25519");
    if (signer == NULL || key == NULL ||
        (ca = certificate(signer, NULL, signer)) == NULL ||
        (cert = certificate(key, ca, signer)) == NULL)
        goto done;
    material.certificate = BIO_new(BIO_s_mem());
    material.private_key = BIO_new(BIO_s_mem());
    material.ca = BIO_new(BIO_s_mem());
    context = SSL_CTX_new(TLS_client_method());
    if (material.certificate == NULL || material.private_key == NULL ||
        material.ca == NULL || context == NULL ||
        !PEM_write_bio_X509(material.certificate, cert) ||
        !PEM_write_bio_X509(material.ca, ca) ||
        !PEM_write_bio_PrivateKey(material.private_key, key, NULL, NULL, 0,
                                  NULL, NULL) ||
        !SSL_CTX_use_certificate(context, cert) ||
        !SSL_CTX_use_PrivateKey(context, key) ||
        !X509_STORE_add_cert(SSL_CTX_get_cert_store(context), ca))
        goto done;
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER, NULL);
    fd = open(material.path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        goto done;
    file = fdopen(fd, "w");
    if (file == NULL)
        goto done;
    fd = -1;
    if (!PEM_write_PrivateKey(file, key, NULL, NULL, 0, NULL, NULL))
        goto done;
    if (fclose(file) != 0) {
        file = NULL;
        goto done;
    }
    file = NULL;
    SSL_CTX_free(client_context);
    client_context = context;
    context = NULL;
    lua_pushcfunction(L, publish_material);
    lua_pushlightuserdata(L, &material);
    status = lua_pcall(L, 1, 1, 0);
    ok = 1;
done:
    if (file != NULL)
        fclose(file);
    if (fd >= 0)
        close(fd);
    SSL_CTX_free(context);
    X509_free(cert);
    X509_free(ca);
    EVP_PKEY_free(key);
    EVP_PKEY_free(signer);
    BIO_free(material.certificate);
    BIO_free(material.private_key);
    BIO_free(material.ca);
    if (!ok)
        return luaL_error(L, "TLS fixture preparation failed");
    if (status != LUA_OK)
        return lua_error(L);
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
    if (argc != 3 && argc != 4)
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
#endif
    lua_getglobal(L, "package");
    lua_pushfstring(L,
                    "%s/share/lua/5.5/?.lua;%s/share/lua/5.5/?/init.lua;%s/"
                    "share/lua/5.5/?/init.lua",
                    argv[2], ROTMOS_LONEJSON_LUA_PREFIX,
                    ROTMOS_PSLOG_LUA_PREFIX);
    lua_setfield(L, -2, "path");
    lua_pushfstring(
        L,
        "%s/lib/lua/5.5/?.so;%s/lib/lua/5.5/?.so;%s/lib/lua/5.5/?.so;%s/?.so",
        argv[2], ROTMOS_LONEJSON_LUA_PREFIX, ROTMOS_PSLOG_LUA_PREFIX,
        ROTMOS_TEST_MODULE_DIR);
    lua_setfield(L, -2, "cpath");
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushcfunction(L, spawn);
    lua_setfield(L, -2, "spawn");
    lua_pushcfunction(L, spawn_logging);
    lua_setfield(L, -2, "spawn_logging");
    lua_pushcfunction(L, spawn_providers);
    lua_setfield(L, -2, "spawn_providers");
    lua_pushcfunction(L, join);
    lua_setfield(L, -2, "join");
    lua_pushcfunction(L, fail_next);
    lua_setfield(L, -2, "fail_next");
    lua_pushcfunction(L, fail_call);
    lua_setfield(L, -2, "fail_call");
    lua_pushcfunction(L, tls_setup);
    lua_setfield(L, -2, "tls_setup");
    lua_pushcfunction(L, spawn_policy);
    lua_setfield(L, -2, "spawn_policy");
    lua_pushcfunction(L, spawn_body);
    lua_setfield(L, -2, "spawn_body");
    lua_pushcfunction(L, upload_progress);
    lua_setfield(L, -2, "upload_progress");
    lua_pushcfunction(L, prepare_body);
    lua_setfield(L, -2, "prepare_body");
    lua_pushcfunction(L, spawn_routes);
    lua_setfield(L, -2, "spawn_routes");
    lua_pushcfunction(L, proxy_setup);
    lua_setfield(L, -2, "proxy_setup");
    lua_pushcfunction(L, proxy_join);
    lua_setfield(L, -2, "proxy_join");
    lua_pushcfunction(L, spawn_proxy);
    lua_setfield(L, -2, "spawn_proxy");
    lua_pushcfunction(L, events_pipe);
    lua_setfield(L, -2, "events_pipe");
    lua_pushcfunction(L, events_read);
    lua_setfield(L, -2, "events_read");
    lua_pushcfunction(L, events_cleanup);
    lua_setfield(L, -2, "events_cleanup");
    lua_pushcfunction(L, spawn_events);
    lua_setfield(L, -2, "spawn_events");
    lua_pushcfunction(L, spawn_codecs);
    lua_setfield(L, -2, "spawn_codecs");
    lua_pushcfunction(L, spawn_typed);
    lua_setfield(L, -2, "spawn_typed");
    lua_pushcfunction(L, spawn_rest);
    lua_setfield(L, -2, "spawn_rest");
    lua_pushcfunction(L, spawn_observations);
    lua_setfield(L, -2, "spawn_observations");
    lua_pushcfunction(L, spawn_single);
    lua_setfield(L, -2, "spawn_single");
    lua_pushcfunction(L, fd_pair);
    lua_setfield(L, -2, "fd_pair");
    lua_pushcfunction(L, fd_read);
    lua_setfield(L, -2, "fd_read");
    lua_pushcfunction(L, fd_write);
    lua_setfield(L, -2, "fd_write");
    lua_pushcfunction(L, fd_close);
    lua_setfield(L, -2, "fd_close");
    lua_pushcfunction(L, fd_closed);
    lua_setfield(L, -2, "fd_closed");
    lua_pushcfunction(L, private_directory);
    lua_setfield(L, -2, "private_directory");
    lua_pushstring(L, argv[2]);
    lua_setfield(L, -2, "root");
    lua_pushstring(L, argv[1]);
    lua_setfield(L, -2, "script");
    if (argc == 4) {
        lua_pushstring(L, argv[3]);
        lua_setfield(L, -2, "workspace");
    }
    lua_setglobal(L, "fixture");
    lua_pushstring(L, "preserved");
    lua_setglobal(L, "host_marker");
    status = luaL_loadfilex(L, argv[1], "t");
    if (status == LUA_OK)
        status = lua_pcall(L, 0, 0, 0);
    if (status != LUA_OK)
        fprintf(stderr, "Lua fixture: %s\n", lua_tostring(L, -1));
    lua_close(L);
    SSL_CTX_free(client_context);
    return status != LUA_OK;
}

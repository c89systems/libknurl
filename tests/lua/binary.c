#define _POSIX_C_SOURCE 200809L
#include "../acl-wire.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t runner;

static void failure(const char *expression, int line)
{
    fprintf(stderr, "knurl runner test:%d: %s\n", line, expression);
    if (runner > 0) {
        (void)kill(runner, SIGTERM);
        (void)waitpid(runner, NULL, 0);
    }
    exit(1);
}

#define CHECK(expression)                                                      \
    ((expression) ? (void)0 : failure(#expression, __LINE__))

static void pause_briefly(void)
{
    struct timespec delay = {0, 10000000};
    nanosleep(&delay, NULL);
}

static void path(char *out, size_t capacity, const char *root, const char *file)
{
    int length = snprintf(out, capacity, "%s/%s", root, file);
    CHECK(length > 0 && (size_t)length < capacity);
}

static int events(const char *file, const char *message)
{
    char line[128];
    FILE *stream = fopen(file, "r");
    int count = 0;
    if (stream == NULL)
        return 0;
    while (fgets(line, sizeof(line), stream) != NULL)
        if (strcmp(line, message) == 0)
            ++count;
    fclose(stream);
    return count;
}

static void milestone(const char *file, const char *message, int count)
{
    int i;
    for (i = 0; i < 400; ++i) {
        if (events(file, message) == count)
            return;
        pause_briefly();
    }
    fprintf(stderr, "missing lifecycle event %s", message);
    CHECK(0);
}

static int bound_socket(unsigned short *port)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(fd, (struct sockaddr *)&address, length) == 0);
    CHECK(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
    *port = ntohs(address.sin_port);
    return fd;
}

static void wait_ok(pid_t child)
{
    int status;
    pid_t finished = waitpid(child, &status, 0);
    if (finished == runner)
        runner = 0;
    CHECK(finished == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void available(unsigned short port)
{
    int i;
    for (i = 0; i < 400; ++i) {
        if (knurl_test_acl_exchange(port, "GET", "/health", NULL, "ready") ==
            200)
            return;
        pause_briefly();
    }
    CHECK(0);
}

static int open_request(unsigned short port, const char *request)
{
    struct sockaddr_in address;
    struct timeval timeout = {3, 0};
    size_t length = strlen(request);
    int fd;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
          0);
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(write(fd, request, length) == (ssize_t)length);
    return fd;
}

static void receive(int fd, char *response, size_t capacity)
{
    size_t used = 0;
    ssize_t n;
    do {
        CHECK(used < capacity - 1);
        n = read(fd, response + used, capacity - 1 - used);
        CHECK(n >= 0);
        used += (size_t)n;
    } while (n != 0);
    close(fd);
    response[used] = '\0';
}

static void read_exact(int fd, void *buffer, size_t length)
{
    char *cursor = buffer;
    ssize_t n;
    while (length != 0) {
        n = read(fd, cursor, length);
        CHECK(n > 0);
        cursor += n;
        length -= (size_t)n;
    }
}

static void read_headers(int fd, char *buffer, size_t capacity)
{
    size_t length = 0;
    do {
        CHECK(length + 1 < capacity);
        read_exact(fd, buffer + length++, 1);
    } while (length < 4 || memcmp(buffer + length - 4, "\r\n\r\n", 4));
    buffer[length] = 0;
}

static void release_gate(const char *gate)
{
    int fd, i;
    for (i = 0; i < 400; ++i) {
        fd = open(gate, O_WRONLY | O_NONBLOCK);
        if (fd >= 0)
            break;
        CHECK(errno == ENXIO);
        pause_briefly();
    }
    CHECK(i < 400 && write(fd, "x", 1) == 1);
    close(fd);
}

static void send_frame(int fd, unsigned char opcode, const char *payload,
                       size_t length)
{
    unsigned char bytes[126];
    size_t i;
    CHECK(length <= sizeof(bytes) - 6);
    bytes[0] = opcode;
    bytes[1] = (unsigned char)(128 | length);
    memset(bytes + 2, 0x67, 4);
    for (i = 0; i < length; ++i)
        bytes[6 + i] = (unsigned char)payload[i] ^ 0x67;
    CHECK(write(fd, bytes, length + 6) == (ssize_t)(length + 6));
}

static void expect_frame(int fd, unsigned char opcode, const char *payload,
                         size_t length)
{
    unsigned char bytes[126];
    CHECK(length <= sizeof(bytes) - 2);
    read_exact(fd, bytes, length + 2);
    CHECK(bytes[0] == opcode && bytes[1] == length &&
          memcmp(bytes + 2, payload, length) == 0);
}

static void websocket(unsigned short port, const char *log, const char *gate)
{
    static const char upgrade[] =
        "Host: localhost\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    static const char farewell[] = "\003\350bye";
    char request[512], response[1024];
    int fd;
    CHECK(knurl_test_acl_exchange(port, "GET", "/ws/alice", NULL, NULL) == 400);
    CHECK(knurl_test_acl_exchange(port, "GET", "/ws/alice?fail=1", NULL,
                                  NULL) == 500);
    snprintf(request, sizeof(request), "GET /ws-denied HTTP/1.1\r\n%s",
             upgrade);
    receive(open_request(port, request), response, sizeof(response));
    CHECK(strncmp(response, "HTTP/1.1 403", 12) == 0);
    snprintf(request, sizeof(request), "GET /ws/ali%%63e HTTP/1.1\r\n%s",
             upgrade);
    fd = open_request(port, request);
    read_headers(fd, response, sizeof(response));
    CHECK(strncmp(response, "HTTP/1.1 101", 12) == 0);
    CHECK(strstr(response, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);
    CHECK(strstr(response, "x-socket: ready\r\n") != NULL);
    expect_frame(fd, 0x81, "hello alice", 11);
    CHECK(knurl_test_acl_exchange(port, "GET", "/app/bob/count", NULL, NULL) ==
          200);
    send_frame(fd, 0x02, "one\0", 4);
    send_frame(fd, 0x80, "two", 3);
    expect_frame(fd, 0x82, "one\0two", 7);
    send_frame(fd, 0x81, "hold", 4);
    milestone(log, "websocket waiting\n", 1);
    CHECK(knurl_test_acl_exchange(port, "GET", "/health", NULL, "ready") ==
          200);
    CHECK(knurl_test_acl_exchange(port, "GET", "/app/bob/count", NULL, NULL) ==
          503);
    release_gate(gate);
    expect_frame(fd, 0x81, "done", 4);
    send_frame(fd, 0x88, farewell, sizeof(farewell) - 1);
    expect_frame(fd, 0x88, farewell, sizeof(farewell) - 1);
    close(fd);
    milestone(log, "websocket closed\n", 1);
}

static void streams(unsigned short port, const char *log, const char *gate)
{
    char response[1024], prefix[10];
    int fd = open_request(
        port,
        "GET /stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    read_headers(fd, response, sizeof(response));
    CHECK(strncmp(response, "HTTP/1.1 200", 12) == 0);
    CHECK(strstr(response, "transfer-encoding: chunked") != NULL);
    CHECK(strstr(response, "x-stream: ready\r\n") != NULL);
    read_exact(fd, prefix, sizeof(prefix));
    CHECK(memcmp(prefix, "5\r\nfirst\r\n", sizeof(prefix)) == 0);
    milestone(log, "stream waiting\n", 1);
    CHECK(knurl_test_acl_exchange(port, "GET", "/health", NULL, "ready") ==
          200);
    release_gate(gate);
    receive(fd, response, sizeof(response));
    CHECK(strcmp(response, "4\r\nlast\r\n0\r\n\r\n") == 0);

    receive(open_request(port, "HEAD /sse HTTP/1.1\r\nHost: localhost\r\n"
                               "Connection: close\r\n\r\n"),
            response, sizeof(response));
    CHECK(strncmp(response, "HTTP/1.1 200", 12) == 0);
    CHECK(strstr(response, "text/event-stream") != NULL);
    CHECK(events(log, "SSE pulled\n") == 0);
    receive(open_request(port, "GET /sse HTTP/1.1\r\nHost: localhost\r\n"
                               "Connection: close\r\n\r\n"),
            response, sizeof(response));
    CHECK(
        strstr(
            response,
            "id: 1\nevent: update\nretry: 500\ndata: hello\ndata: world\n\n") !=
        NULL);
    CHECK(events(log, "SSE pulled\n") == 2);
}

static void typed_and_webdav(unsigned short port)
{
    static const char *const bodies[] = {
        "{\"text\":\"hi\"}", "{}", "<message><text>hi</text></message>", "abc"};
    static const char *const paths[] = {"/typed/alice", "/typed/alice",
                                        "/typed-xml", "/dav/put.txt"};
    static const char *const methods[] = {"POST", "POST", "POST", "PUT"};
    static const char *const status[] = {"HTTP/1.1 200", "HTTP/1.1 400",
                                         "HTTP/1.1 200", "HTTP/1.1 201"};
    char request[512], response[1024];
    size_t i;
    for (i = 0; i < sizeof(bodies) / sizeof(bodies[0]); ++i) {
        snprintf(request, sizeof(request),
                 "%s %s HTTP/1.1\r\nHost: localhost\r\n"
                 "Content-Length: %lu\r\nConnection: close\r\n\r\n%s",
                 methods[i], paths[i], (unsigned long)strlen(bodies[i]),
                 bodies[i]);
        receive(open_request(port, request), response, sizeof(response));
        CHECK(strncmp(response, status[i], 12) == 0);
        if (i == 0)
            CHECK(strstr(response, "{\"text\":\"alice:hi\"}") != NULL);
        if (i == 1)
            CHECK(strstr(response, "request_decode") != NULL);
        if (i == 2)
            CHECK(strstr(response, "<message><text>hi!</text></message>") !=
                  NULL);
    }
    CHECK(knurl_test_acl_exchange(port, "GET", "/dav/put.txt", NULL, "abc") ==
          200);
    CHECK(knurl_test_acl_exchange(port, "GET", "/protected-dav/", NULL, NULL) ==
          403);
}

static void hooks_and_proxy(unsigned short port, const char *log,
                            const char *gate)
{
    static const char body_headers[] =
        "Host: localhost\r\nConnection: close\r\n"
        "Content-Length: 3\r\nExpect: 100-continue\r\n\r\n";
    char request[512], response[2048];
    struct pollfd future;
    int fd;
    snprintf(request, sizeof(request), "POST /hooks/deny HTTP/1.1\r\n%s",
             body_headers);
    receive(open_request(port, request), response, sizeof(response));
    CHECK(strncmp(response, "HTTP/1.1 403", 12) == 0);
    CHECK(strstr(response, "100 Continue") == NULL);

    snprintf(request, sizeof(request), "POST /hooks/block HTTP/1.1\r\n%s",
             body_headers);
    fd = open_request(port, request);
    milestone(log, "hook waiting\n", 1);
    future.fd = fd;
    future.events = POLLIN;
    CHECK(poll(&future, 1, 0) == 0);
    CHECK(knurl_test_acl_exchange(port, "GET", "/health", NULL, "ready") ==
          200);
    release_gate(gate);
    read_headers(fd, response, sizeof(response));
    CHECK(strcmp(response, "HTTP/1.1 100 Continue\r\n\r\n") == 0);
    CHECK(write(fd, "abc", 3) == 3);
    receive(fd, response, sizeof(response));
    CHECK(strncmp(response, "HTTP/1.1 201", 12) == 0);
    CHECK(strstr(response, "x-route: after\r\n") != NULL);
    CHECK(strstr(response,
                 "x-group: group auth, route auth, group before, route before, "
                 "handler, route after, group after\r\n") != NULL);
    CHECK(strstr(response, "\r\n\r\nabc") != NULL);

    CHECK(knurl_test_acl_exchange(port, "GET", "/gateway/deny", NULL,
                                  "preflight refused") == 429);
    fd = open_request(
        port, "GET /gateway/hold?before=1 HTTP/1.1\r\nHost: localhost\r\n"
              "Connection: close\r\nX-Remove: inbound\r\n\r\n");
    milestone(log, "proxy response waiting\n", 1);
    future.fd = fd;
    CHECK(poll(&future, 1, 0) == 0);
    CHECK(knurl_test_acl_exchange(port, "GET", "/health", NULL, "ready") ==
          200);
    release_gate(gate);
    receive(fd, response, sizeof(response));
    CHECK(strncmp(response, "HTTP/1.1 203", 12) == 0);
    CHECK(strstr(response, "x-proxy: lua\r\n") &&
          strstr(response, "upstream data"));
    CHECK(strstr(response, "x-origin:") == NULL);
    milestone(log, "proxy exchange callback\n", 1);

    receive(open_request(port,
                         "GET /gateway-failure HTTP/1.1\r\nHost: localhost\r\n"
                         "Connection: close\r\n\r\n"),
            response, sizeof(response));
    CHECK(strncmp(response, "HTTP/1.1 503", 12) == 0);
    CHECK(strstr(response, "x-fallback: lua\r\n") &&
          strstr(response, "custom gateway response"));
}

static void hook_timeouts(unsigned short port, const char *log,
                          const char *gate)
{
    static const char *const paths[] = {"before", "after"};
    static const char upgrade[] =
        "GET /ws-queue HTTP/1.1\r\nHost: localhost\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    static const char internal_error[] = "\x03\xf3";
    char request[256], response[1024], waiting[80];
    size_t i;
    int fd, ws;
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        snprintf(request, sizeof(request),
                 "GET /%s-timeout HTTP/1.1\r\n"
                 "Host: localhost\r\nConnection: close\r\n\r\n",
                 paths[i]);
        snprintf(waiting, sizeof(waiting), "%s timeout waiting\n", paths[i]);
        fd = open_request(port, request);
        milestone(log, waiting, 1);
        receive(fd, response, sizeof(response));
        CHECK(strncmp(response, "HTTP/1.1 504", 12) == 0);
        CHECK(strstr(response, "too late") == NULL);
        CHECK(knurl_test_acl_exchange(port, "GET", "/health", NULL, "ready") ==
              200);
        if (i == 0) {
            ws = open_request(port, upgrade);
            read_headers(ws, response, sizeof(response));
            CHECK(strncmp(response, "HTTP/1.1 101", 12) == 0);
            expect_frame(ws, 0x88, internal_error, 2);
            send_frame(ws, 0x88, internal_error, 2);
            close(ws);
        }
        release_gate(gate);
    }
    CHECK(events(log, "expired websocket callback ran\n") == 0);
}

static void upload(unsigned short port, const char *log)
{
    static const char request[] =
        "POST /app/alice/upload HTTP/1.1\r\nHost: localhost\r\n"
        "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n1\r\na\r\n";
    static const char remaining[] = "2\r\nbc\r\n0\r\n\r\n";
    char response[1024], *body;
    int fd = open_request(port, request);
    milestone(log, "upload prefix\n", 1);
    CHECK(write(fd, remaining, sizeof(remaining) - 1) ==
          (ssize_t)sizeof(remaining) - 1);
    receive(fd, response, sizeof(response));
    body = strstr(response, "\r\n\r\n");
    CHECK(strncmp(response, "HTTP/1.1 200", 12) == 0);
    CHECK(body != NULL && strcmp(body + 4, "abc") == 0);
}

static void reject_upload(unsigned short port, const char *framing)
{
    char request[256], response[1024], *body;
    int length = snprintf(
        request, sizeof(request),
        "POST /app/alice/reject HTTP/1.1\r\nHost: localhost\r\n%s", framing);
    CHECK(length > 0 && (size_t)length < sizeof(request));
    receive(open_request(port, request), response, sizeof(response));
    body = strstr(response, "\r\n\r\n");
    CHECK(strncmp(response, "HTTP/1.1 403", 12) == 0);
    CHECK(strstr(response, "\r\nconnection: close\r\n") != NULL);
    CHECK(body != NULL && strcmp(body + 4, "rejected") == 0);
}

static int accept_origin(int listener)
{
    struct timeval timeout = {3, 0};
    struct pollfd ready;
    int fd;
    ready.fd = listener;
    ready.events = POLLIN;
    CHECK(poll(&ready, 1, 3000) == 1);
    fd = accept(listener, NULL, NULL);
    CHECK(fd >= 0);
    close(listener);
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
          0);
    return fd;
}

static void origin_request(int fd, const char *expected)
{
    char response[1024];
    size_t used = 0;
    ssize_t n;
    do {
        CHECK(used < sizeof(response) - 1);
        n = read(fd, response + used, sizeof(response) - 1 - used);
        CHECK(n > 0);
        used += (size_t)n;
        response[used] = '\0';
    } while (strstr(response, "\r\n\r\n") == NULL);
    CHECK(strncmp(response, expected, strlen(expected)) == 0);
}

static void geoip_requests(int listener, const char *work)
{
    static const char *const requests[] = {"GET /missing ", "GET /broken ",
                                           "GET /bad-mmdb ", "GET /country "};
    static const char *const fixtures[] = {NULL, NULL, "bad-metadata.gz",
                                           "valid-24-4.gz"};
    char header[128], file_path[4096], bytes[2048];
    struct timeval timeout = {3, 0};
    struct pollfd ready;
    FILE *file;
    size_t i, size;
    int client, length;
    ready.fd = listener;
    ready.events = POLLIN;
    for (i = 0; i < sizeof(requests) / sizeof(requests[0]); ++i) {
        CHECK(poll(&ready, 1, 4000) == 1);
        client = accept(listener, NULL, NULL);
        CHECK(client >= 0);
        CHECK(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout)) == 0);
        origin_request(client, requests[i]);
        if (fixtures[i] != NULL) {
            path(file_path, sizeof(file_path), work, fixtures[i]);
            file = fopen(file_path, "rb");
            CHECK(file != NULL);
            size = fread(bytes, 1, sizeof(bytes), file);
            CHECK(size > 1 && size < sizeof(bytes) && !ferror(file) &&
                  fclose(file) == 0);
        } else if (i != 0) {
            memcpy(bytes, "bad gzip", 8);
            size = 8;
        } else
            size = 0;
        length = snprintf(
            header, sizeof(header),
            "HTTP/1.1 %s\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
            i == 0 ? "404 Not Found" : "200 OK", (unsigned long)size);
        CHECK(length > 0 && (size_t)length < sizeof(header));
        CHECK(write(client, header, (size_t)length) == length);
        if (size != 0)
            CHECK(write(client, bytes, size) == (ssize_t)size);
        close(client);
    }
}

static void websocket_proxy(unsigned short port, int listener, const char *log,
                            const char *gate)
{
    static const char request[] =
        "GET /socket-proxy HTTP/1.1\r\nHost: localhost\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    static const char reply[] =
        "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n"
        "\201\004echo";
    int down = open_request(port, request), up = accept_origin(listener);
    struct pollfd held;
    char headers[1024];
    origin_request(up, "GET /socket-proxy ");
    CHECK(write(up, reply, sizeof(reply) - 1) == (ssize_t)(sizeof(reply) - 1));
    milestone(log, "proxy handshake waiting\n", 1);
    CHECK(knurl_test_acl_exchange(port, "GET", "/health", NULL, "ready") ==
          200);
    held.fd = down;
    held.events = POLLIN;
    CHECK(poll(&held, 1, 50) == 0);
    release_gate(gate);
    read_headers(down, headers, sizeof(headers));
    CHECK(strncmp(headers, "HTTP/1.1 101", 12) == 0);
    CHECK(strstr(headers, "x-handshake: lua\r\n") != NULL);
    expect_frame(down, 0x81, "echo", 4);
    close(up);
    close(down);
}

static void curl_request(unsigned short port, const char *log, int timed_out)
{
    static const char partial[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\none";
    unsigned short upstream;
    char request[256], response[1024];
    int listener, down, up, length;
    listener = bound_socket(&upstream);
    CHECK(listen(listener, 1) == 0);
    length = snprintf(request, sizeof(request),
                      "GET /app/%u/%s HTTP/1.1\r\nHost: localhost\r\n"
                      "Connection: close\r\n\r\n",
                      (unsigned int)upstream,
                      timed_out ? "curl-timeout" : "curl-stream");
    CHECK(length > 0 && (size_t)length < sizeof(request));
    down = open_request(port, request);
    up = accept_origin(listener);
    origin_request(up, "GET /from-curl ");
    if (!timed_out) {
        CHECK(write(up, partial, sizeof(partial) - 1) ==
              (ssize_t)sizeof(partial) - 1);
        milestone(log, "curl first chunk\n", 1);
        CHECK(knurl_test_acl_exchange(port, "GET", "/health", NULL, "ready") ==
              200);
        CHECK(write(up, "two", 3) == 3);
        close(up);
    }
    receive(down, response, sizeof(response));
    if (timed_out)
        close(up);
    CHECK(strncmp(response, "HTTP/1.1 200", 12) == 0);
    CHECK(strstr(response,
                 timed_out ? "\r\n\r\ntimed out" : "\r\n\r\nonetwo") != NULL);
}

static void smtp_exchange(int fd, const char *reply, const char *expected)
{
    char line[256];
    size_t i, length = strlen(reply);
    if (length != 0)
        CHECK(write(fd, reply, length) == (ssize_t)length);
    for (i = 0; i < sizeof(line) - 1; ++i) {
        CHECK(read(fd, line + i, 1) == 1);
        if (line[i] == '\n')
            break;
    }
    CHECK(i < sizeof(line) - 1);
    line[i + 1] = '\0';
    CHECK(strncmp(line, expected, strlen(expected)) == 0);
}

static void smtp_request(unsigned short port, const char *mode)
{
    unsigned short upstream;
    char request[256], response[1024];
    int listener, down, up, length;
    listener = bound_socket(&upstream);
    CHECK(listen(listener, 1) == 0);
    length = snprintf(request, sizeof(request),
                      "GET /app/%u/%s HTTP/1.1\r\nHost: localhost\r\n"
                      "Connection: close\r\n\r\n",
                      (unsigned int)upstream, mode);
    CHECK(length > 0 && (size_t)length < sizeof(request));
    down = open_request(port, request);
    up = accept_origin(listener);
    if (strcmp(mode, "smtp-timeout") != 0) {
        smtp_exchange(up, "220 local test server\r\n", "EHLO ");
        smtp_exchange(up, "250 localhost\r\n",
                      "MAIL FROM:<sender@example.test>\r\n");
        smtp_exchange(up, "250 sender ok\r\n",
                      "RCPT TO:<one@example.test>\r\n");
        if (strcmp(mode, "smtp-rejected") == 0) {
            smtp_exchange(up, "550 rejected\r\n", "QUIT\r\n");
        } else {
            if (strcmp(mode, "smtp-file") == 0)
                smtp_exchange(up, "250 recipient ok\r\n",
                              "RCPT TO:<two@example.test>\r\n");
            smtp_exchange(up, "250 recipient ok\r\n", "DATA\r\n");
            smtp_exchange(up, "354 send message\r\n",
                          "Subject: knurl test\r\n");
            smtp_exchange(up, "", "\r\n");
            smtp_exchange(up, "", "..payload\r\n");
            smtp_exchange(up, "", ".\r\n");
            CHECK(knurl_test_acl_exchange(port, "GET", "/health", NULL,
                                          "ready") == 200);
            smtp_exchange(up, "250 queued\r\n", "QUIT\r\n");
        }
        CHECK(write(up, "221 bye\r\n", 9) == 9);
        close(up);
    }
    receive(down, response, sizeof(response));
    if (strcmp(mode, "smtp-timeout") == 0)
        close(up);
    CHECK(strncmp(response, "HTTP/1.1 200", 12) == 0);
    CHECK(strstr(response, "\r\n\r\nsmtp checked") != NULL);
}

static void exercise(unsigned short port, const char *log, const char *gate,
                     int listener)
{
    char response[1024];
    int status;
    pid_t pending;
    CHECK(knurl_test_acl_exchange(port, "GET", "/country", NULL, NULL) == 403);
    CHECK(knurl_test_acl_exchange(port, "GET", "/country",
                                  "Forwarded: for=1.2.3.4\r\n",
                                  "country allowed") == 200);
    CHECK(knurl_test_acl_exchange(port, "GET", "/country-peer",
                                  "Forwarded: for=1.2.3.4\r\n", NULL) == 403);
    pending = fork();
    CHECK(pending >= 0);
    if (pending == 0)
        _exit(knurl_test_acl_exchange(port, "GET", "/app/ali%63e/hold", NULL,
                                      "alice:41") == 200
                  ? 0
                  : 1);
    milestone(log, "handler waiting\n", 1);
    CHECK(knurl_test_acl_exchange(port, "GET", "/health", NULL, "ready") ==
          200);
    CHECK(knurl_test_acl_exchange(port, "GET", "/asset", NULL, "asset\n") ==
          200);
    CHECK(knurl_test_acl_exchange(port, "GET", "/app/bob/count", NULL, NULL) ==
          503);
    CHECK(waitpid(pending, &status, WNOHANG) == 0);
    release_gate(gate);
    wait_ok(pending);
    CHECK(knurl_test_acl_exchange(port, "GET", "/app/bob/count", NULL,
                                  "bob:42") == 200);
    CHECK(knurl_test_acl_exchange(port, "GET", "/app/bob/json", NULL,
                                  "[\"bob\",{\"count\":42},true]") == 200);
    CHECK(knurl_test_acl_exchange(port, "GET", "/app/bob/cycle", NULL, NULL) ==
          500);
    receive(open_request(port,
                         "GET /app/bob/error HTTP/1.1\r\nHost: localhost\r\n"
                         "Connection: close\r\n\r\n"),
            response, sizeof(response));
    CHECK(strncmp(response, "HTTP/1.1 500", 12) == 0);
    CHECK(strstr(response, "intentional handler error") == NULL &&
          strstr(response, "stack traceback") == NULL);
    reject_upload(port, "Content-Length: 10\r\n\r\n");
    reject_upload(port, "Transfer-Encoding: chunked\r\n\r\n4\r\nabc");
    CHECK(knurl_test_acl_exchange(port, "POST", "/app/alice/reject",
                                  "Content-Length: 17\r\n", NULL) == 413);
    CHECK(events(log, "rejected upload\n") == 2);
    upload(port, log);
    CHECK(knurl_test_acl_exchange(port, "GET", "/app/bob/curl-smoke", NULL,
                                  "curl works") == 200);
    curl_request(port, log, 0);
    curl_request(port, log, 1);
    smtp_request(port, "smtp-memory");
    smtp_request(port, "smtp-file");
    smtp_request(port, "smtp-read");
    smtp_request(port, "smtp-rejected");
    smtp_request(port, "smtp-timeout");
    typed_and_webdav(port);
    streams(port, log, gate);
    websocket(port, log, gate);
    hooks_and_proxy(port, log, gate);
    websocket_proxy(port, listener, log, gate);

    /* Abandon a worker. Its replacement starts from the same parent snapshot.
     */
    (void)knurl_test_acl_exchange(port, "GET", "/app/bob/crash", NULL, NULL);
    milestone(log, "worker start\n", 2);
    available(port);
    CHECK(knurl_test_acl_exchange(port, "GET", "/app/bob/count", NULL,
                                  "bob:41") == 200);
}

int main(int argc, char **argv)
{
    char log[4096], gate[4096], asset[4096], cache[4096], port[16],
        geo_port[16];
    unsigned short selected, upstream = 0;
    pid_t child;
    FILE *file;
    int normal, init_failure, stop_failure, fd, geo_listener = -1;
    CHECK(argc == 5 || argc == 6);
    normal = strcmp(argv[4], "normal") == 0;
    init_failure = strcmp(argv[4], "init-failure") == 0;
    stop_failure = strcmp(argv[4], "stop-failure") == 0;
    path(log, sizeof(log), argv[3], "events");
    path(gate, sizeof(gate), argv[3], "gate");
    path(asset, sizeof(asset), argv[3], "asset");
    (void)unlink(log);
    (void)unlink(gate);
    CHECK(mkfifo(gate, 0600) == 0);
    file = fopen(asset, "w");
    CHECK(file != NULL && fputs("asset\n", file) >= 0 && fclose(file) == 0);
    fd = bound_socket(&selected);
    close(fd);
    snprintf(port, sizeof(port), "%u", (unsigned int)selected);
    if (normal) {
        path(cache, sizeof(cache), argv[3], "geo-cache");
        CHECK(setenv("XDG_CACHE_HOME", cache, 1) == 0);
        CHECK(setenv("no_proxy", "127.0.0.1", 1) == 0);
        geo_listener = bound_socket(&upstream);
        CHECK(listen(geo_listener, 1) == 0);
    }
    snprintf(geo_port, sizeof(geo_port), "%u", (unsigned int)upstream);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        if (geo_listener >= 0)
            close(geo_listener);
        if (argc == 6)
            execl(argv[5], argv[5], argv[1], argv[2], argv[3], port, argv[4],
                  geo_port, "emulated", (char *)NULL);
        else
            execl(argv[1], argv[1], argv[2], argv[3], port, argv[4], geo_port,
                  (char *)NULL);
        perror("starting knurl runner");
        _exit(126);
    }
    runner = child;
    if (normal)
        geoip_requests(geo_listener, argv[3]);
    if (!init_failure) {
        milestone(log, "worker start\n", 1);
        available(selected);
        if (normal) {
            exercise(selected, log, gate, geo_listener);
            close(geo_listener);
        } else if (!stop_failure)
            hook_timeouts(selected, log, gate);
        CHECK(kill(child, SIGTERM) == 0);
    }
    wait_ok(child);
    runner = 0;
    CHECK(events(log, "parent start\n") == 1 &&
          events(log, "parent end\n") == 1);
    CHECK(events(log, "worker start\n") == (normal ? 2 : init_failure ? 0 : 1));
    CHECK(events(log, "worker stop\n") == !init_failure);
    if (init_failure)
        CHECK(events(log, "init failure reported\n") == 1);
    else if (stop_failure)
        CHECK(events(log, "stop failure reported\n") == 1);
    CHECK(unlink(gate) == 0);
    return 0;
}

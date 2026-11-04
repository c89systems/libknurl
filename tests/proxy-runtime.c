#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            (void)fprintf(stderr, "proxy live %d: %s\n", __LINE__, #t);        \
            return 1;                                                          \
        }                                                                      \
    } while (0)
#define OBSERVE(t)                                                             \
    do {                                                                       \
        if (!(t)) {                                                            \
            (void)fprintf(stderr, "proxy callback %d: %s\n", __LINE__, #t);    \
            ++failures;                                                        \
        }                                                                      \
    } while (0)
struct fixture {
    int origin;
    int control[2];
    pid_t client;
    pid_t origin_pid;
    unsigned short origin_port;
};
static int failures;
static int rewrites;
static int modified;
static int gateway_errors;
static int interim_closes;
static int finish_trailers;
static rotmos_status empty_complete(rotmos_request *, void *, rotmos_error *);

static rotmos_bytes view(const char *s)
{
    rotmos_bytes b;
    b.data = s;
    b.length = strlen(s);
    return b;
}

static int write_all(int fd, const void *data, size_t length)
{
    const unsigned char *p;
    ssize_t count;
    p = data;
    while (length) {
        count = write(fd, p, length);
        CHECK(count > 0);
        p += count;
        length -= (size_t)count;
    }
    return 0;
}

static int read_exact(int fd, void *data, size_t length)
{
    unsigned char *p;
    ssize_t count;
    p = data;
    while (length) {
        count = read(fd, p, length);
        CHECK(count > 0);
        p += count;
        length -= (size_t)count;
    }
    return 0;
}

static int head(int fd, char *buffer, size_t capacity)
{
    size_t used;
    used = 0;
    buffer[0] = 0;
    do {
        CHECK(used + 1 < capacity && read_exact(fd, buffer + used, 1) == 0);
        buffer[++used] = 0;
    } while (strstr(buffer, "\r\n\r\n") == NULL);
    return 0;
}

static int connect_to(unsigned short port)
{
    struct sockaddr_in address;
    struct timeval timeout;
    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

static int chunk(int fd, const void *data, size_t length)
{
    char prefix[32];
    int count;
    count = snprintf(prefix, sizeof(prefix), "%lx\r\n", (unsigned long)length);
    CHECK(count > 0 && write_all(fd, prefix, (size_t)count) == 0 &&
          write_all(fd, data, length) == 0 && write_all(fd, "\r\n", 2) == 0);
    return 0;
}

static int read_chunk(int fd, char *body, size_t capacity, size_t *length,
                      int *done)
{
    char line[64];
    size_t used;
    unsigned long value;
    used = 0;
    do {
        CHECK(used + 1 < sizeof(line) && read_exact(fd, line + used, 1) == 0);
        ++used;
    } while (used < 2 || line[used - 2] != '\r' || line[used - 1] != '\n');
    line[used] = 0;
    value = strtoul(line, NULL, 16);
    CHECK(value <= capacity);
    *length = (size_t)value;
    *done = value == 0;
    if (value != 0) {
        CHECK(read_exact(fd, body, (size_t)value) == 0 &&
              read_exact(fd, line, 2) == 0 && memcmp(line, "\r\n", 2) == 0);
    } else {
        do {
            used = 0;
            do {
                CHECK(used + 1 < sizeof(line) &&
                      read_exact(fd, line + used, 1) == 0);
                ++used;
            } while (used < 2 || line[used - 2] != '\r' ||
                     line[used - 1] != '\n');
            line[used] = 0;
            if (strcmp(line, "X-Finish: yes\r\n") == 0)
                ++finish_trailers;
        } while (used != 2);
    }
    return 0;
}

struct decode {
    size_t count;
    int trailer;
};
static rotmos_status receive_body(rotmos_bytes bytes, size_t *consumed,
                                  void *data, rotmos_error *error)
{
    struct decode *d;
    (void)error;
    d = data;
    d->count += bytes.length;
    *consumed = bytes.length;
    return ROTMOS_OK;
}
static rotmos_status receive_trailer(rotmos_header h, void *data,
                                     rotmos_error *error)
{
    struct decode *d;
    (void)error;
    d = data;
    if (h.name.length != 5 || memcmp(h.name.data, "X-Req", 5) != 0 ||
        h.value.length != 3 || memcmp(h.value.data, "yes", 3) != 0)
        return ROTMOS_INVALID;
    ++d->trailer;
    return ROTMOS_OK;
}

static int origin(int listener, int control)
{
    char metadata[4096];
    char buffer[1024];
    char command;
    size_t count;
    size_t consumed;
    size_t remaining;
    ssize_t got;
    int fd;
    int finish;
    rotmos_framer_config config;
    rotmos_framer *framer;
    rotmos_status status;
    struct decode d;
    for (;;) {
        fd = accept(listener, NULL, NULL);
        CHECK(fd >= 0);
        CHECK(head(fd, metadata, sizeof(metadata)) == 0);
        finish = strstr(metadata, " /base/finish ") != NULL;
        if (strstr(metadata, " /base/drop ") != NULL) {
            (void)close(fd);
            continue;
        }
        if (strstr(metadata, " /base/idle ") != NULL) {
            CHECK(write_all(control, "r", 1) == 0 &&
                  read_exact(control, &command, 1) == 0);
            (void)close(fd);
            continue;
        }
        if (strstr(metadata, " /base/truncated ") != NULL ||
            strstr(metadata, " /base/total ") != NULL) {
            const char *message =
                "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc";
            CHECK(write_all(fd, message, strlen(message)) == 0 &&
                  write_all(control, "r", 1) == 0 &&
                  read_exact(control, &command, 1) == 0);
            (void)close(fd);
            continue;
        }
        if (strstr(metadata, " /base/events ") != NULL) {
            const char *message = "HTTP/1.1 200 OK\r\nContent-Type: "
                                  "text/event-stream\r\nTransfer-Encoding: "
                                  "chunked\r\nConnection: close\r\n\r\n";
            CHECK(write_all(fd, message, strlen(message)) == 0 &&
                  chunk(fd, "data: first\n\n", 13) == 0);
            CHECK(read_exact(control, &command, 1) == 0 &&
                  chunk(fd, "data: last\n\n", 12) == 0 &&
                  write_all(fd, "0\r\n\r\n", 5) == 0);
            (void)close(fd);
            continue;
        }
        if (strstr(metadata, " /base/upload ") != NULL) {
            const char *message =
                "HTTP/1.1 103 Early Hints\r\nLink: </ready>\r\n\r\nHTTP/1.1 "
                "200 OK\r\nTransfer-Encoding: chunked\r\nConnection: "
                "close\r\nTrailer: "
                "X-Finish\r\n\r\n";
            CHECK(strncmp(metadata, "POST ", 5) == 0 &&
                  strstr(metadata, "65536") != NULL);
            CHECK(write_all(fd, message, strlen(message)) == 0);
            remaining = 65536;
            while (remaining) {
                got = read(fd, buffer,
                           remaining < sizeof(buffer) ? remaining
                                                      : sizeof(buffer));
                CHECK(got > 0);
                for (count = 0; count < (size_t)got; ++count)
                    CHECK(buffer[count] == 'x');
                CHECK(chunk(fd, buffer, (size_t)got) == 0);
                remaining -= (size_t)got;
            }
            CHECK(write_all(fd, "0\r\nX-Finish: yes\r\n\r\n",
                            strlen("0\r\nX-Finish: yes\r\n\r\n")) == 0);
            (void)close(fd);
            continue;
        }
        if (strstr(metadata, " /base/chunked ") != NULL) {
            CHECK(strncmp(metadata, "POST ", 5) == 0 &&
                  (strstr(metadata, "Transfer-Encoding: chunked") != NULL ||
                   strstr(metadata, "transfer-encoding: chunked") != NULL));
            memset(&d, 0, sizeof(d));
            rotmos_framer_config_init(&config);
            config.chunked = 1;
            config.on_body = receive_body;
            config.on_trailer = receive_trailer;
            config.userdata = &d;
            CHECK(rotmos_framer_new(&config, &framer, NULL) == ROTMOS_OK);
            do {
                got = read(fd, buffer, sizeof(buffer));
                CHECK(got > 0);
                status = rotmos_framer_feed(framer, buffer, (size_t)got,
                                            &consumed, NULL);
                CHECK(status == ROTMOS_OK || status == ROTMOS_EOF);
            } while (status != ROTMOS_EOF);
            CHECK(d.count == 5 && d.trailer == 1 &&
                  rotmos_framer_destroy(framer, NULL) == ROTMOS_OK);
        }
        if (strstr(metadata, " /alt/echo%2Fraw?a=1&a=2 ") != NULL) {
            CHECK(strstr(metadata, "Host: selected.test") != NULL &&
                  strstr(metadata, "X-Forwarded-Host: client.test") != NULL &&
                  strstr(metadata, "poison") == NULL);
        }
        if (strstr(metadata, " /base/raw/a%2Fb?q=%2B ") != NULL)
            CHECK(strncmp(metadata, "GET ", 4) == 0);
        {
            const char *message =
                "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nSet-Cookie: "
                "a=1\r\nSet-Cookie: b=2\r\nX-Mode: valid\r\nConnection: "
                "X-Hop, close\r\nX-Hop: remove\r\n\r\npong";
            if (strstr(metadata, " /base/badedit ") != NULL)
                message = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: "
                          "close\r\nX-Mode: "
                          "invalid\r\n\r\npong";
            CHECK(write_all(fd, message,
                            strlen(message) -
                                (strncmp(metadata, "HEAD ", 5) == 0 ? 4 : 0)) ==
                  0);
        }
        (void)close(fd);
        if (finish)
            break;
    }
    return 0;
}

static rotmos_status preflight(const rotmos_proxy_inbound *in,
                               rotmos_proxy_local_response *local, void *data,
                               rotmos_error *error)
{
    (void)data;
    if (strcmp(rotmos_proxy_inbound_path(in), "/local-large") == 0) {
        char *body;
        rotmos_status status;
        body = malloc(65536);
        if (body == NULL)
            return ROTMOS_NOMEM;
        memset(body, 'l', 65536);
        status = rotmos_proxy_local_respond(local, 200, body, 65536, error);
        free(body);
        if (status == ROTMOS_OK)
            status = rotmos_proxy_local_add_header(local, "Set-Cookie", "a=1",
                                                   error);
        if (status == ROTMOS_OK)
            status = rotmos_proxy_local_add_header(local, "Set-Cookie", "b=2",
                                                   error);
        return status;
    }
    if (strcmp(rotmos_proxy_inbound_path(in), "/deny") == 0) {
        const char bytes[3] = {'n', 0, 'o'};
        return rotmos_proxy_local_respond(local, 403, bytes, sizeof(bytes),
                                          error);
    }
    return ROTMOS_OK;
}
static rotmos_status rewrite(const rotmos_proxy_inbound *in,
                             rotmos_proxy_outbound *out, void *data,
                             rotmos_error *error)
{
    rotmos_bytes value;
    (void)data;
    ++rewrites;
    if (strncmp(rotmos_proxy_inbound_path(in), "/p/", 3) == 0) {
        value = rotmos_proxy_inbound_path_param(in, "item");
        OBSERVE(value.length == 2 && memcmp(value.data, "ok", 2) == 0);
        OBSERVE(rotmos_proxy_outbound_select_target(out, 1, error) ==
                ROTMOS_OK);
        OBSERVE(rotmos_proxy_outbound_set_path(out, "/echo%2Fraw", error) ==
                ROTMOS_OK);
        OBSERVE(rotmos_proxy_outbound_set_query(out, "a=1&a=2", error) ==
                ROTMOS_OK);
        return rotmos_proxy_outbound_set_host(out, "selected.test", error);
    }
    return ROTMOS_OK;
}
static rotmos_status modify(rotmos_proxy_response *response, void *data,
                            rotmos_error *error)
{
    const char *name;
    const char *value;
    size_t i;
    (void)data;
    ++modified;
    for (i = 0; i < rotmos_proxy_response_header_count(response); ++i) {
        OBSERVE(rotmos_proxy_response_header_at(response, i, &name, &value) ==
                ROTMOS_OK);
        if (strcmp(name, "X-Mode") == 0 && strcmp(value, "invalid") == 0) {
            OBSERVE(rotmos_proxy_response_add_header(response, "Content-Length",
                                                     "4",
                                                     error) == ROTMOS_INVALID);
            return ROTMOS_OK;
        }
    }
    return rotmos_proxy_response_add_header(response, "X-Proxy", "edited",
                                            error);
}
static rotmos_status gateway(const rotmos_error *cause, int code,
                             rotmos_proxy_local_response *local, void *data,
                             rotmos_error *error)
{
    (void)data;
    OBSERVE(cause->status != ROTMOS_OK && (code == 502 || code == 504));
    ++gateway_errors;
    OBSERVE(rotmos_proxy_local_respond(local, code == 502 ? 503 : 504,
                                       code == 502 ? "gateway" : "timeout", 7,
                                       error) == ROTMOS_OK);
    return rotmos_proxy_local_add_header(local, "X-Cause", cause->message,
                                         error);
}

static rotmos_status ordinary(rotmos_request *request, rotmos_reply *reply,
                              void *data, rotmos_error *error)
{
    (void)data;
    if (rotmos_request_path(request).length == 5 &&
        memcmp(rotmos_request_path(request).data, "/stop", 5) == 0)
        OBSERVE(rotmos_server_request_stop(rotmos_request_server(request),
                                           error) == ROTMOS_OK);
    return rotmos_reply_bytes(reply, 200, "text/plain", view("pong"), error);
}
static void interim_writable(rotmos_response *response, void *data)
{
    (void)data;
    OBSERVE(!rotmos_response_is_committed(response));
    OBSERVE(rotmos_response_replace_uncommitted(response, NULL) == ROTMOS_OK);
    OBSERVE(rotmos_response_bytes(response, 502, "text/plain", view("replaced"),
                                  NULL) == ROTMOS_OK);
}
static void interim_closed(rotmos_request *request,
                           const rotmos_request_outcome *out, void *data)
{
    (void)request;
    (void)data;
    ++interim_closes;
    if (out->response.status != 502 || !out->response.committed)
        (void)fprintf(stderr,
                      "interim close: status=%d cause=%d committed=%d "
                      "wire=%lu interim=%lu\n",
                      out->response.status, (int)out->response.cause,
                      out->response.committed, out->response.wire_bytes.low,
                      out->response.interim_wire_bytes.low);
    OBSERVE(out->response.status == 502 &&
            out->response.interim_wire_bytes.high == 0 &&
            out->response.interim_wire_bytes.low > 3000 &&
            out->response.committed);
}
static rotmos_status interim_route(rotmos_request *request,
                                   rotmos_request_plan *plan, void *data,
                                   rotmos_error *error)
{
    rotmos_header field;
    rotmos_response *r;
    char *padding;
    (void)data;
    r = rotmos_request_response(request);
    padding = malloc(3001);
    if (padding == NULL)
        return ROTMOS_NOMEM;
    memset(padding, 'x', 3000);
    padding[3000] = 0;
    field.name = view("X-Pad");
    field.value = view(padding);
    OBSERVE(rotmos_response_callbacks(r, interim_writable, NULL, NULL, error) ==
            ROTMOS_OK);
    OBSERVE(rotmos_response_interim(r, 101, NULL, 0, error) == ROTMOS_INVALID);
    {
        rotmos_header invalid;
        invalid.name = view("Content-Length");
        invalid.value = view("0");
        OBSERVE(rotmos_response_interim(r, 103, &invalid, 1, error) ==
                ROTMOS_INVALID);
        invalid.name = view("X-Test");
        invalid.value.data = "a\0b";
        invalid.value.length = 3;
        OBSERVE(rotmos_response_interim(r, 103, &invalid, 1, error) ==
                ROTMOS_INVALID);
        invalid.value.data = padding;
        invalid.value.length = 4096;
        OBSERVE(rotmos_response_interim(r, 103, &invalid, 1, error) ==
                ROTMOS_LIMIT);
    }
    OBSERVE(rotmos_response_interim(r, 103, &field, 1, error) == ROTMOS_OK);
    OBSERVE(rotmos_response_interim(r, 103, &field, 1, error) == ROTMOS_AGAIN);
    plan->on_close = interim_closed;
    plan->on_body_end = empty_complete;
    free(padding);
    rotmos_error_clear(error);
    /* A deferred managed response must supply an empty-body completion. */
    return ROTMOS_OK;
}

static int exchange(unsigned short port, const char *path, int expected,
                    const char *body)
{
    char request[512];
    char response[8192];
    size_t used;
    size_t at;
    int binary_found;
    ssize_t count;
    int fd;
    fd = connect_to(port);
    CHECK(fd >= 0);
    (void)snprintf(request, sizeof(request),
                   "GET %s HTTP/1.1\r\nHost: client.test\r\nConnection: "
                   "close\r\nX-Forwarded-For: poison\r\n\r\n",
                   path);
    CHECK(write_all(fd, request, strlen(request)) == 0);
    used = 0;
    do {
        count = read(fd, response + used, sizeof(response) - 1 - used);
        CHECK(count >= 0);
        used += (size_t)count;
        CHECK(used + 1 < sizeof(response));
    } while (count);
    response[used] = 0;
    (void)snprintf(request, sizeof(request), " %d ", expected);
    if (strstr(response, request) == NULL || strstr(response, body) == NULL)
        (void)fprintf(stderr, "unexpected response for %s: %.256s\n", path,
                      response);
    CHECK(strstr(response, request) != NULL && strstr(response, body) != NULL);
    if (strcmp(path, "/deny") == 0) {
        binary_found = 0;
        for (at = 0; at + 3 <= used; ++at)
            if (memcmp(response + at, "n\0o", 3) == 0)
                ++binary_found;
        CHECK(binary_found == 1);
    }
    if (expected == 200 && strcmp(path, "/finish") != 0 &&
        strcmp(path, "/ping") != 0 && strcmp(path, "/stop") != 0)
        CHECK(strstr(response, "a=1") != NULL &&
              strstr(response, "b=2") != NULL &&
              strstr(response, "remove") == NULL &&
              strstr(response, "edited") != NULL);
    if (strcmp(path, "/interim") == 0)
        CHECK(strstr(response, " 103 ") != NULL &&
              strstr(response, " 200 ") == NULL);
    (void)close(fd);
    return 0;
}

static int client(unsigned short port, int control)
{
    const char *message;
    char metadata[4096];
    char data[2048];
    char command;
    char *upload;
    size_t length;
    size_t received;
    int done;
    int fd;
    int other;
    CHECK(exchange(port, "/p/ok", 200, "pong") == 0);
    CHECK(exchange(port, "/raw/a%2Fb?q=%2B", 200, "pong") == 0);
    CHECK(exchange(port, "/deny", 403, "n") == 0);
    CHECK(exchange(port, "/drop", 503, "gateway") == 0);
    CHECK(exchange(port, "/badedit", 503, "gateway") == 0);
    CHECK(exchange(port, "/interim", 502, "replaced") == 0);
    fd = connect_to(port);
    CHECK(fd >= 0);
    message = "POST /upload HTTP/1.1\r\nHost: client.test\r\nContent-Length: "
              "65537\r\nExpect: 100-continue\r\n\r\n";
    CHECK(write_all(fd, message, strlen(message)) == 0 &&
          head(fd, metadata, sizeof(metadata)) == 0 &&
          strstr(metadata, " 413 ") != NULL);
    (void)close(fd);
    fd = connect_to(port);
    CHECK(fd >= 0);
    message = "GET /local-large HTTP/1.1\r\nHost: client.test\r\n\r\n";
    CHECK(write_all(fd, message, strlen(message)) == 0 &&
          head(fd, metadata, sizeof(metadata)) == 0);
    CHECK(strstr(metadata, " 200 ") != NULL &&
          strstr(metadata, "a=1") != NULL && strstr(metadata, "b=2") != NULL);
    received = 0;
    while (received < 65536) {
        size_t at;
        CHECK(read_exact(fd, data, sizeof(data)) == 0);
        for (at = 0; at < sizeof(data); ++at)
            CHECK(data[at] == 'l');
        received += sizeof(data);
    }
    CHECK(read(fd, data, 1) == 0);
    (void)close(fd);
    fd = connect_to(port);
    CHECK(fd >= 0);
    message = "HEAD /raw/head HTTP/1.1\r\nHost: client.test\r\nContent-Length: "
              "0\r\n\r\n";
    CHECK(write_all(fd, message, strlen(message)) == 0 &&
          head(fd, metadata, sizeof(metadata)) == 0);
    CHECK(strstr(metadata, " 200 ") != NULL &&
          strstr(metadata, "content-length: 4") != NULL &&
          read(fd, data, 1) == 0);
    (void)close(fd);
    upload = malloc(65536);
    CHECK(upload != NULL);
    memset(upload, 'x', 65536);
    fd = connect_to(port);
    CHECK(fd >= 0);
    message = "POST /upload HTTP/1.1\r\nHost: client.test\r\nContent-Length: "
              "65536\r\nExpect: 100-continue\r\n\r\n";
    CHECK(write_all(fd, message, strlen(message)) == 0 &&
          head(fd, metadata, sizeof(metadata)) == 0 &&
          strstr(metadata, " 100 ") != NULL);
    CHECK(write_all(fd, upload, 8192) == 0);
    CHECK(head(fd, metadata, sizeof(metadata)) == 0);
    if (strstr(metadata, " 103 ") == NULL)
        (void)fprintf(stderr, "upload response: %s\n", metadata);
    CHECK(strstr(metadata, " 103 ") != NULL);
    CHECK(strstr(metadata, "Link: </ready>") != NULL);
    CHECK(head(fd, metadata, sizeof(metadata)) == 0 &&
          strstr(metadata, " 200 ") != NULL);
    CHECK(read_chunk(fd, data, sizeof(data), &received, &done) == 0 && !done &&
          received != 0);
    CHECK(write_all(fd, upload + 8192, 65536 - 8192) == 0);
    do {
        CHECK(read_chunk(fd, data, sizeof(data), &length, &done) == 0);
        received += length;
    } while (!done);
    CHECK(received == 65536 && finish_trailers == 1);
    (void)close(fd);
    free(upload);
    fd = connect_to(port);
    CHECK(fd >= 0);
    message = "POST /chunked HTTP/1.1\r\nHost: "
              "client.test\r\nTransfer-Encoding: chunked\r\nTrailer: "
              "X-Req\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\nX-Req: yes\r\n\r\n";
    CHECK(write_all(fd, message, strlen(message)) == 0 &&
          head(fd, metadata, sizeof(metadata)) == 0 &&
          strstr(metadata, " 200 ") != NULL);
    do {
        CHECK(read_chunk(fd, data, sizeof(data), &length, &done) == 0);
    } while (!done);
    (void)close(fd);
    fd = connect_to(port);
    CHECK(fd >= 0);
    message = "GET /events HTTP/1.1\r\nHost: client.test\r\n\r\n";
    CHECK(write_all(fd, message, strlen(message)) == 0 &&
          head(fd, metadata, sizeof(metadata)) == 0 &&
          strstr(metadata, "text/event-stream") != NULL);
    received = 0;
    while (received < 13) {
        CHECK(read_chunk(fd, data, sizeof(data), &length, &done) == 0 && !done);
        received += length;
    }
    CHECK(received == 13 && exchange(port, "/ping", 200, "pong") == 0 &&
          write_all(control, "n", 1) == 0);
    do {
        CHECK(read_chunk(fd, data, sizeof(data), &length, &done) == 0);
        received += length;
    } while (!done);
    CHECK(received == 25);
    (void)close(fd);
    fd = connect_to(port);
    CHECK(fd >= 0);
    message = "GET /idle HTTP/1.1\r\nHost: client.test\r\n\r\n";
    CHECK(write_all(fd, message, strlen(message)) == 0 &&
          read_exact(control, &command, 1) == 0);
    CHECK(exchange(port, "/raw/admission", 503, "exchange limit") == 0);
    CHECK(head(fd, metadata, sizeof(metadata)) == 0 &&
          strstr(metadata, " 504 ") != NULL);
    (void)close(fd);
    CHECK(write_all(control, "n", 1) == 0);
    fd = connect_to(port);
    CHECK(fd >= 0);
    message = "GET /total HTTP/1.1\r\nHost: client.test\r\n\r\n";
    CHECK(write_all(fd, message, strlen(message)) == 0 &&
          read_exact(control, &command, 1) == 0 &&
          head(fd, metadata, sizeof(metadata)) == 0);
    CHECK(strstr(metadata, " 200 ") != NULL &&
          read_chunk(fd, data, sizeof(data), &length, &done) == 0 &&
          length == 3 && !done);
    CHECK(read(fd, data, sizeof(data)) == 0 && write_all(control, "n", 1) == 0);
    (void)close(fd);
    fd = connect_to(port);
    CHECK(fd >= 0);
    message = "GET /truncated HTTP/1.1\r\nHost: client.test\r\n\r\n";
    CHECK(write_all(fd, message, strlen(message)) == 0 &&
          read_exact(control, &command, 1) == 0 &&
          head(fd, metadata, sizeof(metadata)) == 0 &&
          strstr(metadata, " 200 ") != NULL);
    CHECK(read_chunk(fd, data, sizeof(data), &length, &done) == 0 &&
          length == 3 && !done && write_all(control, "n", 1) == 0);
    CHECK(read(fd, data, sizeof(data)) == 0);
    (void)close(fd);
    CHECK(exchange(port, "/finish", 200, "pong") == 0);
    other = exchange(port, "/stop", 200, "pong");
    CHECK(other == 0);
    return 0;
}

static rotmos_status empty_complete(rotmos_request *r, void *data,
                                    rotmos_error *error)
{
    (void)r;
    (void)data;
    (void)error;
    return ROTMOS_OK;
}

static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *p, void *data,
                           rotmos_error *error)
{
    struct fixture *f;
    rotmos_bound_listener listener;
    int result;
    (void)p;
    f = data;
    if (rotmos_server_listener_at(server, 0, &listener, error) != ROTMOS_OK)
        return ROTMOS_INVALID;
    f->origin_pid = fork();
    if (f->origin_pid < 0)
        return ROTMOS_IO;
    if (f->origin_pid == 0) {
        (void)signal(SIGTERM, SIG_DFL);
        (void)close(f->control[0]);
        result = origin(f->origin, f->control[1]);
        if (result)
            (void)kill(getppid(), SIGTERM);
        _exit(result);
    }
    f->client = fork();
    if (f->client < 0)
        return ROTMOS_IO;
    if (f->client == 0) {
        (void)signal(SIGTERM, SIG_DFL);
        (void)close(f->control[1]);
        (void)close(f->origin);
        result = client(listener.port, f->control[0]);
        if (result)
            (void)kill(getppid(), SIGTERM);
        _exit(result);
    }
    (void)close(f->origin);
    (void)close(f->control[0]);
    (void)close(f->control[1]);
    return ROTMOS_OK;
}
static int exited(rotmos_server *server, const rotmos_process_context *p,
                  void *data)
{
    (void)server;
    (void)p;
    (void)data;
    OBSERVE(rewrites >= 8 && modified >= 6 && gateway_errors == 3 &&
            interim_closes == 1);
    return failures ? 1 : 0;
}

static rotmos_status worker_start(rotmos_server *server,
                                  const rotmos_process_context *p, void *data,
                                  rotmos_error *error)
{
    struct fixture *f;
    (void)server;
    (void)p;
    (void)error;
    f = data;
    (void)close(f->origin);
    (void)close(f->control[0]);
    (void)close(f->control[1]);
    return ROTMOS_OK;
}

int main(void)
{
    struct fixture f;
    struct sockaddr_in address;
    socklen_t size;
    char target[128];
    char alternate[128];
    const char *alternates[1];
    const char *paths[] = {"/p/:item", "^/raw/.*$",  "/deny",    "/drop",
                           "/badedit", "/upload",    "/chunked", "/events",
                           "/idle",    "/truncated", "/finish",  "/local-large",
                           "/total"};
    rotmos_server_config config;
    rotmos_server_effective effective;
    rotmos_listener_config listener;
    rotmos_proxy_config proxy;
    rotmos_route_config route;
    rotmos_server *server;
    rotmos_run_result result;
    rotmos_error error;
    rotmos_status status;
    int child_status;
    size_t i;
    memset(&f, 0, sizeof(f));
    f.client = f.origin_pid = -1;
    (void)signal(SIGPIPE, SIG_IGN);
    f.origin = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(f.origin >= 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, f.control) == 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(f.origin, (struct sockaddr *)&address, sizeof(address)) == 0 &&
          listen(f.origin, 8) == 0);
    size = sizeof(address);
    CHECK(getsockname(f.origin, (struct sockaddr *)&address, &size) == 0);
    f.origin_port = ntohs(address.sin_port);
    (void)snprintf(target, sizeof(target), "http://127.0.0.1:%u/base",
                   f.origin_port);
    (void)snprintf(alternate, sizeof(alternate), "http://127.0.0.1:%u/alt",
                   f.origin_port);
    alternates[0] = alternate;
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.io.chunk_bytes = 1024;
    config.io.output_queue_bytes = 4096;
    config.proxy.active_exchanges = 1;
    config.limits.hard_body_bytes.low = 65536;
    config.limits.default_body_bytes.low = 65536;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_start = worker_start;
    config.callbacks.on_worker_exit = exited;
    config.callbacks.userdata = &f;
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    CHECK(rotmos_server_effective_config(server, &effective, &error) ==
              ROTMOS_OK &&
          effective.config.proxy.active_exchanges == 1 &&
          effective.config.proxy.idle_connections == 4);
    rotmos_proxy_config_init(&proxy);
    proxy.path = "/invalid";
    proxy.target = "http://user:pass@localhost/";
    CHECK(rotmos_router_proxy(rotmos_server_router(server), &proxy, NULL,
                              &error) == ROTMOS_INVALID);
    proxy.target = target;
    proxy.buffer_limit_bytes = 8191;
    CHECK(rotmos_router_proxy(rotmos_server_router(server), &proxy, NULL,
                              &error) == ROTMOS_INVALID);
    proxy.buffer_limit_bytes = 0;
    proxy.tls_client_cert_pem = "incomplete identity";
    CHECK(rotmos_router_proxy(rotmos_server_router(server), &proxy, NULL,
                              &error) == ROTMOS_INVALID);
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        rotmos_proxy_config_init(&proxy);
        proxy.path = paths[i];
        proxy.path_kind =
            i == 0 ? ROTMOS_PATH_PARAMS
                   : (i == 1 ? ROTMOS_PATH_REGEX : ROTMOS_PATH_LITERAL);
        proxy.target = target;
        proxy.alternate_targets = alternates;
        proxy.alternate_target_count = 1;
        proxy.preflight = preflight;
        proxy.rewrite = rewrite;
        proxy.modify_response = modify;
        proxy.on_error = gateway;
        proxy.buffer_limit_bytes = 8192;
        proxy.idle_timeout_ms = i == 8 ? 100 : 2000;
        proxy.connect_timeout_ms = 1000;
        proxy.total_timeout_ms = strcmp(paths[i], "/total") == 0 ? 100 : 0;
        status = rotmos_router_proxy(rotmos_server_router(server), &proxy, NULL,
                                     &error);
        if (status != ROTMOS_OK)
            (void)fprintf(stderr, "proxy registration %s: %s\n", proxy.path,
                          error.message);
        CHECK(status == ROTMOS_OK);
    }
    strcpy(target, "changed");
    CHECK(rewrites == 0 && modified == 0 && gateway_errors == 0);
    strcpy(alternate, "changed");
    alternates[0] = NULL;
    rotmos_route_config_init(&route);
    route.path = "/ping";
    route.handler = ordinary;
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    route.path = "/stop";
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    rotmos_route_config_init(&route);
    route.path = "/interim";
    route.on_headers = interim_route;
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    status = rotmos_server_run(server, &result, &error);
    if (status != ROTMOS_OK)
        (void)fprintf(stderr, "%s: %s\n", error.operation, error.message);
    if (status != ROTMOS_OK) {
        if (f.client > 0) {
            (void)kill(f.client, SIGTERM);
            (void)waitpid(f.client, NULL, 0);
        }
        if (f.origin_pid > 0) {
            (void)kill(f.origin_pid, SIGTERM);
            (void)waitpid(f.origin_pid, NULL, 0);
        }
        (void)rotmos_server_destroy(server, NULL);
        return 1;
    }
    CHECK(status == ROTMOS_OK);
    CHECK(waitpid(f.client, &child_status, 0) == f.client &&
          WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(waitpid(f.origin_pid, &child_status, 0) == f.origin_pid &&
          WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    return 0;
}

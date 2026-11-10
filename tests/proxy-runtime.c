#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
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
    int stop_notice[2];
    int completed[2];
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
static int request_trailers;
static unsigned long reported_ids[32];
static int reported, alternate_report, upload_report, pressure_report,
    cancel_report;
static int shutdown_report, upstream_observed;
static void observed(knurl_server *server, const knurl_observation *event,
                     void *data)
{
    (void)server;
    (void)data;
    if (event->kind == KNURL_OBSERVE_REQUEST_COMPLETE && event->request &&
        !strcmp(knurl_request_path(event->request).data, "/drop")) {
        OBSERVE(event->disconnect_side == KNURL_DISCONNECT_UPSTREAM);
        ++upstream_observed;
    }
}
static void exchange_report(knurl_request *request,
                            const knurl_proxy_exchange *event, void *data)
{
    struct fixture *f = data;
    size_t i;
    knurl_bytes path = knurl_request_path(request);
    OBSERVE(reported < 32 &&
            event->request_id.high == (unsigned long)getpid() &&
            event->request_id.low != 0 && !strcmp(event->kind, "http"));
    for (i = 0; i < (size_t)reported && i < 32; ++i)
        OBSERVE(reported_ids[i] != event->request_id.low);
    if (reported < 32)
        reported_ids[reported] = event->request_id.low;
    ++reported;
    if (!strcmp(event->route, "/shutdown")) {
        OBSERVE(!strcmp(event->disconnect_side, "worker") &&
                !strcmp(event->error_category, "shutdown") &&
                event->downstream_bytes.low == 3);
        ++shutdown_report;
    }
    if (path.length == 5 && !memcmp(path.data, "/p/ok", 5)) {
        OBSERVE(strstr(event->upstream, "/alt") && event->status == 200 &&
                event->upstream_bytes.low == 0 &&
                event->downstream_bytes.low == 4 &&
                !strcmp(event->error_category, "none"));
        ++alternate_report;
    }
    if (!strcmp(event->route, "/upload")) {
        OBSERVE(event->upstream_bytes.low == 65536 &&
                event->downstream_bytes.low == 65536 && event->status == 200);
        ++upload_report;
    }
    if (!strcmp(event->route, "/pressure")) {
        OBSERVE(event->downstream_bytes.low == 8u * 1024u * 1024u &&
                !strcmp(event->disconnect_side, "none"));
        ++pressure_report;
    }
    if (!strcmp(event->route, "/pressure/cancel")) {
        OBSERVE(event->downstream_bytes.low < 8u * 1024u * 1024u &&
                !strcmp(event->disconnect_side, "downstream") &&
                !strcmp(event->error_category, "disconnect"));
        ++cancel_report;
    }
    if (!strcmp(event->route, "/drop") || !strcmp(event->route, "/truncated"))
        OBSERVE(!strcmp(event->disconnect_side, "upstream"));
    if (strcmp(event->route, "/shutdown") != 0)
        OBSERVE(write(f->completed[1], "c", 1) == 1);
}
static knurl_status empty_complete(knurl_request *, void *, knurl_error *);

static knurl_bytes view(const char *s)
{
    knurl_bytes b;
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
            if (strcmp(line, "X-Req: yes\r\n") == 0)
                ++request_trailers;
        } while (used != 2);
    }
    return 0;
}

/* The baseline h2-memory/duplex fixtures stop downstream reads while the
 * producer is live. Exercise that invariant with a bounded HTTP/1 transfer:
 * kernel windows plus the relay queues must fill before the entire body. */
#define PRESSURE_BYTES (8u * 1024u * 1024u)
static int pressure_origin(int fd, int control, int cancel)
{
    const char *metadata = "HTTP/1.1 200 OK\r\nContent-Length: 8388608\r\n"
                           "Connection: close\r\n\r\n";
    struct pollfd watch;
    char bytes[1024], command;
    size_t sent = 0;
    ssize_t n;
    int flags, limit = 65536;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &limit, sizeof(limit)) == 0);
    CHECK(write_all(fd, metadata, strlen(metadata)) == 0);
    flags = fcntl(fd, F_GETFL);
    CHECK(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    memset(bytes, 'p', sizeof(bytes));
    watch.fd = fd;
    watch.events = POLLOUT;
    while (sent < PRESSURE_BYTES) {
        size_t size = PRESSURE_BYTES - sent;
        if (size > sizeof(bytes))
            size = sizeof(bytes);
        n = write(fd, bytes, size);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        CHECK(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        n = poll(&watch, 1, 100);
        CHECK(n >= 0);
        if (!n)
            break;
    }
    CHECK(sent > 0 && sent < PRESSURE_BYTES);
    CHECK(write_all(control, "p", 1) == 0 &&
          read_exact(control, &command, 1) == 0 && command == 'r');
    CHECK(fcntl(fd, F_SETFL, flags) == 0);
    if (cancel) {
        watch.events = POLLIN;
        CHECK(poll(&watch, 1, 1000) == 1);
        n = read(fd, bytes, 1);
        /* Closing a pressured socket can deliver FIN or reset, depending on
         * unread kernel data. Both prove the upstream exchange was retired. */
        CHECK(n == 0 || (n < 0 && errno == ECONNRESET));
        CHECK(write_all(control, "c", 1) == 0);
    } else {
        while (sent < PRESSURE_BYTES) {
            size_t size = PRESSURE_BYTES - sent;
            if (size > sizeof(bytes))
                size = sizeof(bytes);
            CHECK(write_all(fd, bytes, size) == 0);
            sent += size;
        }
    }
    return 0;
}

static int origin(int listener, int control)
{
    char metadata[4096];
    char buffer[1024];
    char command;
    size_t count;
    size_t total;
    size_t remaining;
    ssize_t got;
    int fd;
    int finish, done;
    for (;;) {
        fd = accept(listener, NULL, NULL);
        CHECK(fd >= 0);
        CHECK(head(fd, metadata, sizeof(metadata)) == 0);
        if (strstr(metadata, " /base/pressure") != NULL) {
            CHECK(pressure_origin(fd, control,
                                  strstr(metadata, "cancel") != NULL) == 0);
            (void)close(fd);
            continue;
        }
        finish = strstr(metadata, " /base/shutdown ") != NULL;
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
            strstr(metadata, " /base/total ") != NULL || finish) {
            const char *message =
                "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc";
            CHECK(write_all(fd, message, strlen(message)) == 0 &&
                  write_all(control, "r", 1) == 0 &&
                  read_exact(control, &command, 1) == 0);
            (void)close(fd);
            if (finish)
                break;
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
            total = 0;
            request_trailers = 0;
            do {
                CHECK(read_chunk(fd, buffer, sizeof(buffer), &count, &done) ==
                      0);
                total += count;
            } while (!done);
            CHECK(total == 5 && request_trailers == 1);
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

static knurl_status preflight(const knurl_proxy_inbound *in,
                              knurl_proxy_local_response *local, void *data,
                              knurl_error *error)
{
    (void)data;
    if (strcmp(knurl_proxy_inbound_path(in), "/local-large") == 0) {
        char *body;
        knurl_status status;
        body = malloc(65536);
        if (body == NULL)
            return KNURL_NOMEM;
        memset(body, 'l', 65536);
        status = knurl_proxy_local_respond(local, 200, body, 65536, error);
        free(body);
        if (status == KNURL_OK)
            status =
                knurl_proxy_local_add_header(local, "Set-Cookie", "a=1", error);
        if (status == KNURL_OK)
            status =
                knurl_proxy_local_add_header(local, "Set-Cookie", "b=2", error);
        return status;
    }
    if (strcmp(knurl_proxy_inbound_path(in), "/deny") == 0) {
        const char bytes[3] = {'n', 0, 'o'};
        return knurl_proxy_local_respond(local, 403, bytes, sizeof(bytes),
                                         error);
    }
    return KNURL_OK;
}
static knurl_status rewrite(const knurl_proxy_inbound *in,
                            knurl_proxy_outbound *out, void *data,
                            knurl_error *error)
{
    knurl_bytes value;
    (void)data;
    ++rewrites;
    if (strncmp(knurl_proxy_inbound_path(in), "/p/", 3) == 0) {
        value = knurl_proxy_inbound_path_param(in, "item");
        OBSERVE(value.length == 2 && memcmp(value.data, "ok", 2) == 0);
        OBSERVE(knurl_proxy_outbound_select_target(out, 1, error) == KNURL_OK);
        OBSERVE(knurl_proxy_outbound_set_path(out, "/echo%2Fraw", error) ==
                KNURL_OK);
        OBSERVE(knurl_proxy_outbound_set_query(out, "a=1&a=2", error) ==
                KNURL_OK);
        return knurl_proxy_outbound_set_host(out, "selected.test", error);
    }
    return KNURL_OK;
}
static knurl_status modify(knurl_proxy_response *response, void *data,
                           knurl_error *error)
{
    const char *name;
    const char *value;
    size_t i;
    (void)data;
    ++modified;
    for (i = 0; i < knurl_proxy_response_header_count(response); ++i) {
        OBSERVE(knurl_proxy_response_header_at(response, i, &name, &value) ==
                KNURL_OK);
        if (strcmp(name, "X-Mode") == 0 && strcmp(value, "invalid") == 0) {
            OBSERVE(knurl_proxy_response_add_header(response, "Content-Length",
                                                    "4",
                                                    error) == KNURL_INVALID);
            return KNURL_OK;
        }
    }
    return knurl_proxy_response_add_header(response, "X-Proxy", "edited",
                                           error);
}
static knurl_status gateway(const knurl_error *cause, int code,
                            knurl_proxy_local_response *local, void *data,
                            knurl_error *error)
{
    (void)data;
    OBSERVE(cause->status != KNURL_OK && (code == 502 || code == 504));
    ++gateway_errors;
    OBSERVE(knurl_proxy_local_respond(local, code == 502 ? 503 : 504,
                                      code == 502 ? "gateway" : "timeout", 7,
                                      error) == KNURL_OK);
    return knurl_proxy_local_add_header(local, "X-Cause", cause->message,
                                        error);
}

static knurl_status ordinary(knurl_request *request, knurl_reply *reply,
                             void *data, knurl_error *error)
{
    (void)data;
    if (knurl_request_path(request).length == 5 &&
        memcmp(knurl_request_path(request).data, "/stop", 5) == 0)
        OBSERVE(knurl_server_request_stop(knurl_request_server(request),
                                          error) == KNURL_OK);
    return knurl_reply_bytes(reply, 200, "text/plain", view("pong"), error);
}
static void interim_writable(knurl_response *response, void *data)
{
    (void)data;
    OBSERVE(!knurl_response_is_committed(response));
    OBSERVE(knurl_response_replace_uncommitted(response, NULL) == KNURL_OK);
    OBSERVE(knurl_response_bytes(response, 502, "text/plain", view("replaced"),
                                 NULL) == KNURL_OK);
}
static void interim_closed(knurl_request *request,
                           const knurl_request_outcome *out, void *data)
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
static knurl_status interim_route(knurl_request *request,
                                  knurl_request_plan *plan, void *data,
                                  knurl_error *error)
{
    knurl_header field;
    knurl_response *r;
    char *padding;
    (void)data;
    r = knurl_request_response(request);
    padding = malloc(3001);
    if (padding == NULL)
        return KNURL_NOMEM;
    memset(padding, 'x', 3000);
    padding[3000] = 0;
    field.name = view("X-Pad");
    field.value = view(padding);
    OBSERVE(knurl_response_callbacks(r, interim_writable, NULL, NULL, error) ==
            KNURL_OK);
    OBSERVE(knurl_response_interim(r, 101, NULL, 0, error) == KNURL_INVALID);
    {
        knurl_header invalid;
        invalid.name = view("Content-Length");
        invalid.value = view("0");
        OBSERVE(knurl_response_interim(r, 103, &invalid, 1, error) ==
                KNURL_INVALID);
        invalid.name = view("X-Test");
        invalid.value.data = "a\0b";
        invalid.value.length = 3;
        OBSERVE(knurl_response_interim(r, 103, &invalid, 1, error) ==
                KNURL_INVALID);
        invalid.value.data = padding;
        invalid.value.length = 4096;
        OBSERVE(knurl_response_interim(r, 103, &invalid, 1, error) ==
                KNURL_LIMIT);
    }
    OBSERVE(knurl_response_interim(r, 103, &field, 1, error) == KNURL_OK);
    OBSERVE(knurl_response_interim(r, 103, &field, 1, error) == KNURL_AGAIN);
    plan->on_close = interim_closed;
    plan->on_body_end = empty_complete;
    free(padding);
    knurl_error_clear(error);
    /* A deferred managed response must supply an empty-body completion. */
    return KNURL_OK;
}

static int wait_complete(int fd)
{
    /* Reading a response does not establish that its proxy slot is free.
     * The terminal callback and slot retirement run in the same worker turn,
     * before that worker can admit the client's next exchange.
     */
    struct pollfd event;
    char byte;
    memset(&event, 0, sizeof(event));
    event.fd = fd;
    event.events = POLLIN;
    CHECK(poll(&event, 1, 3000) == 1 && read(fd, &byte, 1) == 1 && byte == 'c');
    return 0;
}

static int exchange(unsigned short port, const char *path, int expected,
                    const char *body, int completed)
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
    if (completed >= 0)
        CHECK(wait_complete(completed) == 0);
    return 0;
}

static int client(unsigned short port, int control, int stopping, int completed)
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
    int other, cancel;
    CHECK(exchange(port, "/p/ok", 200, "pong", completed) == 0);
    CHECK(exchange(port, "/raw/a%2Fb?q=%2B", 200, "pong", completed) == 0);
    CHECK(exchange(port, "/deny", 403, "n", -1) == 0);
    CHECK(exchange(port, "/drop", 503, "gateway", completed) == 0);
    CHECK(exchange(port, "/badedit", 503, "gateway", completed) == 0);
    CHECK(exchange(port, "/interim", 502, "replaced", -1) == 0);
    for (cancel = 0; cancel < 2; ++cancel) {
        size_t i;
        fd = connect_to(port);
        other = 65536;
        CHECK(fd >= 0 && setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &other,
                                    sizeof(other)) == 0);
        message = cancel
                      ? "GET /pressure/cancel HTTP/1.1\r\nHost: "
                        "client.test\r\n\r\n"
                      : "GET /pressure HTTP/1.1\r\nHost: client.test\r\n\r\n";
        CHECK(write_all(fd, message, strlen(message)) == 0 &&
              read_exact(control, &command, 1) == 0 && command == 'p');
        CHECK(exchange(port, "/ping", 200, "pong", -1) == 0);
        if (cancel)
            CHECK(close(fd) == 0);
        CHECK(write_all(control, "r", 1) == 0);
        if (cancel) {
            CHECK(read_exact(control, &command, 1) == 0 && command == 'c');
        } else {
            CHECK(head(fd, metadata, sizeof(metadata)) == 0 &&
                  strstr(metadata, " 200 ") != NULL);
            received = 0;
            do {
                CHECK(read_chunk(fd, data, sizeof(data), &length, &done) == 0);
                for (i = 0; i < length; ++i)
                    CHECK(data[i] == 'p');
                received += length;
            } while (!done);
            CHECK(received == PRESSURE_BYTES && close(fd) == 0);
        }
        CHECK(wait_complete(completed) == 0);
        CHECK(exchange(port, "/raw/recovered", 200, "pong", completed) == 0);
    }
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
    CHECK(wait_complete(completed) == 0);
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
    CHECK(wait_complete(completed) == 0);
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
    CHECK(wait_complete(completed) == 0);
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
    CHECK(received == 13 && exchange(port, "/ping", 200, "pong", -1) == 0 &&
          write_all(control, "n", 1) == 0);
    do {
        CHECK(read_chunk(fd, data, sizeof(data), &length, &done) == 0);
        received += length;
    } while (!done);
    CHECK(received == 25);
    (void)close(fd);
    CHECK(wait_complete(completed) == 0);
    fd = connect_to(port);
    CHECK(fd >= 0);
    message = "GET /idle HTTP/1.1\r\nHost: client.test\r\n\r\n";
    CHECK(write_all(fd, message, strlen(message)) == 0 &&
          read_exact(control, &command, 1) == 0);
    CHECK(exchange(port, "/raw/admission", 503, "exchange limit", -1) == 0);
    CHECK(head(fd, metadata, sizeof(metadata)) == 0 &&
          strstr(metadata, " 504 ") != NULL);
    (void)close(fd);
    CHECK(wait_complete(completed) == 0);
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
    CHECK(read(fd, data, sizeof(data)) == 0 && wait_complete(completed) == 0 &&
          write_all(control, "n", 1) == 0);
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
    CHECK(wait_complete(completed) == 0);
    CHECK(exchange(port, "/finish", 200, "pong", completed) == 0);
    fd = connect_to(port);
    message = "GET /shutdown HTTP/1.1\r\nHost: client.test\r\n\r\n";
    CHECK(fd >= 0 && write_all(fd, message, strlen(message)) == 0 &&
          read_exact(control, &command, 1) == 0 && command == 'r' &&
          head(fd, metadata, sizeof(metadata)) == 0 &&
          read_chunk(fd, data, sizeof(data), &length, &done) == 0 &&
          length == 3 && !done);
    other = exchange(port, "/stop", 200, "pong", -1);
    CHECK(other == 0);
    CHECK(read_exact(stopping, &command, 1) == 0 && command == 's');
    CHECK(read(fd, data, 1) == 0 && close(fd) == 0 &&
          write_all(control, "n", 1) == 0);
    return 0;
}

static knurl_status empty_complete(knurl_request *r, void *data,
                                   knurl_error *error)
{
    (void)r;
    (void)data;
    (void)error;
    return KNURL_OK;
}

static knurl_status ready(knurl_server *server, const knurl_process_context *p,
                          void *data, knurl_error *error)
{
    struct fixture *f;
    knurl_bound_listener listener;
    int result;
    (void)p;
    f = data;
    if (knurl_server_listener_at(server, 0, &listener, error) != KNURL_OK)
        return KNURL_INVALID;
    if (knurl_server_set_serving(server, 1, error) != KNURL_OK)
        return KNURL_STATE;
    f->origin_pid = fork();
    if (f->origin_pid < 0)
        return KNURL_IO;
    if (f->origin_pid == 0) {
        close(f->completed[0]);
        close(f->completed[1]);
        close(f->stop_notice[0]);
        close(f->stop_notice[1]);
        (void)signal(SIGTERM, SIG_DFL);
        (void)close(f->control[0]);
        result = origin(f->origin, f->control[1]);
        if (result)
            (void)kill(getppid(), SIGTERM);
        _exit(result);
    }
    f->client = fork();
    if (f->client < 0)
        return KNURL_IO;
    if (f->client == 0) {
        close(f->completed[1]);
        close(f->stop_notice[1]);
        (void)signal(SIGTERM, SIG_DFL);
        (void)close(f->control[1]);
        (void)close(f->origin);
        result = client(listener.port, f->control[0], f->stop_notice[0],
                        f->completed[0]);
        if (result)
            (void)kill(getppid(), SIGTERM);
        _exit(result);
    }
    (void)close(f->origin);
    (void)close(f->control[0]);
    (void)close(f->control[1]);
    close(f->stop_notice[0]);
    close(f->stop_notice[1]);
    close(f->completed[0]);
    close(f->completed[1]);
    return KNURL_OK;
}
static int exited(knurl_server *server, const knurl_process_context *p,
                  void *data)
{
    (void)server;
    (void)p;
    (void)data;
    OBSERVE(rewrites >= 8 && modified >= 6 && gateway_errors == 3 &&
            interim_closes == 1);
    OBSERVE(reported == 17 && alternate_report == 1 && upload_report == 1 &&
            pressure_report == 1 && cancel_report == 1 &&
            shutdown_report == 1 && upstream_observed == 1);
    close(((struct fixture *)data)->stop_notice[1]);
    close(((struct fixture *)data)->completed[1]);
    return failures ? 1 : 0;
}

static knurl_status worker_start(knurl_server *server,
                                 const knurl_process_context *p, void *data,
                                 knurl_error *error)
{
    struct fixture *f;
    (void)server;
    (void)p;
    (void)error;
    f = data;
    close(f->completed[0]);
    close(f->stop_notice[0]);
    (void)close(f->origin);
    (void)close(f->control[0]);
    (void)close(f->control[1]);
    return KNURL_OK;
}

static knurl_status worker_stopping(knurl_server *server,
                                    const knurl_process_context *process,
                                    void *data, knurl_error *error)
{
    struct fixture *f = data;
    (void)server;
    (void)process;
    (void)error;
    return write_all(f->stop_notice[1], "s", 1) ? KNURL_IO : KNURL_OK;
}
int main(void)
{
    struct fixture f;
    struct sockaddr_in address;
    struct timeval timeout;
    socklen_t size;
    char target[128];
    char alternate[128];
    const char *alternates[1];
    const char *paths[] = {
        "/p/:item", "^/raw/.*$",    "/deny",   "/drop",     "/badedit",
        "/upload",  "/chunked",     "/events", "/idle",     "/truncated",
        "/finish",  "/local-large", "/total",  "/pressure", "/pressure/cancel",
        "/shutdown"};
    knurl_server_config config;
    knurl_server_effective effective;
    knurl_listener_config listener;
    knurl_proxy_config proxy;
    knurl_route_config route;
    knurl_server *server;
    knurl_run_result result;
    knurl_error error;
    knurl_status status;
    int child_status;
    size_t i;
    memset(&f, 0, sizeof(f));
    f.client = f.origin_pid = -1;
    (void)signal(SIGPIPE, SIG_IGN);
    f.origin = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(f.origin >= 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, f.control) == 0);
    CHECK(pipe(f.stop_notice) == 0);
    CHECK(pipe(f.completed) == 0);
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    CHECK(setsockopt(f.control[0], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout)) == 0 &&
          setsockopt(f.control[1], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout)) == 0);
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
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    config.io.chunk_bytes = 1024;
    config.io.output_queue_bytes = 4096;
    config.proxy.active_exchanges = 1;
    config.limits.hard_body_bytes.low = 65536;
    config.limits.default_body_bytes.low = 65536;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_start = worker_start;
    config.callbacks.on_worker_exit = exited;
    config.callbacks.on_worker_stopping = worker_stopping;
    config.callbacks.on_observe = observed;
    config.callbacks.userdata = &f;
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    CHECK(knurl_server_effective_config(server, &effective, &error) ==
              KNURL_OK &&
          effective.config.proxy.active_exchanges == 1 &&
          effective.config.proxy.idle_connections == 4);
    knurl_proxy_config_init(&proxy);
    proxy.path = "/invalid";
    proxy.target = "http://user:pass@localhost/";
    CHECK(knurl_router_proxy(knurl_server_router(server), &proxy, NULL,
                             &error) == KNURL_INVALID);
    proxy.target = target;
    proxy.buffer_limit_bytes = 8191;
    CHECK(knurl_router_proxy(knurl_server_router(server), &proxy, NULL,
                             &error) == KNURL_INVALID);
    proxy.buffer_limit_bytes = 0;
    proxy.tls_client_cert_pem = "incomplete identity";
    CHECK(knurl_router_proxy(knurl_server_router(server), &proxy, NULL,
                             &error) == KNURL_INVALID);
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        knurl_proxy_config_init(&proxy);
        proxy.path = paths[i];
        proxy.path_kind =
            i == 0 ? KNURL_PATH_PARAMS
                   : (i == 1 ? KNURL_PATH_REGEX : KNURL_PATH_LITERAL);
        proxy.target = target;
        proxy.alternate_targets = alternates;
        proxy.alternate_target_count = 1;
        proxy.preflight = preflight;
        proxy.rewrite = rewrite;
        proxy.modify_response = modify;
        proxy.on_error = gateway;
        proxy.on_exchange = exchange_report;
        proxy.on_exchange_userdata = &f;
        proxy.buffer_limit_bytes = 8192;
        proxy.idle_timeout_ms = i == 8 ? 100 : 2000;
        proxy.connect_timeout_ms = 1000;
        proxy.total_timeout_ms = strcmp(paths[i], "/total") == 0 ? 100 : 0;
        status = knurl_router_proxy(knurl_server_router(server), &proxy, NULL,
                                    &error);
        if (status != KNURL_OK)
            (void)fprintf(stderr, "proxy registration %s: %s\n", proxy.path,
                          error.message);
        CHECK(status == KNURL_OK);
    }
    strcpy(target, "changed");
    CHECK(rewrites == 0 && modified == 0 && gateway_errors == 0);
    strcpy(alternate, "changed");
    alternates[0] = NULL;
    knurl_route_config_init(&route);
    route.path = "/ping";
    route.handler = ordinary;
    CHECK(knurl_router_route(knurl_server_router(server), &route, NULL,
                             &error) == KNURL_OK);
    route.path = "/stop";
    CHECK(knurl_router_route(knurl_server_router(server), &route, NULL,
                             &error) == KNURL_OK);
    knurl_route_config_init(&route);
    route.path = "/interim";
    route.on_headers = interim_route;
    CHECK(knurl_router_route(knurl_server_router(server), &route, NULL,
                             &error) == KNURL_OK);
    status = knurl_server_run(server, &result, &error);
    if (status != KNURL_OK)
        (void)fprintf(stderr, "%s: %s\n", error.operation, error.message);
    if (status != KNURL_OK) {
        if (f.client > 0) {
            (void)kill(f.client, SIGTERM);
            (void)waitpid(f.client, NULL, 0);
        }
        if (f.origin_pid > 0) {
            (void)kill(f.origin_pid, SIGTERM);
            (void)waitpid(f.origin_pid, NULL, 0);
        }
        (void)knurl_server_destroy(server, NULL);
        return 1;
    }
    CHECK(status == KNURL_OK);
    CHECK(waitpid(f.client, &child_status, 0) == f.client &&
          WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(waitpid(f.origin_pid, &child_status, 0) == f.origin_pid &&
          WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
    return 0;
}

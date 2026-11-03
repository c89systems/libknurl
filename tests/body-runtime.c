#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(test)                                                            \
    do {                                                                       \
        if (!(test)) {                                                         \
            (void)fprintf(stderr, "body line %d: %s\n", __LINE__, #test);      \
            return 1;                                                          \
        }                                                                      \
    } while (0)
#define OBSERVE(test)                                                          \
    do {                                                                       \
        if (!(test)) {                                                         \
            (void)fprintf(stderr, "body callback line %d: %s\n", __LINE__,     \
                          #test);                                              \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

struct fixture {
    pid_t client;
};
struct context {
    rotmos_request *request;
    pthread_t owner;
    const char *path;
    rotmos_timer *timer;
    size_t delivered;
    int calls;
    int resumed;
    int ended;
};
static int failures;
static int opened;
static int closed_count;
static rotmos_http_methods methods;
static rotmos_body_result retained;
static char *transport_path;
static char *artifact_path;

static char *copy_text(const char *text)
{
    char *copy;
    copy = malloc(strlen(text) + 1);
    if (copy != NULL)
        strcpy(copy, text);
    return copy;
}

static int is(struct context *c, const char *path)
{
    return strcmp(c->path, path) == 0;
}

static rotmos_status reply(rotmos_request *request, int status,
                           const char *text, rotmos_error *error)
{
    rotmos_bytes bytes;
    bytes.data = text;
    bytes.length = strlen(text);
    return rotmos_response_bytes(rotmos_request_response(request), status,
                                 "text/plain", bytes, error);
}

static void resumed(rotmos_timer *timer, void *userdata)
{
    struct context *c;
    (void)timer;
    c = userdata;
    c->timer = NULL;
    OBSERVE(c->calls == 1 && pthread_equal(c->owner, pthread_self()));
    c->resumed = 1;
    OBSERVE(rotmos_request_body_resume(c->request, NULL) == ROTMOS_OK);
    OBSERVE(rotmos_request_body_resume(c->request, NULL) == ROTMOS_OK);
    OBSERVE(c->calls == 1);
}

static void deferred(rotmos_timer *timer, void *userdata)
{
    struct context *c;
    (void)timer;
    c = userdata;
    c->timer = NULL;
    OBSERVE(c->ended == 1 && pthread_equal(c->owner, pthread_self()));
    OBSERVE(reply(c->request, 200, "deferred", NULL) == ROTMOS_OK);
}

static rotmos_status consume(rotmos_request *request, const void *data,
                             size_t length, size_t *consumed, void *userdata,
                             rotmos_error *error)
{
    struct context *c;
    const unsigned char *bytes;
    rotmos_bytes view;
    size_t count;
    size_t i;
    c = userdata;
    ++c->calls;
    *consumed = 0;
    OBSERVE(pthread_equal(c->owner, pthread_self()) && length > 0 &&
            length <= 32);
    OBSERVE(rotmos_request_body(request) == NULL &&
            rotmos_request_body_bytes(request, &view, NULL) == ROTMOS_STATE);
    if (is(c, "/invalid")) {
        *consumed = 1;
        return ROTMOS_AGAIN;
    }
    if (is(c, "/body-error"))
        return ROTMOS_IO;
    if (c->calls == 1 && (is(c, "/stream") || is(c, "/again"))) {
        if (is(c, "/stream")) {
            count = 0;
            OBSERVE(rotmos_response_write(rotmos_request_response(request), "r",
                                          1, &count, error) == ROTMOS_OK &&
                    count == 1);
        }
        if (rotmos_loop_timer(rotmos_request_loop(request),
                              is(c, "/again") ? 1100 : 5, 0, resumed, NULL, c,
                              &c->timer, error) != ROTMOS_OK)
            return ROTMOS_IO;
        if (is(c, "/again"))
            return ROTMOS_AGAIN;
        *consumed = 1;
    } else {
        if (is(c, "/stream") || is(c, "/again"))
            OBSERVE(c->resumed);
        *consumed = length;
    }
    bytes = data;
    for (i = 0; i < *consumed; ++i)
        OBSERVE(bytes[i] == (unsigned char)('a' + (c->delivered + i) % 26));
    c->delivered += *consumed;
    return ROTMOS_OK;
}

static void closed(rotmos_request *request, const rotmos_request_outcome *out,
                   void *userdata)
{
    struct context *c;
    c = userdata;
    ++closed_count;
    OBSERVE(pthread_equal(c->owner, pthread_self()));
    OBSERVE(rotmos_request_target(request).length != 0 &&
            rotmos_response_state(rotmos_request_response(request)) >=
                ROTMOS_RESPONSE_COMPLETE);
    OBSERVE(rotmos_request_body_resume(request, NULL) == ROTMOS_STATE);
    if (is(c, "/immediate")) {
        OBSERVE(out->body_complete && out->body_cause == ROTMOS_OK &&
                !c->ended);
    } else if (is(c, "/early") || is(c, "/chunked")) {
        OBSERVE(!out->body_complete && out->body_cause == ROTMOS_CANCELLED &&
                !c->ended && !c->calls);
    } else if (is(c, "/invalid") || is(c, "/bad-plan") || is(c, "/no-end") ||
               is(c, "/dispatch-error") || is(c, "/reply-invalid") ||
               is(c, "/oversize")) {
        OBSERVE(!out->body_complete && out->body_cause != ROTMOS_OK &&
                !c->ended);
    } else if (is(c, "/body-error") || is(c, "/truncated")) {
        OBSERVE(!out->body_complete && out->body_cause == ROTMOS_IO &&
                !c->ended);
    } else if (is(c, "/idle")) {
        OBSERVE(!out->body_complete && out->body_cause == ROTMOS_TIMEOUT &&
                !c->ended);
    } else if (is(c, "/deadline")) {
        OBSERVE(out->body_complete && out->body_cause == ROTMOS_OK &&
                out->cause == ROTMOS_TIMEOUT && c->ended == 1);
    } else if (is(c, "/method") && out->response.status != 200) {
        OBSERVE(!out->body_complete && !c->ended);
    } else {
        OBSERVE(out->body_complete && out->body_cause == ROTMOS_OK &&
                c->ended == 1);
    }
    if (is(c, "/stream") || is(c, "/again"))
        OBSERVE(c->delivered == 64 && out->delivered_body_bytes.low == 64 &&
                out->received_body_bytes.low == 64);
    if (c->timer != NULL)
        rotmos_timer_cancel(c->timer);
    free(c);
}

static rotmos_status spill(rotmos_request *request, const char *directory,
                           const char *prefix, rotmos_body_result *out,
                           rotmos_error *error)
{
    rotmos_body_materialize_config config;
    rotmos_body_materialize_config_init(&config);
    config.memory_limit_bytes = 4;
    config.directory = directory;
    config.prefix = prefix;
    return rotmos_request_body_spill(request, &config, out, error);
}

static rotmos_status complete(rotmos_request *request, void *userdata,
                              rotmos_error *error)
{
    struct context *c;
    rotmos_body *body;
    rotmos_body_result result;
    rotmos_body_materialize_config materialize;
    rotmos_source *reader;
    rotmos_bytes bytes;
    const char *path;
    struct stat st;
    unsigned char buffer[128];
    size_t count;
    size_t i;
    int fd;
    c = userdata;
    ++c->ended;
    OBSERVE(c->ended == 1 && pthread_equal(c->owner, pthread_self()));
    if (is(c, "/deadline"))
        return ROTMOS_OK;
    if (is(c, "/deferred"))
        return rotmos_loop_timer(rotmos_request_loop(request), 5, 0, deferred,
                                 NULL, c, &c->timer, error);
    if (is(c, "/stream")) {
        OBSERVE(rotmos_response_write(rotmos_request_response(request), "d", 1,
                                      &count, error) == ROTMOS_OK &&
                count == 1);
        return rotmos_response_end(rotmos_request_response(request), NULL, 0,
                                   error);
    }
    if (is(c, "/buffer") || is(c, "/spill") || is(c, "/spill-failure")) {
        body = rotmos_request_body(request);
        OBSERVE(body != NULL);
        OBSERVE(rotmos_body_read(body, buffer, 1, &count, error) == ROTMOS_OK &&
                count == 1);
        rotmos_body_result_init(&result);
        OBSERVE(rotmos_request_body_copy(request, buffer, sizeof(buffer),
                                         &result, error) == ROTMOS_OK);
        OBSERVE(result.kind == ROTMOS_BODY_MEMORY &&
                result.memory.length == 64 && result.memory.data == buffer &&
                !result.owns_memory);
        for (i = 0; i < 64; ++i)
            OBSERVE(buffer[i] == (unsigned char)('a' + i % 26));
        rotmos_body_result_cleanup(&result);
        OBSERVE(rotmos_body_read(body, buffer, 1, &count, error) == ROTMOS_OK &&
                count == 1 && buffer[0] == 'a');
        if (is(c, "/buffer")) {
            OBSERVE(rotmos_request_body_copy(request, NULL, 0, &retained,
                                             error) == ROTMOS_OK);
            OBSERVE(retained.owns_memory && retained.memory.length == 64);
            OBSERVE(rotmos_body_result_open_reader(&retained, &reader, error) ==
                    ROTMOS_OK);
            OBSERVE(rotmos_source_read(reader, buffer, 3, &count, error) ==
                        ROTMOS_OK &&
                    count == 3 && memcmp(buffer, "abc", 3) == 0);
            OBSERVE(rotmos_source_reset(reader, error) == ROTMOS_OK);
            OBSERVE(rotmos_source_destroy(reader, error) == ROTMOS_OK);
        } else if (is(c, "/spill")) {
            OBSERVE(rotmos_body_kind(body) == ROTMOS_BODY_FILE);
            if (rotmos_body_file(body, &fd, &path, error) != ROTMOS_OK)
                return ROTMOS_INVALID;
            OBSERVE(rotmos_body_bytes(body, &bytes, NULL) == ROTMOS_STATE &&
                    rotmos_request_body_bytes(request, &bytes, NULL) ==
                        ROTMOS_STATE);
            OBSERVE(fcntl(fd, F_GETFD) >= 0);
            transport_path = copy_text(path);
            OBSERVE(transport_path != NULL);
            rotmos_body_materialize_config_init(&materialize);
            materialize.buffer = buffer;
            materialize.capacity = 4;
            materialize.memory_limit_bytes = 0;
            materialize.directory = ".";
            buffer[4] = 'z';
            OBSERVE(rotmos_request_body_spill(request, &materialize, &result,
                                              error) == ROTMOS_OK);
            OBSERVE(buffer[4] == 'z');
            artifact_path = copy_text(result.path);
            OBSERVE(rotmos_body_result_open_reader(&result, &reader, error) ==
                    ROTMOS_OK);
            OBSERVE(rotmos_source_read(reader, buffer, sizeof(buffer), &count,
                                       error) == ROTMOS_OK &&
                    count == 64);
            OBSERVE(rotmos_source_read(reader, buffer, sizeof(buffer), &count,
                                       error) == ROTMOS_EOF &&
                    count == 0);
            OBSERVE(rotmos_source_reset(reader, error) == ROTMOS_OK);
            OBSERVE(rotmos_source_read(reader, buffer, 3, &count, error) ==
                        ROTMOS_OK &&
                    count == 3 && memcmp(buffer, "abc", 3) == 0);
            OBSERVE(rotmos_source_destroy(reader, error) == ROTMOS_OK);
            OBSERVE(result.kind == ROTMOS_BODY_FILE && artifact_path != NULL &&
                    strcmp(result.path, transport_path) != 0);
            rotmos_body_result_cleanup(&result);
            OBSERVE(stat(artifact_path, &st) == 0);
        } else {
            OBSERVE(spill(request, "no-such-body-directory", "copy", &result,
                          error) == ROTMOS_IO);
            OBSERVE(result.path == NULL && result.memory.data == NULL &&
                    !result.owns_memory && result.length.low == 0);
        }
    }
    return reply(request, 200, "ok", error);
}

static rotmos_status configure_plan(rotmos_request *request,
                                    rotmos_request_plan *plan, void *userdata,
                                    rotmos_error *error)
{
    struct context *c;
    rotmos_u64 length;
    struct stat st;
    size_t count;
    (void)userdata;
    c = calloc(1, sizeof(*c));
    if (c == NULL)
        return ROTMOS_NOMEM;
    c->request = request;
    c->path = rotmos_request_path(request).data;
    c->owner = pthread_self();
    plan->userdata = c;
    plan->on_close = closed;
    plan->on_body_end = complete;
    ++opened;
    if (is(c, "/stop")) {
        OBSERVE(opened == closed_count + 1 &&
                methods == ROTMOS_HTTP_METHODS_ALL);
        (void)rotmos_server_request_stop(rotmos_request_server(request), NULL);
        return failures ? ROTMOS_INVALID : ROTMOS_OK;
    }
    if (is(c, "/survives")) {
        OBSERVE(transport_path != NULL && stat(transport_path, &st) == -1 &&
                errno == ENOENT);
        OBSERVE(artifact_path != NULL && stat(artifact_path, &st) == 0);
        OBSERVE(retained.owns_memory && retained.memory.length == 64 &&
                ((const unsigned char *)retained.memory.data)[63] ==
                    (unsigned char)('a' + 63 % 26));
        rotmos_body_result_cleanup(&retained);
        OBSERVE(unlink(artifact_path) == 0);
        free(transport_path);
        free(artifact_path);
        transport_path = artifact_path = NULL;
    }
    if (is(c, "/dispatch-error"))
        return ROTMOS_IO;
    if (is(c, "/reply-invalid")) {
        plan->disposition = ROTMOS_REQUEST_REPLY;
        return ROTMOS_OK;
    }
    if (is(c, "/immediate")) {
        plan->disposition = ROTMOS_REQUEST_REPLY;
        return reply(request, 200, "ok", error);
    }
    if (is(c, "/early") || is(c, "/chunked")) {
        plan->disposition = ROTMOS_REQUEST_REPLY;
        return reply(request, is(c, "/early") ? 403 : 422, "denied", error);
    }
    if (is(c, "/no-end")) {
        plan->on_body_end = NULL;
        return ROTMOS_OK;
    }
    if (is(c, "/method")) {
        methods |= ROTMOS_HTTP_METHOD_MASK(rotmos_request_method(request));
        return ROTMOS_OK;
    }
    if (is(c, "/buffer") || is(c, "/spill") || is(c, "/spill-failure") ||
        is(c, "/bad-plan")) {
        if (is(c, "/spill"))
            rotmos_body_policy_init_materialized(&plan->body);
        else
            rotmos_body_policy_init_buffered(&plan->body);
        plan->body.max_bytes.low = 4096;
        plan->body.memory_buffer_limit_bytes =
            is(c, "/spill") || is(c, "/bad-plan") ? 4 : 4096;
        return ROTMOS_OK;
    }
    if (is(c, "/ping") || is(c, "/survives") || is(c, "/deferred") ||
        is(c, "/deadline"))
        return ROTMOS_OK;
    rotmos_body_policy_init_stream(&plan->body);
    plan->body.max_bytes.low = is(c, "/oversize") ? 4 : 4096;
    plan->on_body = consume;
    if (is(c, "/stream") || is(c, "/truncated")) {
        length.high = 0;
        length.low = 2;
        OBSERVE(rotmos_response_known_length(rotmos_request_response(request),
                                             length, error) == ROTMOS_OK);
        OBSERVE(rotmos_response_begin(rotmos_request_response(request),
                                      error) == ROTMOS_OK);
        if (is(c, "/truncated"))
            OBSERVE(rotmos_response_write(rotmos_request_response(request), "r",
                                          1, &count, error) == ROTMOS_OK &&
                    count == 1);
    }
    return ROTMOS_OK;
}

static rotmos_status headers(rotmos_request *request, rotmos_request_plan *plan,
                             void *userdata, rotmos_error *error)
{
    const char *path;
    path = rotmos_request_path(request).data;
    if (strcmp(path, "/route") == 0 || strcmp(path, "/raw") == 0)
        return rotmos_router_dispatch_headers(
            rotmos_server_router(rotmos_request_server(request)), request, plan,
            error);
    return configure_plan(request, plan, userdata, error);
}

static rotmos_status facade(rotmos_request *request, rotmos_reply *result,
                            void *userdata, rotmos_error *error)
{
    (void)request;
    (void)userdata;
    return rotmos_reply_text(result, 200, "text/plain", "routed", error);
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
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
        connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
        return -1;
    return fd;
}

static int exchange(unsigned short port, const char *method, const char *path,
                    const char *fields, const char *body, int status,
                    const char *expected)
{
    char request[512];
    char response[2048];
    char prefix[32];
    char *payload;
    ssize_t count;
    size_t used;
    int fd;
    fd = connect_to(port);
    CHECK(fd >= 0);
    (void)snprintf(
        request, sizeof(request),
        "%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n%s\r\n%s",
        method, path, fields, body);
    CHECK(write(fd, request, strlen(request)) == (ssize_t)strlen(request));
    used = 0;
    while (used < sizeof(response) - 1) {
        count = read(fd, response + used, sizeof(response) - 1 - used);
        if (count == -1 && errno == EINTR)
            continue;
        CHECK(count >= 0);
        if (count == 0)
            break;
        used += (size_t)count;
    }
    response[used] = '\0';
    (void)snprintf(prefix, sizeof(prefix), "HTTP/1.1 %d ", status);
    if (strncmp(response, prefix, strlen(prefix)) != 0) {
        (void)fprintf(stderr, "%s: %s\n", path, response);
        return 1;
    }
    payload = strstr(response, "\r\n\r\n");
    CHECK(payload != NULL && strcmp(payload + 4, expected) == 0);
    CHECK(strstr(response, "100 Continue") == NULL);
    CHECK(close(fd) == 0);
    return 0;
}

static int progressive(unsigned short port, const char *pattern)
{
    char response[256];
    const char *head =
        "POST /stream HTTP/1.1\r\nHost: localhost\r\nConnection: "
        "close\r\nContent-Length: 64\r\n\r\n";
    size_t used;
    ssize_t count;
    int fd;
    fd = connect_to(port);
    CHECK(fd >= 0);
    CHECK(write(fd, head, strlen(head)) == (ssize_t)strlen(head));
    CHECK(write(fd, pattern, 4) == 4);
    used = 0;
    do {
        count = read(fd, response + used, sizeof(response) - used - 1);
        CHECK(count > 0);
        used += (size_t)count;
        response[used] = '\0';
    } while (strstr(response, "\r\n\r\nr") == NULL &&
             used < sizeof(response) - 1);
    CHECK(strstr(response, "\r\n\r\nr") != NULL);
    CHECK(exchange(port, "GET", "/ping", "", "", 200, "ok") == 0);
    CHECK(write(fd, pattern + 4, 60) == 60);
    count = read(fd, response, sizeof(response));
    CHECK(count == 1 && response[0] == 'd');
    CHECK(read(fd, response, sizeof(response)) == 0);
    CHECK(close(fd) == 0);
    return 0;
}

static int truncated(unsigned short port)
{
    char response[512];
    const char *request = "POST /truncated HTTP/1.1\r\nHost: "
                          "localhost\r\nContent-Length: 8\r\n\r\nabc";
    size_t used;
    ssize_t count;
    int fd;
    fd = connect_to(port);
    CHECK(fd >= 0 &&
          write(fd, request, strlen(request)) == (ssize_t)strlen(request));
    used = 0;
    do {
        count = read(fd, response + used, sizeof(response) - used - 1);
        CHECK(count > 0);
        used += (size_t)count;
        response[used] = '\0';
    } while (strstr(response, "\r\n\r\nr") == NULL);
    CHECK(shutdown(fd, SHUT_WR) == 0);
    CHECK(read(fd, response, sizeof(response)) == 0);
    CHECK(close(fd) == 0);
    return 0;
}

static int pipeline(unsigned short port)
{
    const char *request =
        "GET /immediate HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char response[512];
    char *second;
    size_t used;
    ssize_t count;
    int fd;
    fd = connect_to(port);
    CHECK(fd >= 0);
    CHECK(write(fd, request, strlen(request)) == (ssize_t)strlen(request));
    used = 0;
    while (used < sizeof(response) - 1) {
        count = read(fd, response + used, sizeof(response) - used - 1);
        if (count == -1 && errno == EINTR)
            continue;
        CHECK(count >= 0);
        if (count == 0)
            break;
        used += (size_t)count;
    }
    response[used] = '\0';
    CHECK(strncmp(response, "HTTP/1.1 200 ", 13) == 0);
    second = strstr(response, "\r\n\r\nokHTTP/1.1 200 ");
    CHECK(second != NULL);
    second = strstr(second + 6, "\r\n\r\n");
    CHECK(second != NULL && strcmp(second + 4, "ok") == 0);
    CHECK(close(fd) == 0);
    return 0;
}

static int client(unsigned short port)
{
    char pattern[65];
    const char *method[] = {"GET",    "POST", "PUT",     "PATCH",
                            "DELETE", "HEAD", "OPTIONS", "PROPFIND",
                            "MKCOL",  "COPY", "MOVE"};
    size_t i;
    for (i = 0; i < 64; ++i)
        pattern[i] = (char)('a' + i % 26);
    pattern[64] = '\0';
    CHECK(pipeline(port) == 0);
    CHECK(progressive(port, pattern) == 0);
    CHECK(exchange(port, "POST", "/again", "Content-Length: 64\r\n", pattern,
                   200, "ok") == 0);
    CHECK(exchange(port, "POST", "/buffer", "Content-Length: 64\r\n", pattern,
                   200, "ok") == 0);
    CHECK(exchange(port, "POST", "/spill", "Content-Length: 64\r\n", pattern,
                   200, "ok") == 0);
    CHECK(exchange(port, "GET", "/survives", "", "", 200, "ok") == 0);
    CHECK(exchange(port, "POST", "/spill-failure", "Content-Length: 64\r\n",
                   pattern, 200, "ok") == 0);
    CHECK(exchange(port, "POST", "/invalid", "Content-Length: 3\r\n", "abc",
                   500, "") == 0);
    CHECK(exchange(port, "POST", "/body-error", "Content-Length: 3\r\n", "abc",
                   500, "") == 0);
    CHECK(exchange(port, "POST", "/early",
                   "Expect: 100-continue\r\nContent-Length: 64\r\n", "", 403,
                   "denied") == 0);
    CHECK(exchange(port, "POST", "/chunked", "Transfer-Encoding: chunked\r\n",
                   "3\r\nabc\r\n0\r\n\r\n", 422, "denied") == 0);
    CHECK(exchange(port, "POST", "/bad-plan",
                   "Expect: 100-continue\r\nContent-Length: 64\r\n", "", 500,
                   "") == 0);
    CHECK(exchange(port, "POST", "/oversize",
                   "Expect: 100-continue\r\nContent-Length: 5\r\n", "", 413,
                   "") == 0);
    CHECK(exchange(port, "GET", "/no-end", "", "", 500, "") == 0);
    CHECK(exchange(port, "GET", "/dispatch-error", "", "", 500, "") == 0);
    CHECK(exchange(port, "GET", "/reply-invalid", "", "", 500, "") == 0);
    CHECK(exchange(port, "GET", "/deferred", "", "", 200, "deferred") == 0);
    CHECK(exchange(port, "GET", "/deadline", "", "", 500, "") == 0);
    CHECK(exchange(port, "POST", "/idle", "Content-Length: 3\r\n", "", 408,
                   "") == 0);
    CHECK(truncated(port) == 0);
    CHECK(exchange(port, "GET", "/route", "", "", 200, "routed") == 0);
    CHECK(exchange(port, "GET", "/raw", "", "", 200, "ok") == 0);
    for (i = 0; i < sizeof(method) / sizeof(method[0]); ++i)
        CHECK(exchange(port, method[i], "/method", "Content-Length: 0\r\n", "",
                       200, strcmp(method[i], "HEAD") == 0 ? "" : "ok") == 0);
    CHECK(exchange(port, "POST", "/method", "", "", 411, "") == 0);
    CHECK(exchange(port, "GET", "/method", "Content-Length: 1\r\n", "x", 413,
                   "") == 0);
    CHECK(exchange(port, "GET", "/stop", "", "", 200, "ok") == 0);
    return 0;
}

static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *process,
                           void *userdata, rotmos_error *error)
{
    struct fixture *fixture;
    rotmos_bound_listener listener;
    int result;
    (void)process;
    fixture = userdata;
    if (rotmos_server_listener_at(server, 0, &listener, error) != ROTMOS_OK)
        return ROTMOS_INVALID;
    fixture->client = fork();
    if (fixture->client == -1)
        return ROTMOS_IO;
    if (fixture->client == 0) {
        result = client(listener.port);
        if (result != 0)
            (void)kill(getppid(), SIGTERM);
        _exit(result);
    }
    return ROTMOS_OK;
}

int main(void)
{
    struct fixture fixture;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_route_config route;
    rotmos_server *server;
    rotmos_run_result result;
    rotmos_error error;
    rotmos_status status;
    int child_status;
    fixture.client = -1;
    rotmos_body_result_init(&retained);
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.workers.shutdown_timeout_ms = 100;
    config.dispatch.mode = ROTMOS_DISPATCH_CALLBACK;
    config.dispatch.on_headers = headers;
    config.limits.body_spool_directory = ".";
    config.timeouts.request_body_idle.milliseconds = 1000;
    config.timeouts.body_min_rate_grace.milliseconds = 1;
    config.timeouts.deferred_response.milliseconds = 50;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.io.chunk_bytes = 32;
    config.io.input_queue_bytes = 64;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.userdata = &fixture;
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    rotmos_route_config_init(&route);
    route.path = "/route";
    route.handler = facade;
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    route.path = "/raw";
    route.handler = NULL;
    route.on_headers = configure_plan;
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    status = rotmos_server_run(server, &result, &error);
    if (status != ROTMOS_OK)
        (void)fprintf(stderr, "%s: %s\n", error.operation, error.message);
    CHECK(status == ROTMOS_OK && !result.escalated);
    CHECK(waitpid(fixture.client, &child_status, 0) == fixture.client);
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    return 0;
}

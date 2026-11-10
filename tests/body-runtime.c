#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
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
    knurl_request *request;
    pthread_t owner;
    const char *path;
    knurl_timer *timer;
    size_t delivered;
    int calls;
    int resumed;
    int ended;
};
static int failures;
static int rate_mode;
static int opened;
static int closed_count;
static knurl_http_methods methods;
static knurl_body_result retained;
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

static knurl_status reply(knurl_request *request, int status, const char *text,
                          knurl_error *error)
{
    knurl_bytes bytes;
    bytes.data = text;
    bytes.length = strlen(text);
    return knurl_response_bytes(knurl_request_response(request), status,
                                "text/plain", bytes, error);
}

static void resumed(knurl_timer *timer, void *userdata)
{
    struct context *c;
    (void)timer;
    c = userdata;
    c->timer = NULL;
    OBSERVE(c->calls == 1 && pthread_equal(c->owner, pthread_self()));
    c->resumed = 1;
    OBSERVE(knurl_request_body_resume(c->request, NULL) == KNURL_OK);
    OBSERVE(knurl_request_body_resume(c->request, NULL) == KNURL_OK);
    OBSERVE(c->calls == 1);
}

static void deferred(knurl_timer *timer, void *userdata)
{
    struct context *c;
    (void)timer;
    c = userdata;
    c->timer = NULL;
    OBSERVE(c->ended == 1 && pthread_equal(c->owner, pthread_self()));
    OBSERVE(reply(c->request, 200, "deferred", NULL) == KNURL_OK);
}

static knurl_status consume(knurl_request *request, const void *data,
                            size_t length, size_t *consumed, void *userdata,
                            knurl_error *error)
{
    struct context *c;
    const unsigned char *bytes;
    knurl_bytes view;
    size_t count;
    size_t i;
    c = userdata;
    ++c->calls;
    *consumed = 0;
    OBSERVE(pthread_equal(c->owner, pthread_self()) && length > 0 &&
            length <= 32);
    OBSERVE(knurl_request_body(request) == NULL &&
            knurl_request_body_bytes(request, &view, NULL) == KNURL_STATE);
    if (is(c, "/invalid")) {
        *consumed = 1;
        return KNURL_AGAIN;
    }
    if (is(c, "/body-error"))
        return KNURL_IO;
    if (c->calls == 1 && (is(c, "/stream") || is(c, "/again"))) {
        if (is(c, "/stream")) {
            count = 0;
            OBSERVE(knurl_response_write(knurl_request_response(request), "r",
                                         1, &count, error) == KNURL_OK &&
                    count == 1);
        }
        if (knurl_loop_timer(knurl_request_loop(request),
                             is(c, "/again") ? 1100 : 5, 0, resumed, NULL, c,
                             &c->timer, error) != KNURL_OK)
            return KNURL_IO;
        if (is(c, "/again"))
            return KNURL_AGAIN;
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
    return KNURL_OK;
}

static void closed(knurl_request *request, const knurl_request_outcome *out,
                   void *userdata)
{
    struct context *c;
    c = userdata;
    ++closed_count;
    OBSERVE(pthread_equal(c->owner, pthread_self()));
    OBSERVE(knurl_request_target(request).length != 0 &&
            knurl_response_state(knurl_request_response(request)) >=
                KNURL_RESPONSE_COMPLETE);
    OBSERVE(knurl_request_body_resume(request, NULL) == KNURL_STATE);
    if (is(c, "/immediate")) {
        OBSERVE(out->body_complete && out->body_cause == KNURL_OK && !c->ended);
    } else if (is(c, "/early") || is(c, "/chunked")) {
        OBSERVE(!out->body_complete && out->body_cause == KNURL_CANCELLED &&
                !c->ended && !c->calls);
    } else if (is(c, "/invalid") || is(c, "/bad-plan") || is(c, "/no-end") ||
               is(c, "/dispatch-error") || is(c, "/reply-invalid") ||
               is(c, "/oversize")) {
        OBSERVE(!out->body_complete && out->body_cause != KNURL_OK &&
                !c->ended);
    } else if (is(c, "/body-error") || is(c, "/truncated")) {
        OBSERVE(!out->body_complete && out->body_cause == KNURL_IO &&
                !c->ended);
    } else if (is(c, "/idle")) {
        OBSERVE(!out->body_complete && out->body_cause == KNURL_TIMEOUT &&
                !c->ended);
    } else if (is(c, "/deadline")) {
        OBSERVE(out->body_complete && out->body_cause == KNURL_OK &&
                out->cause == KNURL_TIMEOUT && c->ended == 1);
    } else if (is(c, "/method") && out->response.status != 200) {
        OBSERVE(!out->body_complete && !c->ended);
    } else {
        OBSERVE(out->body_complete && out->body_cause == KNURL_OK &&
                c->ended == 1);
    }
    if (is(c, "/stream") || is(c, "/again"))
        OBSERVE(c->delivered == 64 && out->delivered_body_bytes.low == 64 &&
                out->received_body_bytes.low == 64);
    if (c->timer != NULL)
        knurl_timer_cancel(c->timer);
    free(c);
}

static knurl_status spill(knurl_request *request, const char *directory,
                          const char *prefix, knurl_body_result *out,
                          knurl_error *error)
{
    knurl_body_materialize_config config;
    knurl_body_materialize_config_init(&config);
    config.memory_limit_bytes = 4;
    config.directory = directory;
    config.prefix = prefix;
    return knurl_request_body_spill(request, &config, out, error);
}

static knurl_status complete(knurl_request *request, void *userdata,
                             knurl_error *error)
{
    struct context *c;
    knurl_body *body;
    knurl_body_result result;
    knurl_body_materialize_config materialize;
    knurl_source *reader;
    knurl_bytes bytes;
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
        return KNURL_OK;
    if (is(c, "/deferred"))
        return knurl_loop_timer(knurl_request_loop(request), 5, 0, deferred,
                                NULL, c, &c->timer, error);
    if (is(c, "/stream")) {
        OBSERVE(knurl_response_write(knurl_request_response(request), "d", 1,
                                     &count, error) == KNURL_OK &&
                count == 1);
        return knurl_response_end(knurl_request_response(request), NULL, 0,
                                  error);
    }
    if (is(c, "/buffer") || is(c, "/spill") || is(c, "/spill-failure")) {
        body = knurl_request_body(request);
        OBSERVE(body != NULL);
        OBSERVE(knurl_body_read(body, buffer, 1, &count, error) == KNURL_OK &&
                count == 1);
        knurl_body_result_init(&result);
        OBSERVE(knurl_request_body_copy(request, buffer, sizeof(buffer),
                                        &result, error) == KNURL_OK);
        OBSERVE(result.kind == KNURL_BODY_MEMORY &&
                result.memory.length == 64 && result.memory.data == buffer &&
                !result.owns_memory);
        for (i = 0; i < 64; ++i)
            OBSERVE(buffer[i] == (unsigned char)('a' + i % 26));
        knurl_body_result_cleanup(&result);
        OBSERVE(knurl_body_read(body, buffer, 1, &count, error) == KNURL_OK &&
                count == 1 && buffer[0] == 'a');
        if (is(c, "/buffer")) {
            OBSERVE(knurl_request_body_copy(request, NULL, 0, &retained,
                                            error) == KNURL_OK);
            OBSERVE(retained.owns_memory && retained.memory.length == 64);
            OBSERVE(knurl_body_result_open_reader(&retained, &reader, error) ==
                    KNURL_OK);
            OBSERVE(knurl_source_read(reader, buffer, 3, &count, error) ==
                        KNURL_OK &&
                    count == 3 && memcmp(buffer, "abc", 3) == 0);
            OBSERVE(knurl_source_reset(reader, error) == KNURL_OK);
            OBSERVE(knurl_source_destroy(reader, error) == KNURL_OK);
        } else if (is(c, "/spill")) {
            OBSERVE(knurl_body_kind(body) == KNURL_BODY_FILE);
            if (knurl_body_file(body, &fd, &path, error) != KNURL_OK)
                return KNURL_INVALID;
            OBSERVE(knurl_body_bytes(body, &bytes, NULL) == KNURL_STATE &&
                    knurl_request_body_bytes(request, &bytes, NULL) ==
                        KNURL_STATE);
            OBSERVE(fcntl(fd, F_GETFD) >= 0);
            transport_path = copy_text(path);
            OBSERVE(transport_path != NULL);
            knurl_body_materialize_config_init(&materialize);
            materialize.buffer = buffer;
            materialize.capacity = 4;
            materialize.memory_limit_bytes = 0;
            materialize.directory = ".";
            buffer[4] = 'z';
            OBSERVE(knurl_request_body_spill(request, &materialize, &result,
                                             error) == KNURL_OK);
            OBSERVE(buffer[4] == 'z');
            artifact_path = copy_text(result.path);
            OBSERVE(knurl_body_result_open_reader(&result, &reader, error) ==
                    KNURL_OK);
            OBSERVE(knurl_source_read(reader, buffer, sizeof(buffer), &count,
                                      error) == KNURL_OK &&
                    count == 64);
            OBSERVE(knurl_source_read(reader, buffer, sizeof(buffer), &count,
                                      error) == KNURL_EOF &&
                    count == 0);
            OBSERVE(knurl_source_reset(reader, error) == KNURL_OK);
            OBSERVE(knurl_source_read(reader, buffer, 3, &count, error) ==
                        KNURL_OK &&
                    count == 3 && memcmp(buffer, "abc", 3) == 0);
            OBSERVE(knurl_source_destroy(reader, error) == KNURL_OK);
            OBSERVE(result.kind == KNURL_BODY_FILE && artifact_path != NULL &&
                    strcmp(result.path, transport_path) != 0);
            knurl_body_result_cleanup(&result);
            OBSERVE(stat(artifact_path, &st) == 0);
        } else {
            OBSERVE(spill(request, "no-such-body-directory", "copy", &result,
                          error) == KNURL_IO);
            OBSERVE(result.path == NULL && result.memory.data == NULL &&
                    !result.owns_memory && result.length.low == 0);
        }
    }
    return reply(request, 200, "ok", error);
}

static knurl_status configure_plan(knurl_request *request,
                                   knurl_request_plan *plan, void *userdata,
                                   knurl_error *error)
{
    struct context *c;
    knurl_u64 length;
    struct stat st;
    size_t count;
    (void)userdata;
    c = calloc(1, sizeof(*c));
    if (c == NULL)
        return KNURL_NOMEM;
    c->request = request;
    c->path = knurl_request_path(request).data;
    c->owner = pthread_self();
    plan->userdata = c;
    plan->on_close = closed;
    plan->on_body_end = complete;
    ++opened;
    if (is(c, "/stop")) {
        OBSERVE(opened == closed_count + 1 &&
                (rate_mode != 0 || methods == KNURL_HTTP_METHODS_ALL));
        (void)knurl_server_request_stop(knurl_request_server(request), NULL);
        return failures ? KNURL_INVALID : KNURL_OK;
    }
    if (is(c, "/survives")) {
        OBSERVE(transport_path != NULL && stat(transport_path, &st) == -1 &&
                errno == ENOENT);
        OBSERVE(artifact_path != NULL && stat(artifact_path, &st) == 0);
        OBSERVE(retained.owns_memory && retained.memory.length == 64 &&
                ((const unsigned char *)retained.memory.data)[63] ==
                    (unsigned char)('a' + 63 % 26));
        knurl_body_result_cleanup(&retained);
        OBSERVE(unlink(artifact_path) == 0);
        free(transport_path);
        free(artifact_path);
        transport_path = artifact_path = NULL;
    }
    if (is(c, "/dispatch-error"))
        return KNURL_IO;
    if (is(c, "/reply-invalid")) {
        plan->disposition = KNURL_REQUEST_REPLY;
        return KNURL_OK;
    }
    if (is(c, "/immediate")) {
        plan->disposition = KNURL_REQUEST_REPLY;
        return reply(request, 200, "ok", error);
    }
    if (is(c, "/early") || is(c, "/chunked")) {
        plan->disposition = KNURL_REQUEST_REPLY;
        return reply(request, is(c, "/early") ? 403 : 422, "denied", error);
    }
    if (is(c, "/no-end")) {
        plan->on_body_end = NULL;
        return KNURL_OK;
    }
    if (is(c, "/method")) {
        methods |= KNURL_HTTP_METHOD_MASK(knurl_request_method(request));
        return KNURL_OK;
    }
    if (is(c, "/buffer") || is(c, "/spill") || is(c, "/spill-failure") ||
        is(c, "/bad-plan")) {
        if (is(c, "/spill"))
            knurl_body_policy_init_materialized(&plan->body);
        else
            knurl_body_policy_init_buffered(&plan->body);
        plan->body.max_bytes.low = 4096;
        plan->body.memory_buffer_limit_bytes = is(c, "/bad-plan") ? 8192
                                               : is(c, "/spill")  ? 4
                                                                  : 4096;
        return KNURL_OK;
    }
    if (is(c, "/ping") || is(c, "/survives") || is(c, "/deferred") ||
        is(c, "/deadline"))
        return KNURL_OK;
    knurl_body_policy_init_stream(&plan->body);
    plan->body.max_bytes.low = is(c, "/oversize") ? 4 : 4096;
    plan->on_body = consume;
    if (is(c, "/stream") || is(c, "/truncated")) {
        length.high = 0;
        length.low = 2;
        OBSERVE(knurl_response_known_length(knurl_request_response(request),
                                            length, error) == KNURL_OK);
        OBSERVE(knurl_response_begin(knurl_request_response(request), error) ==
                KNURL_OK);
        if (is(c, "/truncated"))
            OBSERVE(knurl_response_write(knurl_request_response(request), "r",
                                         1, &count, error) == KNURL_OK &&
                    count == 1);
    }
    return KNURL_OK;
}

static knurl_status headers(knurl_request *request, knurl_request_plan *plan,
                            void *userdata, knurl_error *error)
{
    const char *path;
    path = knurl_request_path(request).data;
    if (strcmp(path, "/route") == 0 || strcmp(path, "/raw") == 0)
        return knurl_router_dispatch_headers(
            knurl_server_router(knurl_request_server(request)), request, plan,
            error);
    return configure_plan(request, plan, userdata, error);
}

static knurl_status facade(knurl_request *request, knurl_reply *result,
                           void *userdata, knurl_error *error)
{
    (void)request;
    (void)userdata;
    return knurl_reply_text(result, 200, "text/plain", "routed", error);
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
    struct pollfd input;
    fd = connect_to(port);
    CHECK(fd >= 0);
    (void)snprintf(
        request, sizeof(request),
        "%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n%s\r\n%s",
        method, path, fields, strcmp(path, "/slow") == 0 ? "" : body);
    CHECK(write(fd, request, strlen(request)) == (ssize_t)strlen(request));
    input.fd = fd;
    input.events = POLLIN;
    input.revents = 0;
    if (strcmp(path, "/slow") == 0) {
        /* Both protections are disabled: silence must not reject the body. */
        CHECK(poll(&input, 1, 1200) == 0);
        CHECK(write(fd, body, strlen(body)) == (ssize_t)strlen(body));
    } else if (rate_mode == 1 && strcmp(path, "/idle") == 0) {
        /* The rate deadline must respect its one-second grace period. */
        CHECK(poll(&input, 1, 500) == 0);
    }
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
    if (rate_mode != 0) {
        if (rate_mode == 1) {
            CHECK(exchange(port, "POST", "/idle", "Content-Length: 3\r\n", "",
                           408, "") == 0);
            CHECK(exchange(port, "POST", "/idle", "Content-Length: 3\r\n", "a",
                           408, "") == 0);
            /* Application backpressure must not spend the rate budget. */
            CHECK(exchange(port, "POST", "/again", "Content-Length: 64\r\n",
                           pattern, 200, "ok") == 0);
        } else {
            CHECK(exchange(port, "POST", "/slow", "Content-Length: 64\r\n",
                           pattern, 200, "ok") == 0);
        }
        CHECK(exchange(port, "GET", "/ping", "", "", 200, "ok") == 0);
        CHECK(exchange(port, "GET", "/stop", "", "", 200, "ok") == 0);
        return 0;
    }
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
                   "request body exceeds route limit") == 0);
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
                   "request body is not allowed for this route") == 0);
    CHECK(exchange(port, "GET", "/stop", "", "", 200, "ok") == 0);
    return 0;
}

static knurl_status ready(knurl_server *server,
                          const knurl_process_context *process, void *userdata,
                          knurl_error *error)
{
    struct fixture *fixture;
    knurl_bound_listener listener;
    int result;
    (void)process;
    fixture = userdata;
    if (knurl_server_listener_at(server, 0, &listener, error) != KNURL_OK)
        return KNURL_INVALID;
    if (knurl_server_set_serving(server, 1, error) != KNURL_OK)
        return KNURL_STATE;
    fixture->client = fork();
    if (fixture->client == -1)
        return KNURL_IO;
    if (fixture->client == 0) {
        result = client(listener.port);
        if (result != 0)
            (void)kill(getppid(), SIGTERM);
        _exit(result);
    }
    return KNURL_OK;
}

int main(int argc, char **argv)
{
    struct fixture fixture;
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_route_config route;
    knurl_server *server;
    knurl_run_result result;
    knurl_error error;
    knurl_status status;
    int child_status;
    CHECK(argc == 1 || (argc == 2 && (strcmp(argv[1], "rate") == 0 ||
                                      strcmp(argv[1], "unlimited") == 0)));
    if (argc == 2)
        rate_mode = strcmp(argv[1], "rate") == 0 ? 1 : 2;
    fixture.client = -1;
    knurl_body_result_init(&retained);
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.workers.shutdown_timeout_ms = 100;
    config.dispatch.mode = KNURL_DISPATCH_CALLBACK;
    config.dispatch.on_headers = headers;
    config.limits.body_spool_directory = ".";
    config.timeouts.request_body_idle.milliseconds = 1000;
    config.timeouts.body_min_rate_grace.milliseconds = rate_mode ? 1000 : 3000;
    config.timeouts.request_body_idle.disabled = rate_mode != 0;
    config.limits.body_min_rate_disabled = rate_mode == 2;
    config.timeouts.deferred_response.milliseconds = 50;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    config.io.chunk_bytes = 32;
    config.io.input_queue_bytes = 64;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.userdata = &fixture;
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    knurl_route_config_init(&route);
    route.path = "/route";
    route.handler = facade;
    CHECK(knurl_router_route(knurl_server_router(server), &route, NULL,
                             &error) == KNURL_OK);
    route.path = "/raw";
    route.handler = NULL;
    route.on_headers = configure_plan;
    CHECK(knurl_router_route(knurl_server_router(server), &route, NULL,
                             &error) == KNURL_OK);
    status = knurl_server_run(server, &result, &error);
    if (status != KNURL_OK)
        (void)fprintf(stderr, "%s: %s\n", error.operation, error.message);
    CHECK(status == KNURL_OK && !result.escalated);
    CHECK(waitpid(fixture.client, &child_status, 0) == fixture.client);
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
    return 0;
}

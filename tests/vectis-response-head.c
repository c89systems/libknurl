#define _POSIX_C_SOURCE 200809L
#include "vectis-server.h"
#include <knurl/knurl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "Vectis head %d: %s\n", __LINE__, #t);             \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct fixture {
    pid_t child;
    int bounded, plain;
    size_t manual_position;
    int failed;
    char empty_file[64];
};
static const char pretty[] =
    "<html>\n<head>\n\t<title>404 Not Found</title></head>\n<body>\n\t"
    "<h1>404 Not Found</h1>\n</body>\n</html>\n";
static void writable(knurl_response *response, void *data)
{
    struct fixture *f = data;
    size_t accepted;
    knurl_status status =
        knurl_response_write(response, &"ok"[f->manual_position],
                             2 - f->manual_position, &accepted, NULL);
    f->manual_position += accepted;
    if (status != KNURL_OK && status != KNURL_AGAIN)
        f->failed = 1;
    if (f->manual_position == 2 &&
        knurl_response_end(response, NULL, 0, NULL) != KNURL_OK)
        f->failed = 1;
    if (knurl_response_queued_bytes(response) > 128)
        f->failed = 1;
}
static knurl_status handler(knurl_request *request, knurl_reply *reply,
                            void *data, knurl_error *error)
{
    const char *path = knurl_request_path(request).data;
    struct fixture *f = data;
    knurl_response *response = knurl_request_response(request);
    knurl_u64 length;
    char *large;
    knurl_status status;
    if (!strcmp(path, "/stop")) {
        if (f->failed)
            return KNURL_STATE;
        (void)knurl_server_request_stop(knurl_request_server(request), NULL);
        return knurl_reply_text(reply, 200, "text/plain", "ok", error);
    }
    if (!strcmp(path, "/large") || !strcmp(path, "/manual") ||
        !strcmp(path, "/replace")) {
        large = malloc(70001);
        if (large == NULL)
            return KNURL_NOMEM;
        memset(large, 'x', 70000);
        large[70000] = '\0';
        status =
            !strcmp(path, "/large")
                ? knurl_reply_header_add(reply, "x-large", large, error)
                : knurl_response_header_add(response, "x-large", large, error);
        free(large);
        if (status != KNURL_OK)
            return status;
        if (strcmp(path, "/large")) {
            length.high = 0;
            length.low = 2;
            status = knurl_response_known_length(response, length, error);
            if (status == KNURL_OK)
                status = knurl_response_callbacks(response, writable, NULL, f,
                                                  error);
            if (status == KNURL_OK)
                status = knurl_response_begin(response, error);
            if (status != KNURL_OK)
                return status;
            if (!strcmp(path, "/manual")) {
                f->manual_position = 0;
                writable(response, f);
                return f->failed ? KNURL_STATE : KNURL_OK;
            }
            status = knurl_response_replace_uncommitted(response, error);
            if (status != KNURL_OK)
                return status;
        }
    }
    if (!strcmp(path, "/missing"))
        return knurl_reply_file(reply, 200, "text/plain",
                                "/libknurl-fixture-missing/file", error);
    if (!strcmp(path, "/file-empty"))
        return knurl_reply_file(reply, 404, "text/plain", f->empty_file, error);
    if (!strcmp(path, "/error") || !strcmp(path, "/head"))
        return knurl_reply_status(reply, 404, error);
    if (!strcmp(path, "/empty"))
        return knurl_reply_text(reply, 404, "text/plain", "", error);
    if (!strcmp(path, "/304"))
        return knurl_reply_text(reply, 304, "text/plain", "suppressed", error);
    if (!strcmp(path, "/diagnostic")) {
        strcpy(error->message, "application diagnostic");
        return KNURL_INVALID;
    }
    return knurl_reply_text(reply, 200, "text/plain", "ok", error);
}
static int connect_to(unsigned short port)
{
    struct sockaddr_in address;
    struct timeval timeout;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1)
        return -1;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address))) {
        (void)close(fd);
        return -1;
    }
    return fd;
}
/* Read exactly one representation, so the next request uses the same socket. */
static int exchange(int fd, const char *method, const char *version,
                    const char *path, int close_connection, int code,
                    const char *body, int large)
{
    char request[256], *response, *payload = NULL, *length, *field;
    size_t used = 0, wanted = (size_t)-1, head = 0, expected;
    ssize_t n;
    int request_length;
    response = malloc(75000);
    CHECK(response != NULL);
    request_length =
        snprintf(request, sizeof(request),
                 "%s %s %s\r\nHost: localhost\r\n%s\r\n", method, path, version,
                 close_connection == 1 ? "Connection: close\r\n" : "");
    CHECK(write(fd, request, (size_t)request_length) == request_length);
    while (used < wanted && used < 74999) {
        n = read(fd, response + used, 74999 - used);
        if (n == -1 && errno == EINTR)
            continue;
        CHECK(n > 0);
        used += (size_t)n;
        response[used] = '\0';
        payload = strstr(response, "\r\n\r\n");
        if (payload != NULL) {
            payload += 4;
            head = (size_t)(payload - response);
            length = strstr(response, "content-length: ");
            CHECK(code == 304 ? length == NULL
                              : length != NULL && length < payload);
            wanted = head + (!strcmp(method, "HEAD") || code == 304
                                 ? 0
                                 : (size_t)strtoul(length + 16, NULL, 10));
        }
    }
    CHECK(payload != NULL && used == wanted && atoi(response + 9) == code);
    CHECK(strstr(response, "server: vectis-head-fixture\r\n") != NULL);
    CHECK(strstr(response, "strict-transport-security:") == NULL);
    CHECK(close_connection
              ? strstr(response, "connection: close\r\n") != NULL
              : strstr(response, "connection: keep-alive\r\n"
                                 "keep-alive: timeout=8\r\n") != NULL);
    if (large) {
        field = strstr(response, "x-large: ");
        CHECK(field != NULL && field < payload);
        field += 9;
        for (expected = 0; expected < 70000; ++expected)
            CHECK(field[expected] == 'x');
        CHECK(!memcmp(field + 70000, "\r\n", 2));
    } else
        CHECK(strstr(response, "x-large:") == NULL);
    expected = !strcmp(method, "HEAD") ? 0 : strlen(body);
    CHECK(used - head == expected && !memcmp(payload, body, expected));
    if (!strcmp(method, "HEAD")) {
        char value[64];
        (void)sprintf(value, "content-length: %lu\r\n",
                      (unsigned long)strlen(body));
        CHECK(strstr(response, value) != NULL);
    }
    free(response);
    return 0;
}
static int client(struct fixture *f, unsigned short port)
{
    const char *paths[] = {"/error", "/empty", "/missing", "/diagnostic",
                           "/head"};
    const char *body;
    int fd, code;
    size_t i;
    fd = connect_to(port);
    CHECK(fd >= 0);
    CHECK(exchange(fd, "GET", "HTTP/1.1", "/ok", 0, 200, "ok", 0) == 0);
    /* The second response consumes the configured keepalive request budget. */
    CHECK(exchange(fd, "GET", "HTTP/1.1", "/ok", 2, 200, "ok", 0) == 0);
    CHECK(read(fd, &code, 1) == 0 && close(fd) == 0);
    fd = connect_to(port);
    CHECK(fd >= 0);
    CHECK(exchange(fd, "GET", "HTTP/1.1", "/304", 1, 304, "", 0) == 0);
    CHECK(close(fd) == 0);
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        fd = connect_to(port);
        CHECK(fd >= 0);
        body = i == 3 ? "application diagnostic" : (f->plain ? "" : pretty);
        CHECK(exchange(fd, i == 4 ? "HEAD" : "GET", "HTTP/1.1", paths[i], 1,
                       i == 3 ? 400 : 404, body, 0) == 0);
        CHECK(close(fd) == 0);
    }
    fd = connect_to(port);
    CHECK(fd >= 0);
    CHECK(exchange(fd, "GET", "HTTP/1.1", "/file-empty", 1, 404,
                   f->plain ? "" : pretty, 0) == 0);
    CHECK(close(fd) == 0);
    fd = connect_to(port);
    CHECK(fd >= 0);
    if (f->bounded) {
        const char *failure =
            f->plain ? ""
                     : "<html>\n<head>\n\t<title>500 Internal Server "
                       "Error</title></head>\n<body>\n\t"
                       "<h1>500 Internal Server Error</h1>\n</body>\n</html>\n";
        CHECK(exchange(fd, "GET", "HTTP/1.1", "/large", 1, 500, failure, 0) ==
              0);
    } else
        CHECK(exchange(fd, "GET", "HTTP/1.1", "/large", 1, 200, "ok", 1) == 0);
    CHECK(close(fd) == 0);
    if (!f->bounded) {
        fd = connect_to(port);
        CHECK(fd >= 0);
        CHECK(exchange(fd, "GET", "HTTP/1.1", "/manual", 1, 200, "ok", 1) == 0);
        CHECK(close(fd) == 0);
        fd = connect_to(port);
        CHECK(fd >= 0);
        CHECK(exchange(fd, "GET", "HTTP/1.1", "/replace", 1, 200, "ok", 0) ==
              0);
        CHECK(close(fd) == 0);
    }
    fd = connect_to(port);
    CHECK(fd >= 0);
    CHECK(exchange(fd, "GET", "HTTP/1.0", "/ok", 2, 200, "ok", 0) == 0);
    CHECK(close(fd) == 0);
    fd = connect_to(port);
    CHECK(fd >= 0);
    CHECK(exchange(fd, "GET", "HTTP/1.1", "/stop", 1, 200, "ok", 0) == 0);
    CHECK(close(fd) == 0);
    return 0;
}
static knurl_status ready(knurl_server *server,
                          const knurl_process_context *process, void *data,
                          knurl_error *error)
{
    struct fixture *f = data;
    knurl_bound_listener listener;
    (void)process;
    if (knurl_server_listener_at(server, 0, &listener, error) != KNURL_OK)
        return KNURL_STATE;
    if (knurl_server_set_serving(server, 1, error) != KNURL_OK)
        return KNURL_STATE;
    f->child = fork();
    if (f->child == 0)
        _exit(client(f, listener.port));
    return f->child > 0 ? KNURL_OK : KNURL_IO;
}
int main(int argc, char **argv)
{
    struct fixture f;
    struct reference_server_config source;
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_route_config route;
    knurl_server *server;
    knurl_error error;
    knurl_run_result result;
    const char *paths[] = {"/ok",         "/error",   "/empty", "/missing",
                           "/diagnostic", "/head",    "/large", "/stop",
                           "/manual",     "/replace", "/304",   "/file-empty"};
    size_t i;
    int status;
    memset(&f, 0, sizeof(f));
    strcpy(f.empty_file, "knurl-head-empty-XXXXXX");
    status = mkstemp(f.empty_file);
    CHECK(status >= 0 && close(status) == 0);
    f.bounded = argc > 1 && !strcmp(argv[1], "bounded");
    f.plain = argc > 1 && !strcmp(argv[1], "plain");
    reference_server_config_init(&source);
    source.server_header = "vectis-head-fixture";
    source.keepalive_timeout_ms = 7001;
    source.keepalive_max_requests = 2;
    source.pretty_error_pages = !f.plain;
    source.worker_count = 1;
    source.worker_set_affinity_disabled = source.kore_quiet = 1;
    CHECK(reference_server_translate(&source, &config, &error) == KNURL_OK);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = 1;
    config.limits.max_response_header_bytes = f.bounded ? 1024 : 0;
    config.io.chunk_bytes = 64;
    config.io.output_queue_bytes = 128;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.userdata = &f;
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        knurl_route_config_init(&route);
        route.method = i == 5 ? KNURL_HTTP_HEAD : KNURL_HTTP_GET;
        route.path = paths[i];
        route.handler = handler;
        route.userdata = &f;
        CHECK(knurl_router_route(knurl_server_router(server), &route, NULL,
                                 &error) == KNURL_OK);
    }
    CHECK(knurl_server_run(server, &result, &error) == KNURL_OK);
    CHECK(waitpid(f.child, &status, 0) == f.child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
    CHECK(unlink(f.empty_file) == 0);
    return 0;
}

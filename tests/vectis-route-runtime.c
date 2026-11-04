#define _POSIX_C_SOURCE 200809L
#include "vectis-route.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "Vectis routes %d: %s\n", __LINE__, #t);           \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct fixture {
    struct reference_app app;
    pid_t child;
    int failed, sources, closed, reads, resets;
    struct producer *current;
};
struct producer {
    struct reference_source source;
    struct fixture *fixture;
    struct reference_request *request;
    rotmos_reply *reply;
    int after_required, after, reads, reset, fail_reset;
};
static size_t read_source(struct reference_source *source, void *out,
                          size_t capacity, struct reference_source_error *error)
{
    struct producer *p = source->impl;
    (void)error;
    ++p->fixture->reads;
    if (!p->reset || (p->after_required && !p->after) || capacity < 3 ||
        strcmp(reference_request_ip(p->request), "127.0.0.1"))
        p->fixture->failed = 1;
    if (p->reads++)
        return 0;
    memcpy(out, "a\0b", 3);
    return 3;
}
static int reset_source(struct reference_source *source,
                        struct reference_source_error *error)
{
    struct producer *p = source->impl;
    ++p->fixture->resets;
    if (p->after_required && !p->after)
        p->fixture->failed = 1;
    if (p->after_required &&
        rotmos_reply_status(p->reply, 222, NULL) != ROTMOS_STATE)
        p->fixture->failed = 1;
    p->reset = 1;
    p->reads = 0;
    if (p->fail_reset) {
        error->code = 3;
        error->message = "reset refused";
        return 3;
    }
    return 0;
}
static void close_source(struct reference_source *source)
{
    struct producer *p = source->impl;
    if (reference_request_path(p->request) == NULL)
        p->fixture->failed = 1;
    ++p->fixture->closed;
    free(p);
}
static struct producer *producer(struct fixture *f,
                                 struct reference_request *request)
{
    struct producer *p = calloc(1, sizeof(*p));
    if (p != NULL) {
        p->source.read = read_source;
        p->source.reset = reset_source;
        p->source.close = close_source;
        p->source.impl = p;
        p->fixture = f;
        p->request = request;
        ++f->sources;
    }
    return p;
}
static reference_status handle(struct reference_app *app,
                               struct reference_request *request,
                               struct reference_response *response, void *data,
                               reference_error *error)
{
    struct fixture *f = data;
    const char *path = reference_request_path(request), *name, *value;
    rotmos_bytes bytes;
    struct producer *p;
    reference_status status;
    int code;
    if (app != &f->app || path == NULL)
        return REFERENCE_STATE;
    if (!strcmp(path, "/meta/first/last")) {
        if (reference_request_method(request) != ROTMOS_HTTP_POST ||
            strcmp(reference_request_param(request, "one"), "first") ||
            strcmp(reference_request_param(request, "two"), "last") ||
            reference_request_param(request, "absent") != NULL ||
            strcmp(reference_request_header(request, "X-repeat"), "first") ||
            reference_request_query_count(request) != 3 ||
            strcmp(reference_request_query(request, "x"), "first") ||
            strcmp(reference_request_query(request, "empty"), "") ||
            reference_request_query(request, "absent") != NULL ||
            !reference_request_query_at(request, 1, &name, &value) ||
            strcmp(name, "x") || strcmp(value, "second") ||
            reference_request_query_at(request, 3, &name, &value) ||
            reference_request_bytes(request, &bytes, error) != REFERENCE_OK ||
            bytes.length != 3 || memcmp(bytes.data, "a\0b", 3))
            return REFERENCE_STATE;
        if (reference_response_status(response, 103, error) != REFERENCE_OK ||
            reference_response_header(response, "Set-Cookie", "one=1", error) !=
                REFERENCE_OK ||
            reference_response_header(response, "Set-Cookie", "two=2", error) !=
                REFERENCE_OK)
            return REFERENCE_STATE;
        return reference_response_text(response, 201, "text/plain", "mapped",
                                       error);
    }
    if (!strcmp(path, "/invalid") || !strcmp(path, "/unsupported") ||
        !strcmp(path, "/failure")) {
        (void)reference_response_header(response, "X-Discard", "discarded",
                                        NULL);
        (void)reference_response_text(response, 200, "text/plain", "discarded",
                                      NULL);
        memset(error->message, 'd', 255);
        error->message[255] = 0;
        memset(error->detail, 'e', 255);
        error->detail[255] = 0;
        error->code = !strcmp(path, "/invalid") ? REFERENCE_INVALID
                      : !strcmp(path, "/unsupported")
                          ? REFERENCE_NOT_IMPLEMENTED
                          : REFERENCE_STATE;
        error->source = 4;
        error->dependency_code = 99;
        return error->code;
    }
    if (!strcmp(path, "/stop")) {
        if (f->failed || f->sources != f->closed || f->resets != 4 ||
            f->reads != 6 || app->requests != app->closed + 1)
            return REFERENCE_STATE;
        if (rotmos_server_request_stop(app->server, NULL) != ROTMOS_OK)
            return REFERENCE_STATE;
        return reference_response_text(response, 200, "text/plain", "stopped",
                                       error);
    }
    if (!strcmp(path, "/empty"))
        return REFERENCE_OK;
    p = producer(f, request);
    if (p == NULL)
        return REFERENCE_NOMEM;
    p->after_required = !strcmp(path, "/live");
    p->reply = response->reply;
    p->fail_reset =
        !strcmp(path, "/reset-failure") || !strcmp(path, "/spooled-failure");
    if (!strcmp(path, "/singlepass")) {
        p->source.reset = NULL;
        p->reset = 1;
    }
    if (!strcmp(path, "/spooled-failure")) {
        if (reference_response_text(response, 202, "text/plain", "prior",
                                    error) != REFERENCE_OK)
            return REFERENCE_STATE;
        status = reference_response_source(response, 200, "text/plain",
                                           &p->source, error);
        if (status != REFERENCE_STATE || error->source != 3 ||
            error->dependency_code != 3 ||
            strcmp(error->message,
                   "failed to reset response source: reset refused") ||
            f->closed != f->sources - 1)
            f->failed = 1;
        close_source(&p->source);
        return REFERENCE_OK;
    }
    f->current = p;
    code = !strcmp(path, "/204")   ? 204
           : !strcmp(path, "/205") ? 205
           : !strcmp(path, "/304") ? 304
           : !strcmp(path, "/103") ? 103
           : !strcmp(path, "/100") ? 100
                                   : 200;
    status = !strcmp(path, "/spooled")
                 ? reference_response_source(response, code,
                                             "application/octet-stream",
                                             &p->source, error)
                 : reference_response_stream_source(
                       response, !strcmp(path, "/bad-transfer") ? 99 : code,
                       "application/octet-stream", &p->source, error);
    if (!strcmp(path, "/spooled")) {
        if (status != REFERENCE_OK || p->reset != 1 || p->reads != 2 ||
            f->closed != f->sources - 1)
            f->failed = 1;
        close_source(&p->source); /* Borrowed source remains caller-owned. */
    } else if (!strcmp(path, "/bad-transfer")) {
        if (status != REFERENCE_INVALID || p->reset || p->reads ||
            f->closed != f->sources - 1)
            f->failed = 1;
        close_source(&p->source);
        return reference_response_text(response, 200, "text/plain", "retained",
                                       error);
    }
    return status;
}
static rotmos_status after(rotmos_request *request, rotmos_reply *reply,
                           int *finish, void *data, rotmos_error *error)
{
    struct fixture *f = data;
    struct producer *p = f->current;
    (void)reply;
    (void)finish;
    (void)error;
    if (p == NULL || p->reset || p->reads)
        return ROTMOS_STATE;
    p->after = 1;
    if (!strcmp(rotmos_request_path(request).data, "/replace"))
        return rotmos_reply_text(reply, 201, "text/plain", "replaced", error);
    return ROTMOS_OK;
}
static rotmos_status live_route(rotmos_request *request, rotmos_reply *reply,
                                void *data, rotmos_error *error)
{
    struct fixture *f = data;
    struct reference_response response;
    reference_error translated;
    reference_status status;
    response.reply = reply;
    reference_error_clear(&translated);
    status = handle(&f->app, rotmos_request_userdata(request), &response, data,
                    &translated);
    (void)error;
    return status == REFERENCE_OK ? ROTMOS_OK : ROTMOS_STATE;
}
static int exchange(unsigned short port, const char *method, const char *path,
                    int code, const void *body, size_t body_size, int flags)
{
    struct sockaddr_in address;
    struct timeval timeout;
    char request[512], response[2048], status[32], *payload;
    size_t length, used = 0;
    ssize_t n;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
          0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    CHECK(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    length = (size_t)sprintf(
        request,
        "%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nX-Repeat: "
        "first\r\nX-Repeat: second\r\nContent-Length: %d\r\n\r\n",
        method, path, flags == 1 ? 3 : 0);
    if (flags == 1) {
        memcpy(request + length, "a\0b", 3);
        length += 3;
    }
    CHECK(write(fd, request, length) == (ssize_t)length);
    while (used < sizeof(response) - 1) {
        n = read(fd, response + used, sizeof(response) - 1 - used);
        if (n < 0 && errno == EINTR)
            continue;
        CHECK(n >= 0);
        if (n == 0)
            break;
        used += (size_t)n;
    }
    close(fd);
    response[used] = 0;
    (void)sprintf(status, "HTTP/1.1 %d ", code);
    CHECK(!strncmp(response, status, strlen(status)));
    payload = strstr(response, "\r\n\r\n");
    CHECK(payload != NULL);
    payload += 4;
    CHECK(used - (size_t)(payload - response) == body_size &&
          !memcmp(payload, body, body_size));
    if (flags == 1)
        CHECK(strstr(response, "201 Created\r\n") &&
              strstr(response, "Set-Cookie: one=1\r\n") &&
              strstr(response, "Set-Cookie: two=2\r\n"));
    if (flags == 2)
        CHECK(strstr(response, "X-Discard") == NULL &&
              strstr(response, "content-type:") == NULL);
    if (flags == 3)
        CHECK(strstr(response, "content-length:") == NULL &&
              strstr(response, "transfer-encoding:") == NULL);
    if (code == 100)
        CHECK(used == strlen("HTTP/1.1 100 Continue\r\n\r\n"));
    return 0;
}
static int client(unsigned short port)
{
    char diagnostic[256];
    memset(diagnostic, 'd', 255);
    diagnostic[255] = 0;
    CHECK(exchange(port, "POST", "/meta/first/last?x=first&x=second&empty=",
                   201, "mapped", 6, 1) == 0);
    CHECK(exchange(port, "GET", "/invalid", 400, diagnostic, 255, 2) == 0);
    CHECK(exchange(port, "GET", "/unsupported", 501, diagnostic, 255, 2) == 0);
    CHECK(exchange(port, "GET", "/failure", 500, diagnostic, 255, 2) == 0);
    CHECK(exchange(port, "GET", "/empty", 204, "", 0, 0) == 0);
    CHECK(exchange(port, "GET", "/live", 200, "3\r\na\0b\r\n0\r\n\r\n", 13,
                   0) == 0);
    CHECK(exchange(port, "HEAD", "/head", 200, "", 0, 3) == 0);
    CHECK(exchange(port, "GET", "/204", 204, "", 0, 3) == 0);
    CHECK(exchange(port, "GET", "/205", 205, "", 0, 0) == 0);
    CHECK(exchange(port, "GET", "/304", 304, "", 0, 3) == 0);
    CHECK(exchange(port, "GET", "/103", 103, "", 0, 3) == 0);
    CHECK(exchange(port, "GET", "/100", 100, "", 0, 3) == 0);
    CHECK(exchange(port, "GET", "/replace", 201, "replaced", 8, 0) == 0);
    CHECK(exchange(port, "GET", "/singlepass", 200, "3\r\na\0b\r\n0\r\n\r\n",
                   13, 0) == 0);
    CHECK(exchange(port, "GET", "/bad-transfer", 200, "retained", 8, 0) == 0);
    CHECK(exchange(port, "GET", "/spooled", 200, "a\0b", 3, 0) == 0);
    CHECK(exchange(port, "GET", "/reset-failure", 500, "", 0, 0) == 0);
    CHECK(exchange(port, "GET", "/spooled-failure", 202, "prior", 5, 0) == 0);
    CHECK(exchange(port, "GET", "/stop", 200, "stopped", 7, 0) == 0);
    return 0;
}
static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *process, void *data,
                           rotmos_error *error)
{
    struct fixture *f = data;
    rotmos_bound_listener listener;
    (void)process;
    if (rotmos_server_listener_at(server, 0, &listener, error) != ROTMOS_OK)
        return ROTMOS_STATE;
    f->child = fork();
    if (f->child == 0)
        _exit(client(listener.port));
    return f->child > 0 ? ROTMOS_OK : ROTMOS_IO;
}
int main(void)
{
    struct fixture f;
    struct reference_route_config route;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_route_config native;
    rotmos_error error;
    reference_error translated;
    rotmos_run_result result;
    const char *paths[] = {"/meta/:one/:two",
                           "/invalid",
                           "/unsupported",
                           "/failure",
                           "/empty",
                           "/head",
                           "/204",
                           "/205",
                           "/304",
                           "/103",
                           "/bad-transfer",
                           "/spooled",
                           "/reset-failure",
                           "/stop",
                           "/100",
                           "/singlepass",
                           "/spooled-failure"};
    size_t i;
    int status;
    memset(&f, 0, sizeof(f));
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = 1;
    config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    reference_route_callbacks(&f.app, &config.callbacks);
    config.callbacks.on_parent_ready = ready;
    CHECK(rotmos_server_new(&config, &f.app.server, &error) == ROTMOS_OK);
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        memset(&route, 0, sizeof(route));
        route.path = paths[i];
        route.method = i == 0   ? ROTMOS_HTTP_POST
                       : i == 5 ? ROTMOS_HTTP_HEAD
                                : ROTMOS_HTTP_GET;
        route.path_kind = i == 0 ? ROTMOS_PATH_PARAMS : ROTMOS_PATH_LITERAL;
        route.handler = handle;
        route.userdata = &f;
        if (i == 0) {
            route.body.mode = ROTMOS_BODY_BUFFERED;
            route.body.max_bytes = 16;
        }
        CHECK(reference_route_register(&f.app, &route, &translated) ==
              REFERENCE_OK);
    }
    rotmos_route_config_init(&native);
    native.method = ROTMOS_HTTP_GET;
    native.path = "/live";
    native.handler = live_route;
    native.userdata = &f;
    native.hooks.after = after;
    native.hooks.userdata = &f;
    CHECK(rotmos_router_route(rotmos_server_router(f.app.server), &native, NULL,
                              &error) == ROTMOS_OK);
    native.path = "/replace";
    CHECK(rotmos_router_route(rotmos_server_router(f.app.server), &native, NULL,
                              &error) == ROTMOS_OK);
    CHECK(rotmos_router_route_count(rotmos_server_router(f.app.server)) == 19);
    CHECK(rotmos_server_run(f.app.server, &result, &error) == ROTMOS_OK);
    CHECK(waitpid(f.child, &status, 0) == f.child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    CHECK(rotmos_server_destroy(f.app.server, &error) == ROTMOS_OK);
    reference_route_cleanup(&f.app);
    return 0;
}

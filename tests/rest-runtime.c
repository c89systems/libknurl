#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
#include <rotmos/lonejson.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "rest %d: %s\n", __LINE__, #t);                    \
            return 1;                                                          \
        }                                                                      \
    } while (0)
#define VERIFY(f, t)                                                           \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "rest callback %d: %s\n", __LINE__, #t);           \
            (f)->failed = 1;                                                   \
        }                                                                      \
    } while (0)
struct fixture {
    pid_t child;
    pthread_t owner;
    int failed, handled, errors, released, source_closed, after, auth,
        decoded_empty;
};
struct decoded {
    struct fixture *f;
    char text[16];
    size_t position;
    int closed;
};
static const char *id(rotmos_request *request)
{
    rotmos_bytes bytes;
    return rotmos_request_path_param(request, "id", &bytes, NULL) == ROTMOS_OK
               ? bytes.data
               : "";
}
static rotmos_status auth(rotmos_request *request, rotmos_reply *reply,
                          int *finish, void *data, rotmos_error *error)
{
    struct fixture *f = data;
    if (!strcmp(id(request), "auth")) {
        ++f->auth;
        *finish = 1;
        return rotmos_reply_text(reply, 401, "text/plain", "auth", error);
    }
    return ROTMOS_OK;
}
static rotmos_status before(rotmos_request *request, rotmos_reply *reply,
                            int *finish, void *data, rotmos_error *error)
{
    (void)finish;
    rotmos_request_set_userdata(request, data);
    return rotmos_reply_header_add(reply, "X-Before", "yes", error);
}
static rotmos_status after(rotmos_request *request, rotmos_reply *reply,
                           int *finish, void *data, rotmos_error *error)
{
    struct fixture *f = data;
    (void)finish;
    ++f->after;
    VERIFY(f, pthread_equal(f->owner, pthread_self()) &&
                  rotmos_request_userdata(request) == f);
    if (!strcmp(id(request), "replace"))
        return rotmos_reply_text(reply, 202, "text/plain", "after", error);
    return rotmos_reply_header_add(reply, "X-After", "yes", error);
}
static rotmos_status validate(rotmos_request *request, const void *value,
                              rotmos_reply *reply, int *finish, void *data,
                              rotmos_error *error)
{
    struct fixture *f = data;
    VERIFY(f, pthread_equal(f->owner, pthread_self()) &&
                  rotmos_request_userdata(request) == f);
    if (!strcmp(id(request), "validation")) {
        (void)value;
        rotmos_error_clear(error);
        return ROTMOS_INVALID;
    }
    if (!strcmp(id(request), "short")) {
        *finish = 1;
        return rotmos_reply_text(reply, 202, "text/plain", "validation", error);
    }
    return ROTMOS_OK;
}
static rotmos_status on_error(rotmos_request *request, rotmos_rest_phase phase,
                              const rotmos_error *cause, rotmos_reply *reply,
                              void *data, rotmos_error *error)
{
    struct fixture *f = data;
    ++f->errors;
    VERIFY(f, pthread_equal(f->owner, pthread_self()) &&
                  cause->status != ROTMOS_OK);
    if (!strcmp(id(request), "custom-error")) {
        VERIFY(f, phase == ROTMOS_REST_HANDLER);
        return rotmos_reply_text(reply, 418, "text/plain", "custom", error);
    }
    return !strcmp(id(request), "fallback-error") ? ROTMOS_STATE : ROTMOS_OK;
}
static rotmos_status handler(rotmos_request *request, const void *decoded,
                             rotmos_reply *reply, void *data,
                             rotmos_error *error)
{
    struct fixture *f = data;
    rotmos_rest_reply_config config;
    const lonejson_json_value *json = decoded;
    rotmos_owned_bytes bytes;
    rotmos_bytes view;
    rotmos_status status;
    ++f->handled;
    VERIFY(f, pthread_equal(f->owner, pthread_self()) &&
                  rotmos_request_userdata(request) == f);
    if (!strcmp(id(request), "conflict"))
        return ROTMOS_CONFLICT;
    if (!strcmp(id(request), "timeout"))
        return ROTMOS_TIMEOUT;
    if (!strcmp(id(request), "unsupported"))
        return ROTMOS_NOT_SUPPORTED;
    if (!strcmp(id(request), "custom-error") ||
        !strcmp(id(request), "fallback-error"))
        return ROTMOS_IO;
    if (!strcmp(id(request), "docs")) {
        rotmos_openapi_route_doc doc;
        rotmos_openapi_route_doc_init(&doc);
        VERIFY(f, rotmos_router_openapi_doc(
                      rotmos_server_router(rotmos_request_server(request)),
                      ROTMOS_HTTP_METHODS_GET, "/late", &doc,
                      NULL) == ROTMOS_STATE);
        status = rotmos_router_openapi(
            rotmos_server_router(rotmos_request_server(request)), NULL,
            ROTMOS_OPENAPI_JSON, &bytes, error);
        if (status != ROTMOS_OK)
            return status;
        view.data = bytes.data;
        view.length = bytes.length;
        status =
            rotmos_reply_bytes(reply, 200, "application/json", view, error);
        rotmos_owned_bytes_cleanup(&bytes);
        return status;
    }
    if (decoded == NULL)
        return ROTMOS_OK;
    rotmos_rest_reply_config_init(&config);
    config.status = 201;
    if (!strcmp(id(request), "bounded"))
        config.max_bytes = 1;
    return rotmos_reply_rest_json(reply, json, &config, error);
}
static rotmos_status custom_decode(rotmos_request *request, rotmos_bytes body,
                                   void **out, void *data, rotmos_error *error)
{
    struct fixture *f = data;
    struct decoded *decoded;
    (void)request;
    (void)error;
    *out = NULL;
    VERIFY(f, pthread_equal(f->owner, pthread_self()));
    if (body.length == 0) {
        ++f->decoded_empty;
        return ROTMOS_OK;
    }
    if (body.length == 3 && !memcmp(body.data, "oom", 3))
        return ROTMOS_NOMEM;
    if (body.length >= 16)
        return ROTMOS_INVALID;
    decoded = calloc(1, sizeof(*decoded));
    if (decoded == NULL)
        return ROTMOS_NOMEM;
    decoded->f = f;
    memcpy(decoded->text, body.data, body.length);
    *out = decoded;
    return ROTMOS_OK;
}
static rotmos_status source_read(void *buffer, size_t capacity, size_t *count,
                                 void *data, rotmos_error *error)
{
    struct decoded *decoded = data;
    size_t n = strlen(decoded->text) - decoded->position;
    (void)error;
    VERIFY(decoded->f, !decoded->closed &&
                           pthread_equal(decoded->f->owner, pthread_self()));
    if (n > capacity)
        n = capacity;
    *count = n;
    if (n == 0)
        return ROTMOS_EOF;
    memcpy(buffer, decoded->text + decoded->position, n);
    decoded->position += n;
    return ROTMOS_OK;
}
static void source_close(void *data, rotmos_status outcome)
{
    struct decoded *decoded = data;
    VERIFY(decoded->f, outcome == ROTMOS_OK && !decoded->closed &&
                           pthread_equal(decoded->f->owner, pthread_self()));
    decoded->closed = 1;
    ++decoded->f->source_closed;
}
static void release_decoded(void *data, void *userdata)
{
    struct fixture *f = userdata;
    struct decoded *decoded = data;
    VERIFY(f, decoded->closed && pthread_equal(f->owner, pthread_self()));
    ++f->released;
    free(decoded);
}
static rotmos_status custom_handler(rotmos_request *request, const void *value,
                                    rotmos_reply *reply, void *data,
                                    rotmos_error *error)
{
    rotmos_source_config config;
    rotmos_source *source;
    rotmos_status status;
    struct decoded *decoded = (struct decoded *)value;
    struct fixture *f = data;
    (void)request;
    VERIFY(f, decoded != NULL && !decoded->closed);
    rotmos_source_config_init(&config);
    config.read = source_read;
    config.close = source_close;
    config.userdata = decoded;
    config.has_length = 1;
    config.length.low = (unsigned long)strlen(decoded->text);
    status = rotmos_source_new(&config, &source, error);
    if (status != ROTMOS_OK)
        return status;
    status = rotmos_reply_source_take(reply, 201, "text/plain", source, error);
    if (status != ROTMOS_OK)
        rotmos_source_destroy(source, NULL);
    return status;
}
static rotmos_status raw_handler(rotmos_request *request, const void *value,
                                 rotmos_reply *reply, void *data,
                                 rotmos_error *error)
{
    rotmos_bytes body;
    (void)data;
    if (value != NULL ||
        rotmos_request_body_bytes(request, &body, error) != ROTMOS_OK)
        return ROTMOS_STATE;
    return rotmos_reply_bytes(reply, 200, "text/plain", body, error);
}
static rotmos_status stop(rotmos_request *request, rotmos_reply *reply,
                          void *data, rotmos_error *error)
{
    struct fixture *f = data;
    (void)rotmos_server_request_stop(rotmos_request_server(request), NULL);
    return rotmos_reply_text(reply, 200, "text/plain",
                             f->failed ? "failed" : "stopped", error);
}
static int exchange(unsigned short port, const char *path, const char *body,
                    int code, const char *expected, int substring)
{
    struct sockaddr_in address;
    struct timeval timeout;
    int fd;
    ssize_t n;
    size_t used = 0;
    char request[512], response[8192], prefix[32], *payload;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
          0);
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    snprintf(request, sizeof(request),
             "POST %s HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
             "%lu\r\nConnection: close\r\n\r\n%s",
             path, (unsigned long)strlen(body), body);
    CHECK(write(fd, request, strlen(request)) == (ssize_t)strlen(request));
    while (used < sizeof(response) - 1 &&
           (n = read(fd, response + used, sizeof(response) - 1 - used)) > 0)
        used += (size_t)n;
    close(fd);
    response[used] = 0;
    snprintf(prefix, sizeof(prefix), "HTTP/1.1 %d ", code);
    if (strncmp(response, prefix, strlen(prefix)))
        fprintf(stderr, "%s: %s\n", path, response);
    CHECK(!strncmp(response, prefix, strlen(prefix)));
    payload = strstr(response, "\r\n\r\n");
    CHECK(payload != NULL);
    payload += 4;
    CHECK(substring ? strstr(payload, expected) != NULL
                    : !strcmp(payload, expected));
    return 0;
}
static int client(unsigned short port)
{
    CHECK(exchange(port, "/api/echo", " { \"message\" : \"hello\" } ", 201,
                   "{\"message\":\"hello\"}\n", 0) == 0);
    CHECK(exchange(port, "/api/echo", "null", 201, "null\n", 0) == 0);
    CHECK(exchange(port, "/api/empty", "", 204, "", 0) == 0);
    CHECK(exchange(port, "/api/auth", "{", 401, "auth", 0) == 0);
    CHECK(exchange(port, "/api/echo", "{", 400, "\"kind\":\"json_parse\"", 1) ==
          0);
    CHECK(exchange(port, "/api/validation", "{}", 400,
                   "\"kind\":\"validation\"", 1) == 0);
    CHECK(exchange(port, "/api/short", "{}", 202, "validation", 0) == 0);
    CHECK(exchange(port, "/api/replace", "{}", 202, "after", 0) == 0);
    CHECK(exchange(port, "/api/conflict", "{}", 409, "\"kind\":\"handler\"",
                   1) == 0);
    CHECK(exchange(port, "/api/timeout", "{}", 504, "\"kind\":\"handler\"",
                   1) == 0);
    CHECK(exchange(port, "/api/unsupported", "{}", 501, "\"kind\":\"handler\"",
                   1) == 0);
    CHECK(exchange(port, "/api/custom-error", "{}", 418, "custom", 0) == 0);
    CHECK(exchange(port, "/api/fallback-error", "{}", 500,
                   "REST error handler failed", 0) == 0);
    CHECK(exchange(port, "/api/bounded", "{}", 500, "\"kind\":\"handler\"",
                   1) == 0);
    CHECK(exchange(port, "/api/docs", "", 200, "\"/api/{id}\"", 1) == 0);
    CHECK(exchange(port, "/custom/stream", "payload", 201, "payload", 0) == 0);
    CHECK(exchange(port, "/custom/oom", "oom", 500, "\"kind\":\"json_parse\"",
                   1) == 0);
    CHECK(exchange(port, "/raw", "not json", 200, "not json", 0) == 0);
    CHECK(exchange(port, "/none", "", 200, "", 0) == 0);
    CHECK(exchange(port, "/custom-empty", "", 200, "", 0) == 0);
    CHECK(exchange(port, "/stop", "", 200, "stopped", 0) == 0);
    return 0;
}
static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *process, void *data,
                           rotmos_error *error)
{
    struct fixture *f = data;
    rotmos_bound_listener listener;
    int status;
    (void)process;
    if (rotmos_server_listener_at(server, 0, &listener, error) != ROTMOS_OK)
        return ROTMOS_STATE;
    f->child = fork();
    if (f->child < 0)
        return ROTMOS_IO;
    if (f->child == 0) {
        signal(SIGTERM, SIG_DFL);
        status = client(listener.port);
        if (status)
            kill(getppid(), SIGTERM);
        _exit(status);
    }
    return ROTMOS_OK;
}
static rotmos_status started(rotmos_server *server,
                             const rotmos_process_context *process, void *data,
                             rotmos_error *error)
{
    struct fixture *f = data;
    (void)server;
    (void)process;
    (void)error;
    f->owner = pthread_self();
    return ROTMOS_OK;
}
static int exited(rotmos_server *server, const rotmos_process_context *process,
                  void *data)
{
    struct fixture *f = data;
    (void)server;
    (void)process;
    return f->failed || f->source_closed != 1 || f->released != 1 ||
           f->auth != 1 || f->errors != 9 || f->handled != 11 ||
           f->after != 13 || f->decoded_empty != 1;
}
static int replies(void)
{
    lonejson *runtime;
    lonejson_json_value value;
    lonejson_error native;
    rotmos_reply *reply;
    rotmos_rest_reply_config config;
    rotmos_error error;
    char *large;
    size_t length = 10485761UL;
    runtime = lonejson_new(NULL, &native);
    CHECK(runtime != NULL);
    lonejson_json_value_init(runtime, &value);
    CHECK(lonejson_json_value_set_buffer(&value, "{}", 2, &native) ==
          LONEJSON_STATUS_OK);
    CHECK(rotmos_reply_new(&reply, &error) == ROTMOS_OK);
    CHECK(rotmos_reply_text(reply, 200, "text/plain", "old", &error) ==
          ROTMOS_OK);
    rotmos_rest_reply_config_init(&config);
    config.max_bytes = 1;
    CHECK(rotmos_reply_rest_json(reply, &value, &config, &error) ==
          ROTMOS_LIMIT);
    CHECK(rotmos_reply_body(reply).length == 3 &&
          !memcmp(rotmos_reply_body(reply).data, "old", 3));
    config.max_bytes = 10;
    config.newline_disabled = 1;
    config.status = 202;
    CHECK(rotmos_reply_rest_json(reply, &value, &config, &error) == ROTMOS_OK);
    CHECK(rotmos_reply_status_code(reply) == 202 &&
          rotmos_reply_body(reply).length == 2 &&
          !memcmp(rotmos_reply_body(reply).data, "{}", 2));
    large = malloc(length + 2);
    CHECK(large != NULL);
    large[0] = '"';
    memset(large + 1, 'x', length);
    large[length + 1] = '"';
    CHECK(lonejson_json_value_set_buffer(&value, large, length + 2, &native) ==
          LONEJSON_STATUS_OK);
    CHECK(rotmos_reply_rest_json(reply, &value, NULL, &error) == ROTMOS_OK &&
          rotmos_reply_body(reply).length == length + 3);
    CHECK(((const char *)rotmos_reply_body(reply).data)[length + 2] == '\n');
    CHECK(rotmos_reply_destroy(reply, &error) == ROTMOS_OK);
    lonejson_json_value_cleanup(&value);
    free(large);
    lonejson_free(runtime);
    return 0;
}
int main(void)
{
    struct fixture f;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_rest_route_config rest;
    rotmos_route_config route;
    rotmos_server *server;
    rotmos_openapi_route_doc doc;
    rotmos_route_group *group;
    rotmos_route_group_config group_config;
    rotmos_error error;
    rotmos_run_result result;
    rotmos_status status;
    int child;
    CHECK(replies() == 0);
    memset(&f, 0, sizeof(f));
    f.child = -1;
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_start = started;
    config.callbacks.on_worker_exit = exited;
    config.callbacks.userdata = &f;
    config.io.chunk_bytes = 5;
    config.io.output_queue_bytes = 1024;
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    rotmos_route_group_config_init(&group_config);
    group_config.prefix = "/api";
    group_config.hooks.before = before;
    group_config.hooks.after = after;
    group_config.hooks.userdata = &f;
    CHECK(rotmos_router_group(rotmos_server_router(server), &group_config,
                              &group, &error) == ROTMOS_OK);
    rotmos_rest_route_config_init(&rest);
    rest.path = "/:id";
    rest.path_kind = ROTMOS_PATH_PARAMS;
    rest.method = ROTMOS_HTTP_POST;
    rest.body.max_bytes.low = 64;
    rest.body.memory_buffer_limit_bytes = 64;
    rest.handler = handler;
    rest.validate = validate;
    rest.on_error = on_error;
    rest.userdata = &f;
    rest.hooks.auth = auth;
    rest.hooks.userdata = &f;
    CHECK(rotmos_route_group_rest_route(group, &rest, NULL, &error) ==
          ROTMOS_OK);
    CHECK(rotmos_route_group_rest_route(group, &rest, NULL, &error) ==
          ROTMOS_CONFLICT);
    rest.path = "/custom/:id";
    rest.hooks.before = before;
    rest.decode = custom_decode;
    rest.release = release_decoded;
    rest.validate = NULL;
    rest.handler = custom_handler;
    CHECK(rotmos_router_rest_route(rotmos_server_router(server), &rest, NULL,
                                   &error) == ROTMOS_OK);
    rest.handler = raw_handler;
    rest.path = "/custom-empty";
    CHECK(rotmos_router_rest_route(rotmos_server_router(server), &rest, NULL,
                                   &error) == ROTMOS_OK);
    rest.path = "/raw";
    rest.decode_disabled = 1;
    rest.handler = raw_handler;
    CHECK(rotmos_router_rest_route(rotmos_server_router(server), &rest, NULL,
                                   &error) == ROTMOS_OK);
    rest.path = "/none";
    rest.decode_disabled = 0;
    rest.decode = NULL;
    rest.release = NULL;
    rotmos_body_policy_init(&rest.body);
    CHECK(rotmos_router_rest_route(rotmos_server_router(server), &rest, NULL,
                                   &error) == ROTMOS_OK);
    rest.handler = NULL;
    CHECK(rotmos_router_rest_route(rotmos_server_router(server), &rest, NULL,
                                   &error) == ROTMOS_INVALID);
    rotmos_route_config_init(&route);
    route.path = "/stop";
    route.handler = stop;
    route.userdata = &f;
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    rotmos_openapi_route_doc_init(&doc);
    doc.summary = "REST endpoint";
    CHECK(rotmos_router_openapi_doc(rotmos_server_router(server),
                                    ROTMOS_HTTP_METHODS_POST, "/api/:id", &doc,
                                    &error) == ROTMOS_OK);
    status = rotmos_server_run(server, &result, &error);
    if (status != ROTMOS_OK)
        fprintf(stderr, "%s: %s\n", error.operation, error.message);
    CHECK(waitpid(f.child, &child, 0) == f.child && WIFEXITED(child) &&
          WEXITSTATUS(child) == 0);
    CHECK(status == ROTMOS_OK && !result.escalated);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    return 0;
}

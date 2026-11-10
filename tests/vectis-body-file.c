#define _POSIX_C_SOURCE 200809L
#include "vectis-route.h"
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "Vectis body/file %d: %s\n", __LINE__, #t);        \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct fixture {
    struct reference_app app;
    char directory[64], file[96], missing[96], replacement[96], transport[128];
    char moved[80];
    pid_t child;
    int body_calls, spilled, hooks, failed;
};
static int put(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_TRUNC | O_CREAT, 0600);
    int failed;
    if (fd == -1)
        return 1;
    failed = write(fd, text, strlen(text)) != (ssize_t)strlen(text);
    return close(fd) != 0 || failed;
}
static reference_status body(struct reference_app *app,
                             struct reference_request *request,
                             struct reference_response *response, void *data,
                             reference_error *error)
{
    struct fixture *f = data;
    struct reference_source *reader = reference_request_body_reader(request);
    struct reference_source_error input = {0, NULL};
    struct stat info;
    const char *path = reference_request_path(request), *stored;
    knurl_bytes bytes;
    unsigned char output[4];
    size_t count, used = 0;
    int spilled =
        !strcmp(path, "/body-spill") || !strcmp(path, "/body-json-spill") ||
        !strcmp(path, "/body-upload") || !strcmp(path, "/body-default");
    (void)app;
    ++f->body_calls;
    if (reader == NULL || reference_request_body_is_spooled(request) != spilled)
        return REFERENCE_STATE;
    stored = reference_request_body_path(request);
    if (spilled) {
        if (stored == NULL || stat(stored, &info) != 0 ||
            (info.st_mode & 0777) != 0600 ||
            reference_request_bytes(request, &bytes, error) !=
                REFERENCE_STATE ||
            strlen(stored) >= sizeof(f->transport))
            return REFERENCE_STATE;
        strcpy(f->transport, stored);
        ++f->spilled;
    } else if (stored != NULL ||
               reference_request_bytes(request, &bytes, error) !=
                   REFERENCE_OK ||
               bytes.length != 1 || memcmp(bytes.data, "a", 1))
        return REFERENCE_STATE;
    while ((count = reader->read(reader, output + used, 1, &input)) != 0) {
        used += count;
        if (used >= sizeof(output))
            return REFERENCE_STATE;
    }
    if (input.code || used != (spilled ? 3U : 1U) ||
        memcmp(output, spilled ? "a\0b" : "a", used) ||
        reader->reset(reader, &input) != 0 ||
        reader->read(reader, output, 1, &input) != 1 || output[0] != 'a')
        return REFERENCE_STATE;
    return reference_response_text(response, 200, "text/plain",
                                   spilled ? "spilled" : "memory", error);
}
static reference_status file(struct reference_app *app,
                             struct reference_request *request,
                             struct reference_response *response, void *data,
                             reference_error *error)
{
    struct fixture *f = data;
    const char *path = reference_request_path(request);
    reference_status status;
    char copied[96];
    if (!strcmp(path, "/stop")) {
        if (f->failed || f->body_calls != 6 || f->spilled != 4 ||
            f->hooks != 4 || access(f->transport, F_OK) != -1 ||
            errno != ENOENT)
            return REFERENCE_STATE;
        if (knurl_server_request_stop(app->server, NULL) != KNURL_OK)
            return REFERENCE_STATE;
        return reference_response_text(response, 200, "text/plain", "stopped",
                                       error);
    }
    strcpy(copied, !strcmp(path, "/file-head") || !strcmp(path, "/file-create")
                       ? f->missing
                       : f->file);
    status = reference_response_file(
        response, !strcmp(path, "/file-bodyless") ? 204 : 200, "text/plain",
        copied, error);
    copied[0] = 'X'; /* The declaration owns its copied path. */
    return status;
}
static knurl_status native_file(knurl_request *request, knurl_reply *reply,
                                void *data, knurl_error *error)
{
    struct fixture *f = data;
    struct reference_response response;
    reference_error translated;
    response.reply = reply;
    reference_error_clear(&translated);
    (void)error;
    return file(&f->app, knurl_request_userdata(request), &response, data,
                &translated) == REFERENCE_OK
               ? KNURL_OK
               : KNURL_STATE;
}
static knurl_status after_file(knurl_request *request, knurl_reply *reply,
                               int *finish, void *data, knurl_error *error)
{
    struct fixture *f = data;
    const char *path = knurl_request_path(request).data;
    (void)finish;
    ++f->hooks;
    if (knurl_reply_payload_kind(reply) != KNURL_REPLY_FILE)
        return KNURL_STATE;
    if (!strcmp(path, "/file-change")) {
        if (put(f->replacement, "after") || rename(f->replacement, f->file))
            return KNURL_IO;
    } else if (!strcmp(path, "/file-create")) {
        if (put(f->missing, "created"))
            return KNURL_IO;
    } else if (!strcmp(path, "/file-remove")) {
        if (unlink(f->file))
            return KNURL_IO;
    } else if (!strcmp(path, "/file-replace"))
        return knurl_reply_text(reply, 201, "text/plain", "override", error);
    return KNURL_OK;
}
static int exchange(unsigned short port, const char *method, const char *path,
                    const void *input, size_t input_size, int code,
                    const char *body, int expect_head)
{
    struct sockaddr_in address;
    struct timeval timeout;
    char request[512], response[2048], status[32], *payload;
    size_t size, used = 0;
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
    size = (size_t)sprintf(request,
                           "%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: "
                           "close\r\nContent-Length: %lu\r\n\r\n",
                           method, path, (unsigned long)input_size);
    if (input_size)
        memcpy(request + size, input, input_size);
    CHECK(write(fd, request, size + input_size) ==
          (ssize_t)(size + input_size));
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
    if (strncmp(response, status, strlen(status)))
        fprintf(stderr, "%s: %s\n", path, response);
    CHECK(!strncmp(response, status, strlen(status)));
    payload = strstr(response, "\r\n\r\n");
    CHECK(payload != NULL);
    payload += 4;
    CHECK(used - (size_t)(payload - response) == strlen(body) &&
          !memcmp(payload, body, strlen(body)));
    if (expect_head)
        CHECK(strstr(response, "content-length: 7\r\n"));
    return 0;
}
static int client(unsigned short port, struct fixture *f)
{
#define GET(path, code, body)                                                  \
    CHECK(exchange(port, "GET", path, "", 0, code, body, 0) == 0)
    CHECK(exchange(port, "POST", "/body-json", "a", 1, 200, "memory", 0) == 0);
    CHECK(exchange(port, "POST", "/body-spill", "a\0b", 3, 200, "spilled", 0) ==
          0);
    CHECK(exchange(port, "POST", "/body-json-spill", "a\0b", 3, 200, "spilled",
                   0) == 0);
    CHECK(exchange(port, "POST", "/body-upload", "a\0b", 3, 200, "spilled",
                   0) == 0);
    CHECK(exchange(port, "POST", "/body-default", "a\0b", 3, 200, "spilled",
                   0) == 0);
    CHECK(exchange(port, "POST", "/body-disabled", "a", 1, 200, "memory", 0) ==
          0);
    CHECK(exchange(port, "POST", "/body-disabled", "a\0b", 3, 413,
                   "failed to stream request body", 0) == 0);
    CHECK(exchange(port, "POST", "/body-disabled", "too big", 7, 413,
                   "request body exceeds route limit", 0) == 0);
    CHECK(rename(f->directory, f->moved) == 0);
    CHECK(exchange(port, "POST", "/body-spill", "a\0b", 3, 413,
                   "failed to stream request body", 0) == 0);
    CHECK(rename(f->moved, f->directory) == 0);
    GET("/file-change", 200, "after");
    GET("/file-create", 200, "created");
    CHECK(exchange(port, "HEAD", "/file-head", "", 0, 200, "", 1) == 0);
    GET("/file-remove", 404, "");
    GET("/file-replace", 201, "override");
    GET("/file-bodyless", 204, "");
    GET("/stop", 200, "stopped");
#undef GET
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
        _exit(client(listener.port, f));
    return f->child > 0 ? KNURL_OK : KNURL_IO;
}
int main(void)
{
    struct fixture f;
    struct reference_route_config route;
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_route_config native;
    knurl_router *wide;
    knurl_error error;
    reference_error translated;
    knurl_run_result result;
    const char *bodies[] = {"/body-json",       "/body-spill",
                            "/body-json-spill", "/body-disabled",
                            "/body-upload",     "/body-default"};
    const char *files[] = {"/file-change", "/file-create", "/file-remove",
                           "/file-replace"};
    size_t i;
    int status;
    memset(&f, 0, sizeof(f));
    strcpy(f.directory, "vectis-body-file-XXXXXX");
    CHECK(mkdtemp(f.directory));
    (void)sprintf(f.moved, "%s-moved", f.directory);
    (void)sprintf(f.file, "%s/original", f.directory);
    (void)sprintf(f.missing, "%s/created", f.directory);
    (void)sprintf(f.replacement, "%s/replacement", f.directory);
    CHECK(put(f.file, "before") == 0);
    CHECK(knurl_router_new(&wide, &error) == KNURL_OK);
    knurl_route_config_init(&native);
    native.method = KNURL_HTTP_POST;
    native.path = "/wide";
    native.handler = native_file;
    native.userdata = &f;
    native.body.mode = KNURL_BODY_BUFFERED;
    native.body.max_bytes.high = native.body.max_bytes.low = 0xffffffffUL;
    native.body.memory_buffer_limit_bytes = 1;
    native.body.disk_spool_disabled = 1;
    CHECK(knurl_router_route(wide, &native, NULL, &error) == KNURL_OK);
    CHECK(knurl_router_destroy(wide, &error) == KNURL_OK);
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = 1;
    config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.limits.body_spool_directory = f.directory;
    config.limits.hard_body_bytes.low = config.limits.default_body_bytes.low =
        8;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    reference_route_callbacks(&f.app, &config.callbacks);
    config.callbacks.on_parent_ready = ready;
    CHECK(knurl_server_new(&config, &f.app.server, &error) == KNURL_OK);
    for (i = 0; i < 6; ++i) {
        memset(&route, 0, sizeof(route));
        route.path = bodies[i];
        route.method = KNURL_HTTP_POST;
        route.body.mode = i == 4 ? KNURL_BODY_STREAMING_UPLOAD
                          : i == 0 || i == 2 || i == 5 ? KNURL_BODY_JSON
                                                       : KNURL_BODY_BUFFERED;
        route.body.max_bytes = i == 5 ? 0 : 4;
        route.body.memory_buffer_limit_bytes = 2;
        route.body.disk_spool_disabled = i == 3;
        route.handler = body;
        route.userdata = &f;
        CHECK(reference_route_register(&f.app, &route, &translated) ==
              REFERENCE_OK);
    }
    route.path = "/invalid-policy";
    route.body.max_bytes = 9;
    CHECK(reference_route_register(&f.app, &route, &translated) ==
          REFERENCE_INVALID);
    route.body.max_bytes = 4;
    route.body.memory_buffer_limit_bytes = 5;
    CHECK(reference_route_register(&f.app, &route, &translated) ==
          REFERENCE_INVALID);
    route.body.mode = KNURL_BODY_STREAMING_UPLOAD;
    route.body.memory_buffer_limit_bytes = 2;
    route.body.disk_spool_disabled = 1;
    CHECK(reference_route_register(&f.app, &route, &translated) ==
          REFERENCE_INVALID);
    for (i = 0; i < 4; ++i) {
        knurl_route_config_init(&native);
        native.method = KNURL_HTTP_GET;
        native.path = files[i];
        native.handler = native_file;
        native.userdata = &f;
        native.hooks.after = after_file;
        native.hooks.userdata = &f;
        CHECK(knurl_router_route(knurl_server_router(f.app.server), &native,
                                 NULL, &error) == KNURL_OK);
    }
    memset(&route, 0, sizeof(route));
    route.method = KNURL_HTTP_HEAD;
    route.path = "/file-head";
    route.handler = file;
    route.userdata = &f;
    CHECK(reference_route_register(&f.app, &route, &translated) ==
          REFERENCE_OK);
    route.path = "/file-bodyless";
    route.method = KNURL_HTTP_GET;
    CHECK(reference_route_register(&f.app, &route, &translated) ==
          REFERENCE_OK);
    route.path = "/stop";
    CHECK(reference_route_register(&f.app, &route, &translated) ==
          REFERENCE_OK);
    CHECK(knurl_server_validate(f.app.server, &error) == KNURL_OK);
    CHECK(knurl_server_run(f.app.server, &result, &error) == KNURL_OK);
    CHECK(waitpid(f.child, &status, 0) == f.child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    CHECK(knurl_server_destroy(f.app.server, &error) == KNURL_OK);
    reference_route_cleanup(&f.app);
    CHECK(unlink(f.missing) == 0 && rmdir(f.directory) == 0);
    return 0;
}

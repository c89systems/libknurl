#define _POSIX_C_SOURCE 200809L
#include "vectis-route.h"
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "Vectis request/reply %d: %s\n", __LINE__, #t);    \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct fixture {
    struct reference_app app;
    char root[64], file[80], empty[80], missing[80];
    pid_t child;
};
static const char stamp[] = "Wed, 21 Oct 2015 07:28:00 GMT";
static reference_status body(struct reference_request *request,
                             struct reference_response *response,
                             struct fixture *f, reference_error *error)
{
    const char *mode = reference_request_param(request, "mode");
    struct reference_body_config config;
    struct reference_body_result result;
    struct reference_body_spill_config spill;
    struct reference_body_spill_result spooled;
    struct reference_mutable_bytes copied;
    struct reference_source *reader;
    struct reference_source_error input = {0, NULL};
    unsigned char buffer[16], output[4];
    char path[128];
    reference_status status;
    size_t length = !strcmp(mode, "empty") ? 0 : 3;
    memset(buffer, 'Z', sizeof(buffer));
    reference_body_config_init(&config);
    config.directory = f->root;
    config.prefix = "owned";
    if (!strcmp(mode, "copy")) {
        status = reference_request_body_copy(request, &copied, error);
        if (status != REFERENCE_OK)
            return status;
        if (copied.size != length || memcmp(copied.data, "A\0B", length))
            return REFERENCE_STATE;
        reference_mutable_bytes_cleanup(&copied);
    } else if (!strcmp(mode, "spill")) {
        spill.memory_limit_bytes = 1;
        spill.directory = f->root;
        spill.prefix = "spilled";
        status = reference_request_body_spill(request, &spill, &spooled, error);
        if (status != REFERENCE_OK)
            return status;
        if (!spooled.spooled_to_disk || spooled.path == NULL ||
            spooled.size != length || spooled.memory.data != NULL)
            return REFERENCE_STATE;
        strcpy(path, spooled.path);
        reference_body_spill_cleanup(&spooled);
        if (access(path, F_OK) || unlink(path))
            return REFERENCE_STATE;
    } else {
        config.buffer = buffer;
        config.buffer_size = sizeof(buffer);
        if (!strcmp(mode, "lower"))
            config.memory_limit_bytes = 1;
        if (!strcmp(mode, "ignore")) {
            config.buffer = NULL;
            config.buffer_size = 999;
        }
        if (!strcmp(mode, "failed")) {
            config.memory_limit_bytes = 1;
            config.directory = f->missing;
            status =
                reference_request_materialize(request, &config, &result, error);
            if (status != REFERENCE_STATE || result.kind != 0 || result.path ||
                result.memory.data || result.size)
                return REFERENCE_STATE;
        } else {
            status =
                reference_request_materialize(request, &config, &result, error);
            if (status != REFERENCE_OK)
                return status;
            if (result.size != length)
                return REFERENCE_STATE;
            if (!strcmp(mode, "lower")) {
                if (result.kind != 2 || result.path == NULL)
                    return REFERENCE_STATE;
                strcpy(path, result.path);
            } else if (result.kind != 1 || result.memory.size != length ||
                       result.owns_memory != !strcmp(mode, "ignore") ||
                       (!strcmp(mode, "ignore")
                            ? result.memory.data == buffer
                            : result.memory.data != buffer) ||
                       memcmp(result.memory.data, "A\0B", length))
                return REFERENCE_STATE;
            status = reference_body_open_reader(&result, &reader, error);
            if (status != REFERENCE_OK)
                return status;
            if (result.kind == 2) {
                reference_body_result_cleanup(&result);
                if (access(path, F_OK) || unlink(path))
                    return REFERENCE_STATE;
            }
            if (reader->read(reader, output, sizeof(output), &input) !=
                    length ||
                input.code || memcmp(output, "A\0B", length) ||
                reader->reset(reader, &input) != 0 ||
                reader->read(reader, output, sizeof(output), &input) != length)
                return REFERENCE_STATE;
            reader->close(reader);
            reference_body_result_cleanup(&result);
        }
    }
    reader = reference_request_body_reader(request);
    if (!reader || reader->reset(reader, &input) ||
        reader->read(reader, output, sizeof(output), &input) != length ||
        input.code || memcmp(output, "A\0B", length))
        return REFERENCE_STATE;
    return reference_response_text(response, 200, "text/plain", "ok", error);
}
static reference_status handler(struct reference_app *app,
                                struct reference_request *request,
                                struct reference_response *response, void *data,
                                reference_error *error)
{
    struct fixture *f = data;
    const char *path = reference_request_path(request);
    const char *parameter;
    knurl_bytes bytes;
    if (!strncmp(path, "/body/", 6))
        return body(request, response, f, error);
    if (!strncmp(path, "/ordered/", 9)) {
        parameter = reference_request_param(request, "id");
        return reference_response_text(
            response, 200, "text/plain",
            parameter != NULL ? parameter : "literal", error);
    }
    if (!strncmp(path, "/regex/", 7) || !strcmp(path, "/mask"))
        return reference_response_text(response, 200, "text/plain", "matched",
                                       error);
    if (!strcmp(path, "/stop")) {
        (void)knurl_server_request_stop(app->server, NULL);
        return reference_response_text(response, 200, "text/plain", "ok",
                                       error);
    }
    if (!strcmp(path, "/headers")) {
#define FIELD(n, v)                                                            \
    do {                                                                       \
        if (reference_response_header(response, n, v, error) != REFERENCE_OK)  \
            return error->code;                                                \
    } while (0)
        FIELD("X-Order", "first");
        FIELD("x-order", "last");
        FIELD("Set-Cookie", "one=1");
        FIELD("Set-Cookie", "two=2");
        FIELD("Connection", "staged");
        FIELD("Keep-Alive", "custom");
        FIELD("Trailer", "X-End");
        FIELD("Upgrade", "fixture");
        FIELD("Content-Length", "ignored");
        FIELD("Transfer-Encoding", "ignored");
        FIELD("Content-Type", "application/x-overridden");
        bytes.data = "A\0B";
        bytes.length = 3;
        return reference_response_bytes(
            response, 200, "application/octet-stream", bytes, error);
    }
    if (!strcmp(path, "/override"))
        FIELD("Last-Modified", "application-date");
    if (!strcmp(path, "/representation"))
        FIELD("Content-Length", "chosen-length");
#undef FIELD
    return reference_response_file(response, 200, "text/plain",
                                   !strcmp(path, "/empty")     ? f->empty
                                   : !strcmp(path, "/missing") ? f->missing
                                                               : f->file,
                                   error);
}
static int exchange(unsigned short port, const char *method, const char *path,
                    const char *since, int code, const char *payload,
                    size_t size, const char *last_modified, const char *length)
{
    struct sockaddr_in address;
    struct timeval timeout;
    char request[512], response[2048], expected[128], *end, *value;
    size_t used = 0, nrequest;
    ssize_t n;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    nrequest = (size_t)snprintf(
        request, sizeof(request),
        "%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: "
        "close\r\n%s%s%sContent-Length: %d\r\n\r\n",
        method, path, since ? "If-Modified-Since: " : "", since ? since : "",
        since ? "\r\n" : "",
        !strcmp(method, "POST") && strncmp(path, "/body/empty", 11) &&
                !strncmp(path, "/body/", 6)
            ? 3
            : 0);
    if (!strcmp(method, "POST") && strncmp(path, "/body/empty", 11) &&
        !strncmp(path, "/body/", 6)) {
        memcpy(request + nrequest, "A\0B", 3);
        nrequest += 3;
    }
    CHECK(write(fd, request, nrequest) == (ssize_t)nrequest);
    while (used < sizeof(response) - 1) {
        n = read(fd, response + used, sizeof(response) - 1 - used);
        if (n == -1 && errno == EINTR)
            continue;
        CHECK(n >= 0);
        if (!n)
            break;
        used += (size_t)n;
    }
    CHECK(close(fd) == 0);
    response[used] = 0;
    if (atoi(response + 9) != code)
        fprintf(stderr, "%s: %s\n", path, response);
    CHECK(atoi(response + 9) == code);
    end = strstr(response, "\r\n\r\n");
    CHECK(end != NULL);
    end += 4;
    CHECK(used - (size_t)(end - response) == size &&
          !memcmp(end, payload, size));
    if (last_modified) {
        (void)sprintf(expected, "%s: %s\r\n",
                      !strcmp(path, "/override") ? "Last-Modified"
                                                 : "last-modified",
                      last_modified);
        CHECK(strstr(response, expected) != NULL);
    } else
        CHECK(strstr(response, "last-modified:") == NULL);
    if (length) {
        (void)sprintf(expected, "%s: %s\r\n",
                      !strcmp(path, "/representation") ? "Content-Length"
                                                       : "content-length",
                      length);
        CHECK(strstr(response, expected) != NULL);
    }
    if (!strcmp(path, "/headers")) {
        CHECK(strstr(response, "X-Order: last\r\n") &&
              !strstr(response, "X-Order: first") &&
              !strstr(response, "x-order:"));
        CHECK(strstr(response, "Set-Cookie: one=1\r\n") &&
              strstr(response, "Set-Cookie: two=2\r\n"));
        CHECK(strstr(response, "Connection: staged\r\n") &&
              strstr(response, "Keep-Alive: custom\r\n") &&
              strstr(response, "Trailer: X-End\r\n") &&
              strstr(response, "Upgrade: fixture\r\n"));
        CHECK(!strstr(response, "ignored") &&
              strstr(response, "Content-Type: application/octet-stream\r\n"));
        value = strstr(response, "X-Order:");
        CHECK(value < strstr(response, "Set-Cookie:"));
    }
    return 0;
}
static int client(unsigned short port)
{
    const char *modes[] = {"fixed",  "copy",  "spill", "lower",
                           "ignore", "empty", "failed"};
    const char *methods[] = {"GET", "HEAD", "POST"};
    size_t i;
    char path[48];
#define GET(p, c, b, s, m, l)                                                  \
    CHECK(exchange(port, "GET", p, NULL, c, b, s, m, l) == 0)
    GET("/file", 200, "file", 4, stamp, "4");
    for (i = 0; i < 3; ++i)
        CHECK(exchange(port, methods[i], "/file", stamp, 304, "", 0, NULL,
                       "0") == 0);
    CHECK(exchange(port, "HEAD", "/file", NULL, 200, "", 0, stamp, "4") == 0);
    CHECK(exchange(port, "GET", "/file", "Thu, 22 Oct 2015 07:28:00 GMT", 200,
                   "file", 4, stamp, "4") == 0);
    CHECK(exchange(port, "GET", "/file", "invalid", 200, "file", 4, stamp,
                   "4") == 0);
    GET("/override", 200, "file", 4, stamp, "4");
    CHECK(exchange(port, "GET", "/override", stamp, 304, "", 0,
                   "application-date", "0") == 0);
    CHECK(exchange(port, "HEAD", "/representation", NULL, 200, "", 0, stamp,
                   "chosen-length") == 0);
    CHECK(exchange(port, "HEAD", "/representation", stamp, 304, "", 0, NULL,
                   "chosen-length") == 0);
    CHECK(exchange(port, "GET", "/empty", stamp, 200, "", 0, NULL, "0") == 0);
    GET("/missing", 404, "", 0, NULL, "0");
    GET("/headers", 200, "A\0B", 3, NULL, "3");
    GET("/ordered/fixed", 200, "fixed", 5, NULL, "5");
    GET("/ordered/a%20b", 200, "a b", 3, NULL, "3");
    GET("/regex/word", 200, "matched", 7, NULL, "7");
    GET("/mask", 404, "", 0, NULL, "0");
    CHECK(exchange(port, "PUT", "/mask", NULL, 200, "matched", 7, NULL, "7") ==
          0);
    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        (void)sprintf(path, "/body/%s", modes[i]);
        CHECK(exchange(port, "POST", path, NULL, 200, "ok", 2, NULL, "2") == 0);
    }
    GET("/stop", 200, "ok", 2, NULL, "2");
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
        _exit(client(listener.port));
    return f->child > 0 ? KNURL_OK : KNURL_IO;
}
int main(void)
{
    struct fixture f;
    struct reference_route_config route;
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_run_result result;
    knurl_error error;
    reference_error translated;
    struct timespec times[2];
    const char *paths[] = {"/file",        "/empty",          "/missing",
                           "/override",    "/representation", "/headers",
                           "/ordered/:id", "/ordered/fixed",  "^/regex/[a-z]+$",
                           "/mask",        "/body/:mode",     "/stop"};
    size_t i, count;
    int fd, status;
    memset(&f, 0, sizeof(f));
    strcpy(f.root, "vectis-request-reply-XXXXXX");
    CHECK(mkdtemp(f.root));
    (void)sprintf(f.file, "%s/file", f.root);
    (void)sprintf(f.empty, "%s/empty", f.root);
    (void)sprintf(f.missing, "%s/missing", f.root);
    fd = open(f.file, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0 && write(fd, "file", 4) == 4 && close(fd) == 0);
    fd = open(f.empty, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    times[0].tv_sec = times[1].tv_sec = 1445412480;
    times[0].tv_nsec = times[1].tv_nsec = 0;
    CHECK(utimensat(AT_FDCWD, f.file, times, 0) == 0);
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    reference_route_callbacks(&f.app, &config.callbacks);
    config.callbacks.on_parent_ready = ready;
    CHECK(knurl_server_new(&config, &f.app.server, &error) == KNURL_OK);
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        memset(&route, 0, sizeof(route));
        route.path = paths[i];
        route.handler = handler;
        route.userdata = &f;
        route.method = KNURL_HTTP_ANY;
        route.methods =
            i == 9 ? KNURL_HTTP_METHODS_PUT | KNURL_HTTP_METHODS_POST : 0;
        route.path_kind = i == 6 || i == 10 ? KNURL_PATH_PARAMS
                          : i == 8          ? KNURL_PATH_REGEX
                                            : KNURL_PATH_LITERAL;
        if (i == 10) {
            route.body.mode = KNURL_BODY_BUFFERED;
            route.body.max_bytes = 16;
            route.body.memory_buffer_limit_bytes = 2;
        }
        CHECK(reference_route_register(&f.app, &route, &translated) ==
              REFERENCE_OK);
    }
    count = knurl_router_route_count(knurl_server_router(f.app.server));
    CHECK(reference_route_register(&f.app, &route, &translated) ==
          REFERENCE_CONFLICT);
    route.path = "^([";
    route.path_kind = KNURL_PATH_REGEX;
    CHECK(reference_route_register(&f.app, &route, &translated) ==
          REFERENCE_INVALID);
    CHECK(knurl_router_route_count(knurl_server_router(f.app.server)) == count);
    CHECK(knurl_server_run(f.app.server, &result, &error) == KNURL_OK);
    CHECK(waitpid(f.child, &status, 0) == f.child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    CHECK(knurl_server_destroy(f.app.server, &error) == KNURL_OK);
    reference_route_cleanup(&f.app);
    CHECK(unlink(f.file) == 0 && unlink(f.empty) == 0 && rmdir(f.root) == 0);
    return 0;
}

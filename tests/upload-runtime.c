#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "upload %d: %s\n", __LINE__, #t);                  \
            return 1;                                                          \
        }                                                                      \
    } while (0)
#define VERIFY(f, t)                                                           \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "upload callback %d: %s\n", __LINE__, #t);         \
            (f)->failed = 1;                                                   \
        }                                                                      \
    } while (0)
struct fixture {
    pid_t child;
    pthread_t owner;
    int failed, opened, closed, finished, aborted, resumed, after;
};
struct upload {
    struct fixture *f;
    rotmos_request *request;
    rotmos_timer *timer;
    size_t count;
    int calls;
};
static const char *name(rotmos_request *request)
{
    rotmos_bytes value;
    return rotmos_request_path_param(request, "id", &value, NULL) == ROTMOS_OK
               ? value.data
               : "";
}
static rotmos_status auth(rotmos_request *request, rotmos_reply *reply,
                          int *stop, void *data, rotmos_error *error)
{
    struct fixture *f = data;
    VERIFY(f, pthread_equal(f->owner, pthread_self()));
    if (!strcmp(name(request), "deny")) {
        *stop = 1;
        return rotmos_reply_text(reply, 403, "text/plain", "denied", error);
    }
    return ROTMOS_OK;
}
static rotmos_status before(rotmos_request *request, rotmos_reply *reply,
                            int *stop, void *data, rotmos_error *error)
{
    (void)stop;
    rotmos_request_set_userdata(request, data);
    return rotmos_reply_header_add(reply, "X-Before", "yes", error);
}
static rotmos_status after(rotmos_request *request, rotmos_reply *reply,
                           int *stop, void *data, rotmos_error *error)
{
    struct fixture *f = data;
    (void)stop;
    VERIFY(f, rotmos_reply_is_selected(reply));
    ++f->after;
    if (!strcmp(name(request), "replace"))
        return rotmos_reply_text(reply, 202, "text/plain", "replaced", error);
    return ROTMOS_OK;
}
static void resume(rotmos_timer *timer, void *data)
{
    struct upload *s = data;
    (void)timer;
    s->timer = NULL;
    VERIFY(s->f, s->calls == 1);
    ++s->f->resumed;
    VERIFY(s->f, rotmos_request_body_resume(s->request, NULL) == ROTMOS_OK);
}
static rotmos_status open_upload(rotmos_request *request, void *data,
                                 void **state, rotmos_error *error)
{
    struct fixture *f = data;
    struct upload *s;
    (void)error;
    VERIFY(f, rotmos_request_userdata(request) == f &&
                  pthread_equal(f->owner, pthread_self()));
    if (!strcmp(name(request), "open-fail"))
        return ROTMOS_IO;
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return ROTMOS_NOMEM;
    s->f = f;
    s->request = request;
    *state = s;
    ++f->opened;
    return ROTMOS_OK;
}
static rotmos_status write_upload(rotmos_request *request, const void *data,
                                  size_t length, size_t *consumed, void *state,
                                  void *userdata, rotmos_error *error)
{
    struct upload *s = state;
    const unsigned char *bytes = data;
    size_t i, n;
    (void)userdata;
    VERIFY(s->f,
           pthread_equal(s->f->owner, pthread_self()) && request == s->request);
    ++s->calls;
    *consumed = 0;
    if (!strcmp(name(request), "write-fail"))
        return ROTMOS_IO;
    if (!strcmp(name(request), "zero"))
        return ROTMOS_OK;
    if (s->calls == 1 && !strcmp(name(request), "paused")) {
        if (rotmos_loop_timer(rotmos_request_loop(request), 5, 0, resume, NULL,
                              s, &s->timer, error) != ROTMOS_OK)
            return ROTMOS_IO;
        return ROTMOS_AGAIN;
    }
    n = length > 3 ? 3 : length;
    for (i = 0; i < n; ++i)
        VERIFY(s->f, bytes[i] == 'a' + (s->count + i) % 26);
    s->count += n;
    *consumed = n;
    if (n < length)
        VERIFY(s->f, rotmos_request_body_resume(request, NULL) == ROTMOS_OK);
    return ROTMOS_OK;
}
static rotmos_status finish_upload(rotmos_request *request, rotmos_reply *reply,
                                   void *state, void *data, rotmos_error *error)
{
    struct upload *s = state;
    struct fixture *f = data;
    char text[32];
    ++f->finished;
    VERIFY(f, s->request == request && pthread_equal(f->owner, pthread_self()));
    if (!strcmp(name(request), "finish-fail"))
        return ROTMOS_IO;
    if (!strcmp(name(request), "no-reply"))
        return ROTMOS_OK;
    snprintf(text, sizeof(text), "%lu", (unsigned long)s->count);
    return rotmos_reply_text(reply, 201, "text/plain", text, error);
}
static void close_upload(rotmos_request *request, rotmos_status outcome,
                         void *state, void *data)
{
    struct fixture *f = data;
    struct upload *s = state;
    ++f->closed;
    VERIFY(f, s->request == request &&
                  rotmos_request_path(request).length > 0 &&
                  rotmos_request_userdata(request) == f &&
                  pthread_equal(f->owner, pthread_self()));
    if (!strcmp(name(request), "abort")) {
        VERIFY(f, outcome != ROTMOS_OK && s->count > 0 && s->count < 19);
        ++f->aborted;
    } else if (!strcmp(name(request), "write-fail") ||
               !strcmp(name(request), "finish-fail") ||
               !strcmp(name(request), "zero") ||
               !strcmp(name(request), "no-reply"))
        VERIFY(f, outcome != ROTMOS_OK);
    else
        VERIFY(f, outcome == ROTMOS_OK);
    if (s->timer != NULL)
        rotmos_timer_cancel(s->timer);
    free(s);
}
static rotmos_status stateless_write(rotmos_request *request, const void *bytes,
                                     size_t length, size_t *consumed,
                                     void *state, void *data,
                                     rotmos_error *error)
{
    (void)request;
    (void)bytes;
    (void)data;
    (void)error;
    (void)state;
    *consumed = length;
    return ROTMOS_OK;
}
static rotmos_status stateless_finish(rotmos_request *request,
                                      rotmos_reply *reply, void *state,
                                      void *data, rotmos_error *error)
{
    (void)request;
    (void)state;
    (void)data;
    return rotmos_reply_text(reply, 200, "text/plain", "stateless", error);
}
static rotmos_status stop_server(rotmos_request *request, rotmos_reply *reply,
                                 void *data, rotmos_error *error)
{
    struct fixture *f = data;
    (void)rotmos_server_request_stop(rotmos_request_server(request), NULL);
    return rotmos_reply_text(reply, 200, "text/plain",
                             f->failed ? "failed" : "stopped", error);
}
static int send_request(unsigned short port, const char *path, int length,
                        const char *body)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    struct timeval timeout;
    char request[512];
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    if (length < 0)
        snprintf(
            request, sizeof(request),
            "POST %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
            path);
    else
        snprintf(request, sizeof(request),
                 "POST %s HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                 "%d\r\nConnection: close\r\n\r\n%s",
                 path, length, body);
    if (write(fd, request, strlen(request)) != (ssize_t)strlen(request)) {
        close(fd);
        return -1;
    }
    return fd;
}
static int exchange(unsigned short port, const char *path, int length,
                    const char *body, int expected, const char *result,
                    int hook)
{
    int fd = send_request(port, path, length, body);
    ssize_t n;
    size_t used = 0;
    char response[2048], status[32], *payload;
    CHECK(fd >= 0);
    while (used < sizeof(response) - 1 &&
           (n = read(fd, response + used, sizeof(response) - 1 - used)) > 0)
        used += (size_t)n;
    close(fd);
    response[used] = 0;
    snprintf(status, sizeof(status), "HTTP/1.1 %d ", expected);
    if (strncmp(response, status, strlen(status)) != 0)
        fprintf(stderr, "%s: %s\n", path, response);
    CHECK(strncmp(response, status, strlen(status)) == 0);
    payload = strstr(response, "\r\n\r\n");
    CHECK(payload != NULL && strcmp(payload + 4, result) == 0);
    if (hook)
        CHECK(strstr(response, "X-Before: yes\r\n") != NULL ||
              strstr(response, "x-before: yes\r\n") != NULL);
    return 0;
}
static int client(unsigned short port)
{
    int fd;
    struct linger reset;
    CHECK(exchange(port, "/api/upload/good", 19, "abcdefghijklmnopqrs", 201,
                   "19", 1) == 0);
    CHECK(exchange(port, "/api/upload/paused", 19, "abcdefghijklmnopqrs", 201,
                   "19", 1) == 0);
    CHECK(exchange(port, "/api/upload/replace", 3, "abc", 202, "replaced", 1) ==
          0);
    CHECK(exchange(port, "/api/upload/empty", 0, "", 201, "0", 1) == 0);
    CHECK(exchange(port, "/api/upload/deny", 3, "abc", 403, "denied", 0) == 0);
    CHECK(exchange(port, "/api/upload/good", 33, "", 413, "", 0) == 0);
    CHECK(exchange(port, "/api/upload/good", -1, "", 411, "", 0) == 0);
    CHECK(exchange(port, "/api/upload/open-fail", 3, "abc", 500, "", 0) == 0);
    CHECK(exchange(port, "/api/upload/write-fail", 3, "abc", 500, "", 0) == 0);
    CHECK(exchange(port, "/api/upload/finish-fail", 3, "abc", 500, "", 0) == 0);
    CHECK(exchange(port, "/api/upload/no-reply", 3, "abc", 500, "", 0) == 0);
    CHECK(exchange(port, "/api/upload/zero", 3, "abc", 500, "", 0) == 0);
    CHECK(exchange(port, "/stateless", 3, "abc", 200, "stateless", 0) == 0);
    fd = send_request(port, "/api/upload/abort", 19, "abc");
    CHECK(fd >= 0);
    (void)poll(NULL, 0, 10);
    reset.l_onoff = 1;
    reset.l_linger = 0;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
    close(fd);
    CHECK(exchange(port, "/stop", 0, "", 200, "stopped", 0) == 0);
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
    if (f->failed || f->opened != 9 || f->closed != f->opened ||
        f->finished != 6 || f->resumed != 1 || f->aborted != 1 ||
        f->after != 4) {
        fprintf(stderr,
                "upload counts: failed=%d opened=%d closed=%d finished=%d "
                "resumed=%d aborted=%d after=%d\n",
                f->failed, f->opened, f->closed, f->finished, f->resumed,
                f->aborted, f->after);
        return 1;
    }
    return 0;
}
int main(void)
{
    struct fixture f;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_upload_stream_config upload;
    rotmos_route_group_config group_config;
    rotmos_route_group *group;
    rotmos_route_config stop;
    rotmos_server *server;
    rotmos_error error;
    rotmos_run_result result;
    int child;
    rotmos_status status;
    memset(&f, 0, sizeof(f));
    f.child = -1;
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.http.backend_quiet = config.workers.affinity_disabled = 1;
    config.io.chunk_bytes = 5;
    config.io.input_queue_bytes = 32;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_start = started;
    config.callbacks.on_worker_exit = exited;
    config.callbacks.userdata = &f;
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    rotmos_route_group_config_init(&group_config);
    group_config.prefix = "/api";
    group_config.hooks.before = before;
    group_config.hooks.after = after;
    group_config.hooks.userdata = &f;
    CHECK(rotmos_router_group(rotmos_server_router(server), &group_config,
                              &group, &error) == ROTMOS_OK);
    rotmos_upload_stream_config_init(&upload);
    upload.method = ROTMOS_HTTP_POST;
    upload.path = "/upload/:id";
    upload.path_kind = ROTMOS_PATH_PARAMS;
    upload.body.max_bytes.low = 32;
    upload.open = open_upload;
    upload.write = write_upload;
    upload.finish = finish_upload;
    upload.close = close_upload;
    upload.hooks.auth = auth;
    upload.hooks.userdata = upload.userdata = &f;
    CHECK(rotmos_route_group_upload_stream(group, &upload, NULL, &error) ==
          ROTMOS_OK);
    CHECK(rotmos_route_group_upload_stream(group, &upload, NULL, &error) ==
          ROTMOS_CONFLICT);
    upload.path = "/bad";
    upload.write = NULL;
    CHECK(rotmos_router_upload_stream(rotmos_server_router(server), &upload,
                                      NULL, &error) == ROTMOS_INVALID);
    rotmos_upload_stream_config_init(&upload);
    upload.path = "/stateless";
    upload.write = stateless_write;
    upload.finish = stateless_finish;
    CHECK(rotmos_router_upload_stream(rotmos_server_router(server), &upload,
                                      NULL, &error) == ROTMOS_OK);
    CHECK(f.opened == 0 && f.closed == 0);
    rotmos_route_config_init(&stop);
    stop.path = "/stop";
    stop.handler = stop_server;
    stop.userdata = &f;
    CHECK(rotmos_router_route(rotmos_server_router(server), &stop, NULL,
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

#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
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
            fprintf(stderr, "source %d: %s\n", __LINE__, #t);                  \
            return 1;                                                          \
        }                                                                      \
    } while (0)
#define VERIFY(f, t)                                                           \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "source callback %d: %s\n", __LINE__, #t);         \
            __atomic_store_n(&(f)->failed, 1, __ATOMIC_RELEASE);               \
        }                                                                      \
    } while (0)
enum {
    NORMAL,
    BLOCKED,
    EARLY,
    PAUSED,
    CANCEL,
    ZERO,
    OVER,
    SHORT,
    EOF_BYTES,
    ERROR,
    REPLACE,
    SUPPRESSED,
    OWNER_EARLY,
    PRE_EOF
};
struct fixture {
    pthread_t owner;
    pid_t child;
    knurl_source *paused;
    unsigned int failed, released, blocked, cancelled, started;
    int opened, closed, native_reads, source_wakes;
};
struct context {
    struct fixture *f;
    knurl_source *source;
    knurl_waker *waker;
    unsigned int resumed;
    int mode, calls;
};
static void ready_source(knurl_loop *loop, void *data)
{
    struct context *c = data;
    (void)loop;
    VERIFY(c->f, pthread_equal(pthread_self(), c->f->owner));
    c->f->source_wakes++;
    VERIFY(c->f, c->source->resume(c->source, NULL) == KNURL_OK);
    __atomic_store_n(&c->resumed, 1u, __ATOMIC_RELEASE);
}
static knurl_status read_source(void *buffer, size_t capacity, size_t *count,
                                void *data, knurl_error *error)
{
    struct context *c = data;
    struct fixture *f = c->f;
    (void)error;
    ++c->calls;
    *count = 0;
    if (c->mode == OWNER_EARLY || c->mode == PRE_EOF)
        VERIFY(f, pthread_equal(pthread_self(), f->owner));
    else
        VERIFY(f, !pthread_equal(pthread_self(), f->owner));
    if (c->mode == CANCEL) {
        __atomic_add_fetch(&f->started, 1u, __ATOMIC_RELEASE);
        while (!c->source->cancelled(c->source))
            (void)poll(NULL, 0, 1);
        __atomic_add_fetch(&f->cancelled, 1u, __ATOMIC_RELEASE);
        return KNURL_CANCELLED;
    }
    if (c->mode == BLOCKED && c->calls == 1) {
        __atomic_add_fetch(&f->blocked, 1u, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&f->released, __ATOMIC_ACQUIRE)) {
            if (c->source->cancelled(c->source))
                return KNURL_CANCELLED;
            (void)poll(NULL, 0, 1);
        }
    }
    if ((c->mode == EARLY || c->mode == OWNER_EARLY) && c->calls == 1) {
        if (c->mode == EARLY) {
            VERIFY(f, c->waker->signal(c->waker, NULL) == KNURL_OK);
            /* Deliver readiness before this native read acknowledges AGAIN. */
            while (!__atomic_load_n(&c->resumed, __ATOMIC_ACQUIRE) &&
                   !c->source->cancelled(c->source))
                (void)poll(NULL, 0, 1);
        } else
            VERIFY(f, c->source->resume(c->source, NULL) == KNURL_OK);
        return KNURL_AGAIN;
    }
    if (c->mode == PAUSED && c->calls == 1)
        return KNURL_AGAIN;
    if (c->mode == ZERO)
        return KNURL_OK;
    if (c->mode == OVER) {
        *count = capacity + 1;
        return KNURL_OK;
    }
    if (c->mode == EOF_BYTES) {
        *count = 1;
        return KNURL_EOF;
    }
    if (c->mode == ERROR)
        return KNURL_IO;
    if (c->mode == SHORT || c->mode == PRE_EOF)
        return KNURL_EOF;
    if (c->calls >
        ((c->mode == EARLY || c->mode == PAUSED || c->mode == OWNER_EARLY)
             ? 2
             : (c->mode == NORMAL ? 3 : 1)))
        return KNURL_EOF;
    VERIFY(f, capacity >= 3 && capacity <= 257);
    memcpy(buffer, "abc", 3);
    *count = 3;
    return KNURL_OK;
}
static void free_context(void *data) { free(data); }
static void close_source(void *data, knurl_status outcome)
{
    struct context *c = data;
    struct fixture *f = c->f;
    VERIFY(f, pthread_equal(pthread_self(), f->owner));
    ++f->closed;
    if (c->mode == PAUSED)
        f->paused = NULL;
    if (c->mode == REPLACE || c->mode == SUPPRESSED)
        VERIFY(f, c->calls == 0 && outcome == KNURL_CANCELLED);
    else if (c->mode == CANCEL)
        VERIFY(f, outcome != KNURL_OK && c->calls == 1);
    else if (c->mode == ZERO || c->mode == OVER || c->mode == EOF_BYTES)
        VERIFY(f, outcome == KNURL_INVALID);
    else if (c->mode == SHORT || c->mode == ERROR)
        VERIFY(f, outcome == KNURL_IO);
    else
        VERIFY(f, outcome == KNURL_OK);
    if (c->mode != OWNER_EARLY && c->mode != PRE_EOF)
        f->native_reads += c->calls;
    if (c->waker != NULL)
        VERIFY(f, c->waker->destroy(c->waker, NULL) == KNURL_OK);
    else
        free(c);
}
static knurl_status route(knurl_request *request, knurl_reply *reply,
                          void *data, knurl_error *error)
{
    struct fixture *f = data;
    struct context *c;
    knurl_source_config config;
    knurl_status status;
    const char *path = request->path(request).data;
    char byte;
    size_t count;
    if (!strcmp(path, "/release")) {
        __atomic_store_n(&f->released, 1u, __ATOMIC_RELEASE);
        return reply->text(reply, 200, "text/plain", "released", error);
    }
    if (!strcmp(path, "/probe")) {
        return reply->text(reply, 200, "text/plain",
                           __atomic_load_n(&f->blocked, __ATOMIC_ACQUIRE) >= 2
                               ? "running"
                               : "waiting",
                           error);
    }
    if (!strcmp(path, "/wake")) {
        VERIFY(f, f->paused != NULL);
        if (f->paused != NULL)
            VERIFY(f, f->paused->resume(f->paused, NULL) == KNURL_OK);
        return reply->text(reply, 200, "text/plain", "awake", error);
    }
    if (!strcmp(path, "/cancelled"))
        return reply->text(
            reply, 200, "text/plain",
            __atomic_load_n(&f->cancelled, __ATOMIC_ACQUIRE) ? "yes" : "no",
            error);
    if (!strcmp(path, "/first-started") || !strcmp(path, "/started"))
        return reply->text(reply, 200, "text/plain",
                           __atomic_load_n(&f->started, __ATOMIC_ACQUIRE) >=
                                   (!strcmp(path, "/first-started") ? 1u : 2u)
                               ? "yes"
                               : "no",
                           error);
    if (!strcmp(path, "/stop")) {
        (void)knurl_server_request_stop(request->server(request), NULL);
        return reply->text(reply, 200, "text/plain",
                           __atomic_load_n(&f->failed, __ATOMIC_ACQUIRE)
                               ? "failed"
                               : "stopped",
                           error);
    }
    c = calloc(1, sizeof(*c));
    if (c == NULL)
        return KNURL_NOMEM;
    c->f = f;
    c->mode = !strcmp(path, "/blocked")     ? BLOCKED
              : !strcmp(path, "/early")     ? EARLY
              : !strcmp(path, "/paused")    ? PAUSED
              : !strcmp(path, "/cancel")    ? CANCEL
              : !strcmp(path, "/zero")      ? ZERO
              : !strcmp(path, "/over")      ? OVER
              : !strcmp(path, "/short")     ? SHORT
              : !strcmp(path, "/eof-bytes") ? EOF_BYTES
              : !strcmp(path, "/error")     ? ERROR
              : !strcmp(path, "/replace")   ? REPLACE
              : !strcmp(path, "/head") || !strcmp(path, "/204") ||
                      !strcmp(path, "/304")
                  ? SUPPRESSED
              : !strcmp(path, "/owner-early") ? OWNER_EARLY
              : !strcmp(path, "/pre-eof")     ? PRE_EOF
                                              : NORMAL;
    knurl_source_config_init(&config);
    config.read = read_source;
    config.close = close_source;
    config.userdata = c;
    config.execution = c->mode == OWNER_EARLY ? KNURL_SOURCE_OWNER_LOOP
                                              : KNURL_SOURCE_NATIVE_READER;
    if (c->mode == NORMAL || c->mode == BLOCKED || c->mode == SHORT ||
        c->mode == SUPPRESSED || c->mode == PRE_EOF) {
        config.has_length = 1;
        config.length.low = c->mode == NORMAL ? 9 : c->mode == PRE_EOF ? 0 : 3;
    }
    status = knurl_source_new(&config, &c->source, error);
    if (status != KNURL_OK) {
        free(c);
        return status;
    }
    ++f->opened;
    VERIFY(f, c->source->resume(c->source, NULL) == KNURL_STATE);
    if (c->mode == EARLY) {
        status = knurl_waker_new(request->loop(request), ready_source,
                                 free_context, c, &c->waker, error);
        if (status != KNURL_OK) {
            (void)c->source->destroy(c->source, NULL);
            return status;
        }
    }
    if (c->mode == PRE_EOF)
        VERIFY(f, c->source->read(c->source, &byte, 1, &count, error) ==
                          KNURL_EOF &&
                      count == 0);
    if (c->mode == PAUSED)
        f->paused = c->source;
    status = reply->source_take(reply,
                                !strcmp(path, "/204")   ? 204
                                : !strcmp(path, "/304") ? 304
                                                        : 200,
                                "text/plain", c->source, error);
    if (status != KNURL_OK) {
        (void)c->source->destroy(c->source, NULL);
        return status;
    }
    if (c->mode == REPLACE)
        return reply->text(reply, 200, "text/plain", "replaced", error);
    return KNURL_OK;
}
static int connect_request(unsigned short port, const char *path, int head)
{
    struct sockaddr_in address;
    struct timeval timeout;
    char request[256];
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    snprintf(request, sizeof(request),
             "%s %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
             head ? "HEAD" : "GET", path);
    if (write(fd, request, strlen(request)) != (ssize_t)strlen(request)) {
        close(fd);
        return -1;
    }
    return fd;
}
static int receive(int fd, int code, const char *body, int failure)
{
    char bytes[2048], status[32], *payload;
    size_t n = 0;
    ssize_t got;
    while (n < sizeof(bytes) - 1 &&
           (got = read(fd, bytes + n, sizeof(bytes) - 1 - n)) > 0)
        n += (size_t)got;
    close(fd);
    bytes[n] = 0;
    if (failure) {
        CHECK(strncmp(bytes, "HTTP/1.1 200 ", 13) == 0 ||
              strncmp(bytes, "HTTP/1.1 500 ", 13) == 0);
        CHECK(strstr(bytes, "\r\n\r\n") != NULL);
        CHECK(strstr(bytes, "abc") == NULL);
        CHECK(strstr(bytes, "0\r\n\r\n") == NULL ||
              strstr(bytes, "HTTP/1.1 500 ") != NULL);
        return 0;
    }
    snprintf(status, sizeof(status), "HTTP/1.1 %d ", code);
    CHECK(strncmp(bytes, status, strlen(status)) == 0);
    payload = strstr(bytes, "\r\n\r\n");
    CHECK(payload != NULL);
    CHECK(strcmp(payload + 4, body) == 0);
    return 0;
}
static int exchange(unsigned short port, const char *path, int code,
                    const char *body)
{
    int fd = connect_request(port, path, 0);
    CHECK(fd >= 0);
    return receive(fd, code, body, 0);
}
static int await_yes(unsigned short port, const char *path)
{
    char bytes[1024];
    int i, fd;
    ssize_t n;
    size_t used;
    for (i = 0; i < 100; ++i) {
        fd = connect_request(port, path, 0);
        CHECK(fd >= 0);
        used = 0;
        while (used < sizeof(bytes) - 1 &&
               (n = read(fd, bytes + used, sizeof(bytes) - 1 - used)) > 0)
            used += (size_t)n;
        close(fd);
        bytes[used] = 0;
        if (strstr(bytes, "\r\n\r\nyes") || strstr(bytes, "\r\n\r\nrunning"))
            return 0;
        (void)poll(NULL, 0, 1);
    }
    CHECK(0);
    return 1;
}
static int client(unsigned short port)
{
    int queued[36], i, fd;
    struct linger reset;
    const char *bad[] = {"/zero", "/over", "/short", "/eof-bytes", "/error"};
    CHECK(exchange(port, "/normal", 200, "abcabcabc") == 0);
    CHECK(exchange(port, "/early", 200, "3\r\nabc\r\n0\r\n\r\n") == 0);
    CHECK(exchange(port, "/owner-early", 200, "3\r\nabc\r\n0\r\n\r\n") == 0);
    fd = connect_request(port, "/paused", 0);
    CHECK(fd >= 0);
    (void)poll(NULL, 0, 25);
    CHECK(exchange(port, "/wake", 200, "awake") == 0);
    CHECK(receive(fd, 200, "3\r\nabc\r\n0\r\n\r\n", 0) == 0);
    CHECK(exchange(port, "/replace", 200, "replaced") == 0);
    fd = connect_request(port, "/head", 1);
    CHECK(fd >= 0);
    CHECK(receive(fd, 200, "", 0) == 0);
    CHECK(exchange(port, "/204", 204, "") == 0);
    CHECK(exchange(port, "/304", 304, "") == 0);
    CHECK(exchange(port, "/pre-eof", 200, "") == 0);
    for (i = 0; i < 5; ++i) {
        fd = connect_request(port, bad[i], 0);
        CHECK(fd >= 0);
        CHECK(receive(fd, 0, "", 1) == 0);
    }
    for (i = 0; i < 36; ++i) {
        queued[i] = connect_request(port, "/blocked", 0);
        CHECK(queued[i] >= 0);
    }
    CHECK(await_yes(port, "/probe") == 0);
    CHECK(exchange(port, "/release", 200, "released") == 0);
    for (i = 0; i < 36; ++i)
        CHECK(receive(queued[i], 200, "abc", 0) == 0);
    fd = connect_request(port, "/cancel", 0);
    CHECK(fd >= 0);
    CHECK(await_yes(port, "/first-started") == 0);
    reset.l_onoff = 1;
    reset.l_linger = 0;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
    close(fd);
    CHECK(await_yes(port, "/cancelled") == 0);
    fd = connect_request(port, "/cancel", 0);
    CHECK(fd >= 0);
    CHECK(await_yes(port, "/started") == 0);
    CHECK(exchange(port, "/stop", 200, "stopped") == 0);
    close(fd);
    return 0;
}
static knurl_status worker_start(knurl_server *server,
                                 const knurl_process_context *process,
                                 void *data, knurl_error *error)
{
    struct fixture *f = data;
    (void)server;
    (void)process;
    (void)error;
    f->owner = pthread_self();
    return KNURL_OK;
}
static int worker_exit(knurl_server *server,
                       const knurl_process_context *process, void *data)
{
    struct fixture *f = data;
    (void)server;
    (void)process;
    return __atomic_load_n(&f->failed, __ATOMIC_ACQUIRE) ||
           f->opened != f->closed || f->native_reads < 10 ||
           f->source_wakes != 1 ||
           __atomic_load_n(&f->cancelled, __ATOMIC_ACQUIRE) != 2;
}
static knurl_status parent_ready(knurl_server *server,
                                 const knurl_process_context *process,
                                 void *data, knurl_error *error)
{
    struct fixture *f = data;
    knurl_bound_listener listener;
    int status;
    (void)process;
    if (server->listener_at(server, 0, &listener, error) != KNURL_OK)
        return KNURL_STATE;
    if (server->set_serving(server, 1, error) != KNURL_OK)
        return KNURL_STATE;
    f->child = fork();
    if (f->child < 0)
        return KNURL_IO;
    if (f->child == 0) {
        signal(SIGTERM, SIG_DFL);
        status = client(listener.port);
        if (status)
            kill(getppid(), SIGTERM);
        _exit(status);
    }
    return KNURL_OK;
}
int main(void)
{
    struct fixture f;
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_route_config handler;
    knurl_server *server;
    knurl_run_result result;
    knurl_error error;
    knurl_status status;
    int child;
    memset(&f, 0, sizeof(f));
    f.child = -1;
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.workers.shutdown_timeout_ms = 500;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    config.io.chunk_bytes = 257;
    config.io.output_queue_bytes = 1024;
    config.timeouts.response_write_idle.milliseconds = 5;
    config.callbacks.on_worker_start = worker_start;
    config.callbacks.on_worker_exit = worker_exit;
    config.callbacks.on_parent_ready = parent_ready;
    config.callbacks.userdata = &f;
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    knurl_route_config_init(&handler);
    handler.path = "^/.*$";
    handler.path_kind = KNURL_PATH_REGEX;
    handler.handler = route;
    handler.userdata = &f;
    CHECK(knurl_router_route(server->router(server), &handler, NULL, &error) ==
          KNURL_OK);
    status = server->run(server, &result, &error);
    if (status != KNURL_OK)
        fprintf(stderr, "%s: %s\n", error.operation, error.message);
    CHECK(waitpid(f.child, &child, 0) == f.child && WIFEXITED(child) &&
          WEXITSTATUS(child) == 0);
    CHECK(status == KNURL_OK && !result.escalated);
    CHECK(server->destroy(server, &error) == KNURL_OK);
    return 0;
}

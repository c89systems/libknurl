#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
#include "acl-wire.h"
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "resume line %d: %s\n", __LINE__, #x);             \
            exit(1);                                                           \
        }                                                                      \
    } while (0)
struct fixture {
    int gate[2], notes[2], phase, fast, closed;
    pid_t client;
    knurl_request *pending;
    knurl_reply *reply;
    knurl_watch *watch;
};
static char byte(int fd)
{
    struct pollfd p;
    char value = 0;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    CHECK(poll(&p, 1, 3000) == 1 && read(fd, &value, 1) == 1);
    return value;
}
static void ready_watch(knurl_watch *watch, unsigned int events, void *data)
{
    struct fixture *f = data;
    knurl_error error;
    int phase = f->phase;
    (void)watch;
    (void)events;
    CHECK(byte(f->gate[0]) == 'g');
    CHECK(f->fast == f->phase);
    if (phase == 3)
        CHECK(knurl_reply_header_set(f->reply, "X-Resumed", "yes", NULL) ==
              KNURL_OK);
    CHECK(f->pending->resume(f->pending, KNURL_OK, 0, &error) == KNURL_OK);
    if (phase == 3)
        CHECK(f->pending->resume(f->pending, KNURL_OK, 0, &error) ==
              KNURL_STATE);
}
static knurl_status suspend(knurl_request *r, knurl_reply *reply, void *data,
                            knurl_error *error)
{
    struct fixture *f = data;
    f->pending = r;
    f->reply = reply;
    ++f->phase;
    if (f->watch == NULL)
        CHECK(knurl_loop_watch(knurl_request_loop(r), f->gate[0],
                               KNURL_READABLE, ready_watch, NULL, f, &f->watch,
                               error) == KNURL_OK);
    CHECK(write(f->notes[1], "p", 1) == 1);
    return KNURL_AGAIN;
}
static knurl_status before(knurl_request *r, knurl_reply *reply, int *finish,
                           void *data, knurl_error *error)
{
    (void)finish;
    return suspend(r, reply, data, error);
}
static knurl_status handler(knurl_request *r, knurl_reply *reply, void *data,
                            knurl_error *error)
{
    CHECK(knurl_reply_text(reply, 200, "text/plain", "resumed", error) ==
          KNURL_OK);
    return suspend(r, reply, data, error);
}
static knurl_status after(knurl_request *r, knurl_reply *reply, int *finish,
                          void *data, knurl_error *error)
{
    (void)finish;
    return suspend(r, reply, data, error);
}
static knurl_status fast(knurl_request *r, knurl_reply *reply, void *data,
                         knurl_error *error)
{
    struct fixture *f = data;
    ++f->fast;
    CHECK(knurl_request_resume(r, KNURL_OK, 0, error) == KNURL_STATE);
    return knurl_reply_text(reply, 200, "text/plain", "fast", error);
}
static knurl_status stop(knurl_request *r, knurl_reply *reply, void *data,
                         knurl_error *error)
{
    struct fixture *f = data;
    CHECK(f->phase == 3 && f->fast == 3 && f->closed == 1);
    CHECK(knurl_server_request_stop(knurl_request_server(r), error) ==
          KNURL_OK);
    return knurl_reply_text(reply, 200, "text/plain", "stop", error);
}
static void closed(knurl_server *s, knurl_request *r,
                   const knurl_request_outcome *out, void *data)
{
    struct fixture *f = data;
    (void)s;
    (void)out;
    if (r == f->pending) {
        ++f->closed;
        knurl_watch_cancel(f->watch);
        f->watch = NULL;
        f->pending = NULL;
    }
}
static knurl_status ready(knurl_server *s, const knurl_process_context *process,
                          void *data, knurl_error *error)
{
    struct fixture *f = data;
    knurl_bound_listener info;
    struct sockaddr_in addr;
    char buffer[1024];
    const char *request =
        "GET /slow HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ssize_t count;
    size_t used;
    int fd, i;
    (void)process;
    CHECK(knurl_server_listener_at(s, 0, &info, error) == KNURL_OK);
    CHECK(knurl_server_set_serving(s, 1, error) == KNURL_OK);
    f->client = fork();
    CHECK(f->client >= 0);
    if (f->client != 0)
        return KNURL_OK;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(info.port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    CHECK(write(fd, request, strlen(request)) == (ssize_t)strlen(request));
    for (i = 0; i < 3; ++i) {
        CHECK(byte(f->notes[0]) == 'p');
        CHECK(knurl_test_acl_exchange(info.port, "GET", "/fast", NULL,
                                      "fast") == 200);
        CHECK(write(f->gate[1], "g", 1) == 1);
    }
    used = 0;
    while ((count = read(fd, buffer + used, sizeof(buffer) - used - 1)) > 0)
        used += (size_t)count;
    CHECK(count == 0);
    buffer[used] = 0;
    CHECK(strstr(buffer, "X-Resumed: yes") &&
          strstr(buffer, "\r\n\r\nresumed"));
    close(fd);
    CHECK(knurl_test_acl_exchange(info.port, "GET", "/stop", NULL, "stop") ==
          200);
    _exit(0);
}
int main(void)
{
    struct fixture f;
    knurl_server_config c;
    knurl_listener_config l;
    knurl_route_config r;
    knurl_server *s;
    int status;
    memset(&f, 0, sizeof(f));
    CHECK(pipe(f.gate) == 0 && pipe(f.notes) == 0);
    knurl_server_config_init(&c);
    knurl_listener_config_init(&l);
    c.listeners = &l;
    c.listener_count = 1;
    c.workers.count = 1;
    c.workers.affinity_disabled = c.http.backend_quiet = 1;
    c.callbacks.on_parent_ready = ready;
    c.callbacks.on_request_close = closed;
    c.callbacks.userdata = &f;
    CHECK(knurl_server_new(&c, &s, NULL) == KNURL_OK);
    knurl_route_config_init(&r);
    r.path = "/slow";
    r.handler = handler;
    r.userdata = &f;
    r.hooks.before = before;
    r.hooks.after = after;
    r.hooks.userdata = &f;
    CHECK(knurl_router_route(knurl_server_router(s), &r, NULL, NULL) ==
          KNURL_OK);
    knurl_route_config_init(&r);
    r.path = "/fast";
    r.handler = fast;
    r.userdata = &f;
    CHECK(knurl_router_route(knurl_server_router(s), &r, NULL, NULL) ==
          KNURL_OK);
    r.path = "/stop";
    r.handler = stop;
    CHECK(knurl_router_route(knurl_server_router(s), &r, NULL, NULL) ==
          KNURL_OK);
    CHECK(knurl_server_run(s, NULL, NULL) == KNURL_OK);
    CHECK(waitpid(f.client, &status, 0) == f.client && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    CHECK(knurl_server_destroy(s, NULL) == KNURL_OK);
    close(f.gate[0]);
    close(f.gate[1]);
    close(f.notes[0]);
    close(f.notes[1]);
    return 0;
}

#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
#include "acl-wire.h"
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "blocking-route:%d: %s\n", __LINE__, #x);          \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

struct fixture {
    pthread_t owner;
    int entered[2], release[2];
    pid_t client;
};

static knurl_status application(const knurl_request_snapshot *request,
                                knurl_source *input, knurl_reply *reply,
                                void *data, knurl_error *error)
{
    struct fixture *f = data;
    knurl_bytes user, section;
    char byte, chunk[4];
    size_t count, total = 0;
    knurl_status status;

    CHECK(!pthread_equal(f->owner, pthread_self()));
    CHECK(knurl_request_snapshot_path_param(request, "user", &user, error) ==
          KNURL_OK);
    CHECK(knurl_request_snapshot_path_param(request, "section", &section,
                                            error) == KNURL_OK);
    if (!strcmp(section.data, "hold")) {
        CHECK(write(f->entered[1], "s", 1) == 1);
        CHECK(read(f->release[0], &byte, 1) == 1);
        if (byte == 't')
            CHECK(knurl_source_cancelled(input));
        CHECK(user.length == 5 && !memcmp(user.data, "alice", 5));
    }
    if (request->method == KNURL_HTTP_POST) {
        while ((status = knurl_source_read(input, chunk + total,
                                           sizeof(chunk) - total, &count,
                                           error)) == KNURL_OK) {
            total += count;
            CHECK(total <= 3);
        }
        CHECK(status == KNURL_EOF && total == 3);
        user.data = chunk;
        user.length = total;
    }
    return knurl_reply_bytes(reply, 200, "text/plain", user, error);
}

static knurl_status native_io(const knurl_upload_metadata *request,
                              knurl_source *input, knurl_reply *reply,
                              void *data, knurl_error *error)
{
    struct fixture *f = data;
    (void)request;
    (void)input;
    CHECK(!pthread_equal(f->owner, pthread_self()));
    return knurl_reply_text(reply, 200, "text/plain", "io", error);
}

static knurl_status health(knurl_request *request, knurl_reply *reply,
                           void *data, knurl_error *error)
{
    struct fixture *f = data;
    CHECK(pthread_equal(f->owner, pthread_self()));
    CHECK(knurl_server_set_blocking_pool(knurl_request_server(request), NULL,
                                         error) == KNURL_STATE);
    return knurl_reply_text(reply, 200, "text/plain", "ready", error);
}

static knurl_status stop(knurl_request *request, knurl_reply *reply, void *data,
                         knurl_error *error)
{
    (void)data;
    CHECK(knurl_server_request_stop(knurl_request_server(request), error) ==
          KNURL_OK);
    return knurl_reply_status(reply, 204, error);
}

static void wait_entry(struct fixture *f)
{
    struct pollfd note;
    char byte;
    note.fd = f->entered[0];
    note.events = POLLIN;
    CHECK(poll(&note, 1, 3000) == 1 && read(note.fd, &byte, 1) == 1 &&
          byte == 's');
}

static pid_t held_request(unsigned short port, int expected)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0)
        _exit(knurl_test_acl_exchange(port, "GET", "/app/ali%63e/hold", NULL,
                                      expected == 200 ? "alice" : NULL) ==
                      expected
                  ? 0
                  : 1);
    return child;
}

static void wait_success(pid_t child)
{
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
}

static void client(struct fixture *f, unsigned short port)
{
    struct timespec delay = {0, 10000000};
    pid_t child = held_request(port, 200);
    int status = 0, i;

    wait_entry(f);
    CHECK(knurl_test_acl_exchange(port, "GET", "/health", NULL, "ready") ==
          200);
    CHECK(knurl_test_acl_exchange(port, "GET", "/io", NULL, "io") == 200);
    CHECK(waitpid(child, &status, WNOHANG) == 0);
    CHECK(knurl_test_acl_exchange(port, "GET", "/app/bob/open", NULL, NULL) ==
          503);
    CHECK(knurl_test_acl_exchange(port, "POST", "/app/bob/echo",
                                  "Content-Length: 3\r\n\r\nabc", NULL) == 503);
    CHECK(write(f->release[1], "r", 1) == 1);
    wait_success(child);
    CHECK(knurl_test_acl_exchange(port, "POST", "/app/alice/echo",
                                  "Transfer-Encoding: chunked\r\n\r\n"
                                  "1\r\na\r\n2\r\nbc\r\n0\r\n",
                                  "abc") == 200);

    child = held_request(port, 504);
    wait_entry(f);
    wait_success(child);
    CHECK(knurl_test_acl_exchange(port, "GET", "/app/bob/open", NULL, NULL) ==
          503);
    CHECK(knurl_test_acl_exchange(port, "GET", "/health", NULL, "ready") ==
          200);
    CHECK(write(f->release[1], "t", 1) == 1);
    for (i = 0; i < 100; ++i) {
        status =
            knurl_test_acl_exchange(port, "GET", "/app/bob/open", NULL, NULL);
        if (status != 503)
            break;
        nanosleep(&delay, NULL);
    }
    CHECK(status == 200);
    CHECK(knurl_test_acl_exchange(port, "GET", "/group/bob/open", NULL,
                                  "bob") == 200);
    CHECK(knurl_test_acl_exchange(port, "GET", "/stop", NULL, NULL) == 204);
}

static knurl_status ready(knurl_server *server, const knurl_process_context *p,
                          void *data, knurl_error *error)
{
    struct fixture *f = data;
    knurl_bound_listener listener;
    (void)p;
    CHECK(knurl_server_listener_at(server, 0, &listener, error) == KNURL_OK);
    CHECK(knurl_server_set_serving(server, 1, error) == KNURL_OK);
    f->client = fork();
    CHECK(f->client >= 0);
    if (f->client == 0) {
        client(f, listener.port);
        _exit(0);
    }
    return KNURL_OK;
}

static knurl_status worker(knurl_server *server, const knurl_process_context *p,
                           void *data, knurl_error *error)
{
    struct fixture *f = data;
    (void)server;
    (void)p;
    (void)error;
    f->owner = pthread_self();
    return KNURL_OK;
}

int main(void)
{
    struct fixture f;
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_blocking_pool_config pool;
    knurl_blocking_route_config app;
    knurl_route_group_config prefix;
    knurl_upload_reader_config io;
    knurl_route_config route;
    knurl_server *server;
    knurl_router *router;
    knurl_route_group *group;
    knurl_error error;

    CHECK(pipe(f.entered) == 0 && pipe(f.release) == 0);
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    config.listeners = &listener;
    config.listener_count = 1;
    config.workers.count = 1;
    config.workers.affinity_disabled = 1;
    config.http.backend_quiet = 1;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_start = worker;
    config.callbacks.userdata = &f;
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    router = knurl_server_router(server);

    knurl_blocking_pool_config_init(&pool);
    CHECK(pool.threads == 0 && pool.max_tasks == 0 && pool.timeout_ms == 0);
    pool.threads = 9;
    CHECK(knurl_server_set_blocking_pool(server, &pool, &error) ==
          KNURL_INVALID);
    pool.threads = pool.max_tasks = 1;
    pool.timeout_ms = 1500;
    CHECK(server->set_blocking_pool(server, &pool, &error) == KNURL_OK);

    knurl_blocking_route_config_init(&app);
    CHECK(app.method == KNURL_HTTP_GET);
    app.path = "/app/:user/:section";
    app.path_kind = KNURL_PATH_PARAMS;
    app.methods = KNURL_HTTP_METHODS_GET | KNURL_HTTP_METHODS_POST;
    app.buffer_bytes = 2;
    app.handler = application;
    app.userdata = &f;
    CHECK(knurl_router_blocking_route(router, &app, NULL, &error) == KNURL_OK);
    CHECK(knurl_router_blocking_route(router, &app, NULL, &error) ==
          KNURL_CONFLICT);
    knurl_route_group_config_init(&prefix);
    prefix.prefix = "/group";
    CHECK(knurl_router_group(router, &prefix, &group, &error) == KNURL_OK);
    app.path = "/:user/:section";
    CHECK(group->blocking_route(group, &app, NULL, &error) == KNURL_OK);

    knurl_upload_reader_config_init(&io);
    io.path = "/io";
    io.method = KNURL_HTTP_GET;
    io.handler = native_io;
    io.userdata = &f;
    CHECK(knurl_router_upload_reader(router, &io, NULL, &error) == KNURL_OK);

    knurl_route_config_init(&route);
    route.path = "/health";
    route.handler = health;
    route.userdata = &f;
    CHECK(knurl_router_route(router, &route, NULL, &error) == KNURL_OK);
    route.path = "/stop";
    route.handler = stop;
    CHECK(knurl_router_route(router, &route, NULL, &error) == KNURL_OK);

    CHECK(knurl_server_run(server, NULL, &error) == KNURL_OK);
    wait_success(f.client);
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
    close(f.entered[0]);
    close(f.entered[1]);
    close(f.release[0]);
    close(f.release[1]);
    return 0;
}

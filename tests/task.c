#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
#include "acl-wire.h"
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "task:%d: %s\n", __LINE__, #x);                    \
            exit(1);                                                           \
        }                                                                      \
    } while (0)
struct fixture;
struct context {
    struct fixture *fixture;
    int id, started;
};
struct fixture {
    pthread_t owner;
    knurl_task_pool *pool;
    knurl_task *first;
    knurl_request *request;
    struct context contexts[3];
    int notes[2], retired, pools, fast;
    pid_t client;
};
static knurl_status owner_call(void *data, knurl_error *error)
{
    struct context *c = data;
    struct fixture *f = c->fixture;
    CHECK(pthread_equal(f->owner, pthread_self()));
    CHECK(c->id == 0 && write(f->notes[1], "p", 1) == 1);
    knurl_error_clear(error);
    return KNURL_OK;
}
static knurl_status work(knurl_task *task, void *data, knurl_error *error)
{
    struct context *c = data;
    struct timespec pause;
    pause.tv_sec = 0;
    pause.tv_nsec = 1000000;
    CHECK(!pthread_equal(c->fixture->owner, pthread_self()));
    ++c->started;
    CHECK(c->id != 1);
    if (c->id == 0)
        CHECK(task->call(task, owner_call, c, error) == KNURL_OK);
    while (!task->cancelled(task))
        nanosleep(&pause, NULL);
    CHECK(task->call(task, owner_call, c, error) == KNURL_CANCELLED);
    return KNURL_CANCELLED;
}
static void pool_retired(void *data)
{
    struct fixture *f = data;
    CHECK(f->retired >= (f->pools == 0 ? 2 : 3));
    ++f->pools;
}
static void retired(knurl_task *task, knurl_status status,
                    const knurl_error *error, void *data)
{
    struct context *c = data;
    struct fixture *f = c->fixture;
    CHECK(pthread_equal(f->owner, pthread_self()));
    CHECK(status == KNURL_CANCELLED && error->status == status);
    CHECK(task->cancelled(task));
    ++f->retired;
    if (c->id == 0)
        CHECK(c->started == 1 && f->fast == 1);
    if (c->id == 1)
        CHECK(c->started == 0);
    if (f->retired == 2)
        CHECK(f->pool->close(f->pool, NULL) == KNURL_OK);
    if (f->retired == 2 || f->retired == 3) {
        CHECK(f->request->resume(f->request, KNURL_OK, 0, NULL) == KNURL_OK);
        f->request = NULL;
    }
}
static void create(knurl_request *r, struct fixture *f)
{
    knurl_task_pool_config c;
    knurl_task_pool_config_init(&c);
    c.threads = 9;
    CHECK(knurl_task_pool_new(knurl_request_server(r), &c, &f->pool, NULL) ==
          KNURL_INVALID);
    c.threads = 1;
    c.max_tasks = 2;
    c.retired = pool_retired;
    c.userdata = f;
    CHECK(knurl_task_pool_new(knurl_request_server(r), &c, &f->pool, NULL) ==
          KNURL_OK);
}
static knurl_status run(knurl_request *r, knurl_reply *reply, void *data,
                        knurl_error *error)
{
    struct fixture *f = data;
    knurl_task_config c;
    knurl_task *queued, *rejected;
    create(r, f);
    knurl_task_config_init(&c);
    c.work = work;
    c.retired = retired;
    c.timeout_disabled = 1;
    c.userdata = &f->contexts[0];
    CHECK(f->pool->submit(f->pool, &c, &f->first, error) == KNURL_OK);
    c.userdata = &f->contexts[1];
    CHECK(f->pool->submit(f->pool, &c, &queued, error) == KNURL_OK);
    c.userdata = &f->contexts[2];
    CHECK(f->pool->submit(f->pool, &c, &rejected, error) == KNURL_AGAIN &&
          rejected == NULL);
    CHECK(queued->cancel(queued, error) == KNURL_OK);
    CHECK(knurl_reply_text(reply, 200, "text/plain", "done", error) ==
          KNURL_OK);
    f->request = r;
    return KNURL_AGAIN;
}
static knurl_status fast(knurl_request *r, knurl_reply *reply, void *data,
                         knurl_error *error)
{
    struct fixture *f = data;
    (void)r;
    ++f->fast;
    CHECK(f->first->cancel(f->first, error) == KNURL_OK);
    return knurl_reply_text(reply, 200, "text/plain", "fast", error);
}
static knurl_status close_running(knurl_request *r, knurl_reply *reply,
                                  void *data, knurl_error *error)
{
    struct fixture *f = data;
    knurl_task_config c;
    knurl_task *task;
    create(r, f);
    knurl_task_config_init(&c);
    c.work = work;
    c.retired = retired;
    c.timeout_disabled = 1;
    c.userdata = &f->contexts[2];
    CHECK(f->pool->submit(f->pool, &c, &task, error) == KNURL_OK);
    CHECK(f->pool->close(f->pool, error) == KNURL_OK);
    CHECK(knurl_reply_text(reply, 200, "text/plain", "closed", error) ==
          KNURL_OK);
    f->request = r;
    return KNURL_AGAIN;
}
static knurl_status stop(knurl_request *r, knurl_reply *reply, void *data,
                         knurl_error *error)
{
    struct fixture *f = data;
    CHECK(f->retired == 3 && f->contexts[2].started <= 1);
    CHECK(knurl_server_request_stop(knurl_request_server(r), error) ==
          KNURL_OK);
    return knurl_reply_text(reply, 200, "text/plain", "stop", error);
}
static int exited(knurl_server *s, const knurl_process_context *p, void *data)
{
    struct fixture *f = data;
    (void)s;
    (void)p;
    CHECK(f->retired == 3 && f->pools == 2);
    return 0;
}
static knurl_status ready(knurl_server *s, const knurl_process_context *p,
                          void *data, knurl_error *error)
{
    struct fixture *f = data;
    knurl_bound_listener info;
    struct pollfd note;
    char byte;
    pid_t slow;
    int status;
    (void)p;
    CHECK(knurl_server_listener_at(s, 0, &info, error) == KNURL_OK);
    f->client = fork();
    CHECK(f->client >= 0);
    if (f->client != 0)
        return KNURL_OK;
    slow = fork();
    CHECK(slow >= 0);
    if (slow == 0)
        _exit(knurl_test_acl_exchange(info.port, "GET", "/run", NULL, "done") ==
                      200
                  ? 0
                  : 1);
    note.fd = f->notes[0];
    note.events = POLLIN;
    note.revents = 0;
    CHECK(poll(&note, 1, 3000) == 1 && read(note.fd, &byte, 1) == 1 &&
          byte == 'p');
    CHECK(knurl_test_acl_exchange(info.port, "GET", "/fast", NULL, "fast") ==
          200);
    CHECK(waitpid(slow, &status, 0) == slow && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    CHECK(knurl_test_acl_exchange(info.port, "GET", "/close", NULL, "closed") ==
          200);
    CHECK(knurl_test_acl_exchange(info.port, "GET", "/stop", NULL, "stop") ==
          200);
    _exit(0);
}
int main(void)
{
    struct fixture f;
    knurl_server_config c;
    knurl_listener_config l;
    knurl_route_config route;
    knurl_server *s;
    knurl_task_pool *invalid;
    int i, status;
    const char *paths[] = {"/run", "/fast", "/close", "/stop"};
    knurl_route_fn handlers[] = {run, fast, close_running, stop};
    memset(&f, 0, sizeof(f));
    f.owner = pthread_self();
    CHECK(pipe(f.notes) == 0);
    for (i = 0; i < 3; ++i) {
        f.contexts[i].fixture = &f;
        f.contexts[i].id = i;
    }
    knurl_server_config_init(&c);
    knurl_listener_config_init(&l);
    c.listeners = &l;
    c.listener_count = 1;
    c.workers.count = 1;
    c.workers.affinity_disabled = c.http.backend_quiet = 1;
    c.callbacks.on_parent_ready = ready;
    c.callbacks.on_worker_exit = exited;
    c.callbacks.userdata = &f;
    CHECK(knurl_server_new(&c, &s, NULL) == KNURL_OK);
    CHECK(knurl_task_pool_new(s, NULL, &invalid, NULL) == KNURL_STATE &&
          invalid == NULL);
    for (i = 0; i < 4; ++i) {
        knurl_route_config_init(&route);
        route.path = paths[i];
        route.handler = handlers[i];
        route.userdata = &f;
        CHECK(knurl_router_route(knurl_server_router(s), &route, NULL, NULL) ==
              KNURL_OK);
    }
    CHECK(knurl_server_run(s, NULL, NULL) == KNURL_OK);
    CHECK(waitpid(f.client, &status, 0) == f.client && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    CHECK(knurl_server_destroy(s, NULL) == KNURL_OK);
    close(f.notes[0]);
    close(f.notes[1]);
    return 0;
}

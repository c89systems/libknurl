#define _POSIX_C_SOURCE 200809L
#include "kore_internal.h"
#include "proxy_runtime.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

struct knurl_kore_watch {
    struct kore_event event;
    knurl_watch *watch;
};

static void native_ready(void *userdata, int failed)
{
    struct knurl_kore_watch *native;
    unsigned int flags;
    (void)failed;
    native = userdata;
    flags = 0;
    if (native->event.flags & KORE_EVENT_READ)
        flags |= KNURL_READABLE;
    if (native->event.flags & KORE_EVENT_WRITE)
        flags |= KNURL_WRITABLE;
    if (native->event.flags & KORE_EVENT_READ_CLOSED)
        flags |= KNURL_READ_CLOSED;
    if (native->event.flags & KORE_EVENT_ERROR)
        flags |= KNURL_EVENT_ERROR;
    native->event.flags = 0;
    knurl_loop_watch_ready(native->watch, flags);
}

static int managed_fd(struct knurl_runtime *runtime, int fd)
{
    struct connection *connection;
    struct kore_server *server;
    struct listener *listener;
    if (fd == runtime->wake[0] || fd == runtime->wake[1])
        return 1;
    TAILQ_FOREACH(connection, &connections, list)
    if (connection->fd == fd)
        return 1;
    LIST_FOREACH(server, &kore_servers, list)
    LIST_FOREACH(listener, &server->listeners, list)
    if (listener->fd == fd)
        return 1;
    return 0;
}

static knurl_status watch_update(void *userdata, knurl_watch *watch,
                                 unsigned int previous, unsigned int interest,
                                 knurl_error *error)
{
    struct knurl_kore_watch *native;
    int result;
    if (previous == interest)
        return KNURL_OK;
    if (interest != 0 && previous == 0 && managed_fd(userdata, watch->fd))
        return knurl_fail(error, KNURL_CONFLICT, "loop_watch",
                          "descriptor belongs to managed transport");
    native = watch->backend;
    if (native == NULL) {
        native = calloc(1, sizeof(*native));
        if (native == NULL)
            return knurl_fail(error, KNURL_NOMEM, "loop_watch",
                              "cannot allocate native watch");
        native->event.handle = native_ready;
        native->watch = watch;
        watch->backend = native;
    }
    result = kore_library_watch_update(watch->fd, previous, interest, native);
    if (result != 0) {
        (void)knurl_fail(error, result == EEXIST ? KNURL_CONFLICT : KNURL_IO,
                         "loop_watch",
                         "cannot change native descriptor interest");
        if (error != NULL) {
            error->native_code = result;
            error->source = KNURL_ERROR_SYSTEM;
        }
        return result == EEXIST ? KNURL_CONFLICT : KNURL_IO;
    }
    return KNURL_OK;
}

static void watch_release(void *userdata, knurl_watch *watch)
{
    (void)userdata;
    free(watch->backend);
    watch->backend = NULL;
}

static void loop_wake(void *userdata)
{
    struct knurl_runtime *runtime;
    unsigned char byte;
    ssize_t result;
    int saved;
    runtime = userdata;
    if (runtime->wake[1] < 0)
        return;
    byte = 1;
    saved = errno;
    do {
        result = write(runtime->wake[1], &byte, 1);
    } while (result == -1 && errno == EINTR);
    errno = saved;
}

knurl_status knurl_kore_loop_create(struct knurl_runtime *runtime,
                                    knurl_error *error)
{
    struct knurl_loop_backend backend;
    backend.userdata = runtime;
    backend.watch_update = watch_update;
    backend.watch_release = watch_release;
    backend.wake = loop_wake;
    return knurl_loop_create(&backend, &runtime->server->loop, error);
}

u_int64_t knurl_kore_loop_wait(void *userdata, u_int64_t maximum)
{
    struct knurl_runtime *runtime;
    long wait;
    runtime = userdata;
    if (runtime->response_count != 0 || !TAILQ_EMPTY(&disconnected))
        maximum = 0;
    maximum = knurl_kore_responses_wait(runtime, maximum);
    maximum = knurl_kore_websockets_wait(runtime, maximum);
    if (runtime->server->loop != NULL) {
        wait = knurl_loop_wait(runtime->server->loop);
        if ((u_int64_t)wait < maximum)
            maximum = (u_int64_t)wait;
    }
    if (maximum != KORE_WAIT_INFINITE && maximum > INT_MAX)
        maximum = INT_MAX;
    return maximum;
}

void knurl_kore_loop_turn(void *userdata)
{
    struct knurl_runtime *runtime;
    knurl_loop *loop;
    runtime = userdata;
    loop = runtime->server->loop;
    if (worker == NULL)
        knurl_kore_tls_observe(runtime, NULL);
    knurl_kore_responses_turn(runtime);
    knurl_kore_websockets_turn(runtime);
    if (loop != NULL) {
        knurl_loop_turn(loop);
        if (loop->failure.status != KNURL_OK && runtime->status == KNURL_OK) {
            runtime->status = loop->failure.status;
            runtime->error = loop->failure;
            if (worker != NULL)
                knurl_kore_child_failure(runtime, &runtime->error);
            kore_quit = KORE_QUIT_FATAL;
        }
    }
    if (runtime->server->stop_requested ||
        runtime->server->terminating_signal) {
        if (worker != NULL) {
            if (!runtime->stop_sent) {
                runtime->stop_sent = 1;
                kore_shutdown();
            }
        } else if (kore_quit == KORE_QUIT_NONE)
            kore_quit = KORE_QUIT_NORMAL;
    }
}

void knurl_kore_parent_drain(void *userdata, u_int64_t maximum)
{
    struct knurl_runtime *runtime;
    runtime = userdata;
    if (runtime->server->loop != NULL && runtime->server->loop->native_ready)
        kore_library_parent_drain(maximum);
    else
        (void)usleep((useconds_t)(maximum * 1000));
}

void knurl_kore_loop_shutdown(void *userdata)
{
    struct knurl_runtime *runtime;
    runtime = userdata;
    knurl_proxy_worker_clear(runtime->server);
    knurl_kore_responses_shutdown(runtime);
    knurl_kore_websockets_shutdown(runtime);
    knurl_loop_shutdown(runtime->server->loop);
    if (runtime->server->loop != NULL &&
        runtime->server->loop->failure.status != KNURL_OK &&
        runtime->status == KNURL_OK) {
        runtime->error = runtime->server->loop->failure;
        runtime->status = runtime->error.status;
    }
    runtime->control_watch = NULL;
}

void knurl_kore_loop_finish(struct knurl_runtime *runtime)
{
    knurl_loop_release(runtime->server->loop);
    runtime->server->loop = NULL;
}

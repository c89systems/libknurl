#define _POSIX_C_SOURCE 200809L
#include "kore_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

struct rotmos_kore_watch {
    struct kore_event event;
    rotmos_watch *watch;
};

static void native_ready(void *userdata, int failed)
{
    struct rotmos_kore_watch *native;
    unsigned int flags;
    (void)failed;
    native = userdata;
    flags = 0;
    if (native->event.flags & KORE_EVENT_READ)
        flags |= ROTMOS_READABLE;
    if (native->event.flags & KORE_EVENT_WRITE)
        flags |= ROTMOS_WRITABLE;
    if (native->event.flags & KORE_EVENT_READ_CLOSED)
        flags |= ROTMOS_READ_CLOSED;
    if (native->event.flags & KORE_EVENT_ERROR)
        flags |= ROTMOS_EVENT_ERROR;
    native->event.flags = 0;
    rotmos_loop_watch_ready(native->watch, flags);
}

static int managed_fd(struct rotmos_runtime *runtime, int fd)
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

static rotmos_status watch_update(void *userdata, rotmos_watch *watch,
                                  unsigned int previous, unsigned int interest,
                                  rotmos_error *error)
{
    struct rotmos_kore_watch *native;
    int result;
    if (previous == interest)
        return ROTMOS_OK;
    if (interest != 0 && previous == 0 && managed_fd(userdata, watch->fd))
        return rotmos_fail(error, ROTMOS_CONFLICT, "loop_watch",
                           "descriptor belongs to managed transport");
    native = watch->backend;
    if (native == NULL) {
        native = calloc(1, sizeof(*native));
        if (native == NULL)
            return rotmos_fail(error, ROTMOS_NOMEM, "loop_watch",
                               "cannot allocate native watch");
        native->event.handle = native_ready;
        native->watch = watch;
        watch->backend = native;
    }
    result = kore_library_watch_update(watch->fd, previous, interest, native);
    if (result != 0) {
        (void)rotmos_fail(error, result == EEXIST ? ROTMOS_CONFLICT : ROTMOS_IO,
                          "loop_watch",
                          "cannot change native descriptor interest");
        if (error != NULL) {
            error->native_code = result;
            error->source = ROTMOS_ERROR_SYSTEM;
        }
        return result == EEXIST ? ROTMOS_CONFLICT : ROTMOS_IO;
    }
    return ROTMOS_OK;
}

static void watch_release(void *userdata, rotmos_watch *watch)
{
    (void)userdata;
    free(watch->backend);
    watch->backend = NULL;
}

static void loop_wake(void *userdata)
{
    struct rotmos_runtime *runtime;
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

rotmos_status rotmos_kore_loop_create(struct rotmos_runtime *runtime,
                                      rotmos_error *error)
{
    struct rotmos_loop_backend backend;
    backend.userdata = runtime;
    backend.watch_update = watch_update;
    backend.watch_release = watch_release;
    backend.wake = loop_wake;
    return rotmos_loop_create(&backend, &runtime->server->loop, error);
}

u_int64_t rotmos_kore_loop_wait(void *userdata, u_int64_t maximum)
{
    struct rotmos_runtime *runtime;
    long wait;
    runtime = userdata;
    if (runtime->response_count != 0 || !TAILQ_EMPTY(&disconnected))
        maximum = 0;
    maximum = rotmos_kore_responses_wait(runtime, maximum);
    if (runtime->server->loop != NULL) {
        wait = rotmos_loop_wait(runtime->server->loop);
        if ((u_int64_t)wait < maximum)
            maximum = (u_int64_t)wait;
    }
    if (maximum != KORE_WAIT_INFINITE && maximum > INT_MAX)
        maximum = INT_MAX;
    return maximum;
}

void rotmos_kore_loop_turn(void *userdata)
{
    struct rotmos_runtime *runtime;
    rotmos_loop *loop;
    runtime = userdata;
    loop = runtime->server->loop;
    rotmos_kore_responses_turn(runtime);
    if (loop != NULL) {
        rotmos_loop_turn(loop);
        if (loop->failure.status != ROTMOS_OK && runtime->status == ROTMOS_OK) {
            runtime->status = loop->failure.status;
            runtime->error = loop->failure;
            if (worker != NULL)
                kore_msg_send(KORE_MSG_PARENT, KORE_MSG_FATALX, NULL, 0);
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

void rotmos_kore_parent_drain(void *userdata, u_int64_t maximum)
{
    struct rotmos_runtime *runtime;
    runtime = userdata;
    if (runtime->server->loop != NULL && runtime->server->loop->native_ready)
        kore_library_parent_drain(maximum);
    else
        (void)usleep((useconds_t)(maximum * 1000));
}

void rotmos_kore_loop_shutdown(void *userdata)
{
    struct rotmos_runtime *runtime;
    runtime = userdata;
    rotmos_kore_responses_shutdown(runtime);
    rotmos_loop_shutdown(runtime->server->loop);
    if (runtime->server->loop != NULL &&
        runtime->server->loop->failure.status != ROTMOS_OK &&
        runtime->status == ROTMOS_OK) {
        runtime->error = runtime->server->loop->failure;
        runtime->status = runtime->error.status;
    }
    runtime->control_watch = NULL;
}

void rotmos_kore_loop_finish(struct rotmos_runtime *runtime)
{
    rotmos_loop_release(runtime->server->loop);
    runtime->server->loop = NULL;
}

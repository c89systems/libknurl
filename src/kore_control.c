#define _POSIX_C_SOURCE 200809L
#include "kore_internal.h"

#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

static int control_callback(struct knurl_runtime *runtime, unsigned int events)
{
    knurl_error error;
    knurl_status status;
    knurl_supervision_config *config;
    config = &runtime->server->config.supervision;
    knurl_error_clear(&error);
    status =
        config->on_event(runtime->server, events, config->userdata, &error);
    if (status != KNURL_OK) {
        if (runtime->status == KNURL_OK) {
            runtime->status = status;
            if (error.status == KNURL_OK)
                (void)knurl_fail(&error, status, "server_control",
                                 "supervision callback failed");
            runtime->error = error;
        }
        (void)knurl_server_request_stop(runtime->server, NULL);
        return 0;
    }
    return 1;
}

static void control_event(knurl_watch *watch, unsigned int flags,
                          void *userdata)
{
    struct knurl_runtime *runtime;
    unsigned int events;
    int available;
    int fd;
    (void)watch;
    runtime = userdata;
    fd = runtime->server->config.supervision.control_fd;
    events = 0;
    if (flags & KNURL_READABLE)
        events |= KNURL_CONTROL_READ;
    if (flags & KNURL_WRITABLE)
        events |= KNURL_CONTROL_WRITE;
    if (events != 0 && !control_callback(runtime, events))
        return;
    if (!(flags & (KNURL_READ_CLOSED | KNURL_EVENT_ERROR)))
        return;
    available = 0;
    if (ioctl(fd, FIONREAD, &available) == -1) {
        if (errno == EINTR)
            return;
        events = KNURL_CONTROL_ERROR;
    } else if (available > 0)
        return;
    else
        events = (flags & KNURL_EVENT_ERROR) ? KNURL_CONTROL_ERROR
                                             : KNURL_CONTROL_EOF;
    (void)control_callback(runtime, events);
    knurl_watch_cancel(runtime->control_watch);
    runtime->control_watch = NULL;
    (void)knurl_server_request_stop(runtime->server, NULL);
}

knurl_status knurl_kore_control_start(struct knurl_runtime *runtime,
                                      knurl_error *error)
{
    knurl_supervision_config *config;
    int flags;
    config = &runtime->server->config.supervision;
    if (!config->enabled)
        return KNURL_OK;
    flags = fcntl(config->control_fd, F_GETFL);
    if (flags == -1 ||
        fcntl(config->control_fd, F_SETFL, flags | O_NONBLOCK) == -1 ||
        fcntl(config->control_fd, F_SETFD, FD_CLOEXEC) == -1)
        return knurl_fail(error, KNURL_IO, "server_control",
                          "cannot prepare owned control descriptor");
    return knurl_loop_watch(runtime->server->loop, config->control_fd,
                            config->interest, control_event, NULL, runtime,
                            &runtime->control_watch, error);
}

knurl_status knurl_server_control_interest(knurl_server *server,
                                           unsigned int interest,
                                           knurl_error *error)
{
    if (server == NULL || server->runtime == NULL ||
        server->runtime->parent_pid != getpid() ||
        server->runtime->control_watch == NULL)
        return knurl_fail(error, KNURL_STATE, "server_control_interest",
                          "parent loop has no active control watch");
    return knurl_watch_update(server->runtime->control_watch, interest, error);
}

void knurl_kore_control_close(struct knurl_runtime *runtime)
{
    knurl_supervision_config *config;
    config = &runtime->server->config.supervision;
    if (config->enabled && config->control_fd >= 0) {
        (void)close(config->control_fd);
        config->control_fd = -1;
    }
}

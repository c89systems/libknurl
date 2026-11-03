#define _POSIX_C_SOURCE 200809L
#include "kore_internal.h"

#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

static int control_callback(struct rotmos_runtime *runtime, unsigned int events)
{
    rotmos_error error;
    rotmos_status status;
    rotmos_supervision_config *config;
    config = &runtime->server->config.supervision;
    rotmos_error_clear(&error);
    status =
        config->on_event(runtime->server, events, config->userdata, &error);
    if (status != ROTMOS_OK) {
        if (runtime->status == ROTMOS_OK) {
            runtime->status = status;
            if (error.status == ROTMOS_OK)
                (void)rotmos_fail(&error, status, "server_control",
                                  "supervision callback failed");
            runtime->error = error;
        }
        (void)rotmos_server_request_stop(runtime->server, NULL);
        return 0;
    }
    return 1;
}

static void control_event(rotmos_watch *watch, unsigned int flags,
                          void *userdata)
{
    struct rotmos_runtime *runtime;
    unsigned int events;
    int available;
    int fd;
    (void)watch;
    runtime = userdata;
    fd = runtime->server->config.supervision.control_fd;
    events = 0;
    if (flags & ROTMOS_READABLE)
        events |= ROTMOS_CONTROL_READ;
    if (flags & ROTMOS_WRITABLE)
        events |= ROTMOS_CONTROL_WRITE;
    if (events != 0 && !control_callback(runtime, events))
        return;
    if (!(flags & (ROTMOS_READ_CLOSED | ROTMOS_EVENT_ERROR)))
        return;
    available = 0;
    if (ioctl(fd, FIONREAD, &available) == -1) {
        if (errno == EINTR)
            return;
        events = ROTMOS_CONTROL_ERROR;
    } else if (available > 0)
        return;
    else
        events = (flags & ROTMOS_EVENT_ERROR) ? ROTMOS_CONTROL_ERROR
                                              : ROTMOS_CONTROL_EOF;
    (void)control_callback(runtime, events);
    rotmos_watch_cancel(runtime->control_watch);
    runtime->control_watch = NULL;
    (void)rotmos_server_request_stop(runtime->server, NULL);
}

rotmos_status rotmos_kore_control_start(struct rotmos_runtime *runtime,
                                        rotmos_error *error)
{
    rotmos_supervision_config *config;
    int flags;
    config = &runtime->server->config.supervision;
    if (!config->enabled)
        return ROTMOS_OK;
    flags = fcntl(config->control_fd, F_GETFL);
    if (flags == -1 ||
        fcntl(config->control_fd, F_SETFL, flags | O_NONBLOCK) == -1 ||
        fcntl(config->control_fd, F_SETFD, FD_CLOEXEC) == -1)
        return rotmos_fail(error, ROTMOS_IO, "server_control",
                           "cannot prepare owned control descriptor");
    return rotmos_loop_watch(runtime->server->loop, config->control_fd,
                             config->interest, control_event, NULL, runtime,
                             &runtime->control_watch, error);
}

rotmos_status rotmos_server_control_interest(rotmos_server *server,
                                             unsigned int interest,
                                             rotmos_error *error)
{
    if (server == NULL || server->runtime == NULL ||
        server->runtime->parent_pid != getpid() ||
        server->runtime->control_watch == NULL)
        return rotmos_fail(error, ROTMOS_STATE, "server_control_interest",
                           "parent loop has no active control watch");
    return rotmos_watch_update(server->runtime->control_watch, interest, error);
}

void rotmos_kore_control_close(struct rotmos_runtime *runtime)
{
    rotmos_supervision_config *config;
    config = &runtime->server->config.supervision;
    if (config->enabled && config->control_fd >= 0) {
        (void)close(config->control_fd);
        config->control_fd = -1;
    }
}

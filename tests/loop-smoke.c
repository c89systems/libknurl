#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>

#include <sys/socket.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct fixture {
    int control;
    int pipe[2];
    int received;
    int eof;
    int delivered;
    int retired;
    int replaced;
    int failed;
    rotmos_loop *loop;
    rotmos_waker *waker;
};

static void retire(void *userdata) { ++((struct fixture *)userdata)->retired; }

static void deferred(rotmos_loop *loop, void *userdata)
{
    (void)loop;
    ++((struct fixture *)userdata)->delivered;
}

static void timer(rotmos_timer *handle, void *userdata)
{
    (void)handle;
    ++((struct fixture *)userdata)->delivered;
}

static void wake(rotmos_loop *loop, void *userdata)
{
    struct fixture *fixture;
    (void)loop;
    fixture = userdata;
    ++fixture->delivered;
    (void)rotmos_waker_destroy(fixture->waker, NULL);
}

static void watch(rotmos_watch *handle, unsigned int flags, void *userdata)
{
    struct fixture *fixture;
    rotmos_watch *replacement;
    char byte;
    fixture = userdata;
    if ((flags & ROTMOS_READABLE) && read(fixture->pipe[0], &byte, 1) == 1)
        ++fixture->delivered;
    if (!fixture->replaced) {
        if (rotmos_loop_watch(fixture->loop, fixture->pipe[0], ROTMOS_READABLE,
                              watch, retire, fixture, &replacement,
                              NULL) != ROTMOS_CONFLICT ||
            replacement != NULL)
            fixture->failed = 1;
    }
    rotmos_watch_cancel(handle);
    if (!fixture->replaced) {
        fixture->replaced = 1;
        if (rotmos_loop_watch(fixture->loop, fixture->pipe[0], ROTMOS_READABLE,
                              watch, retire, fixture, &replacement,
                              NULL) != ROTMOS_OK)
            fixture->failed = 1;
    }
}

static rotmos_status control(rotmos_server *server, unsigned int events,
                             void *userdata, rotmos_error *error)
{
    struct fixture *fixture;
    char byte;
    (void)server;
    (void)error;
    fixture = userdata;
    if ((events & ROTMOS_CONTROL_READ) && read(fixture->control, &byte, 1) == 1)
        ++fixture->received;
    if (events & ROTMOS_CONTROL_EOF) {
        fixture->eof = 1;
        if (fixture->received != 3)
            return ROTMOS_INVALID;
    }
    return ROTMOS_OK;
}

static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *process,
                           void *userdata, rotmos_error *error)
{
    struct fixture *fixture;
    rotmos_loop *loop;
    rotmos_watch *fd_watch;
    rotmos_timer *clock;
    rotmos_u64 now;
    int i;
    (void)process;
    fixture = userdata;
    loop = rotmos_server_loop(server);
    fixture->loop = loop;
    if (rotmos_loop_now(loop, &now, error) != ROTMOS_OK ||
        rotmos_loop_watch(loop, fixture->pipe[0], ROTMOS_READABLE, watch,
                          retire, fixture, &fd_watch, error) != ROTMOS_OK ||
        rotmos_loop_timer(loop, 0, 0, timer, retire, fixture, &clock, error) !=
            ROTMOS_OK ||
        rotmos_loop_defer(loop, deferred, retire, fixture, error) !=
            ROTMOS_OK ||
        rotmos_waker_new(loop, wake, retire, fixture, &fixture->waker, error) !=
            ROTMOS_OK)
        return ROTMOS_INVALID;
    for (i = 0; i < 100; ++i)
        if (rotmos_waker_signal(fixture->waker, error) != ROTMOS_OK)
            return ROTMOS_INVALID;
    return ROTMOS_OK;
}

int main(void)
{
    struct fixture fixture;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_server *server;
    rotmos_run_result result;
    rotmos_error error;
    int pair[2];
    memset(&fixture, 0, sizeof(fixture));
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 ||
        pipe(fixture.pipe) != 0)
        return 1;
    fixture.control = pair[0];
    if (write(pair[1], "abc", 3) != 3 || shutdown(pair[1], SHUT_WR) != 0 ||
        write(fixture.pipe[1], "xy", 2) != 2)
        return 2;
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.supervision.enabled = 1;
    config.supervision.control_fd = pair[0];
    config.supervision.interest = ROTMOS_CONTROL_READ;
    config.supervision.on_event = control;
    config.supervision.userdata = &fixture;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.userdata = &fixture;
    if (rotmos_server_new(&config, &server, &error) != ROTMOS_OK)
        return 3;
    if (rotmos_server_run(server, &result, &error) != ROTMOS_OK) {
        (void)fprintf(stderr, "%s: %s\n", error.operation, error.message);
        return 4;
    }
    if (!fixture.eof || fixture.received != 3 || fixture.delivered != 5 ||
        fixture.retired != 5 || !fixture.replaced || fixture.failed ||
        result.reason != ROTMOS_RUN_REQUESTED_STOP || result.escalated ||
        fcntl(pair[0], F_GETFD) != -1 || fcntl(fixture.pipe[0], F_GETFD) == -1)
        return 5;
    if (rotmos_server_run(server, NULL, &error) != ROTMOS_STATE)
        return 6;
    (void)rotmos_server_destroy(server, NULL);
    (void)close(pair[1]);
    (void)close(fixture.pipe[0]);
    (void)close(fixture.pipe[1]);
    return 0;
}

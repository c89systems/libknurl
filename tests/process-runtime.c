#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
#ifdef ROTMOS_PROCESS_BACKEND_TEST
#include "process-backend.h"
#endif

#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(test)                                                            \
    do {                                                                       \
        if (!(test)) {                                                         \
            (void)fprintf(stderr, "process line %d mode %d: %s\n", __LINE__,   \
                          fixture.mode, #test);                                \
            return 1;                                                          \
        }                                                                      \
    } while (0)

struct fixture {
    int mode;
    int failure;
    int starts;
    int restarts;
    int exits;
    int escalations;
    int ready;
    int notes[2];
    int block[2];
    long worker_pid;
    long parent_pid;
    rotmos_server *server;
};

static int child_context(struct fixture *fixture,
                         const rotmos_process_context *process)
{
    return process->role == ROTMOS_PROCESS_HTTP && process->worker_index == 0 &&
           process->process_id == (long)getpid() &&
           process->process_id != fixture->parent_pid;
}

static rotmos_status hook_failure(rotmos_error *error, rotmos_status status)
{
    rotmos_error_clear(error);
    error->status = status;
    error->source = ROTMOS_ERROR_CORE;
    (void)strcpy(error->operation, "fixture_child_start");
    (void)strcpy(error->message, "expected child hook failure");
    return status;
}

static rotmos_status process_start(rotmos_server *server,
                                   const rotmos_process_context *process,
                                   void *userdata, rotmos_error *error)
{
    struct fixture *fixture;
    (void)server;
    fixture = userdata;
    if (process->role == ROTMOS_PROCESS_PARENT)
        return process->process_id == fixture->parent_pid ? ROTMOS_OK
                                                          : ROTMOS_INVALID;
    if (!child_context(fixture, process))
        return hook_failure(error, ROTMOS_STATE);
    if (fixture->mode == 3)
        return hook_failure(error, ROTMOS_INVALID);
    return ROTMOS_OK;
}

static void vanish(rotmos_timer *timer, void *userdata)
{
    (void)timer;
    (void)userdata;
    _exit(0);
}

#ifdef ROTMOS_PROCESS_BACKEND_TEST
static void budget(rotmos_timer *timer, void *userdata)
{
    struct fixture *fixture;
    char byte;
    (void)timer;
    fixture = userdata;
    rotmos_test_child_budget(fixture->mode == 11 ? 1 : 30);
    if (fixture->mode == 11) {
        rotmos_test_child_budget(0);
        return;
    }
    while (read(fixture->block[0], &byte, 1) == -1 && errno == EINTR) {
    }
}
#endif

static rotmos_status worker_start(rotmos_server *server,
                                  const rotmos_process_context *process,
                                  void *userdata, rotmos_error *error)
{
    struct fixture *fixture;
    rotmos_timer *timer;
#ifdef ROTMOS_PROCESS_BACKEND_TEST
    char byte;
#endif
    (void)server;
    fixture = userdata;
    if (!child_context(fixture, process))
        return hook_failure(error, ROTMOS_STATE);
    if (fixture->mode == 2)
        return hook_failure(error, ROTMOS_IO);
    if (fixture->mode == 8 && !process->restarted)
        return rotmos_loop_timer(rotmos_server_loop(server), 25, 0, vanish,
                                 NULL, fixture, &timer, error);
#ifdef ROTMOS_PROCESS_BACKEND_TEST
    if (fixture->mode == 10) {
        rotmos_test_child_budget(30);
        while (read(fixture->block[0], &byte, 1) == -1 && errno == EINTR) {
        }
    }
    if ((fixture->mode == 9 && !process->restarted) || fixture->mode == 11)
        return rotmos_loop_timer(rotmos_server_loop(server), 1, 0, budget, NULL,
                                 fixture, &timer, error);
#endif
    return ROTMOS_OK;
}

static rotmos_status stopping(rotmos_server *server,
                              const rotmos_process_context *process,
                              void *userdata, rotmos_error *error)
{
    struct fixture *fixture;
    char byte;
    (void)server;
    fixture = userdata;
    if (!child_context(fixture, process) ||
        write(fixture->notes[1], "S", 1) != 1)
        return hook_failure(error, ROTMOS_STATE);
    if (fixture->mode == 6 || fixture->mode == 12) {
#ifdef ROTMOS_PROCESS_BACKEND_TEST
        if (fixture->mode == 12)
            rotmos_test_child_budget(30);
#endif
        while (read(fixture->block[0], &byte, 1) == -1 && errno == EINTR) {
        }
    }
    return ROTMOS_OK;
}

static int exited(rotmos_server *server, const rotmos_process_context *process,
                  void *userdata)
{
    struct fixture *fixture;
    (void)server;
    fixture = userdata;
    if (!child_context(fixture, process) ||
        write(fixture->notes[1], "X", 1) != 1)
        return 89;
    return fixture->mode == 7 ? 23 : 0;
}

static void observed(rotmos_server *server, const rotmos_worker_event *event,
                     void *userdata)
{
    struct fixture *fixture;
    fixture = userdata;
    if (getpid() != fixture->parent_pid ||
        event->process.role != ROTMOS_PROCESS_HTTP ||
        event->process.worker_index != 0 || event->process.process_id <= 0)
        fixture->failure = 1;
    if (event->kind == ROTMOS_WORKER_STARTED) {
        ++fixture->starts;
        fixture->worker_pid = event->process.process_id;
        if (event->process.restarted || event->error.status != ROTMOS_OK)
            fixture->failure = 1;
        if (fixture->mode == 5)
            (void)rotmos_server_request_stop(server, NULL);
    } else if (event->kind == ROTMOS_WORKER_RESTARTED) {
        ++fixture->restarts;
        if (!event->process.restarted || event->error.status != ROTMOS_OK)
            fixture->failure = 1;
        (void)rotmos_server_request_stop(server, NULL);
    } else if (event->kind == ROTMOS_WORKER_EXITED) {
        ++fixture->exits;
        if (!event->expected && event->error.status == ROTMOS_OK)
            fixture->failure = 1;
    } else if (event->kind == ROTMOS_WORKER_ESCALATED) {
        ++fixture->escalations;
        if (!event->abandoned || event->error.status != ROTMOS_TIMEOUT)
            fixture->failure = 1;
    }
}

static void stop_timer(rotmos_timer *timer, void *userdata)
{
    struct fixture *fixture;
    (void)timer;
    fixture = userdata;
    (void)rotmos_server_request_stop(fixture->server, NULL);
}

static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *process,
                           void *userdata, rotmos_error *error)
{
    struct fixture *fixture;
    rotmos_timer *timer;
    (void)process;
    fixture = userdata;
    fixture->server = server;
    ++fixture->ready;
    if (fixture->starts != 1)
        return hook_failure(error, ROTMOS_STATE);
    if (fixture->mode == 0 || fixture->mode == 1)
        return kill((pid_t)fixture->worker_pid, SIGKILL) == 0 ? ROTMOS_OK
                                                              : ROTMOS_IO;
    if (fixture->mode == 9 || fixture->mode == 8)
        return ROTMOS_OK;
    if (fixture->mode == 11)
        return rotmos_loop_timer(rotmos_server_loop(server), 60, 0, stop_timer,
                                 NULL, fixture, &timer, error);
    return rotmos_server_request_stop(server, error);
}

int main(void)
{
    struct fixture fixture;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_run_result result;
    rotmos_server *server;
    rotmos_error error;
    rotmos_status status;
    char notes[8];
    ssize_t count;
    size_t used;
    int mode;
    int total;
    int wanted;
    int recovered;
    memset(&fixture, 0, sizeof(fixture));
    fixture.parent_pid = (long)getpid();
    CHECK(pipe(fixture.notes) == 0 && pipe(fixture.block) == 0);
    CHECK(fcntl(fixture.notes[0], F_SETFL, O_NONBLOCK) == 0);
    total = 9;
#ifdef ROTMOS_PROCESS_BACKEND_TEST
    total = 13;
#endif
    for (mode = 0; mode < total; ++mode) {
        fixture.mode = mode;
        fixture.starts = fixture.restarts = fixture.exits = fixture.ready = 0;
        fixture.escalations = fixture.failure = 0;
        rotmos_server_config_init(&config);
        rotmos_listener_config_init(&listener);
        listener.bind = "127.0.0.1";
        config.listeners = &listener;
        config.listener_count = config.workers.count = 1;
        config.workers.affinity_disabled = config.http.backend_quiet = 1;
        config.workers.shutdown_timeout_ms = mode == 12 ? 500 : 40;
        config.workers.death_policy =
            mode == 1 ? ROTMOS_WORKER_TERMINATE : ROTMOS_WORKER_RESTART;
        config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
        config.callbacks.on_process_start = process_start;
        config.callbacks.on_worker_start = worker_start;
        config.callbacks.on_worker_stopping = stopping;
        config.callbacks.on_worker_exit = exited;
        config.callbacks.on_worker_event = observed;
        config.callbacks.on_parent_ready = ready;
        config.callbacks.userdata = &fixture;
        CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
        CHECK(rotmos_server_validate(server, &error) == ROTMOS_OK);
        status = rotmos_server_run(server, &result, &error);
        if (status != ROTMOS_OK)
            (void)fprintf(stderr, "mode %d: %s: %s\n", mode, error.operation,
                          error.message);
        CHECK(status ==
              (mode == 2 ? ROTMOS_IO
                         : (mode == 3 ? ROTMOS_INVALID
                                      : (mode == 10 ? ROTMOS_TIMEOUT
                                                    : (mode == 1 || mode == 7
                                                           ? ROTMOS_BACKEND
                                                           : ROTMOS_OK)))));
        CHECK(!fixture.failure);
        CHECK(result.reason ==
              (mode == 2 || mode == 3 || mode == 10
                   ? ROTMOS_RUN_STARTUP_FAILURE
                   : (mode == 1 || mode == 7 ? ROTMOS_RUN_BACKEND_FAILURE
                                             : ROTMOS_RUN_REQUESTED_STOP)));
        recovered = mode == 0 || mode == 8 || mode == 9;
        CHECK(result.recovered_failures.high == 0 &&
              result.recovered_failures.low == (unsigned long)recovered);
        CHECK(fixture.restarts == recovered);
        CHECK(fixture.exits == 1 + recovered);
        CHECK(fixture.ready ==
              !(mode == 2 || mode == 3 || mode == 5 || mode == 10));
        CHECK(result.escalated ==
              (mode == 6 || mode == 9 || mode == 10 || mode == 12));
        CHECK(result.has_failure == (mode == 0 || mode == 1 || mode == 2 ||
                                     mode == 3 || mode == 6 || mode == 7 ||
                                     mode == 8 || mode == 9 || mode == 10 ||
                                     mode == 12));
        if (result.has_failure) {
            CHECK(result.last_failure.process.process_id > 0);
            CHECK(result.last_failure.error.status ==
                  (mode == 2 ? ROTMOS_IO
                             : (mode == 3 ? ROTMOS_INVALID
                                          : (mode == 6 || mode == 9 ||
                                                     mode == 10 || mode == 12
                                                 ? ROTMOS_TIMEOUT
                                                 : ROTMOS_BACKEND))));
            CHECK(result.last_failure.abandoned ==
                  (mode != 7 && mode != 2 && mode != 3));
            if (mode == 2 || mode == 3)
                CHECK(strcmp(result.last_failure.error.message,
                             "expected child hook failure") == 0);
        }
        used = 0;
        while ((count = read(fixture.notes[0], notes + used,
                             sizeof(notes) - used)) > 0)
            used += (size_t)count;
        if (mode == 12)
            CHECK(strstr(result.last_failure.error.message,
                         "operation deadline") != NULL);
        wanted =
            mode == 1 || mode == 10 ? 0 : (mode == 6 || mode == 12 ? 1 : 2);
        CHECK(used == (size_t)wanted);
        if (wanted > 0)
            CHECK(notes[0] == 'S');
        if (wanted == 2)
            CHECK(notes[1] == 'X');
        CHECK(rotmos_server_destroy(server, NULL) == ROTMOS_OK);
    }
    CHECK(close(fixture.notes[0]) == 0 && close(fixture.notes[1]) == 0);
    CHECK(close(fixture.block[0]) == 0 && close(fixture.block[1]) == 0);
    return 0;
}

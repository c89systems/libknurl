#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
#include <stdio.h>
#include <string.h>
/* Test-only expiry seam. Compile the actual storage implementation so the
 * provider can expire its own timer before acknowledgement without sleeping. */
#include "../src/storage.c"
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "deadline %d: %s\n", __LINE__, #t);                \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct fixture {
    rotmos_server *server;
    rotmos_storage_provider *provider;
    int completed, interrupted, retired, cancelled, closed, failed;
};
#define VERIFY(f, t)                                                           \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "deadline callback %d: %s\n", __LINE__, #t);       \
            (f)->failed = 1;                                                   \
        }                                                                      \
    } while (0)
static rotmos_status invoke(rotmos_storage_operation *op,
                            const rotmos_storage_arguments *args, void *data,
                            rotmos_error *error)
{
    rotmos_storage_result result;
    (void)data;
    (void)args;
    /* Expire while the owner is in invoke, before the loop can dispatch its
     * timer; acknowledgement must still observe the absolute deadline. */
    if (op->timer != NULL)
        op->timer->due.high = op->timer->due.low = 0;
    if (args->action == ROTMOS_STORAGE_STAT)
        return rotmos_storage_operation_complete(op, NULL, error);
    rotmos_storage_result_init(&result);
    result.committed = 1;
    return rotmos_storage_operation_complete(op, &result, error);
}
static rotmos_status cancel(rotmos_storage_operation *op,
                            const rotmos_storage_arguments *args, void *data,
                            rotmos_error *error)
{
    struct fixture *f = data;
    (void)op;
    (void)args;
    (void)error;
    ++f->cancelled;
    return ROTMOS_OK;
}
static void closed(void *data)
{
    struct fixture *f = data;
    ++f->closed;
}
static void completed(rotmos_storage_operation *op,
                      const rotmos_storage_outcome *outcome, void *data)
{
    struct fixture *f = data;
    VERIFY(f,
           !strcmp(rotmos_storage_operation_arguments(op)->path, "/disabled") &&
               outcome->cause == ROTMOS_OK && !outcome->cancelled &&
               outcome->result->committed);
    ++f->completed;
}
static void interrupted(rotmos_storage_operation *op, rotmos_status cause,
                        void *data)
{
    struct fixture *f = data;
    VERIFY(f, strstr(rotmos_storage_operation_arguments(op)->path, "/timed") !=
                      NULL &&
                  cause == ROTMOS_TIMEOUT);
    ++f->interrupted;
}
static void retired(const rotmos_storage_outcome *outcome, void *data)
{
    struct fixture *f = data;
    VERIFY(f, outcome->cancelled
                  ? outcome->cause == ROTMOS_TIMEOUT
                  : outcome->cause == ROTMOS_OK && outcome->result->committed);
    if (++f->retired == 3) {
        VERIFY(f,
               rotmos_storage_provider_close(f->provider, NULL) == ROTMOS_OK);
        rotmos_server_request_stop(f->server, NULL);
    }
}
static rotmos_status start(rotmos_server *server,
                           const rotmos_process_context *process, void *data,
                           rotmos_error *error)
{
    struct fixture *f = data;
    rotmos_storage_provider_config provider;
    rotmos_storage_operation_config config;
    rotmos_storage_arguments args;
    rotmos_storage_operation *operation;
    rotmos_status status;
    (void)process;
    f->server = server;
    rotmos_storage_provider_config_init(&provider);
    provider.capabilities = ROTMOS_STORAGE_CAN_WRITE;
    provider.invoke = invoke;
    provider.cancel = cancel;
    provider.close = closed;
    provider.userdata = f;
    status = rotmos_storage_provider_new(&provider, &f->provider, error);
    if (status != ROTMOS_OK)
        return status;
    rotmos_storage_arguments_init(&args);
    args.action = ROTMOS_STORAGE_DELETE;
    args.path = "/timed";
    rotmos_storage_operation_config_init(&config);
    config.timeout_ms = 1;
    config.completed = completed;
    config.interrupted = interrupted;
    config.retired = retired;
    config.userdata = f;
    status =
        rotmos_storage_operation_start(f->provider, rotmos_server_loop(server),
                                       &args, &config, &operation, error);
    if (status != ROTMOS_OK)
        return status;
    args.path = "/disabled";
    config.timeout_disabled = 1;
    status =
        rotmos_storage_operation_start(f->provider, rotmos_server_loop(server),
                                       &args, &config, &operation, error);
    if (status != ROTMOS_OK)
        return status;
    args.path = "/timed-empty";
    args.action = ROTMOS_STORAGE_STAT;
    config.timeout_disabled = 0;
    return rotmos_storage_operation_start(f->provider,
                                          rotmos_server_loop(server), &args,
                                          &config, &operation, error);
}
static int exit_worker(rotmos_server *server,
                       const rotmos_process_context *process, void *data)
{
    struct fixture *f = data;
    (void)server;
    (void)process;
    return f->failed || f->completed != 1 || f->interrupted != 2 ||
           f->retired != 3 || f->cancelled != 0 || f->closed != 1;
}
int main(void)
{
    struct fixture f;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_server *server;
    rotmos_error error;
    rotmos_run_result result;
    memset(&f, 0, sizeof(f));
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.callbacks.on_worker_start = start;
    config.callbacks.on_worker_exit = exit_worker;
    config.callbacks.userdata = &f;
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    CHECK(rotmos_server_run(server, &result, &error) == ROTMOS_OK &&
          !result.escalated);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    return 0;
}

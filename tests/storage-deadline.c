#define _POSIX_C_SOURCE 200809L
#include <knurl/knurl.h>
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
    knurl_server *server;
    knurl_storage_provider *provider;
    int completed, interrupted, retired, cancelled, closed, failed;
};
#define VERIFY(f, t)                                                           \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "deadline callback %d: %s\n", __LINE__, #t);       \
            (f)->failed = 1;                                                   \
        }                                                                      \
    } while (0)
static knurl_status invoke(knurl_storage_operation *op,
                           const knurl_storage_arguments *args, void *data,
                           knurl_error *error)
{
    knurl_storage_result result;
    (void)data;
    (void)args;
    /* Expire while the owner is in invoke, before the loop can dispatch its
     * timer; acknowledgement must still observe the absolute deadline. */
    if (op->timer != NULL)
        op->timer->due.high = op->timer->due.low = 0;
    if (args->action == KNURL_STORAGE_STAT)
        return knurl_storage_operation_complete(op, NULL, error);
    knurl_storage_result_init(&result);
    result.committed = 1;
    return knurl_storage_operation_complete(op, &result, error);
}
static knurl_status cancel(knurl_storage_operation *op,
                           const knurl_storage_arguments *args, void *data,
                           knurl_error *error)
{
    struct fixture *f = data;
    (void)op;
    (void)args;
    (void)error;
    ++f->cancelled;
    return KNURL_OK;
}
static void closed(void *data)
{
    struct fixture *f = data;
    ++f->closed;
}
static void completed(knurl_storage_operation *op,
                      const knurl_storage_outcome *outcome, void *data)
{
    struct fixture *f = data;
    VERIFY(f,
           !strcmp(knurl_storage_operation_arguments(op)->path, "/disabled") &&
               outcome->cause == KNURL_OK && !outcome->cancelled &&
               outcome->result->committed);
    ++f->completed;
}
static void interrupted(knurl_storage_operation *op, knurl_status cause,
                        void *data)
{
    struct fixture *f = data;
    VERIFY(f, strstr(knurl_storage_operation_arguments(op)->path, "/timed") !=
                      NULL &&
                  cause == KNURL_TIMEOUT);
    ++f->interrupted;
}
static void retired(const knurl_storage_outcome *outcome, void *data)
{
    struct fixture *f = data;
    VERIFY(f, outcome->cancelled
                  ? outcome->cause == KNURL_TIMEOUT
                  : outcome->cause == KNURL_OK && outcome->result->committed);
    if (++f->retired == 3) {
        VERIFY(f, knurl_storage_provider_close(f->provider, NULL) == KNURL_OK);
        knurl_server_request_stop(f->server, NULL);
    }
}
static knurl_status start(knurl_server *server,
                          const knurl_process_context *process, void *data,
                          knurl_error *error)
{
    struct fixture *f = data;
    knurl_storage_provider_config provider;
    knurl_storage_operation_config config;
    knurl_storage_arguments args;
    knurl_storage_operation *operation;
    knurl_status status;
    (void)process;
    f->server = server;
    knurl_storage_provider_config_init(&provider);
    provider.capabilities = KNURL_STORAGE_CAN_WRITE;
    provider.invoke = invoke;
    provider.cancel = cancel;
    provider.close = closed;
    provider.userdata = f;
    status = knurl_storage_provider_new(&provider, &f->provider, error);
    if (status != KNURL_OK)
        return status;
    knurl_storage_arguments_init(&args);
    args.action = KNURL_STORAGE_DELETE;
    args.path = "/timed";
    knurl_storage_operation_config_init(&config);
    config.timeout_ms = 1;
    config.completed = completed;
    config.interrupted = interrupted;
    config.retired = retired;
    config.userdata = f;
    status =
        knurl_storage_operation_start(f->provider, knurl_server_loop(server),
                                      &args, &config, &operation, error);
    if (status != KNURL_OK)
        return status;
    args.path = "/disabled";
    config.timeout_disabled = 1;
    status =
        knurl_storage_operation_start(f->provider, knurl_server_loop(server),
                                      &args, &config, &operation, error);
    if (status != KNURL_OK)
        return status;
    args.path = "/timed-empty";
    args.action = KNURL_STORAGE_STAT;
    config.timeout_disabled = 0;
    return knurl_storage_operation_start(f->provider, knurl_server_loop(server),
                                         &args, &config, &operation, error);
}
static int exit_worker(knurl_server *server,
                       const knurl_process_context *process, void *data)
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
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_server *server;
    knurl_error error;
    knurl_run_result result;
    memset(&f, 0, sizeof(f));
    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;
    config.callbacks.on_worker_start = start;
    config.callbacks.on_worker_exit = exit_worker;
    config.callbacks.userdata = &f;
    CHECK(knurl_server_new(&config, &server, &error) == KNURL_OK);
    CHECK(knurl_server_run(server, &result, &error) == KNURL_OK &&
          !result.escalated);
    CHECK(knurl_server_destroy(server, &error) == KNURL_OK);
    return 0;
}

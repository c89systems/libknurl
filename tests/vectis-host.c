#include "vectis-host.h"
/* The baseline bridge's 100 ms readiness poll and 60 s auth-maintenance tick
 * are host adapters over the public loop. They contain no HTTP or queues. */
static void serving(rotmos_timer *timer, void *data)
{
    struct reference_kore_host *host = data;
    int ready = host->app_ready == NULL ||
                __sync_fetch_and_add(host->app_ready, 0) != 0;
    (void)timer;
    (void)rotmos_server_set_serving(host->app->server, ready, NULL);
}
static void maintenance(rotmos_timer *timer, void *data)
{
    struct reference_kore_host *host = data;
    (void)timer;
    host->auth_cleanup(host->app);
}
static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *process, void *data,
                           rotmos_error *error)
{
    struct reference_kore_host *host = data;
    rotmos_timer *timer;
    rotmos_status status = ROTMOS_OK;
    (void)process;
    if (host->app_ready != NULL)
        status = rotmos_loop_timer(rotmos_server_loop(server), 100, 100,
                                   serving, NULL, host, &timer, error);
    if (status == ROTMOS_OK && host->auth_cleanup != NULL)
        status = rotmos_loop_timer(rotmos_server_loop(server), 60000, 60000,
                                   maintenance, NULL, host, &timer, error);
    if (status == ROTMOS_OK && host->notify_ready != NULL)
        host->notify_ready(host->app, host->control_fd);
    return status;
}
static rotmos_status control(rotmos_server *server, unsigned int events,
                             void *data, rotmos_error *error)
{
    struct reference_kore_host *host = data;
    reference_error cause;
    reference_status status;
    int stop = 0;
    if (events & (ROTMOS_CONTROL_EOF | ROTMOS_CONTROL_ERROR))
        return rotmos_server_request_stop(server, error);
    reference_error_clear(&cause);
    status = host->control(host->app, host->control_fd, &stop, &cause);
    /* Baseline closes down on malformed/failed control input as on STOP. */
    return stop || status != REFERENCE_OK
               ? rotmos_server_request_stop(server, error)
               : ROTMOS_OK;
}
static int exited(rotmos_server *server, const rotmos_process_context *process,
                  void *data)
{
    struct reference_kore_host *host = data;
    reference_error error;
    reference_status status;
    (void)server;
    (void)process;
    reference_error_clear(&error);
    status = host->fork_exit != NULL ? host->fork_exit(host->app, "kore_worker",
                                                       host->userdata, &error)
                                     : REFERENCE_OK;
    /* The callback can free app, host and userdata. */
    return status == REFERENCE_OK ? 0 : 1;
}
static rotmos_status request_start(rotmos_server *server,
                                   rotmos_request *request, void *data,
                                   rotmos_error *error)
{
    struct reference_kore_host *host = data;
    return host->requests.on_request_start(server, request,
                                           host->requests.userdata, error);
}
static void request_close(rotmos_server *server, rotmos_request *request,
                          const rotmos_request_outcome *outcome, void *data)
{
    struct reference_kore_host *host = data;
    host->requests.on_request_close(server, request, outcome,
                                    host->requests.userdata);
}
void reference_host_configure(struct reference_kore_host *host,
                              rotmos_server_config *config)
{
    config->callbacks.on_parent_ready = ready;
    config->callbacks.on_worker_exit = exited;
    config->callbacks.userdata = host;
    reference_route_callbacks(host->app, &host->requests);
    config->callbacks.on_request_start = request_start;
    config->callbacks.on_request_close = request_close;
    config->start_paused = host->app_ready != NULL &&
                           __sync_fetch_and_add(host->app_ready, 0) == 0;
    if (host->control_fd >= 0) {
        config->supervision.enabled = 1;
        config->supervision.control_fd = host->control_fd;
        config->supervision.on_event = control;
        config->supervision.userdata = host;
    }
}
reference_status reference_host_run(struct reference_kore_host *host,
                                    reference_error *error)
{
    rotmos_error cause;
    rotmos_status status = rotmos_server_run(host->app->server, NULL, &cause);
    /* Vectis's existing outer child calls its kore_parent fork-exit hook after
     * this return; foreground run never calls that hook. */
    return reference_error_from_rotmos(status, &cause, error);
}

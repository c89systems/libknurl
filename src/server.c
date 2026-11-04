#define _POSIX_C_SOURCE 200809L
#include "server_internal.h"
#include "router_internal.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

int rotmos_server_running(const rotmos_server *server)
{
    return server->state == ROTMOS_SERVER_STARTING ||
           server->state == ROTMOS_SERVER_READY_PAUSED ||
           server->state == ROTMOS_SERVER_SERVING ||
           server->state == ROTMOS_SERVER_STOPPING;
}

static rotmos_status
supervision_validate(const rotmos_supervision_config *config,
                     rotmos_error *error)
{
    if (config->enabled &&
        (config->control_fd < 0 || config->on_event == NULL ||
         (config->interest & ~(ROTMOS_CONTROL_READ | ROTMOS_CONTROL_WRITE)) !=
             0 ||
         fcntl(config->control_fd, F_GETFD) == -1))
        return rotmos_fail(error, ROTMOS_INVALID, "server_supervision",
                           "supervision requires a valid descriptor, callback "
                           "and read/write interest");
    return ROTMOS_OK;
}

rotmos_status rotmos_server_new(const rotmos_server_config *input,
                                rotmos_server **out, rotmos_error *error)
{
    rotmos_server_config config;
    rotmos_timeout_config requested;
    size_t worker_count;
    rotmos_server *server;
    rotmos_status status;
    if (out != NULL)
        *out = NULL;
    if (out == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "server_new",
                           "server output is required");
    rotmos_server_config_init(&config);
    if (input != NULL)
        config = *input;
    requested = config.timeouts;
    worker_count = config.workers.count;
    status = rotmos_config_normalize(&config, error);
    if (status != ROTMOS_OK)
        return status;
    status = supervision_validate(&config.supervision, error);
    if (status != ROTMOS_OK)
        return status;
    server = malloc(sizeof(*server));
    if (server == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "server_new",
                           "cannot allocate server");
    status = rotmos_config_copy(&config, &server->config, error);
    if (status != ROTMOS_OK) {
        free(server);
        return status;
    }
    status = rotmos_router_new(&server->router, error);
    if (status != ROTMOS_OK) {
        rotmos_config_cleanup(&server->config);
        free(server);
        return status;
    }
    server->router->server = server;
    server->requested_timeouts = requested;
    server->requested_worker_count = worker_count;
    server->state = ROTMOS_SERVER_DECLARED;
    server->runtime = NULL;
    server->loop = NULL;
    server->proxy = NULL;
    server->notify_fd = -1;
    server->stop_requested = server->terminating_signal = 0;
    server->process.role = ROTMOS_PROCESS_PARENT;
    server->process.process_id = 0;
    server->process.worker_index = 0;
    server->process.restarted = 0;
    if (!server->config.supervision.enabled)
        server->config.supervision.control_fd = -1;
    *out = server;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_server_effective_config(const rotmos_server *server,
                                             rotmos_server_effective *out,
                                             rotmos_error *error)
{
    if (server == NULL || out == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "server_effective_config",
                           "server and output are required");
    out->config = server->config;
    out->requested_timeouts = server->requested_timeouts;
    out->requested_worker_count = server->requested_worker_count;
    out->max_header_fields = 23;
    out->backend_name = "kore";
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_server_state rotmos_server_get_state(const rotmos_server *server)
{
    return server != NULL ? server->state : ROTMOS_SERVER_FAILED;
}

rotmos_router *rotmos_server_router(rotmos_server *server)
{
    return server != NULL ? server->router : NULL;
}

rotmos_status rotmos_server_router_take(rotmos_server *server,
                                        rotmos_router *router,
                                        rotmos_error *error)
{
    if (server == NULL || router == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "server_router_take",
                           "server and unattached router are required");
    if (rotmos_server_running(server) || router->server != NULL ||
        router->frozen || server->router->count != 0)
        return rotmos_fail(
            error, ROTMOS_STATE, "server_router_take",
            "replace an empty router outside a run using an unattached router");
    rotmos_router_release(server->router);
    server->router = router;
    router->server = server;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status
rotmos_server_supervision_take(rotmos_server *server,
                               const rotmos_supervision_config *config,
                               rotmos_error *error)
{
    rotmos_status status;
    if (server == NULL || config == NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "server_supervision_take",
                           "server and supervision declaration are required");
    if (rotmos_server_running(server))
        return rotmos_fail(
            error, ROTMOS_STATE, "server_supervision_take",
            "supervision declaration cannot change during a run");
    status = supervision_validate(config, error);
    if (status != ROTMOS_OK)
        return status;
    if (server->config.supervision.enabled && config->enabled &&
        server->config.supervision.control_fd == config->control_fd)
        return rotmos_fail(error, ROTMOS_STATE, "server_supervision_take",
                           "descriptor is already owned by this server");
    if (server->config.supervision.enabled &&
        server->config.supervision.control_fd >= 0)
        (void)close(server->config.supervision.control_fd);
    server->config.supervision = *config;
    if (!config->enabled)
        server->config.supervision.control_fd = -1;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

rotmos_status rotmos_server_destroy(rotmos_server *server, rotmos_error *error)
{
    if (server != NULL) {
        if (rotmos_server_running(server))
            return rotmos_fail(
                error, ROTMOS_STATE, "server_destroy",
                "server is running; request stop and wait for run to return");
        rotmos_router_release(server->router);
        if (server->config.supervision.enabled &&
            server->config.supervision.control_fd >= 0)
            (void)close(server->config.supervision.control_fd);
        rotmos_config_cleanup(&server->config);
        free(server);
    }
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

#define _POSIX_C_SOURCE 200809L
#include "server_internal.h"
#include "router_internal.h"
#include "logging_internal.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

int knurl_server_running(const knurl_server *server)
{
    return server->state == KNURL_SERVER_STARTING ||
           server->state == KNURL_SERVER_READY_PAUSED ||
           server->state == KNURL_SERVER_SERVING ||
           server->state == KNURL_SERVER_STOPPING;
}

static knurl_status supervision_validate(const knurl_supervision_config *config,
                                         knurl_error *error)
{
    if (config->enabled &&
        (config->control_fd < 0 || config->on_event == NULL ||
         (config->interest & ~(KNURL_CONTROL_READ | KNURL_CONTROL_WRITE)) !=
             0 ||
         fcntl(config->control_fd, F_GETFD) == -1))
        return knurl_fail(error, KNURL_INVALID, "server_supervision",
                          "supervision requires a valid descriptor, callback "
                          "and read/write interest");
    return KNURL_OK;
}

knurl_status knurl_server_new(const knurl_server_config *input,
                              knurl_server **out, knurl_error *error)
{
    knurl_server_config config;
    knurl_timeout_config requested;
    size_t worker_count;
    knurl_server *server;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (out == NULL)
        return knurl_fail(error, KNURL_INVALID, "server_new",
                          "server output is required");
    knurl_server_config_init(&config);
    if (input != NULL)
        config = *input;
    requested = config.timeouts;
    worker_count = config.workers.count;
    status = knurl_config_normalize(&config, error);
    if (status != KNURL_OK)
        return status;
    status = supervision_validate(&config.supervision, error);
    if (status != KNURL_OK)
        return status;
    server = malloc(sizeof(*server));
    if (server == NULL)
        return knurl_fail(error, KNURL_NOMEM, "server_new",
                          "cannot allocate server");
    status = knurl_config_copy(&config, &server->config, error);
    if (status != KNURL_OK) {
        free(server);
        return status;
    }
    status = knurl_router_new(&server->router, error);
    if (status != KNURL_OK) {
        knurl_config_cleanup(&server->config);
        free(server);
        return status;
    }
    server->router->server = server;
    server->requested_timeouts = requested;
    server->requested_worker_count = worker_count;
    server->state = KNURL_SERVER_DECLARED;
    server->runtime = NULL;
    server->loop = NULL;
    server->proxy = NULL;
    server->autoblock = NULL;
    server->logging = NULL;
    server->notify_fd = -1;
    server->stop_requested = server->terminating_signal = 0;
    server->process.role = KNURL_PROCESS_PARENT;
    server->process.process_id = 0;
    server->process.worker_index = 0;
    server->process.restarted = 0;
    if (!server->config.supervision.enabled)
        server->config.supervision.control_fd = -1;
    *out = server;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_server_effective_config(const knurl_server *server,
                                           knurl_server_effective *out,
                                           knurl_error *error)
{
    if (server == NULL || out == NULL)
        return knurl_fail(error, KNURL_INVALID, "server_effective_config",
                          "server and output are required");
    out->config = server->config;
    out->requested_timeouts = server->requested_timeouts;
    out->requested_worker_count = server->requested_worker_count;
    out->max_header_fields = 23;
    out->backend_name = "kore";
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_server_state knurl_server_get_state(const knurl_server *server)
{
    return server != NULL ? server->state : KNURL_SERVER_FAILED;
}

knurl_router *knurl_server_router(knurl_server *server)
{
    return server != NULL ? server->router : NULL;
}

knurl_status knurl_server_router_take(knurl_server *server,
                                      knurl_router *router, knurl_error *error)
{
    if (server == NULL || router == NULL)
        return knurl_fail(error, KNURL_INVALID, "server_router_take",
                          "server and unattached router are required");
    if (knurl_server_running(server) || router->server != NULL ||
        router->frozen || server->router->count != 0)
        return knurl_fail(
            error, KNURL_STATE, "server_router_take",
            "replace an empty router outside a run using an unattached router");
    knurl_router_release(server->router);
    server->router = router;
    router->server = server;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status
knurl_server_supervision_take(knurl_server *server,
                              const knurl_supervision_config *config,
                              knurl_error *error)
{
    knurl_status status;
    if (server == NULL || config == NULL)
        return knurl_fail(error, KNURL_INVALID, "server_supervision_take",
                          "server and supervision declaration are required");
    if (knurl_server_running(server))
        return knurl_fail(error, KNURL_STATE, "server_supervision_take",
                          "supervision declaration cannot change during a run");
    status = supervision_validate(config, error);
    if (status != KNURL_OK)
        return status;
    if (server->config.supervision.enabled && config->enabled &&
        server->config.supervision.control_fd == config->control_fd)
        return knurl_fail(error, KNURL_STATE, "server_supervision_take",
                          "descriptor is already owned by this server");
    if (server->config.supervision.enabled &&
        server->config.supervision.control_fd >= 0)
        (void)close(server->config.supervision.control_fd);
    server->config.supervision = *config;
    if (!config->enabled)
        server->config.supervision.control_fd = -1;
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_server_destroy(knurl_server *server, knurl_error *error)
{
    if (server != NULL) {
        if (knurl_server_running(server))
            return knurl_fail(
                error, KNURL_STATE, "server_destroy",
                "server is running; request stop and wait for run to return");
        knurl_router_release(server->router);
        if (server->config.supervision.enabled &&
            server->config.supervision.control_fd >= 0)
            (void)close(server->config.supervision.control_fd);
        knurl_config_cleanup(&server->config);
        knurl_logging_clear(server);
        free(server);
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

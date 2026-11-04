#include "kore_internal.h"
#include <string.h>
struct connection_snapshot {
    rotmos_connection_info info;
    char peer[46], local[46];
};
static int snapshot(struct rotmos_runtime *runtime,
                    struct connection *connection,
                    struct connection_snapshot *out)
{
    size_t i;
    memset(out, 0, sizeof(*out));
    out->info.listener_index = (size_t)-1;
    if (connection == NULL || connection->owner == NULL)
        return 0;
    for (i = 0; i < runtime->bound_count; ++i)
        if (connection->owner->server == runtime->native_servers[i]) {
            out->info.listener_index = i;
            break;
        }
    if (out->info.listener_index == (size_t)-1)
        return 0;
    out->info.tls = connection->owner->server->tls;
    out->info.process = runtime->server->process;
    rotmos_kore_endpoint(connection->fd, 1, &out->info.peer, out->peer);
    rotmos_kore_endpoint(connection->fd, 0, &out->info.local, out->local);
    return 1;
}
static void observe(struct rotmos_runtime *runtime,
                    const rotmos_connection_info *info,
                    rotmos_observation_kind kind, int status,
                    rotmos_status cause)
{
    rotmos_observation event;
    if (runtime->server->config.callbacks.on_observe == NULL)
        return;
    memset(&event, 0, sizeof(event));
    event.connection = *info;
    event.kind = kind;
    event.status = status;
    event.cause = cause;
    runtime->server->config.callbacks.on_observe(
        runtime->server, &event, runtime->server->config.callbacks.userdata);
}
int rotmos_kore_accept(void *data, struct connection *connection)
{
    struct rotmos_runtime *runtime = data;
    struct connection_snapshot copy;
    rotmos_admission_action action = ROTMOS_ADMISSION_ALLOW;
    rotmos_status status = ROTMOS_OK;
    rotmos_error error;
    const rotmos_admission_config *config = &runtime->server->config.admission;
    if (!snapshot(runtime, connection, &copy))
        return 1;
    observe(runtime, &copy.info, ROTMOS_OBSERVE_CONNECTION_ACCEPTED, 0,
            ROTMOS_OK);
    if (config->on_connection != NULL)
        status = config->on_connection(runtime->server, &copy.info, &action,
                                       config->userdata, &error);
    if (status != ROTMOS_OK || action != ROTMOS_ADMISSION_ALLOW) {
        observe(runtime, &copy.info, ROTMOS_OBSERVE_REQUEST_REJECTED, 0,
                status != ROTMOS_OK ? status : ROTMOS_CANCELLED);
        return 0;
    }
    return 1;
}
static void event(void *data, struct connection *connection,
                  rotmos_observation_kind kind, int status, rotmos_status cause)
{
    struct rotmos_runtime *runtime = data;
    struct connection_snapshot copy;
    if (snapshot(runtime, connection, &copy))
        observe(runtime, &copy.info, kind, status, cause);
}
void rotmos_kore_tcp_stall(void *data, struct connection *connection)
{
    /* Vectis's hook also reports a peer closing before its first response;
     * it does not distinguish that case from a deadline. */
    event(data, connection, ROTMOS_OBSERVE_TCP_STALL, 0, ROTMOS_OK);
}
void rotmos_kore_tls_failure(void *data, struct connection *connection)
{
    event(data, connection, ROTMOS_OBSERVE_TLS_FAILURE, 0, ROTMOS_BACKEND);
}
void rotmos_kore_http_status(void *data, struct http_request *request,
                             int status)
{
    event(data, request->owner, ROTMOS_OBSERVE_HTTP_STATUS, status, ROTMOS_OK);
}
void rotmos_kore_connection_status(void *data, struct connection *connection,
                                   int status)
{
    event(data, connection, ROTMOS_OBSERVE_HTTP_STATUS, status, ROTMOS_OK);
}

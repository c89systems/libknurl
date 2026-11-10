#include "kore_internal.h"
#include "autoblock_internal.h"
#include "policy_internal.h"
#include <string.h>
struct connection_snapshot {
    knurl_connection_info info;
    char peer[46], local[46];
};
static int snapshot(struct knurl_runtime *runtime,
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
    knurl_kore_endpoint(connection->fd, 1, &out->info.peer, out->peer);
    knurl_kore_endpoint(connection->fd, 0, &out->info.local, out->local);
    return 1;
}
static void observe(struct knurl_runtime *runtime,
                    const knurl_connection_info *info,
                    knurl_observation_kind kind, int status, knurl_status cause)
{
    knurl_observation event;
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
int knurl_kore_accept(void *data, struct connection *connection)
{
    struct knurl_runtime *runtime = data;
    struct connection_snapshot copy;
    knurl_admission_action action = KNURL_ADMISSION_ALLOW;
    knurl_status status = KNURL_OK;
    knurl_error error;
    const knurl_admission_config *config = &runtime->server->config.admission;
    if (!snapshot(runtime, connection, &copy))
        return 1;
    observe(runtime, &copy.info, KNURL_OBSERVE_CONNECTION_ACCEPTED, 0,
            KNURL_OK);
    if (!knurl_autoblock_allowed(runtime->server, copy.info.peer.address))
        action = KNURL_ADMISSION_CLOSE;
    else if (config->on_connection != NULL)
        status = config->on_connection(runtime->server, &copy.info, &action,
                                       config->userdata, &error);
    if (status != KNURL_OK || action != KNURL_ADMISSION_ALLOW) {
        observe(runtime, &copy.info, KNURL_OBSERVE_REQUEST_REJECTED, 0,
                status != KNURL_OK ? status : KNURL_CANCELLED);
        return 0;
    }
    return 1;
}
static void event(void *data, struct connection *connection,
                  knurl_observation_kind kind, int status, knurl_status cause)
{
    struct knurl_runtime *runtime = data;
    struct connection_snapshot copy;
    if (snapshot(runtime, connection, &copy)) {
        knurl_autoblock_note(runtime->server, copy.info.peer.address, kind,
                             status, NULL);
        observe(runtime, &copy.info, kind, status, cause);
    }
}
void knurl_kore_tcp_stall(void *data, struct connection *connection)
{
    /* Vectis's hook also reports a peer closing before its first response;
     * it does not distinguish that case from a deadline. */
    event(data, connection, KNURL_OBSERVE_TCP_STALL, 0, KNURL_OK);
}
void knurl_kore_tls_failure(void *data, struct connection *connection)
{
    event(data, connection, KNURL_OBSERVE_TLS_FAILURE, 0, KNURL_BACKEND);
}
void knurl_kore_http_status(void *data, struct http_request *request,
                            int status)
{
    knurl_request *managed = request->hdlr_extra;
    if (managed != NULL && managed->lifecycle_started) {
        /* Managed final status is counted once at close. Native 100 Continue
         * still uses the already resolved request identity, never the proxy. */
        if (status < 200)
            knurl_autoblock_note(managed->server, managed->effective_address,
                                 KNURL_OBSERVE_HTTP_STATUS, status, NULL);
        knurl_policy_observe(managed, KNURL_OBSERVE_HTTP_STATUS, NULL, status,
                             KNURL_OK);
        return;
    }
    event(data, request->owner, KNURL_OBSERVE_HTTP_STATUS, status, KNURL_OK);
}
void knurl_kore_connection_status(void *data, struct connection *connection,
                                  int status)
{
    event(data, connection, KNURL_OBSERVE_HTTP_STATUS, status, KNURL_OK);
}

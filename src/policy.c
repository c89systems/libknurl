#include "policy_internal.h"
#include "server_internal.h"
#include "response_internal.h"
#include "loop_internal.h"
#include "autoblock_internal.h"
#include <stdlib.h>
#include <string.h>
void rotmos_policy_observe(rotmos_request *r, rotmos_observation_kind kind,
                           const char *name, int status, rotmos_status cause)
{
    rotmos_observation event;
    int busy;
    if (r->server->config.callbacks.on_observe == NULL || r->observing ||
        !r->lifecycle_started)
        return;
    memset(&event, 0, sizeof(event));
    event.kind = kind;
    event.request = r;
    event.connection.peer = r->peer;
    event.connection.local = r->local;
    event.connection.listener_index = r->listener;
    event.connection.tls = r->tls;
    event.connection.process = r->server->process;
    event.effective_address = r->effective_address;
    event.correlation_id = r->correlation_id;
    event.name = rotmos_view(name);
    event.status = status;
    event.cause = cause;
    event.committed = r->response->outcome.committed;
    event.disconnect_side = r->disconnect_side;
    if (kind == ROTMOS_OBSERVE_REQUEST_COMPLETE) {
        event.has_outcome = 1;
        event.outcome = r->outcome;
        event.input_counts_known = r->body_store != NULL;
    }
    busy = r->response->busy;
    r->response->busy = 1;
    r->observing = 1;
    r->server->config.callbacks.on_observe(
        r->server, &event, r->server->config.callbacks.userdata);
    r->observing = 0;
    r->response->busy = busy;
}
rotmos_status rotmos_policy_start(rotmos_request *r, rotmos_error *error)
{
    rotmos_status status;
    r->lifecycle_started = 1;
    r->response->busy = 1;
    status = rotmos_request_identity(r, error);
    r->response->busy = 0;
    if (status != ROTMOS_OK)
        return status;
    if (r->server->config.callbacks.on_request_start != NULL) {
        r->response->busy = 1;
        status = r->server->config.callbacks.on_request_start(
            r->server, r, r->server->config.callbacks.userdata, error);
        r->response->busy = 0;
        if (status != ROTMOS_OK)
            return status;
    }
    rotmos_policy_observe(r, ROTMOS_OBSERVE_REQUEST_STARTED, NULL, 0,
                          ROTMOS_OK);
    return ROTMOS_OK;
}
rotmos_status rotmos_request_event(rotmos_request *r, const char *name,
                                   rotmos_error *error)
{
    rotmos_status status;
    size_t i;
    if (r == NULL || name == NULL || name[0] == 0)
        return rotmos_fail(error, ROTMOS_INVALID, "request_event",
                           "request and event name are required");
    if (!r->lifecycle_started || r->close_notified || r->observing)
        return rotmos_fail(error, ROTMOS_STATE, "request_event",
                           "request is closed or observing");
    status = rotmos_loop_owner(rotmos_request_loop(r), error);
    if (status != ROTMOS_OK)
        return status;
    for (i = 0; name[i] != 0; ++i)
        if ((unsigned char)name[i] < 32 || (unsigned char)name[i] == 127)
            return rotmos_fail(error, ROTMOS_INVALID, "request_event",
                               "event name contains a control byte");
    rotmos_autoblock_note(r->server, r->effective_address,
                          ROTMOS_OBSERVE_APPLICATION, 0, name);
    rotmos_policy_observe(r, ROTMOS_OBSERVE_APPLICATION, name, 0, ROTMOS_OK);
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
rotmos_status rotmos_policy_admit(rotmos_request *r, int *handled,
                                  rotmos_error *error)
{
    const rotmos_admission_config *config = &r->server->config.admission;
    rotmos_reply *reply;
    rotmos_status status;
    rotmos_admission_action action = ROTMOS_ADMISSION_ALLOW;
    rotmos_bytes empty = {0};
    *handled = 0;
    if (!rotmos_autoblock_allowed(r->server, r->effective_address)) {
        *handled = 1;
        r->response->close_connection = 1;
        status =
            rotmos_response_bytes(r->response, 403, "text/plain", empty, error);
        if (status == ROTMOS_OK)
            rotmos_policy_observe(r, ROTMOS_OBSERVE_REQUEST_REJECTED, NULL, 403,
                                  ROTMOS_OK);
        return status;
    }
    if (config->on_request == NULL)
        return ROTMOS_OK;
    status = rotmos_reply_new(&reply, error);
    if (status != ROTMOS_OK)
        return status;
    r->response->busy = 1;
    status = config->on_request(r, reply, &action, config->userdata, error);
    r->response->busy = 0;
    if (status != ROTMOS_OK)
        goto done;
    if (action < ROTMOS_ADMISSION_ALLOW || action > ROTMOS_ADMISSION_CLOSE) {
        status = rotmos_fail(error, ROTMOS_INVALID, "admission",
                             "unknown admission action");
        goto done;
    }
    if (action == ROTMOS_ADMISSION_ALLOW)
        goto done;
    *handled = 1;
    r->response->close_connection = 1;
    if (action == ROTMOS_ADMISSION_CLOSE) {
        r->disconnect_side = ROTMOS_DISCONNECT_HOST;
        status = rotmos_response_abort(r->response, ROTMOS_CANCELLED, error);
        rotmos_policy_observe(r, ROTMOS_OBSERVE_REQUEST_REJECTED, NULL, 0,
                              ROTMOS_CANCELLED);
        goto done;
    }
    if (!reply->selected)
        status = rotmos_reply_status(reply, 403, error);
    if (status == ROTMOS_OK &&
        (reply->kind == ROTMOS_REPLY_FILE ||
         reply->kind == ROTMOS_REPLY_SOURCE ||
         reply->body_length > r->server->config.io.output_queue_bytes))
        status = rotmos_fail(
            error, ROTMOS_LIMIT, "admission",
            "rejection requires bounded copied bytes/status/headers");
    if (status == ROTMOS_OK)
        status = rotmos_response_submit(r->response, reply, error);
    if (status == ROTMOS_OK) {
        r->response->complete_reply = 1;
        r->response->phase = ROTMOS_RESPONSE_FINISHING;
        rotmos_policy_observe(r, ROTMOS_OBSERVE_REQUEST_REJECTED, NULL,
                              r->response->outcome.status, ROTMOS_OK);
    }
done:
    rotmos_reply_destroy(reply, NULL);
    return status;
}
void rotmos_policy_closed(rotmos_request *r)
{
    if (!r->lifecycle_started)
        return;
    rotmos_autoblock_note(r->server, r->effective_address,
                          ROTMOS_OBSERVE_HTTP_STATUS,
                          r->outcome.response.status, NULL);
    rotmos_policy_observe(r, ROTMOS_OBSERVE_REQUEST_COMPLETE, NULL,
                          r->outcome.response.status, r->outcome.cause);
    if (r->server->config.callbacks.on_request_close != NULL)
        r->server->config.callbacks.on_request_close(
            r->server, r, &r->outcome, r->server->config.callbacks.userdata);
}

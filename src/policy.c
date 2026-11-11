#include "policy_internal.h"
#include "server_internal.h"
#include "response_internal.h"
#include "loop_internal.h"
#include "autoblock_internal.h"
#include <stdlib.h>
#include <string.h>
void knurl_policy_observe(knurl_request *r, knurl_observation_kind kind,
                          const char *name, int status, knurl_status cause)
{
    knurl_observation event;
    int busy;
    if (r->impl->server->impl->config.callbacks.on_observe == NULL ||
        r->impl->observing || !r->impl->lifecycle_started)
        return;
    memset(&event, 0, sizeof(event));
    event.kind = kind;
    event.request = r;
    event.connection.peer = r->impl->peer;
    event.connection.local = r->impl->local;
    event.connection.listener_index = r->impl->listener;
    event.connection.tls = r->impl->tls;
    event.connection.process = r->impl->server->impl->process;
    event.effective_address = r->impl->effective_address;
    event.correlation_id = r->impl->correlation_id;
    event.name = knurl_view(name);
    event.status = status;
    event.cause = cause;
    event.committed = r->impl->response->impl->outcome.committed;
    event.disconnect_side = r->impl->disconnect_side;
    if (kind == KNURL_OBSERVE_REQUEST_COMPLETE) {
        event.has_outcome = 1;
        event.outcome = r->impl->outcome;
        event.input_counts_known = r->impl->body_store != NULL;
    }
    busy = r->impl->response->impl->busy;
    r->impl->response->impl->busy = 1;
    r->impl->observing = 1;
    r->impl->server->impl->config.callbacks.on_observe(
        r->impl->server, &event,
        r->impl->server->impl->config.callbacks.userdata);
    r->impl->observing = 0;
    r->impl->response->impl->busy = busy;
}
knurl_status knurl_policy_start(knurl_request *r, knurl_error *error)
{
    knurl_status status;
    r->impl->lifecycle_started = 1;
    r->impl->response->impl->busy = 1;
    status = knurl_request_identity(r, error);
    r->impl->response->impl->busy = 0;
    if (status != KNURL_OK)
        return status;
    if (r->impl->server->impl->config.callbacks.on_request_start != NULL) {
        r->impl->response->impl->busy = 1;
        status = r->impl->server->impl->config.callbacks.on_request_start(
            r->impl->server, r,
            r->impl->server->impl->config.callbacks.userdata, error);
        r->impl->response->impl->busy = 0;
        if (status != KNURL_OK)
            return status;
    }
    knurl_policy_observe(r, KNURL_OBSERVE_REQUEST_STARTED, NULL, 0, KNURL_OK);
    return KNURL_OK;
}
knurl_status knurl_request_event(knurl_request *r, const char *name,
                                 knurl_error *error)
{
    knurl_status status;
    size_t i;
    if (r == NULL || name == NULL || name[0] == 0)
        return knurl_fail(error, KNURL_INVALID, "request_event",
                          "request and event name are required");
    if (!r->impl->lifecycle_started || r->impl->close_notified ||
        r->impl->observing)
        return knurl_fail(error, KNURL_STATE, "request_event",
                          "request is closed or observing");
    status = knurl_loop_owner(knurl_request_loop(r), error);
    if (status != KNURL_OK)
        return status;
    for (i = 0; name[i] != 0; ++i)
        if ((unsigned char)name[i] < 32 || (unsigned char)name[i] == 127)
            return knurl_fail(error, KNURL_INVALID, "request_event",
                              "event name contains a control byte");
    knurl_autoblock_note(r->impl->server, r->impl->effective_address,
                         KNURL_OBSERVE_APPLICATION, 0, name, r, NULL);
    knurl_policy_observe(r, KNURL_OBSERVE_APPLICATION, name, 0, KNURL_OK);
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_policy_admit(knurl_request *r, int *handled,
                                knurl_error *error)
{
    const knurl_admission_config *config =
        &r->impl->server->impl->config.admission;
    knurl_reply *reply;
    knurl_status status;
    knurl_admission_action action = KNURL_ADMISSION_ALLOW;
    knurl_bytes empty = {0};
    *handled = 0;
    if (!knurl_autoblock_allowed(r->impl->server, r->impl->effective_address)) {
        *handled = 1;
        r->impl->response->impl->close_connection = 1;
        status = knurl_response_bytes(r->impl->response, 403, "text/plain",
                                      empty, error);
        if (status == KNURL_OK)
            knurl_policy_observe(r, KNURL_OBSERVE_REQUEST_REJECTED, NULL, 403,
                                 KNURL_OK);
        return status;
    }
    if (config->on_request == NULL)
        return KNURL_OK;
    status = knurl_reply_new(&reply, error);
    if (status != KNURL_OK)
        return status;
    r->impl->response->impl->busy = 1;
    status = config->on_request(r, reply, &action, config->userdata, error);
    r->impl->response->impl->busy = 0;
    if (status != KNURL_OK)
        goto done;
    if (action < KNURL_ADMISSION_ALLOW || action > KNURL_ADMISSION_CLOSE) {
        status = knurl_fail(error, KNURL_INVALID, "admission",
                            "unknown admission action");
        goto done;
    }
    if (action == KNURL_ADMISSION_ALLOW)
        goto done;
    *handled = 1;
    r->impl->response->impl->close_connection = 1;
    if (action == KNURL_ADMISSION_CLOSE) {
        r->impl->disconnect_side = KNURL_DISCONNECT_HOST;
        status =
            knurl_response_abort(r->impl->response, KNURL_CANCELLED, error);
        knurl_policy_observe(r, KNURL_OBSERVE_REQUEST_REJECTED, NULL, 0,
                             KNURL_CANCELLED);
        goto done;
    }
    if (!reply->impl->selected)
        status = knurl_reply_status(reply, 403, error);
    if (status == KNURL_OK &&
        (reply->impl->kind == KNURL_REPLY_FILE ||
         reply->impl->kind == KNURL_REPLY_SOURCE ||
         reply->impl->body_length >
             r->impl->server->impl->config.io.output_queue_bytes))
        status = knurl_fail(
            error, KNURL_LIMIT, "admission",
            "rejection requires bounded copied bytes/status/headers");
    if (status == KNURL_OK)
        status = knurl_response_submit(r->impl->response, reply, error);
    if (status == KNURL_OK) {
        r->impl->response->impl->complete_reply = 1;
        r->impl->response->impl->phase = KNURL_RESPONSE_FINISHING;
        knurl_policy_observe(r, KNURL_OBSERVE_REQUEST_REJECTED, NULL,
                             r->impl->response->impl->outcome.status, KNURL_OK);
    }
done:
    knurl_reply_destroy(reply, NULL);
    return status;
}
void knurl_policy_closed(knurl_request *r)
{
    if (!r->impl->lifecycle_started)
        return;
    knurl_autoblock_note(r->impl->server, r->impl->effective_address,
                         KNURL_OBSERVE_HTTP_STATUS,
                         r->impl->outcome.response.status, NULL, r, NULL);
    knurl_policy_observe(r, KNURL_OBSERVE_REQUEST_COMPLETE, NULL,
                         r->impl->outcome.response.status,
                         r->impl->outcome.cause);
    if (r->impl->server->impl->config.callbacks.on_request_close != NULL)
        r->impl->server->impl->config.callbacks.on_request_close(
            r->impl->server, r, &r->impl->outcome,
            r->impl->server->impl->config.callbacks.userdata);
}

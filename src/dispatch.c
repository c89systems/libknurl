#include "request_internal.h"
#include "response_internal.h"
#include "server_internal.h"

void knurl_request_plan_init(knurl_request_plan *plan)
{
    if (plan == NULL)
        return;
    plan->disposition = KNURL_REQUEST_MANAGED;
    knurl_body_policy_init(&plan->body);
    plan->on_body = NULL;
    plan->on_body_end = NULL;
    plan->on_writable = NULL;
    plan->on_close = NULL;
    plan->userdata = NULL;
}

void knurl_dispatch_config_init(knurl_dispatch_config *config)
{
    if (config == NULL)
        return;
    config->mode = KNURL_DISPATCH_ROUTER;
    config->on_headers = NULL;
    config->userdata = NULL;
}

static knurl_status facade_complete(knurl_request *request, void *userdata,
                                    knurl_error *error)
{
    knurl_status status;
    knurl_reply *reply;
    knurl_reply *failure;
    knurl_response *response;
    int code;
    int handler_failed;
    (void)userdata;
    status = knurl_reply_new(&request->impl->reply, error);
    if (status != KNURL_OK)
        return status;
    reply = request->impl->reply;
    reply->impl->owner = request;
    response = request->impl->response;
    status = knurl_router_invoke(request, error);
    handler_failed = status != KNURL_OK;
    if (status == KNURL_OK) {
        if (response->impl->phase == KNURL_RESPONSE_BUILDING)
            status = knurl_response_submit(response, reply, error);
        else if (reply->impl->selected)
            status = KNURL_CONFLICT;
    }
    if (status == KNURL_OK)
        return status;
    request->impl->outcome.cause = status;
    code = status == KNURL_INVALID
               ? 400
               : (status == KNURL_NOT_SUPPORTED ? 501 : 500);
    if (knurl_reply_new(&failure, NULL) == KNURL_OK) {
        if (knurl_response_replace_uncommitted(response, NULL) == KNURL_OK &&
            knurl_reply_failure(failure, code,
                                handler_failed && error != NULL
                                    ? error->message
                                    : NULL) == KNURL_OK &&
            knurl_response_submit(response, failure, NULL) == KNURL_OK) {
            (void)knurl_reply_destroy(failure, NULL);
            return KNURL_OK;
        }
        (void)knurl_reply_destroy(failure, NULL);
    }
    (void)knurl_response_abort(response, status, NULL);
    return status;
}

knurl_status knurl_router_dispatch_headers(knurl_router *router,
                                           knurl_request *request,
                                           knurl_request_plan *plan,
                                           knurl_error *error)
{
    knurl_status status;
    if (router == NULL || request == NULL || plan == NULL ||
        request->impl->response == NULL || !request->impl->dispatch_started)
        return knurl_fail(error, KNURL_INVALID, "router_dispatch_headers",
                          "provide a live request, router and initialized "
                          "plan during headers dispatch");
    if (request->impl->router_dispatched)
        return knurl_fail(error, KNURL_STATE, "router_dispatch_headers",
                          "this request was already routed");
    request->impl->router_dispatched = 1;
    status = knurl_router_select(router, request, error);
    if (status != KNURL_OK) {
        if (status == KNURL_EOF)
            request->impl->rejection_status = 404;
        return status;
    }
    plan->body = request->impl->route->impl->config.body;
    if (request->impl->route->impl->preflight != NULL) {
        status = request->impl->route->impl->preflight(
            request, plan, request->impl->route->impl->config.userdata, error);
        if (status != KNURL_OK || plan->disposition != KNURL_REQUEST_MANAGED)
            return status;
    }
    if (request->impl->route->impl->config.on_headers != NULL)
        return request->impl->route->impl->config.on_headers(
            request, plan, request->impl->route->impl->config.userdata, error);
    plan->on_body_end = facade_complete;
    return KNURL_OK;
}

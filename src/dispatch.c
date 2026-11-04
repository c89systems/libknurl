#include "request_internal.h"
#include "response_internal.h"
#include "server_internal.h"

void rotmos_request_plan_init(rotmos_request_plan *plan)
{
    if (plan == NULL)
        return;
    plan->disposition = ROTMOS_REQUEST_MANAGED;
    rotmos_body_policy_init(&plan->body);
    plan->on_body = NULL;
    plan->on_body_end = NULL;
    plan->on_writable = NULL;
    plan->on_close = NULL;
    plan->userdata = NULL;
}

void rotmos_dispatch_config_init(rotmos_dispatch_config *config)
{
    if (config == NULL)
        return;
    config->mode = ROTMOS_DISPATCH_ROUTER;
    config->on_headers = NULL;
    config->userdata = NULL;
}

static rotmos_status facade_complete(rotmos_request *request, void *userdata,
                                     rotmos_error *error)
{
    rotmos_status status;
    rotmos_reply *reply;
    rotmos_response *response;
    int code;
    (void)userdata;
    status = rotmos_reply_new(&request->reply, error);
    if (status != ROTMOS_OK)
        return status;
    reply = request->reply;
    reply->owner = request;
    response = request->response;
    status = rotmos_router_invoke(request, error);
    if (status == ROTMOS_OK) {
        if (response->phase == ROTMOS_RESPONSE_BUILDING)
            status = rotmos_response_submit(response, reply, error);
        else if (reply->selected)
            status = ROTMOS_CONFLICT;
    }
    if (status == ROTMOS_OK)
        return status;
    request->outcome.cause = status;
    code = status == ROTMOS_INVALID
               ? 400
               : (status == ROTMOS_NOT_SUPPORTED ? 501 : 500);
    if (rotmos_response_replace_uncommitted(response, NULL) == ROTMOS_OK) {
        (void)rotmos_reply_status(reply, code, NULL);
        if (rotmos_response_submit(response, reply, error) == ROTMOS_OK)
            return ROTMOS_OK;
    }
    (void)rotmos_response_abort(response, status, NULL);
    return status;
}

rotmos_status rotmos_router_dispatch_headers(rotmos_router *router,
                                             rotmos_request *request,
                                             rotmos_request_plan *plan,
                                             rotmos_error *error)
{
    rotmos_status status;
    if (router == NULL || request == NULL || plan == NULL ||
        request->response == NULL || !request->dispatch_started)
        return rotmos_fail(error, ROTMOS_INVALID, "router_dispatch_headers",
                           "provide a live request, router and initialized "
                           "plan during headers dispatch");
    if (request->router_dispatched)
        return rotmos_fail(error, ROTMOS_STATE, "router_dispatch_headers",
                           "this request was already routed");
    request->router_dispatched = 1;
    status = rotmos_router_select(router, request, error);
    if (status != ROTMOS_OK) {
        if (status == ROTMOS_EOF)
            request->rejection_status = 404;
        return status;
    }
    plan->body = request->route->config.body;
    if (request->route->preflight != NULL) {
        status = request->route->preflight(
            request, plan, request->route->config.userdata, error);
        if (status != ROTMOS_OK || plan->disposition != ROTMOS_REQUEST_MANAGED)
            return status;
    }
    if (request->route->config.on_headers != NULL)
        return request->route->config.on_headers(
            request, plan, request->route->config.userdata, error);
    plan->on_body_end = facade_complete;
    return ROTMOS_OK;
}

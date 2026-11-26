#include "request_internal.h"
#include "response_internal.h"
#include "loop_internal.h"
#include "kore_internal.h"
#include "auth_internal.h"

#include <string.h>

static knurl_status fail(knurl_error *error, knurl_status status,
                         const char *message)
{
    return knurl_fail(error, status, "route_hooks", message);
}

static knurl_status attach(knurl_router *router, knurl_async_route_hooks *out,
                           const knurl_async_route_hooks *hooks,
                           knurl_error *error)
{
    if (router->impl->frozen)
        return fail(error, KNURL_STATE, "configure async hooks while inactive");
    if (hooks != NULL)
        *out = *hooks;
    else
        memset(out, 0, sizeof(*out));
    knurl_error_clear(error);
    return KNURL_OK;
}

knurl_status knurl_route_set_async_hooks(knurl_route *route,
                                         const knurl_async_route_hooks *hooks,
                                         knurl_error *error)
{
    if (route == NULL)
        return fail(error, KNURL_INVALID, "route required");
    return attach(route->impl->router, &route->impl->async_hooks, hooks, error);
}

knurl_status
knurl_route_group_set_async_hooks(knurl_route_group *group,
                                  const knurl_async_route_hooks *hooks,
                                  knurl_error *error)
{
    if (group == NULL)
        return fail(error, KNURL_INVALID, "route group required");
    return attach(group->impl->router, &group->impl->async_hooks, hooks, error);
}

static const knurl_async_route_hooks *group_hooks(const knurl_route *route)
{
    static const knurl_async_route_hooks none = {0};
    knurl_route_group *group = route->impl->group;
    return group != NULL ? &group->impl->async_hooks : &none;
}

void knurl_request_hook_cancel(knurl_request *request)
{
    void (*cancel)(void *) = request->impl->hook_cancel;
    void *data = request->impl->hook_userdata;
    request->impl->hook_cancel = NULL;
    request->impl->hook_userdata = NULL;
    request->impl->hook_stage = KNURL_HOOK_NONE;
    if (cancel != NULL)
        cancel(data);
}

knurl_status knurl_request_hook_wait(knurl_request *request,
                                     void (*cancel)(void *), void *data,
                                     knurl_error *error)
{
    knurl_status status;
    if (request == NULL || cancel == NULL)
        return fail(error, KNURL_INVALID,
                    "request and cancel callback required");
    status = knurl_loop_owner(knurl_request_loop(request), error);
    if (status != KNURL_OK)
        return status;
    if (!request->impl->hook_invoking ||
        request->impl->hook_stage == KNURL_HOOK_NONE ||
        request->impl->hook_cancel != NULL || request->impl->close_notified)
        return fail(error, KNURL_STATE, "async hook is not installing a wait");
    request->impl->hook_cancel = cancel;
    request->impl->hook_userdata = data;
    knurl_error_clear(error);
    return KNURL_OK;
}

static knurl_status invoke(knurl_request *request,
                           const knurl_async_route_hooks *hooks,
                           knurl_async_route_hook_fn callback,
                           unsigned int stage, knurl_error *error)
{
    knurl_response *response = request->impl->response;
    knurl_status status;
    if (callback == NULL)
        return KNURL_OK;
    request->impl->hook_stage = stage;
    request->impl->hook_invoking = 1;
    status = callback(
        request, stage == KNURL_HOOK_AFTER ? response->impl->headers : NULL,
        stage == KNURL_HOOK_AFTER ? response->impl->outcome.status : 0,
        hooks->userdata, error);
    request->impl->hook_invoking = 0;
    if (status == KNURL_AGAIN && request->impl->hook_cancel != NULL)
        return KNURL_AGAIN;
    knurl_request_hook_cancel(request);
    if (status == KNURL_LIMIT && stage == KNURL_HOOK_ADMISSION)
        request->impl->rejection_status = 503;
    return status == KNURL_AGAIN
               ? fail(error, KNURL_STATE,
                      "async hook returned AGAIN without a wait")
               : status;
}

knurl_status knurl_request_hooks_admit(knurl_request *request,
                                       knurl_error *error)
{
    const knurl_async_route_hooks *local =
        &request->impl->route->impl->async_hooks;
    const knurl_async_route_hooks *group = group_hooks(request->impl->route);
    const knurl_async_route_hooks *hooks;
    knurl_async_route_hook_fn callback;
    unsigned int step;
    knurl_status status;
    if (group->auth == NULL && group->before == NULL && local->auth == NULL &&
        local->before == NULL) {
        request->impl->hook_admission_complete = 1;
        return KNURL_OK;
    }
    while (request->impl->hook_before < 4) {
        step = request->impl->hook_before++;
        hooks = step % 2 == 0 ? group : local;
        callback = step < 2 ? hooks->auth : hooks->before;
        status = invoke(request, hooks, callback, KNURL_HOOK_ADMISSION, error);
        if (status != KNURL_OK)
            return status;
    }
    request->impl->hook_admission_complete = 1;
    return KNURL_OK;
}

knurl_status knurl_request_proxy_admit(knurl_request *request,
                                       knurl_error *error)
{
    const knurl_async_route_hooks *hooks =
        &request->impl->route->impl->proxy_hooks;
    if (request->impl->proxy_admission_invoked || hooks->before == NULL)
        return KNURL_OK;
    request->impl->proxy_admission_invoked = 1;
    return invoke(request, hooks, hooks->before, KNURL_HOOK_ADMISSION, error);
}

knurl_status knurl_request_proxy_authenticate(knurl_request *request,
                                              knurl_oauth2_client *client,
                                              knurl_error *error)
{
    knurl_async_route_hooks native;
    if (client == NULL || request->impl->proxy_authorization != NULL ||
        request->impl->proxy_token_failure != KNURL_OK)
        return KNURL_OK;
    memset(&native, 0, sizeof(native));
    native.userdata = client;
    return invoke(request, &native, knurl_oauth2_proxy_admit,
                  KNURL_HOOK_ADMISSION, error);
}

int knurl_request_has_after_hooks(const knurl_request *request)
{
    const knurl_route *route = request->impl->route;
    return route != NULL && request->impl->hook_admission_complete &&
           request->impl->hook_after < KNURL_AFTER_HOOK_COUNT &&
           (route->impl->proxy_hooks.after != NULL ||
            route->impl->async_hooks.after != NULL ||
            group_hooks(route)->after != NULL);
}

knurl_status knurl_request_hooks_after(knurl_request *request,
                                       knurl_error *error)
{
    const knurl_async_route_hooks *hooks;
    unsigned int step;
    knurl_status status;
    if (request->impl->hook_cancel != NULL)
        return KNURL_AGAIN;
    while (request->impl->hook_after < KNURL_AFTER_HOOK_COUNT) {
        step = request->impl->hook_after++;
        hooks = step == 0   ? &request->impl->route->impl->proxy_hooks
                : step == 1 ? &request->impl->route->impl->async_hooks
                            : group_hooks(request->impl->route);
        status = invoke(request, hooks, hooks->after, KNURL_HOOK_AFTER, error);
        if (status != KNURL_OK)
            return status;
    }
    return KNURL_OK;
}

knurl_status knurl_request_hook_resume(knurl_request *request,
                                       knurl_status status, knurl_reply *reply,
                                       const knurl_error *cause,
                                       knurl_error *error)
{
    unsigned int stage;
    knurl_response *response;
    knurl_status owner;
    int rejected;
    if (request == NULL || status < KNURL_OK || status > KNURL_BACKEND ||
        status == KNURL_AGAIN || status == KNURL_EOF)
        return fail(error, KNURL_INVALID,
                    "request and terminal hook status required");
    owner = knurl_loop_owner(knurl_request_loop(request), error);
    if (owner != KNURL_OK)
        return owner;
    if (request->impl->hook_cancel == NULL || request->impl->hook_invoking ||
        request->impl->close_notified)
        return fail(error, KNURL_STATE, "async hook does not have a live wait");
    response = request->impl->response;
    stage = request->impl->hook_stage;
    request->impl->hook_stage = KNURL_HOOK_NONE;
    request->impl->hook_cancel = NULL;
    request->impl->hook_userdata = NULL;
    rejected = stage == KNURL_HOOK_ADMISSION && knurl_reply_is_selected(reply);
    if (status == KNURL_OK && rejected) {
        status = knurl_response_submit(response, reply, error);
        if (status == KNURL_OK) {
            response->impl->complete_reply = 1;
            response->impl->phase = KNURL_RESPONSE_FINISHING;
        }
    } else if (status == KNURL_OK && stage == KNURL_HOOK_AFTER && reply != NULL)
        status = knurl_response_hook_edits(response, reply, error);
    if (status != KNURL_OK) {
        request->impl->hook_after = KNURL_AFTER_HOOK_COUNT;
        if (cause != NULL && cause->status == status && status != KNURL_TIMEOUT)
            knurl_request_log_handler_error(request, cause);
        if (stage == KNURL_HOOK_AFTER) {
            if (request->impl->outcome.cause == KNURL_OK)
                request->impl->outcome.cause = status;
            knurl_request_stop_input(request, status);
            knurl_response_error(response, status,
                                 status == KNURL_TIMEOUT ? 504 : 500);
        } else
            knurl_kore_request_fail(request, status,
                                    status == KNURL_TIMEOUT ? 504 : 500);
    } else if (stage == KNURL_HOOK_ADMISSION)
        knurl_kore_headers_resume(request, !rejected);
    else
        response->impl->schedule(response);
    knurl_error_clear(error);
    return KNURL_OK;
}

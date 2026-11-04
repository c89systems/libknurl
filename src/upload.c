#include <rotmos/upload.h>
#include "request_internal.h"
#include "response_internal.h"
#include <stdlib.h>
#include <string.h>
struct upload {
    rotmos_upload_stream_config config;
    void *state;
    int opened;
};
void rotmos_upload_stream_config_init(rotmos_upload_stream_config *config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    rotmos_body_policy_init_upload(&config->body);
}
static rotmos_status open_upload(rotmos_request *request, struct upload *s,
                                 rotmos_error *error)
{
    rotmos_status status;
    if (s->opened)
        return ROTMOS_OK;
    status = s->config.open != NULL
                 ? s->config.open(request, s->config.userdata, &s->state, error)
                 : ROTMOS_OK;
    if (status == ROTMOS_OK)
        s->opened = 1;
    else if (status == ROTMOS_AGAIN || status == ROTMOS_EOF)
        status = rotmos_fail(error, ROTMOS_INVALID, "upload_open",
                             "open must return a terminal status");
    return status;
}
static rotmos_status body(rotmos_request *request, const void *data,
                          size_t length, size_t *consumed, void *userdata,
                          rotmos_error *error)
{
    struct upload *s = request->codec_data;
    rotmos_status status;
    (void)userdata;
    *consumed = 0;
    status = open_upload(request, s, error);
    if (status == ROTMOS_OK)
        status = s->config.write(request, data, length, consumed, s->state,
                                 s->config.userdata, error);
    if ((status == ROTMOS_OK && (*consumed == 0 || *consumed > length)) ||
        (status != ROTMOS_OK && *consumed != 0))
        return rotmos_fail(
            error, ROTMOS_INVALID, "upload_write",
            "write must consume a positive prefix or AGAIN with zero");
    return status;
}
static rotmos_status submit(rotmos_request *request, rotmos_error *error)
{
    rotmos_status status =
        rotmos_response_submit(request->response, request->reply, error);
    if (status == ROTMOS_OK && request->in_headers) {
        request->response->complete_reply = 1;
        request->response->phase = ROTMOS_RESPONSE_FINISHING;
    }
    return status;
}
static rotmos_status finish(rotmos_request *request, void *userdata,
                            rotmos_error *error)
{
    struct upload *s = request->codec_data;
    rotmos_status status;
    (void)userdata;
    status = open_upload(request, s, error);
    if (status == ROTMOS_OK)
        status = s->config.finish(request, request->reply, s->state,
                                  s->config.userdata, error);
    if (status == ROTMOS_OK && !request->reply->selected)
        status = rotmos_fail(error, ROTMOS_STATE, "upload_finish",
                             "finish must select a reply");
    if (status == ROTMOS_OK)
        status = rotmos_router_hooks_after(request, error);
    if (status == ROTMOS_OK)
        status = submit(request, error);
    return status;
}
static void closed(rotmos_request *request,
                   const rotmos_request_outcome *outcome, void *userdata)
{
    struct upload *s = request->codec_data;
    rotmos_status cause = outcome->cause;
    (void)userdata;
    if (cause == ROTMOS_OK)
        cause = outcome->body_cause;
    if (cause == ROTMOS_OK)
        cause = outcome->response.cause;
    if (s->opened && s->config.close != NULL)
        s->config.close(request, cause, s->state, s->config.userdata);
}
static rotmos_status headers(rotmos_request *request, rotmos_request_plan *plan,
                             void *userdata, rotmos_error *error)
{
    struct upload *s;
    rotmos_status status;
    int stopped;
    status = rotmos_reply_new(&request->reply, error);
    if (status != ROTMOS_OK)
        return status;
    request->reply->owner = request;
    status = rotmos_router_hooks_before(request, &stopped, error);
    if (status != ROTMOS_OK)
        return status;
    if (stopped) {
        plan->disposition = ROTMOS_REQUEST_REPLY;
        return submit(request, error);
    }
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "upload",
                           "allocating upload context");
    s->config = *(const rotmos_upload_stream_config *)userdata;
    request->codec_data = s;
    request->codec_cleanup = free;
    plan->body = s->config.body;
    plan->body.mode = ROTMOS_BODY_STREAM;
    plan->on_body = body;
    plan->on_body_end = finish;
    plan->on_close = closed;
    plan->userdata = rotmos_request_userdata(request);
    return ROTMOS_OK;
}
static rotmos_status add(rotmos_router *router, rotmos_route_group *group,
                         const rotmos_upload_stream_config *config,
                         rotmos_route **out, rotmos_error *error)
{
    rotmos_upload_stream_config *copy;
    rotmos_route_config route;
    rotmos_route *registered;
    rotmos_status status;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || config->write == NULL || config->finish == NULL ||
        (config->body.mode != ROTMOS_BODY_STREAMING_UPLOAD &&
         config->body.mode != ROTMOS_BODY_STREAM))
        return rotmos_fail(error, ROTMOS_INVALID, "upload",
                           "provide stream policy and write/finish callbacks");
    copy = malloc(sizeof(*copy));
    if (copy == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "upload",
                           "copying upload declaration");
    *copy = *config;
    rotmos_route_config_init(&route);
    route.method = config->method;
    route.methods = config->methods;
    route.path = config->path;
    route.path_kind = config->path_kind;
    route.body = config->body;
    route.hooks = config->hooks;
    route.on_headers = headers;
    route.userdata = copy;
    status =
        rotmos_router_facade_route(router, group, &route, &registered, error);
    if (status != ROTMOS_OK)
        free(copy);
    else {
        registered->release_userdata = free;
        if (out != NULL)
            *out = registered;
    }
    return status;
}
rotmos_status
rotmos_router_upload_stream(rotmos_router *router,
                            const rotmos_upload_stream_config *config,
                            rotmos_route **out, rotmos_error *error)
{
    return add(router, NULL, config, out, error);
}
rotmos_status
rotmos_route_group_upload_stream(rotmos_route_group *group,
                                 const rotmos_upload_stream_config *config,
                                 rotmos_route **out, rotmos_error *error)
{
    if (group == NULL) {
        if (out != NULL)
            *out = NULL;
        return rotmos_fail(error, ROTMOS_INVALID, "upload",
                           "group is required");
    }
    return add(NULL, group, config, out, error);
}

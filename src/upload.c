#include <knurl/upload.h>
#include "request_internal.h"
#include "response_internal.h"
#include <stdlib.h>
#include <string.h>
struct upload {
    knurl_upload_stream_config config;
    void *state;
    int opened;
};
void knurl_upload_stream_config_init(knurl_upload_stream_config *config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    knurl_body_policy_init_upload(&config->body);
}
static knurl_status open_upload(knurl_request *request, struct upload *s,
                                knurl_error *error)
{
    knurl_status status;
    if (s->opened)
        return KNURL_OK;
    status = s->config.open != NULL
                 ? s->config.open(request, s->config.userdata, &s->state, error)
                 : KNURL_OK;
    if (status == KNURL_OK)
        s->opened = 1;
    else if (status == KNURL_AGAIN || status == KNURL_EOF)
        status = knurl_fail(error, KNURL_INVALID, "upload_open",
                            "open must return a terminal status");
    return status;
}
static knurl_status body(knurl_request *request, const void *data,
                         size_t length, size_t *consumed, void *userdata,
                         knurl_error *error)
{
    struct upload *s = request->impl->codec_data;
    knurl_status status;
    (void)userdata;
    *consumed = 0;
    status = open_upload(request, s, error);
    if (status == KNURL_OK)
        status = s->config.write(request, data, length, consumed, s->state,
                                 s->config.userdata, error);
    if ((status == KNURL_OK && (*consumed == 0 || *consumed > length)) ||
        (status != KNURL_OK && *consumed != 0))
        return knurl_fail(
            error, KNURL_INVALID, "upload_write",
            "write must consume a positive prefix or AGAIN with zero");
    return status;
}
static knurl_status submit(knurl_request *request, knurl_error *error)
{
    knurl_status status = knurl_response_submit(request->impl->response,
                                                request->impl->reply, error);
    if (status == KNURL_OK && request->impl->in_headers) {
        request->impl->response->impl->complete_reply = 1;
        request->impl->response->impl->phase = KNURL_RESPONSE_FINISHING;
    }
    return status;
}
static knurl_status finish(knurl_request *request, void *userdata,
                           knurl_error *error)
{
    struct upload *s = request->impl->codec_data;
    knurl_status status;
    (void)userdata;
    status = open_upload(request, s, error);
    if (status == KNURL_OK)
        status = s->config.finish(request, request->impl->reply, s->state,
                                  s->config.userdata, error);
    if (status == KNURL_OK)
        status = knurl_router_hooks_after(request, error);
    if (status == KNURL_OK)
        status = submit(request, error);
    return status;
}
static void closed(knurl_request *request, const knurl_request_outcome *outcome,
                   void *userdata)
{
    struct upload *s = request->impl->codec_data;
    knurl_status cause = outcome->cause;
    (void)userdata;
    if (cause == KNURL_OK)
        cause = outcome->body_cause;
    if (cause == KNURL_OK)
        cause = outcome->response.cause;
    if (s->opened && s->config.close != NULL)
        s->config.close(request, cause, s->state, s->config.userdata);
}
static knurl_status headers(knurl_request *request, knurl_request_plan *plan,
                            void *userdata, knurl_error *error)
{
    struct upload *s;
    knurl_status status;
    int stopped;
    status = knurl_reply_new(&request->impl->reply, error);
    if (status != KNURL_OK)
        return status;
    request->impl->reply->impl->owner = request;
    status = knurl_router_hooks_before(request, &stopped, error);
    if (status != KNURL_OK)
        return status;
    if (stopped) {
        plan->disposition = KNURL_REQUEST_REPLY;
        return submit(request, error);
    }
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return knurl_fail(error, KNURL_NOMEM, "upload",
                          "allocating upload context");
    s->config = *(const knurl_upload_stream_config *)userdata;
    request->impl->codec_data = s;
    request->impl->codec_cleanup = free;
    plan->body = s->config.body;
    plan->body.mode = KNURL_BODY_STREAM;
    plan->on_body = body;
    plan->on_body_end = finish;
    plan->on_close = closed;
    plan->userdata = knurl_request_userdata(request);
    return KNURL_OK;
}
static knurl_status add(knurl_router *router, knurl_route_group *group,
                        const knurl_upload_stream_config *config,
                        knurl_route **out, knurl_error *error)
{
    knurl_upload_stream_config *copy;
    knurl_route_config route;
    knurl_route *registered;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || config->write == NULL || config->finish == NULL ||
        (config->body.mode != KNURL_BODY_STREAMING_UPLOAD &&
         config->body.mode != KNURL_BODY_STREAM))
        return knurl_fail(error, KNURL_INVALID, "upload",
                          "provide stream policy and write/finish callbacks");
    copy = malloc(sizeof(*copy));
    if (copy == NULL)
        return knurl_fail(error, KNURL_NOMEM, "upload",
                          "copying upload declaration");
    *copy = *config;
    knurl_route_config_init(&route);
    route.method = config->method;
    route.methods = config->methods;
    route.path = config->path;
    route.path_kind = config->path_kind;
    route.body = config->body;
    route.hooks = config->hooks;
    route.on_headers = headers;
    route.userdata = copy;
    status =
        knurl_router_facade_route(router, group, &route, &registered, error);
    if (status != KNURL_OK)
        free(copy);
    else {
        registered->impl->release_userdata = free;
        if (out != NULL)
            *out = registered;
    }
    return status;
}
knurl_status
knurl_router_upload_stream(knurl_router *router,
                           const knurl_upload_stream_config *config,
                           knurl_route **out, knurl_error *error)
{
    return add(router, NULL, config, out, error);
}
knurl_status
knurl_route_group_upload_stream(knurl_route_group *group,
                                const knurl_upload_stream_config *config,
                                knurl_route **out, knurl_error *error)
{
    if (group == NULL) {
        if (out != NULL)
            *out = NULL;
        return knurl_fail(error, KNURL_INVALID, "upload", "group is required");
    }
    return add(NULL, group, config, out, error);
}

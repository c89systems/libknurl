#include "json_internal.h"
#include "reply_internal.h"
#include "request_internal.h"
#include <stdlib.h>
#include <string.h>
struct rest_context {
    rotmos_rest_route_config config;
    lonejson *runtime;
    lonejson_json_value json;
    rotmos_body_result input;
    void *decoded;
};
static void release(void *data)
{
    struct rest_context *s = data;
    if (s->config.decode != NULL && s->decoded != NULL &&
        s->config.release != NULL)
        s->config.release(s->decoded, s->config.userdata);
    if (s->runtime != NULL) {
        lonejson_json_value_cleanup(&s->json);
        lonejson_free(s->runtime);
    }
    rotmos_body_result_cleanup(&s->input);
    free(s);
}
static rotmos_status recover(rotmos_request *request, struct rest_context *s,
                             rotmos_rest_phase phase, rotmos_status status,
                             rotmos_error *error)
{
    rotmos_error cause, handler_error;
    const char *kind = phase == ROTMOS_REST_DECODE     ? "json_parse"
                       : phase == ROTMOS_REST_VALIDATE ? "validation"
                                                       : "handler";
    if (error != NULL && error->status == status)
        cause = *error;
    else
        rotmos_fail(&cause, status, "rest", "REST callback failed");
    /* Discard a partial handler payload on the owner loop, retaining headers.
     */
    if (rotmos_reply_status(request->reply, 500, error) != ROTMOS_OK)
        return ROTMOS_STATE;
    request->reply->selected = 0;
    if (s->config.on_error != NULL) {
        rotmos_error_clear(&handler_error);
        status = s->config.on_error(request, phase, &cause, request->reply,
                                    s->config.userdata, &handler_error);
        if (status != ROTMOS_OK) {
            if (error != NULL) {
                if (handler_error.status == status)
                    *error = handler_error;
                else
                    rotmos_fail(error, status, "rest_error",
                                "REST error handler failed");
            }
            return status;
        }
        if (status == ROTMOS_OK && rotmos_reply_is_selected(request->reply)) {
            rotmos_error_clear(error);
            return ROTMOS_OK;
        }
    }
    status = rotmos_reply_error_json(request->reply,
                                     phase != ROTMOS_REST_HANDLER &&
                                             (cause.status == ROTMOS_INVALID ||
                                              cause.status == ROTMOS_LIMIT ||
                                              cause.status == ROTMOS_OVERFLOW)
                                         ? 400
                                         : 0,
                                     kind, &cause, error);
    if (status != ROTMOS_OK) {
        /* Fixed finite fallback also avoids recursion through application error
         * policy. */
        status = rotmos_reply_text(
            request->reply, 500, "application/json; charset=utf-8",
            "{\"ok\":false,\"error\":{\"kind\":\"json_encode\",\"message\":"
            "\"REST error response JSON encode failed\"}}\n",
            error);
    }
    return status;
}
static rotmos_status dispatch(rotmos_request *request, rotmos_reply *reply,
                              void *data, rotmos_error *error)
{
    const rotmos_rest_route_config *config = data;
    struct rest_context *s;
    rotmos_bytes body;
    rotmos_u64 length;
    size_t size;
    lonejson_error native;
    rotmos_status status;
    int finished = 0;
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "rest",
                           "allocating REST request");
    s->config = *config;
    request->codec_data = s;
    request->codec_cleanup = release;
    if (!config->decode_disabled) {
        if (rotmos_body_kind(rotmos_request_body(request)) ==
            ROTMOS_BODY_FILE) {
            /* REST decoding is explicitly buffered. Retain a public body copy
             * through custom decoded-value release, leaving transport storage
             * and its reader unchanged. */
            status = rotmos_body_length(rotmos_request_body(request), &length,
                                        error);
            if (status == ROTMOS_OK)
                status = rotmos_u64_to_size(length, &size, error);
            if (status == ROTMOS_OK)
                status = rotmos_request_body_copy(request, NULL, 0, &s->input,
                                                  error);
            body = s->input.memory;
        } else
            status = rotmos_request_body_bytes(request, &body, error);
        if (status != ROTMOS_OK)
            return recover(request, s, ROTMOS_REST_DECODE, status, error);
        if (config->decode != NULL || body.length != 0) {
            if (config->decode != NULL) {
                status = config->decode(request, body, &s->decoded,
                                        config->userdata, error);
                if (status != ROTMOS_OK)
                    s->decoded = NULL;
            } else {
                s->runtime = lonejson_new(NULL, &native);
                status = s->runtime == NULL
                             ? rotmos_json_error(native.code, &native, error)
                             : ROTMOS_OK;
                if (status == ROTMOS_OK) {
                    lonejson_json_value_init(s->runtime, &s->json);
                    status = rotmos_json_error(
                        lonejson_json_value_set_buffer(&s->json, body.data,
                                                       body.length, &native),
                        &native, error);
                    if (status == ROTMOS_OK)
                        s->decoded = &s->json;
                }
            }
            if (status != ROTMOS_OK)
                return recover(request, s, ROTMOS_REST_DECODE, status, error);
        }
    }
    if (config->validate != NULL) {
        status = config->validate(request, s->decoded, reply, &finished,
                                  config->userdata, error);
        if (status != ROTMOS_OK)
            return recover(request, s, ROTMOS_REST_VALIDATE, status, error);
        if (finished)
            return ROTMOS_OK;
    }
    status =
        config->handler(request, s->decoded, reply, config->userdata, error);
    return status == ROTMOS_OK
               ? ROTMOS_OK
               : recover(request, s, ROTMOS_REST_HANDLER, status, error);
}
void rotmos_rest_route_config_init(rotmos_rest_route_config *config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    rotmos_body_policy_init_buffered(&config->body);
    config->body.max_bytes.low = 1048576;
    config->body.memory_buffer_limit_bytes = 1048576;
}
static rotmos_status add(rotmos_router *router, rotmos_route_group *group,
                         const rotmos_rest_route_config *config,
                         rotmos_route **out, rotmos_error *error)
{
    rotmos_rest_route_config *copy;
    rotmos_route_config route;
    rotmos_route *registered;
    rotmos_status status;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || config->handler == NULL ||
        (config->body.mode != ROTMOS_BODY_BUFFERED &&
         config->body.mode != ROTMOS_BODY_NONE))
        return rotmos_fail(
            error, ROTMOS_INVALID, "rest",
            "provide a handler and bounded buffered body policy");
    copy = malloc(sizeof(*copy));
    if (copy == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "rest",
                           "copying REST declaration");
    *copy = *config;
    rotmos_route_config_init(&route);
    route.method = config->method;
    route.methods = config->methods;
    route.path = config->path;
    route.path_kind = config->path_kind;
    route.body = config->body;
    route.hooks = config->hooks;
    route.handler = dispatch;
    route.userdata = copy;
    status = group != NULL
                 ? rotmos_route_group_route(group, &route, &registered, error)
                 : rotmos_router_route(router, &route, &registered, error);
    if (status != ROTMOS_OK)
        free(copy);
    else {
        registered->release_userdata = free;
        if (out != NULL)
            *out = registered;
    }
    return status;
}
rotmos_status rotmos_router_rest_route(rotmos_router *router,
                                       const rotmos_rest_route_config *config,
                                       rotmos_route **out, rotmos_error *error)
{
    return add(router, NULL, config, out, error);
}
rotmos_status
rotmos_route_group_rest_route(rotmos_route_group *group,
                              const rotmos_rest_route_config *config,
                              rotmos_route **out, rotmos_error *error)
{
    if (group == NULL) {
        if (out != NULL)
            *out = NULL;
        return rotmos_fail(error, ROTMOS_INVALID, "rest", "group is required");
    }
    return add(NULL, group, config, out, error);
}

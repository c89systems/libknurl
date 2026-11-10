#include "json_internal.h"
#include "reply_internal.h"
#include "request_internal.h"
#include <stdlib.h>
#include <string.h>
struct rest_context {
    knurl_rest_route_config config;
    lonejson *runtime;
    lonejson_json_value json;
    knurl_body_result input;
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
    knurl_body_result_cleanup(&s->input);
    free(s);
}
static knurl_status recover(knurl_request *request, struct rest_context *s,
                            knurl_rest_phase phase, knurl_status status,
                            knurl_error *error)
{
    knurl_error cause, handler_error;
    const char *kind = phase == KNURL_REST_DECODE     ? "json_parse"
                       : phase == KNURL_REST_VALIDATE ? "validation"
                                                      : "handler";
    if (error != NULL && error->status == status)
        cause = *error;
    else
        knurl_fail(&cause, status, "rest", "REST callback failed");
    /* Discard a partial handler payload on the owner loop, retaining headers.
     */
    if (knurl_reply_status(request->reply, 500, error) != KNURL_OK)
        return KNURL_STATE;
    request->reply->selected = 0;
    if (s->config.on_error != NULL) {
        knurl_error_clear(&handler_error);
        status = s->config.on_error(request, phase, &cause, request->reply,
                                    s->config.userdata, &handler_error);
        if (status != KNURL_OK) {
            if (error != NULL) {
                if (handler_error.status == status)
                    *error = handler_error;
                else
                    knurl_fail(error, status, "rest_error",
                               "REST error handler failed");
            }
            return status;
        }
        if (status == KNURL_OK && knurl_reply_is_selected(request->reply)) {
            knurl_error_clear(error);
            return KNURL_OK;
        }
    }
    status = knurl_reply_error_json(request->reply,
                                    phase != KNURL_REST_HANDLER &&
                                            (cause.status == KNURL_INVALID ||
                                             cause.status == KNURL_LIMIT ||
                                             cause.status == KNURL_OVERFLOW)
                                        ? 400
                                        : 0,
                                    kind, &cause, error);
    if (status != KNURL_OK) {
        /* Fixed finite fallback also avoids recursion through application error
         * policy. */
        status = knurl_reply_text(
            request->reply, 500, "application/json; charset=utf-8",
            "{\"ok\":false,\"error\":{\"kind\":\"json_encode\",\"message\":"
            "\"REST error response JSON encode failed\"}}\n",
            error);
    }
    return status;
}
static knurl_status dispatch(knurl_request *request, knurl_reply *reply,
                             void *data, knurl_error *error)
{
    const knurl_rest_route_config *config = data;
    struct rest_context *s;
    knurl_bytes body;
    knurl_u64 length;
    size_t size;
    lonejson_error native;
    knurl_status status;
    int finished = 0;
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return knurl_fail(error, KNURL_NOMEM, "rest",
                          "allocating REST request");
    s->config = *config;
    request->codec_data = s;
    request->codec_cleanup = release;
    if (!config->decode_disabled) {
        if (knurl_body_kind(knurl_request_body(request)) == KNURL_BODY_FILE) {
            /* REST decoding is explicitly buffered. Retain a public body copy
             * through custom decoded-value release, leaving transport storage
             * and its reader unchanged. */
            status =
                knurl_body_length(knurl_request_body(request), &length, error);
            if (status == KNURL_OK)
                status = knurl_u64_to_size(length, &size, error);
            if (status == KNURL_OK)
                status =
                    knurl_request_body_copy(request, NULL, 0, &s->input, error);
            body = s->input.memory;
        } else
            status = knurl_request_body_bytes(request, &body, error);
        if (status != KNURL_OK)
            return recover(request, s, KNURL_REST_DECODE, status, error);
        if (config->decode != NULL || body.length != 0) {
            if (config->decode != NULL) {
                status = config->decode(request, body, &s->decoded,
                                        config->userdata, error);
                if (status != KNURL_OK)
                    s->decoded = NULL;
            } else {
                s->runtime = lonejson_new(NULL, &native);
                status = s->runtime == NULL
                             ? knurl_json_error(native.code, &native, error)
                             : KNURL_OK;
                if (status == KNURL_OK) {
                    lonejson_json_value_init(s->runtime, &s->json);
                    status = knurl_json_error(
                        lonejson_json_value_set_buffer(&s->json, body.data,
                                                       body.length, &native),
                        &native, error);
                    if (status == KNURL_OK)
                        s->decoded = &s->json;
                }
            }
            if (status != KNURL_OK)
                return recover(request, s, KNURL_REST_DECODE, status, error);
        }
    }
    if (config->validate != NULL) {
        status = config->validate(request, s->decoded, reply, &finished,
                                  config->userdata, error);
        if (status != KNURL_OK)
            return recover(request, s, KNURL_REST_VALIDATE, status, error);
        if (finished)
            return KNURL_OK;
    }
    status =
        config->handler(request, s->decoded, reply, config->userdata, error);
    return status == KNURL_OK
               ? KNURL_OK
               : recover(request, s, KNURL_REST_HANDLER, status, error);
}
void knurl_rest_route_config_init(knurl_rest_route_config *config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    knurl_body_policy_init_buffered(&config->body);
    config->body.max_bytes.low = 1048576;
    config->body.memory_buffer_limit_bytes = 1048576;
}
static knurl_status add(knurl_router *router, knurl_route_group *group,
                        const knurl_rest_route_config *config,
                        knurl_route **out, knurl_error *error)
{
    knurl_rest_route_config *copy;
    knurl_route_config route;
    knurl_route *registered;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || config->handler == NULL ||
        (config->body.mode != KNURL_BODY_BUFFERED &&
         config->body.mode != KNURL_BODY_NONE))
        return knurl_fail(error, KNURL_INVALID, "rest",
                          "provide a handler and bounded buffered body policy");
    copy = malloc(sizeof(*copy));
    if (copy == NULL)
        return knurl_fail(error, KNURL_NOMEM, "rest",
                          "copying REST declaration");
    *copy = *config;
    knurl_route_config_init(&route);
    route.method = config->method;
    route.methods = config->methods;
    route.path = config->path;
    route.path_kind = config->path_kind;
    route.body = config->body;
    route.hooks = config->hooks;
    route.handler = dispatch;
    route.userdata = copy;
    status = group != NULL
                 ? knurl_route_group_route(group, &route, &registered, error)
                 : knurl_router_route(router, &route, &registered, error);
    if (status != KNURL_OK)
        free(copy);
    else {
        registered->release_userdata = free;
        if (out != NULL)
            *out = registered;
    }
    return status;
}
knurl_status knurl_router_rest_route(knurl_router *router,
                                     const knurl_rest_route_config *config,
                                     knurl_route **out, knurl_error *error)
{
    return add(router, NULL, config, out, error);
}
knurl_status knurl_route_group_rest_route(knurl_route_group *group,
                                          const knurl_rest_route_config *config,
                                          knurl_route **out, knurl_error *error)
{
    if (group == NULL) {
        if (out != NULL)
            *out = NULL;
        return knurl_fail(error, KNURL_INVALID, "rest", "group is required");
    }
    return add(NULL, group, config, out, error);
}

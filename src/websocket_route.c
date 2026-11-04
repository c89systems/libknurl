#include "request_internal.h"
#include "reply_internal.h"
#include <rotmos/websocket.h>

#include <stdlib.h>
#include <string.h>

struct websocket_route {
    rotmos_websocket_route_config config;
    rotmos_reply *fields;
    char *protocol;
};

static void release(void *data)
{
    struct websocket_route *route;
    route = data;
    (void)rotmos_reply_destroy(route->fields, NULL);
    free(route->protocol);
    free(route);
}

static rotmos_status dispatch(rotmos_request *request,
                              rotmos_request_plan *plan, void *data,
                              rotmos_error *error)
{
    struct websocket_route *route;
    rotmos_websocket *ws;
    rotmos_websocket_config config;
    rotmos_status status;
    rotmos_bytes body;
    route = data;
    config = route->config.websocket;
    if (route->config.on_headers != NULL) {
        status = route->config.on_headers(request, &config, plan,
                                          route->config.userdata, error);
        if (status != ROTMOS_OK || plan->disposition == ROTMOS_REQUEST_REPLY)
            return status;
        if (plan->disposition != ROTMOS_REQUEST_MANAGED)
            return rotmos_fail(
                error, ROTMOS_INVALID, "websocket_route",
                "admission must proceed with MANAGED or reject with REPLY");
    }
    status = rotmos_websocket_accept(request, &config, &ws, error);
    if (status == ROTMOS_INVALID) {
        body = rotmos_view("invalid WebSocket handshake\n");
        status = rotmos_response_bytes(rotmos_request_response(request), 400,
                                       "text/plain", body, error);
        if (status == ROTMOS_OK)
            plan->disposition = ROTMOS_REQUEST_REPLY;
    } else if (status == ROTMOS_OK)
        plan->disposition = ROTMOS_REQUEST_WEBSOCKET;
    return status;
}

rotmos_status
rotmos_router_websocket(rotmos_router *router,
                        const rotmos_websocket_route_config *input,
                        rotmos_route **out, rotmos_error *error)
{
    struct websocket_route *state;
    rotmos_route_config config;
    rotmos_route *route;
    rotmos_status status;
    const rotmos_header *field;
    size_t i;
    char *name;
    char *value;
    if (out != NULL)
        *out = NULL;
    if (router == NULL || input == NULL ||
        (input->websocket.header_count != 0 &&
         input->websocket.headers == NULL))
        return rotmos_fail(error, ROTMOS_INVALID, "websocket_route",
                           "provide router, configuration and valid fields");
    state = calloc(1, sizeof(*state));
    if (state == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "websocket_route",
                           "cannot allocate route facade");
    state->config = *input;
    status = rotmos_reply_new(&state->fields, error);
    if (input->websocket.protocol != NULL) {
        state->protocol = rotmos_copy_text(input->websocket.protocol);
        if (state->protocol == NULL)
            status = rotmos_fail(error, ROTMOS_NOMEM, "websocket_route",
                                 "cannot copy selected protocol");
    }
    for (i = 0; status == ROTMOS_OK && i < input->websocket.header_count; ++i) {
        field = &input->websocket.headers[i];
        if (field->name.data == NULL || field->value.data == NULL ||
            field->name.length == (size_t)-1 ||
            field->value.length == (size_t)-1 ||
            memchr(field->name.data, 0, field->name.length) != NULL ||
            memchr(field->value.data, 0, field->value.length) != NULL) {
            status = rotmos_fail(error, ROTMOS_INVALID, "websocket_route",
                                 "invalid handshake field text");
            break;
        }
        name = malloc(field->name.length + 1);
        value = malloc(field->value.length + 1);
        if (name == NULL || value == NULL) {
            free(name);
            free(value);
            status = rotmos_fail(error, ROTMOS_NOMEM, "websocket_route",
                                 "cannot copy handshake fields");
            break;
        }
        memcpy(name, field->name.data, field->name.length);
        name[field->name.length] = '\0';
        memcpy(value, field->value.data, field->value.length);
        value[field->value.length] = '\0';
        status = rotmos_reply_header_add(state->fields, name, value, error);
        free(name);
        free(value);
    }
    if (status == ROTMOS_OK) {
        state->config.websocket.protocol = state->protocol;
        state->config.websocket.headers = state->fields->headers;
        state->config.path = NULL;
        rotmos_route_config_init(&config);
        config.method = ROTMOS_HTTP_GET;
        config.path = input->path;
        config.path_kind = input->path_kind;
        config.on_headers = dispatch;
        config.userdata = state;
        status = rotmos_router_route(router, &config, &route, error);
        if (status == ROTMOS_OK) {
            route->release_userdata = release;
            if (out != NULL)
                *out = route;
            return ROTMOS_OK;
        }
    }
    release(state);
    return status;
}

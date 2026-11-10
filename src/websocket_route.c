#include "request_internal.h"
#include "reply_internal.h"
#include <knurl/websocket.h>

#include <stdlib.h>
#include <string.h>

struct websocket_route {
    knurl_websocket_route_config config;
    knurl_reply *fields;
    char *protocol;
};

static void release(void *data)
{
    struct websocket_route *route;
    route = data;
    (void)knurl_reply_destroy(route->fields, NULL);
    free(route->protocol);
    free(route);
}

static knurl_status dispatch(knurl_request *request, knurl_request_plan *plan,
                             void *data, knurl_error *error)
{
    struct websocket_route *route;
    knurl_websocket *ws;
    knurl_websocket_config config;
    knurl_status status;
    knurl_bytes body;
    route = data;
    config = route->config.websocket;
    if (route->config.on_headers != NULL) {
        status = route->config.on_headers(request, &config, plan,
                                          route->config.userdata, error);
        if (status != KNURL_OK || plan->disposition == KNURL_REQUEST_REPLY)
            return status;
        if (plan->disposition != KNURL_REQUEST_MANAGED)
            return knurl_fail(
                error, KNURL_INVALID, "websocket_route",
                "admission must proceed with MANAGED or reject with REPLY");
    }
    status = knurl_websocket_accept(request, &config, &ws, error);
    if (status == KNURL_INVALID) {
        body = knurl_view("invalid WebSocket handshake\n");
        status = knurl_response_bytes(knurl_request_response(request), 400,
                                      "text/plain", body, error);
        if (status == KNURL_OK)
            plan->disposition = KNURL_REQUEST_REPLY;
    } else if (status == KNURL_OK)
        plan->disposition = KNURL_REQUEST_WEBSOCKET;
    return status;
}

knurl_status knurl_router_websocket(knurl_router *router,
                                    const knurl_websocket_route_config *input,
                                    knurl_route **out, knurl_error *error)
{
    struct websocket_route *state;
    knurl_route_config config;
    knurl_route *route;
    knurl_status status;
    const knurl_header *field;
    size_t i;
    char *name;
    char *value;
    if (out != NULL)
        *out = NULL;
    if (router == NULL || input == NULL ||
        (input->websocket.header_count != 0 &&
         input->websocket.headers == NULL))
        return knurl_fail(error, KNURL_INVALID, "websocket_route",
                          "provide router, configuration and valid fields");
    state = calloc(1, sizeof(*state));
    if (state == NULL)
        return knurl_fail(error, KNURL_NOMEM, "websocket_route",
                          "cannot allocate route facade");
    state->config = *input;
    status = knurl_reply_new(&state->fields, error);
    if (input->websocket.protocol != NULL) {
        state->protocol = knurl_copy_text(input->websocket.protocol);
        if (state->protocol == NULL)
            status = knurl_fail(error, KNURL_NOMEM, "websocket_route",
                                "cannot copy selected protocol");
    }
    for (i = 0; status == KNURL_OK && i < input->websocket.header_count; ++i) {
        field = &input->websocket.headers[i];
        if (field->name.data == NULL || field->value.data == NULL ||
            field->name.length == (size_t)-1 ||
            field->value.length == (size_t)-1 ||
            memchr(field->name.data, 0, field->name.length) != NULL ||
            memchr(field->value.data, 0, field->value.length) != NULL) {
            status = knurl_fail(error, KNURL_INVALID, "websocket_route",
                                "invalid handshake field text");
            break;
        }
        name = malloc(field->name.length + 1);
        value = malloc(field->value.length + 1);
        if (name == NULL || value == NULL) {
            free(name);
            free(value);
            status = knurl_fail(error, KNURL_NOMEM, "websocket_route",
                                "cannot copy handshake fields");
            break;
        }
        memcpy(name, field->name.data, field->name.length);
        name[field->name.length] = '\0';
        memcpy(value, field->value.data, field->value.length);
        value[field->value.length] = '\0';
        status = knurl_reply_header_add(state->fields, name, value, error);
        free(name);
        free(value);
    }
    if (status == KNURL_OK) {
        state->config.websocket.protocol = state->protocol;
        state->config.websocket.headers = state->fields->headers;
        state->config.path = NULL;
        knurl_route_config_init(&config);
        config.method = KNURL_HTTP_GET;
        config.path = input->path;
        config.path_kind = input->path_kind;
        config.on_headers = dispatch;
        config.userdata = state;
        status = knurl_router_route(router, &config, &route, error);
        if (status == KNURL_OK) {
            route->release_userdata = release;
            if (out != NULL)
                *out = route;
            return KNURL_OK;
        }
    }
    release(state);
    return status;
}

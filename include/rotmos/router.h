#ifndef ROTMOS_ROUTER_H
#define ROTMOS_ROUTER_H

#include <rotmos/http.h>
#include <rotmos/reply.h>
#include <rotmos/dispatch.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rotmos_route_path_kind {
    ROTMOS_PATH_LITERAL = 0,
    ROTMOS_PATH_PARAMS,
    ROTMOS_PATH_REGEX
} rotmos_route_path_kind;

typedef rotmos_status (*rotmos_route_fn)(rotmos_request *request,
                                         rotmos_reply *reply, void *userdata,
                                         rotmos_error *error);

/* A middleware callback selects a staged reply and sets finish to stop
 * dispatch. After callbacks see the handler's staged result; all run before
 * submission. */
typedef rotmos_status (*rotmos_route_hook_fn)(rotmos_request *request,
                                              rotmos_reply *reply, int *finish,
                                              void *userdata,
                                              rotmos_error *error);

typedef struct rotmos_route_hooks {
    rotmos_route_hook_fn auth;
    rotmos_route_hook_fn before;
    rotmos_route_hook_fn after;
    void *userdata;
} rotmos_route_hooks;

typedef struct rotmos_route_config {
    rotmos_http_method method;
    rotmos_http_methods methods;
    const char *path;
    rotmos_route_path_kind path_kind;
    rotmos_body_policy body;
    rotmos_route_fn handler;
    rotmos_headers_fn on_headers;
    void *userdata;
    rotmos_route_hooks hooks;
} rotmos_route_config;

typedef struct rotmos_route_group_config {
    const char *prefix;
    rotmos_route_hooks hooks;
} rotmos_route_group_config;

ROTMOS_API void rotmos_route_config_init(rotmos_route_config *config);
ROTMOS_API void
rotmos_route_group_config_init(rotmos_route_group_config *config);
ROTMOS_API rotmos_status rotmos_router_new(rotmos_router **out,
                                           rotmos_error *error);
ROTMOS_API rotmos_status rotmos_router_destroy(rotmos_router *router,
                                               rotmos_error *error);
ROTMOS_API rotmos_status rotmos_router_route(rotmos_router *router,
                                             const rotmos_route_config *config,
                                             rotmos_route **out,
                                             rotmos_error *error);
ROTMOS_API rotmos_status rotmos_router_group(
    rotmos_router *router, const rotmos_route_group_config *config,
    rotmos_route_group **out, rotmos_error *error);
ROTMOS_API rotmos_status rotmos_route_group_route(
    rotmos_route_group *group, const rotmos_route_config *config,
    rotmos_route **out, rotmos_error *error);
ROTMOS_API size_t rotmos_router_route_count(const rotmos_router *router);
/* Registry snapshots are borrowed until router destruction and remain stable
 * across later registration. Indexed lookup reports INVALID out of range. */
ROTMOS_API rotmos_status rotmos_router_route_at(const rotmos_router *router,
                                                size_t index,
                                                const rotmos_route **out,
                                                rotmos_error *error);
ROTMOS_API const rotmos_route_config *
rotmos_route_declaration(const rotmos_route *route);
/* Selects one route. Ordinary handlers are invoked after body completion;
 * raw on_headers routes own their plan and admission policy. */
ROTMOS_API rotmos_status
rotmos_router_dispatch_headers(rotmos_router *router, rotmos_request *request,
                               rotmos_request_plan *plan, rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif

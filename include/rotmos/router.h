#ifndef ROTMOS_ROUTER_H
#define ROTMOS_ROUTER_H

/** @file router.h Public router API. Expected failures return rotmos_status and
 * optional bounded rotmos_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <rotmos/http.h>
#include <rotmos/reply.h>
#include <rotmos/dispatch.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Values for route path kind. */
typedef enum rotmos_route_path_kind {
    /** Route path matching: literal. */
    ROTMOS_PATH_LITERAL = 0,
    /** Route path matching: params. */
    ROTMOS_PATH_PARAMS,
    /** Route path matching: regex. */
    ROTMOS_PATH_REGEX
} rotmos_route_path_kind;

/** Owner-loop staged handler after admitted body completion. Select a reply or
 * leave it empty for 204; serialization/hook timing follows the registration
 * family.
 */
typedef rotmos_status (*rotmos_route_fn)(rotmos_request *request,
                                         rotmos_reply *reply, void *userdata,
                                         rotmos_error *error);

/** A middleware callback selects a staged reply and sets finish to stop
 * dispatch. After callbacks see the handler's staged result; all run before
 * submission. */
typedef rotmos_status (*rotmos_route_hook_fn)(rotmos_request *request,
                                              rotmos_reply *reply, int *finish,
                                              void *userdata,
                                              rotmos_error *error);

/** Staged auth, before and after middleware. finish short-circuits dispatch;
 * after-hooks complete before transport submission.
 */
typedef struct rotmos_route_hooks {
    /** Authentication callback/declaration, evaluated before route
     * hooks/handler.
     */
    rotmos_route_hook_fn auth;
    /** Before-handler staged-reply hook; may finish dispatch early. */
    rotmos_route_hook_fn before;
    /** After-handler staged-reply hook; completes before transport submission.
     */
    rotmos_route_hook_fn after;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} rotmos_route_hooks;

/** Configuration for route. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_route_config {
    /** Single HTTP method; methods supplies the explicit multi-method mask when
     * nonzero.
     */
    rotmos_http_method method;
    /** HTTP method mask; zero uses this operation's single-method/default
     * policy.
     */
    rotmos_http_methods methods;
    /** Resource or route path, interpreted according to the enclosing
     * operation.
     */
    const char *path;
    /** Literal, parameter or regular-expression path matching. */
    rotmos_route_path_kind path_kind;
    /** Request body policy, or payload bytes in a response/result declaration.
     */
    rotmos_body_policy body;
    /** Application handler invoked under this route's documented execution and
     * lifetime rules.
     */
    rotmos_route_fn handler;
    /** Headers-time dispatch callback, before managed body delivery. */
    rotmos_headers_fn on_headers;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
    /** Staged auth/before/after callbacks; all finish before transport
     * submission.
     */
    rotmos_route_hooks hooks;
} rotmos_route_config;

/** Configuration for route group. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_route_group_config {
    /** Group path prefix or explicit materialization filename prefix. */
    const char *prefix;
    /** Staged auth/before/after callbacks; all finish before transport
     * submission.
     */
    rotmos_route_hooks hooks;
} rotmos_route_group_config;

/** Initialize route config with its documented defaults; call before applying
 * overrides.
 */
ROTMOS_API void rotmos_route_config_init(rotmos_route_config *config);
/** Initialize route group config with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void
rotmos_route_group_config_init(rotmos_route_group_config *config);
/** Create an owned inert route registry; no sockets, callbacks or files are
 * opened.
 */
ROTMOS_API rotmos_status rotmos_router_new(rotmos_router **out,
                                           rotmos_error *error);
/** Destroy an untransferred router and its declarations; callback/provider
 * owners must remain alive through destruction.
 */
ROTMOS_API rotmos_status rotmos_router_destroy(rotmos_router *router,
                                               rotmos_error *error);
/** Copy a route declaration; borrow callback userdata through router
 * destruction. Return a router-owned route.
 */
ROTMOS_API rotmos_status rotmos_router_route(rotmos_router *router,
                                             const rotmos_route_config *config,
                                             rotmos_route **out,
                                             rotmos_error *error);
/** Copy a prefix/hook declaration; return a group borrowed from its router. */
ROTMOS_API rotmos_status rotmos_router_group(
    rotmos_router *router, const rotmos_route_group_config *config,
    rotmos_route_group **out, rotmos_error *error);
/** Register a route under the group's prefix and hooks; ownership matches
 * rotmos_router_route.
 */
ROTMOS_API rotmos_status rotmos_route_group_route(
    rotmos_route_group *group, const rotmos_route_config *config,
    rotmos_route **out, rotmos_error *error);
/** Return the number of routes in the shared dispatch/OpenAPI registry. */
ROTMOS_API size_t rotmos_router_route_count(const rotmos_router *router);
/** Registry snapshots are borrowed until router destruction and remain stable
 * across later registration. Indexed lookup reports INVALID out of range. */
ROTMOS_API rotmos_status rotmos_router_route_at(const rotmos_router *router,
                                                size_t index,
                                                const rotmos_route **out,
                                                rotmos_error *error);
/** Borrow immutable route configuration until router destruction; no callback
 * is invoked.
 */
ROTMOS_API const rotmos_route_config *
rotmos_route_declaration(const rotmos_route *route);
/** Selects one route. Ordinary handlers are invoked after body completion;
 * raw on_headers routes own their plan and admission policy. */
ROTMOS_API rotmos_status
rotmos_router_dispatch_headers(rotmos_router *router, rotmos_request *request,
                               rotmos_request_plan *plan, rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif

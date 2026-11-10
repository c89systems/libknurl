#ifndef KNURL_ROUTER_H
#define KNURL_ROUTER_H

/** @file router.h Public router API. Expected failures return knurl_status and
 * optional bounded knurl_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <knurl/http.h>
#include <knurl/reply.h>
#include <knurl/dispatch.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Values for route path kind. */
typedef enum knurl_route_path_kind {
    /** Route path matching: literal. */
    KNURL_PATH_LITERAL = 0,
    /** Route path matching: params. */
    KNURL_PATH_PARAMS,
    /** Route path matching: regex. */
    KNURL_PATH_REGEX
} knurl_route_path_kind;

/** Owner-loop staged handler after admitted body completion. Select a reply or
 * leave it empty for 204; serialization/hook timing follows the registration
 * family.
 */
typedef knurl_status (*knurl_route_fn)(knurl_request *request,
                                       knurl_reply *reply, void *userdata,
                                       knurl_error *error);

/** A middleware callback selects a staged reply and sets finish to stop
 * dispatch. After callbacks see the handler's staged result; all run before
 * submission. */
typedef knurl_status (*knurl_route_hook_fn)(knurl_request *request,
                                            knurl_reply *reply, int *finish,
                                            void *userdata, knurl_error *error);

/** Staged auth, before and after middleware. finish short-circuits dispatch;
 * after-hooks complete before transport submission.
 */
typedef struct knurl_route_hooks {
    /** Authentication callback/declaration, evaluated before route
     * hooks/handler.
     */
    knurl_route_hook_fn auth;
    /** Before-handler staged-reply hook; may finish dispatch early. */
    knurl_route_hook_fn before;
    /** After-handler staged-reply hook; completes before transport submission.
     */
    knurl_route_hook_fn after;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} knurl_route_hooks;

/** Configuration for route. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_route_config {
    /** Single HTTP method; methods supplies the explicit multi-method mask when
     * nonzero.
     */
    knurl_http_method method;
    /** HTTP method mask; zero uses this operation's single-method/default
     * policy.
     */
    knurl_http_methods methods;
    /** Resource or route path, interpreted according to the enclosing
     * operation.
     */
    const char *path;
    /** Literal, parameter or regular-expression path matching. */
    knurl_route_path_kind path_kind;
    /** Body admission, storage and delivery policy for this route. */
    knurl_body_policy body;
    /** Application handler invoked under this route's documented execution and
     * lifetime rules.
     */
    knurl_route_fn handler;
    /** Headers-time dispatch callback, before managed body delivery. */
    knurl_headers_fn on_headers;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
    /** Staged auth/before/after callbacks; all finish before transport
     * submission.
     */
    knurl_route_hooks hooks;
} knurl_route_config;

/** Configuration for route group. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_route_group_config {
    /** Copied route path prefix; empty adds no prefix to the group's routes. */
    const char *prefix;
    /** Staged auth/before/after callbacks; all finish before transport
     * submission.
     */
    knurl_route_hooks hooks;
} knurl_route_group_config;

/** Initialize route config with its documented defaults; call before applying
 * overrides.
 */
KNURL_API void knurl_route_config_init(knurl_route_config *config);
/** Initialize route group config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_route_group_config_init(knurl_route_group_config *config);
/** Create an owned inert route registry; no sockets, callbacks or files are
 * opened.
 */
KNURL_API knurl_status knurl_router_new(knurl_router **out, knurl_error *error);
/** Destroy an untransferred router and its declarations; callback/provider
 * owners must remain alive through destruction.
 */
KNURL_API knurl_status knurl_router_destroy(knurl_router *router,
                                            knurl_error *error);
/** Copy a route declaration; borrow callback userdata through router
 * destruction. Return a router-owned route.
 */
KNURL_API knurl_status knurl_router_route(knurl_router *router,
                                          const knurl_route_config *config,
                                          knurl_route **out,
                                          knurl_error *error);
/** Copy a prefix/hook declaration; return a group borrowed from its router. */
KNURL_API knurl_status
knurl_router_group(knurl_router *router, const knurl_route_group_config *config,
                   knurl_route_group **out, knurl_error *error);
/** Register a route under the group's prefix and hooks; ownership matches
 * knurl_router_route.
 */
KNURL_API knurl_status knurl_route_group_route(knurl_route_group *group,
                                               const knurl_route_config *config,
                                               knurl_route **out,
                                               knurl_error *error);
/** Return the number of routes in the shared dispatch/OpenAPI registry. */
KNURL_API size_t knurl_router_route_count(const knurl_router *router);
/** Registry snapshots are borrowed until router destruction and remain stable
 * across later registration. Indexed lookup reports INVALID out of range. */
KNURL_API knurl_status knurl_router_route_at(const knurl_router *router,
                                             size_t index,
                                             const knurl_route **out,
                                             knurl_error *error);
/** Borrow immutable route configuration until router destruction; no callback
 * is invoked.
 */
KNURL_API const knurl_route_config *
knurl_route_declaration(const knurl_route *route);
/** Selects one route. Ordinary handlers are invoked after body completion;
 * raw on_headers routes own their plan and admission policy. */
KNURL_API knurl_status knurl_router_dispatch_headers(knurl_router *router,
                                                     knurl_request *request,
                                                     knurl_request_plan *plan,
                                                     knurl_error *error);

#ifdef __cplusplus
}
#endif
#endif

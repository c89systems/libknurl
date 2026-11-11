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

/** Values for openapi format. */
typedef enum knurl_openapi_format {
    /** OpenAPI output encoding: json. */
    KNURL_OPENAPI_JSON = 0,
    /** OpenAPI output encoding: yaml. */
    KNURL_OPENAPI_YAML
} knurl_openapi_format;

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

struct knurl_error;
struct knurl_json_route_config;
struct knurl_json_typed_route_config;
struct knurl_openapi_document;
struct knurl_openapi_route_doc;
struct knurl_owned_bytes;
struct knurl_proxy_config;
struct knurl_request;
struct knurl_request_plan;
struct knurl_rest_route_config;
struct knurl_route;
struct knurl_route_config;
struct knurl_route_group;
struct knurl_route_group_config;
struct knurl_static_config;
struct knurl_upload_file_config;
struct knurl_upload_reader_config;
struct knurl_upload_stream_config;
struct knurl_webdav_config;
struct knurl_websocket_route_config;
struct knurl_xml_route_config;
/** Library-owned router receiver. Pass this handle as the first
 * argument to its methods. Obtain it through the documented constructor or
 * borrowed accessor. Do not copy, initialize or modify the shell or impl.
 */
struct knurl_router {
    /** Registration copies strings, targets, TLS material and callbacks. */
    knurl_status (*proxy)(struct knurl_router *router,
                          const struct knurl_proxy_config *config,
                          struct knurl_route **route,
                          struct knurl_error *error);
    /** Register an application WebSocket route. Handshake callbacks run on the
     * owner loop; connection handles outlive the HTTP request.
     */
    knurl_status (*websocket)(struct knurl_router *,
                              const struct knurl_websocket_route_config *,
                              struct knurl_route **, struct knurl_error *);
    /** Live input uses the bounded upload-reader queue (buffer_bytes=0: 256
     * KiB). Native parsing retains no live request/Lua handle; the completed
     * record and handler return to the owner loop. Records survive response
     * cleanup. Ordinary hooks and empty-reply 204 behavior apply. No transport
     * body is collected. Borrowed runtime allocators and mapped field callbacks
     * used by parsing must support native threads, as with the underlying
     * upload-reader contract. */
    knurl_status (*xml_route)(struct knurl_router *,
                              const struct knurl_xml_route_config *,
                              struct knurl_route **, struct knurl_error *);
    /** Copies declarations; callback userdata remains caller-owned.
     * Registration invokes no callbacks and opens no resources. Body mode is
     * STREAMING_UPLOAD or STREAM; both use the same bounded live input
     * delivery. */
    knurl_status (*upload_stream)(struct knurl_router *,
                                  const struct knurl_upload_stream_config *,
                                  struct knurl_route **, struct knurl_error *);
    /** Bounded live queue, at most buffer_bytes (zero selects 256 KiB) and OS
     * pipe capacity. No materialization/spool or owner-loop wait/join. Caller
     * userdata stays alive until server run returns, including native
     * cancellation ACK. */
    knurl_status (*upload_reader)(struct knurl_router *,
                                  const struct knurl_upload_reader_config *,
                                  struct knurl_route **, struct knurl_error *);
    /** Explicit caller artifact, opened/truncated only after admission.
     * Incremental writes and flush/close run natively; partial output remains
     * on failure. Success matches Vectis: 200 text/plain with the decimal
     * uploaded byte count.
     */
    knurl_status (*upload_file)(struct knurl_router *,
                                const struct knurl_upload_file_config *,
                                struct knurl_route **, struct knurl_error *);
    /** Copies declarations, borrows schemas/runtime/userdata. Ordinary route
     * output is materialized before after-hooks, without a new output-size
     * ceiling, and its records are then cleaned. Typed-reply records instead
     * remain valid through producer cleanup when an explicit live reply borrows
     * them. Ordinary staged middleware order applies. No schema means a NULL
     * callback view. Optional max_output_bytes bounds ordinary output; zero
     * adds no limit. */
    knurl_status (*json_route)(struct knurl_router *router,
                               const struct knurl_json_route_config *config,
                               struct knurl_route **out,
                               struct knurl_error *error);
    /** Register a mapped JSON input/staged-reply route. Schemas/runtime and
     * handler userdata remain borrowed; explicit live outputs retain their
     * record owners.
     */
    knurl_status (*json_typed_route)(
        struct knurl_router *router,
        const struct knurl_json_typed_route_config *config,
        struct knurl_route **out, struct knurl_error *error);
    /** Register a buffered REST route. Decode/validate/handler/error phases
     * reuse Core; successful decoded owners release once.
     */
    knurl_status (*rest_route)(struct knurl_router *,
                               const struct knurl_rest_route_config *,
                               struct knurl_route **, struct knurl_error *);
    /** Destroy an untransferred router and its declarations; callback/provider
     * owners must remain alive through destruction.
     */
    knurl_status (*destroy)(struct knurl_router *router,
                            struct knurl_error *error);
    /** Copy a route declaration; borrow callback userdata through router
     * destruction. Return a router-owned route.
     */
    knurl_status (*route)(struct knurl_router *router,
                          const struct knurl_route_config *config,
                          struct knurl_route **out, struct knurl_error *error);
    /** Copy a prefix/hook declaration; return a group borrowed from its router.
     */
    knurl_status (*group)(struct knurl_router *router,
                          const struct knurl_route_group_config *config,
                          struct knurl_route_group **out,
                          struct knurl_error *error);
    /** Return the number of routes in the shared dispatch/OpenAPI registry. */
    size_t (*route_count)(const struct knurl_router *router);
    /** Registry snapshots are borrowed until router destruction and remain
     * stable across later registration. Indexed lookup reports INVALID out of
     * range. */
    knurl_status (*route_at)(const struct knurl_router *router, size_t index,
                             const struct knurl_route **out,
                             struct knurl_error *error);
    /** Selects one route. Ordinary handlers are invoked after body completion;
     * raw on_headers routes own their plan and admission policy. */
    knurl_status (*dispatch_headers)(struct knurl_router *router,
                                     struct knurl_request *request,
                                     struct knurl_request_plan *plan,
                                     struct knurl_error *error);
    /** Router-owned annotations; copies selectors, descriptions, names and
     * tags. Maps (including child maps) stay immutable/alive through router
     * destruction. Attachment is inert and unavailable during a run.
     * Overlapping annotations for the same normalized path/method fail with
     * CONFLICT. Routes and their count remain the router's existing registry,
     * not an application-side copy. */
    knurl_status (*openapi_doc)(struct knurl_router *, knurl_http_methods,
                                const char *path,
                                const struct knurl_openapi_route_doc *,
                                struct knurl_error *);
    /** Materialized JSON/YAML OpenAPI 3.1.0 metadata. NULL document selects
     * defaults; zero max_bytes adds no output ceiling. Output is owned through
     * owned_bytes_cleanup; failure leaves an empty output. Generation never
     * invokes route callbacks. */
    knurl_status (*openapi)(const struct knurl_router *,
                            const struct knurl_openapi_document *,
                            knurl_openapi_format, struct knurl_owned_bytes *,
                            struct knurl_error *);
    /** Register a static file route. Declarations are copied, callbacks remain
     * borrowed; range/conditional delivery uses the asset engine.
     */
    knurl_status (*static_file)(struct knurl_router *,
                                const struct knurl_static_config *,
                                struct knurl_route **, struct knurl_error *);
    /** Register a confined filesystem directory mount. Directory indexes are
     * disabled by default; no file lookup occurs during registration.
     */
    knurl_status (*static_directory)(struct knurl_router *,
                                     const struct knurl_static_config *,
                                     struct knurl_route **,
                                     struct knurl_error *);
    /** Register a borrowed read-only asset-provider mount. Retain the provider
     * and hook userdata through router destruction.
     */
    knurl_status (*static_assets)(struct knurl_router *,
                                  const struct knurl_static_config *,
                                  struct knurl_route **, struct knurl_error *);
    /** Copied declarations/strings; callback userdata and custom provider are
     * borrowed through router close. NULL provider declares a filesystem
     * provider using storage and the server router. Protected mounts require
     * auth and an atomic scope-capable mutable provider. Read-only mounts
     * reject writes before receiving a body. Host owns credentials, identity,
     * packed content and VM. PUT is incremental; PROPFIND depth 0/1 advances
     * one bounded listing entry. Zero operation timeout selects 30000 ms;
     * explicit disable retains shutdown escalation. Neither LOCK nor UNLOCK is
     * implemented. */
    knurl_status (*webdav)(struct knurl_router *,
                           const struct knurl_webdav_config *,
                           struct knurl_route **, struct knurl_error *);
    /** Private library state; applications must not access it. */
    struct knurl_router_impl *impl;
};

struct knurl_error;
struct knurl_json_route_config;
struct knurl_json_typed_route_config;
struct knurl_rest_route_config;
struct knurl_route;
struct knurl_route_config;
struct knurl_static_config;
struct knurl_upload_file_config;
struct knurl_upload_reader_config;
struct knurl_upload_stream_config;
struct knurl_webdav_config;
struct knurl_xml_route_config;
/** Library-owned route group receiver. Pass this handle as the first
 * argument to its methods. Obtain it through the documented constructor or
 * borrowed accessor. Do not copy, initialize or modify the shell or impl.
 */
struct knurl_route_group {
    /** Register a mapped XML live-input route under the group prefix. The
     * bounded native parser returns the mapped record to owner-loop handlers;
     * retain schemas/userdata through router destruction.
     */
    knurl_status (*xml_route)(struct knurl_route_group *,
                              const struct knurl_xml_route_config *,
                              struct knurl_route **, struct knurl_error *);
    /** Register a live chunk upload route under the group prefix. Owner-loop
     * callbacks consume prefixes; AGAIN pauses until body_resume. Userdata
     * remains borrowed.
     */
    knurl_status (*upload_stream)(struct knurl_route_group *,
                                  const struct knurl_upload_stream_config *,
                                  struct knurl_route **, struct knurl_error *);
    /** Register a bounded native-reader upload route under the group prefix.
     * Native handlers use copied metadata and a borrowed reader; retain
     * userdata through cancellation acknowledgement.
     */
    knurl_status (*upload_reader)(struct knurl_route_group *,
                                  const struct knurl_upload_reader_config *,
                                  struct knurl_route **, struct knurl_error *);
    /** Register a native incremental file upload under the group prefix. The
     * explicit destination opens after admission; partial files remain caller
     * artifacts.
     */
    knurl_status (*upload_file)(struct knurl_route_group *,
                                const struct knurl_upload_file_config *,
                                struct knurl_route **, struct knurl_error *);
    /** Register a mapped JSON input/output route under the group prefix.
     * Schemas/runtime and handler userdata remain borrowed; ordinary output is
     * serialized before after-hooks.
     */
    knurl_status (*json_route)(struct knurl_route_group *group,
                               const struct knurl_json_route_config *config,
                               struct knurl_route **out,
                               struct knurl_error *error);
    /** Register a mapped JSON input/staged-reply route under the group prefix.
     * Schemas/runtime and handler userdata remain borrowed; explicit live
     * outputs retain their record owners.
     */
    knurl_status (*json_typed_route)(
        struct knurl_route_group *group,
        const struct knurl_json_typed_route_config *config,
        struct knurl_route **out, struct knurl_error *error);
    /** Register a buffered REST route under the group prefix.
     * Decode/validate/handler/error phases reuse Core; successful decoded
     * owners release once.
     */
    knurl_status (*rest_route)(struct knurl_route_group *,
                               const struct knurl_rest_route_config *,
                               struct knurl_route **, struct knurl_error *);
    /** Register a route under the group's prefix and hooks; ownership matches
     * knurl_router_route.
     */
    knurl_status (*route)(struct knurl_route_group *group,
                          const struct knurl_route_config *config,
                          struct knurl_route **out, struct knurl_error *error);
    /** Register a static file route under the group prefix. Declarations are
     * copied, callbacks remain borrowed; range/conditional delivery uses the
     * asset engine.
     */
    knurl_status (*static_file)(struct knurl_route_group *,
                                const struct knurl_static_config *,
                                struct knurl_route **, struct knurl_error *);
    /** Register a confined filesystem directory mount under the group prefix.
     * Directory indexes are disabled by default; no file lookup occurs during
     * registration.
     */
    knurl_status (*static_directory)(struct knurl_route_group *,
                                     const struct knurl_static_config *,
                                     struct knurl_route **,
                                     struct knurl_error *);
    /** Register a borrowed read-only asset-provider mount under the group
     * prefix. Retain the provider and hook userdata through router destruction.
     */
    knurl_status (*static_assets)(struct knurl_route_group *,
                                  const struct knurl_static_config *,
                                  struct knurl_route **, struct knurl_error *);
    /** Register a WebDAV mount under the group prefix. Declarations are copied;
     * custom provider/auth owners remain borrowed. See knurl_router_webdav.
     */
    knurl_status (*webdav)(struct knurl_route_group *,
                           const struct knurl_webdav_config *,
                           struct knurl_route **, struct knurl_error *);
    /** Private library state; applications must not access it. */
    struct knurl_route_group_impl *impl;
};

struct knurl_route_config;
/** Library-owned route receiver. Pass this handle as the first
 * argument to its methods. Obtain it through the documented constructor or
 * borrowed accessor. Do not copy, initialize or modify the shell or impl.
 */
struct knurl_route {
    /** Borrow immutable route configuration until router destruction; no
     * callback is invoked.
     */
    const struct knurl_route_config *(*declaration)(
        const struct knurl_route *route);
    /** Private library state; applications must not access it. */
    struct knurl_route_impl *impl;
};

#ifdef __cplusplus
}
#endif
#endif

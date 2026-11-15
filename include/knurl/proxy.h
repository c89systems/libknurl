#ifndef KNURL_PROXY_H
#define KNURL_PROXY_H

/** @file proxy.h Public proxy API. Expected failures return knurl_status and
 * optional bounded knurl_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <knurl/router.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Protocol selection for ordinary HTTP and SSE upstream exchanges. */
typedef enum knurl_proxy_http_version {
    /** Prefer HTTP/2 over HTTPS with HTTP/1.1 fallback; cleartext uses
     * HTTP/1.1.
     */
    KNURL_PROXY_HTTP_AUTO = 0,
    /** Force HTTP/1.1 for every exchange. WebSocket always uses HTTP/1.1. */
    KNURL_PROXY_HTTP_1_1 = 1
} knurl_proxy_http_version;

/** Callback-scoped inbound proxy metadata; never retain after the hook. */
typedef struct knurl_proxy_inbound knurl_proxy_inbound;
/** Callback-scoped validated proxy request editor; failed edits are sticky. */
typedef struct knurl_proxy_outbound knurl_proxy_outbound;
/** Callback-scoped sanitized downstream response metadata/editor. */
typedef struct knurl_proxy_response knurl_proxy_response;
/** Callback-scoped local response builder used before upstream/downstream
 * commitment.
 */
typedef struct knurl_proxy_local_response knurl_proxy_local_response;
/** Terminal owner-loop proxy accounting, once per admitted upstream exchange
 * before host request close. Strings/request are borrowed for the callback; no
 * request I/O is permitted. HTTP counts exclude heads/framing/trailers;
 * WebSocket counts exclude its handshake. Counts measure accepted bytes, not
 * proven peer delivery.
 */
typedef struct knurl_proxy_exchange {
    /** Exact terminal proxy exchange identifier. */
    knurl_u64 request_id;
    /** HTTP upload payload bytes consumed by curl, excluding HTTP framing;
     * WebSocket opaque bytes accepted upstream, including frame bytes but
     * excluding the opening handshake.
     */
    knurl_u64 upstream_bytes;
    /** HTTP payload bytes accepted downstream, excluding HTTP framing;
     * WebSocket opaque bytes accepted downstream, including frame bytes but
     * excluding the opening handshake.
     */
    knurl_u64 downstream_bytes;
    /** Exact monotonic operation duration in milliseconds. */
    knurl_u64 duration_ms;
    /** Copied configured route name/path for terminal proxy accounting. */
    const char *route;
    /** Selected configured upstream URL in terminal accounting. */
    const char *upstream;
    /** Representation or event discriminator; inspect before interpreting other
     * fields.
     */
    const char *kind;
    /** Downstream/upstream/host attribution when known. */
    const char *disconnect_side;
    /** Terminal proxy failure category; "none" on success. */
    const char *error_category;
    /** HTTP status code; zero means unknown/unset where documented by this
     * event/builder.
     */
    int status;
    /** Portable terminal status; OK indicates successful completion. */
    knurl_status cause;
} knurl_proxy_exchange;
/** Once-per-admitted-exchange owner-loop terminal accounting. Request/snapshot
 * are borrowed for the call; no request I/O.
 */
typedef void (*knurl_proxy_exchange_fn)(knurl_request *,
                                        const knurl_proxy_exchange *, void *);

/** Maximum copied body size for a proxy-generated local response. */
#define KNURL_PROXY_LOCAL_BODY_LIMIT 65536u

/**
 * Set the local response status and copy its optional body. A callback that
 * leaves the status unset forwards the request (preflight) or uses the default
 * gateway error (on_error). Body bytes may contain NUL. Status must be final,
 * excluding 101; 204, 205, and 304 require an empty body. A failed edit is
 * sticky and rejects the callback result even when its error is ignored.
 */
KNURL_API knurl_status knurl_proxy_local_respond(
    knurl_proxy_local_response *response, int status, const void *body,
    size_t body_length, knurl_error *error);

/** Add a copied end-to-end response header after setting the local status. */
KNURL_API knurl_status knurl_proxy_local_add_header(
    knurl_proxy_local_response *response, const char *name, const char *value,
    knurl_error *error);

/**
 * Headers-time admission hook. The inbound view is borrowed and has no body.
 * Return KNURL_OK without a local response to continue to the upstream;
 * call knurl_proxy_local_respond() to answer locally before any connect.
 * A local reply closes the downstream connection after sending its body.
 */
typedef knurl_status (*knurl_proxy_preflight_fn)(
    const knurl_proxy_inbound *inbound, knurl_proxy_local_response *response,
    void *userdata, knurl_error *error);

/**
 * Optional gateway error hook before downstream headers are committed. The
 * cause and local response are borrowed for this synchronous callback. Leave
 * the response unset to use the default_status (502 or 504). Returning an
 * error or making an invalid edit also falls back to that default response.
 * Once headers have been sent, the transport aborts instead of calling this.
 */
typedef knurl_status (*knurl_proxy_on_error_fn)(
    const knurl_error *cause, int default_status,
    knurl_proxy_local_response *response, void *userdata, knurl_error *error);

/**
 * Synchronous request rewrite, called after libknurl validates and sanitizes
 * the inbound headers and before any upstream connection. Both views are
 * borrowed for this call. Returning an error rejects the request locally.
 * A failed outbound edit rejects the request even if its error is ignored.
 * The callback must not block, retain either view, or consume a request body.
 */
typedef knurl_status (*knurl_proxy_rewrite_fn)(
    const knurl_proxy_inbound *inbound, knurl_proxy_outbound *outbound,
    void *userdata, knurl_error *error);

/** Return the inbound method; the view is borrowed for the callback. */
KNURL_API knurl_http_method
knurl_proxy_inbound_method(const knurl_proxy_inbound *in);
/** Return the validated raw path, borrowed until the callback returns. */
KNURL_API const char *knurl_proxy_inbound_path(const knurl_proxy_inbound *in);
/** Return the raw query, or NULL when absent; borrowed for the callback. */
KNURL_API const char *knurl_proxy_inbound_query(const knurl_proxy_inbound *in);
/** Return the inbound Host value, borrowed until the callback returns. */
KNURL_API const char *knurl_proxy_inbound_host(const knurl_proxy_inbound *in);
/** Return nonzero when this request is a validated WebSocket upgrade. */
KNURL_API int knurl_proxy_inbound_websocket(const knurl_proxy_inbound *in);
/** Return a borrowed route capture, or an empty view when unmatched. */
KNURL_API knurl_bytes
knurl_proxy_inbound_path_param(const knurl_proxy_inbound *in, const char *name);
/** Return the number of inbound request header fields, including repeats. */
KNURL_API size_t
knurl_proxy_inbound_header_count(const knurl_proxy_inbound *in);
/** Read one inbound header; output strings are borrowed for the callback. */
KNURL_API knurl_status
knurl_proxy_inbound_header_at(const knurl_proxy_inbound *in, size_t index,
                              const char **name, const char **value);

/**
 * Select the configured target at index zero or an alternate in registration
 * order. An unconfigured index fails admission even if the error is ignored.
 */
KNURL_API knurl_status knurl_proxy_outbound_select_target(
    knurl_proxy_outbound *out, size_t index, knurl_error *error);
/** Set the forwarded method; WebSocket upgrades must retain GET. */
KNURL_API knurl_status knurl_proxy_outbound_set_method(
    knurl_proxy_outbound *out, knurl_http_method method, knurl_error *error);
/** Copy a validated raw origin-form path for the upstream request. */
KNURL_API knurl_status knurl_proxy_outbound_set_path(knurl_proxy_outbound *out,
                                                     const char *raw_path,
                                                     knurl_error *error);
/** Copy the raw query; NULL removes it. Final target validation still applies.
 */
KNURL_API knurl_status knurl_proxy_outbound_set_query(knurl_proxy_outbound *out,
                                                      const char *raw_query,
                                                      knurl_error *error);
/** Copy an explicit Host value; NULL restores the selected target authority. */
KNURL_API knurl_status knurl_proxy_outbound_set_host(knurl_proxy_outbound *out,
                                                     const char *host,
                                                     knurl_error *error);
/** Append a copied end-to-end request header, preserving repeated fields. */
KNURL_API knurl_status
knurl_proxy_outbound_add_header(knurl_proxy_outbound *out, const char *name,
                                const char *value, knurl_error *error);
/** Replace all fields with this name by one copied end-to-end header.
 * Name and value may be borrowed from header_at(); successful replacement
 * invalidates strings borrowed from the removed fields. */
KNURL_API knurl_status
knurl_proxy_outbound_set_header(knurl_proxy_outbound *out, const char *name,
                                const char *value, knurl_error *error);
/** Remove all fields with this name, which may be borrowed from header_at().
 * Removed fields' borrowed strings become invalid. Transport, forwarding,
 * and handshake fields are owned by libknurl. */
KNURL_API knurl_status knurl_proxy_outbound_remove_header(
    knurl_proxy_outbound *out, const char *name, knurl_error *error);

/**
 * Synchronous final-response metadata hook, before downstream headers are
 * committed. Receives no body. Called for HTTP, SSE, and non-101 WebSocket
 * rejections; a validated 101 handshake bypasses it. The view is borrowed
 * only for this call and must not be retained or used to block the worker.
 * Returning an error or ignoring a failed edit rejects the upstream response
 * before commitment.
 */
typedef knurl_status (*knurl_proxy_modify_response_fn)(
    knurl_proxy_response *response, void *userdata, knurl_error *error);

/** Return the current downstream final status before headers are committed. */
KNURL_API int knurl_proxy_response_status(const knurl_proxy_response *response);
/** Return the number of sanitized downstream response header fields. */
KNURL_API size_t
knurl_proxy_response_header_count(const knurl_proxy_response *response);
/** Read one response header; output strings are borrowed for the callback. */
KNURL_API knurl_status knurl_proxy_response_header_at(
    const knurl_proxy_response *response, size_t index, const char **name,
    const char **value);

/**
 * Set a final non-101 status without changing whether the upstream body can be
 * sent. HEAD and 204/205/304 body rules remain in force. Failed edits are
 * sticky even if the response hook ignores their error.
 */
KNURL_API knurl_status knurl_proxy_response_set_status(
    knurl_proxy_response *response, int status, knurl_error *error);
/** Append a copied end-to-end response header, preserving repeated fields. */
KNURL_API knurl_status knurl_proxy_response_add_header(
    knurl_proxy_response *response, const char *name, const char *value,
    knurl_error *error);
/** Replace all fields with this name by one copied end-to-end header. */
KNURL_API knurl_status knurl_proxy_response_set_header(
    knurl_proxy_response *response, const char *name, const char *value,
    knurl_error *error);
/** Remove all fields with this name; transport, forwarding, and handshake
 * fields are owned by libknurl. */
KNURL_API knurl_status knurl_proxy_response_remove_header(
    knurl_proxy_response *response, const char *name, knurl_error *error);

/**
 * Registration input for one in-process reverse proxy route.
 *
 * Zero-initialize or call knurl_proxy_config_init(). libknurl copies the
 * path and configured target URLs on successful registration. The route uses
 * existing libknurl path matching and registration order. Request and response
 * bodies flow through bounded chunk buffers; they are never materialized.
 */
typedef struct knurl_proxy_config {
    /** Optional retained route ACL, checked before proxy callbacks/allocation.
     */
    knurl_acl *acl;
    /** Required libknurl route pattern; copied before registration returns. */
    const char *path;
    /** Zero selects all supported libknurl HTTP methods. WebSocket requires
     * GET. */
    knurl_http_methods methods;
    /** Zero selects a literal path. Parameter and regex routes are supported.
     */
    knurl_route_path_kind path_kind;
    /** Required HTTP or HTTPS base URL. Its authority is fixed for this route.
     */
    const char *target;
    /** Optional additional allowed base URLs. The director may select only a
     * configured target. Each string and the array are borrowed until this
     * registration call returns, then copied by libknurl. */
    const char *const *alternate_targets;
    /** Number of additional configured targets in alternate_targets. */
    size_t alternate_target_count;
    /** Zero selects KNURL_PROXY_HTTP_AUTO. */
    knurl_proxy_http_version upstream_http_version;
    /** Zero uses a 10-second connection deadline; negative values are invalid.
     */
    long connect_timeout_ms;
    /** Zero uses a 60-second no-progress deadline; negative values are invalid.
     */
    long idle_timeout_ms;
    /** Zero disables the total deadline, including for SSE and WebSocket. */
    long total_timeout_ms;
    /** Zero uses a 16 KiB per-direction transport chunk limit. Allowed range is
     * 8 KiB through 1 MiB. Header limits are separate. */
    size_t buffer_limit_bytes;
    /** Optional PEM CA bundle for verifying HTTPS and WSS upstream peers.
     * NULL uses libcurl's configured default trust store. The PEM text is
     * borrowed until registration returns, then copied. It replaces libcurl's
     * default CA bundle for this route; peer and hostname checks remain on.
     * A nonempty bundle may contain at most 256 KiB of PEM text. */
    const char *tls_ca_pem;
    /** Optional PEM client certificate chain and private key for HTTPS/WSS
     * upstream authentication. Both must be set together. Each is borrowed
     * until registration returns, then copied, and may contain 1 to 256 KiB
     * of PEM text. No certificate or key file is created. */
    const char *tls_client_cert_pem;
    /** PEM key paired with tls_client_cert_pem; copied and cleansed on release.
     */
    const char *tls_client_key_pem;
    /** Optional synchronous rewrite hook and borrowed application context. */
    knurl_proxy_rewrite_fn rewrite;
    /** Caller-owned context; retain it until router destruction completes. */
    void *rewrite_userdata;
    /** Optional headers-time admission hook and borrowed application context.
     */
    knurl_proxy_preflight_fn preflight;
    /** Caller-owned context; retain it until router destruction completes. */
    void *preflight_userdata;
    /** Optional final response hook and borrowed application context. */
    knurl_proxy_modify_response_fn modify_response;
    /** Caller-owned context; retain it until router destruction completes. */
    void *modify_response_userdata;
    /** Optional uncommitted gateway-error hook and borrowed context. */
    knurl_proxy_on_error_fn on_error;
    /** Caller-owned context; retain it until router destruction completes. */
    void *on_error_userdata;
    /** Owner-loop terminal proxy accounting callback, once per admitted
     * exchange; must not perform request I/O.
     */
    knurl_proxy_exchange_fn on_exchange;
    /** Caller-owned terminal accounting context retained through router
     * destruction.
     */
    void *on_exchange_userdata;
} knurl_proxy_config;

/** Set the documented zero-value defaults. NULL is ignored. */
KNURL_API void knurl_proxy_config_init(knurl_proxy_config *config);

/** Registration copies strings, targets, TLS material and callbacks. */
KNURL_API knurl_status knurl_router_proxy(knurl_router *router,
                                          const knurl_proxy_config *config,
                                          knurl_route **route,
                                          knurl_error *error);

#ifdef __cplusplus
}
#endif
#endif

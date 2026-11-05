#ifndef ROTMOS_PROXY_H
#define ROTMOS_PROXY_H

#include <rotmos/router.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Protocol selection for ordinary HTTP and SSE upstream exchanges. */
typedef enum rotmos_proxy_http_version {
    /* Prefer HTTP/2 over HTTPS with HTTP/1.1 fallback; cleartext uses HTTP/1.1.
     */
    ROTMOS_PROXY_HTTP_AUTO = 0,
    /* Force HTTP/1.1 for every exchange. WebSocket always uses HTTP/1.1. */
    ROTMOS_PROXY_HTTP_1_1 = 1
} rotmos_proxy_http_version;

typedef struct rotmos_proxy_inbound rotmos_proxy_inbound;
typedef struct rotmos_proxy_outbound rotmos_proxy_outbound;
typedef struct rotmos_proxy_response rotmos_proxy_response;
typedef struct rotmos_proxy_local_response rotmos_proxy_local_response;
/* Terminal accounting lifted from Vectis's proxy telemetry. HTTP counts are
 * upload bytes consumed by curl and body bytes accepted downstream, excluding
 * heads/chunk framing/trailers. WebSocket counts are accepted opaque relay
 * bytes, excluding the opening handshake. Strings/request are borrowed during
 * the owner-loop callback, once per admitted upstream exchange, before host
 * request-close notification. The callback must not perform request I/O.
 * Hosts retain event formatting and aggregation. */
typedef struct rotmos_proxy_exchange {
    rotmos_u64 request_id, upstream_bytes, downstream_bytes, duration_ms;
    const char *route, *upstream, *kind, *disconnect_side, *error_category;
    int status;
    rotmos_status cause;
} rotmos_proxy_exchange;
typedef void (*rotmos_proxy_exchange_fn)(rotmos_request *,
                                         const rotmos_proxy_exchange *, void *);

/** Maximum copied body size for a proxy-generated local response. */
#define ROTMOS_PROXY_LOCAL_BODY_LIMIT 65536u

/**
 * Set the local response status and copy its optional body. A callback that
 * leaves the status unset forwards the request (preflight) or uses the default
 * gateway error (on_error). Body bytes may contain NUL. Status must be final,
 * excluding 101; 204, 205, and 304 require an empty body. A failed edit is
 * sticky and rejects the callback result even when its error is ignored.
 */
ROTMOS_API rotmos_status rotmos_proxy_local_respond(
    rotmos_proxy_local_response *response, int status, const void *body,
    size_t body_length, rotmos_error *error);

/** Add a copied end-to-end response header after setting the local status. */
ROTMOS_API rotmos_status rotmos_proxy_local_add_header(
    rotmos_proxy_local_response *response, const char *name, const char *value,
    rotmos_error *error);

/**
 * Headers-time admission hook. The inbound view is borrowed and has no body.
 * Return ROTMOS_OK without a local response to continue to the upstream;
 * call rotmos_proxy_local_respond() to answer locally before any connect.
 * A local reply closes the downstream connection after sending its body.
 */
typedef rotmos_status (*rotmos_proxy_preflight_fn)(
    const rotmos_proxy_inbound *inbound, rotmos_proxy_local_response *response,
    void *userdata, rotmos_error *error);

/**
 * Optional gateway error hook before downstream headers are committed. The
 * cause and local response are borrowed for this synchronous callback. Leave
 * the response unset to use the default_status (502 or 504). Returning an
 * error or making an invalid edit also falls back to that default response.
 * Once headers have been sent, the transport aborts instead of calling this.
 */
typedef rotmos_status (*rotmos_proxy_on_error_fn)(
    const rotmos_error *cause, int default_status,
    rotmos_proxy_local_response *response, void *userdata, rotmos_error *error);

/**
 * Synchronous request rewrite, called after librotmos validates and sanitizes
 * the inbound headers and before any upstream connection. Both views are
 * borrowed for this call. Returning an error rejects the request locally.
 * A failed outbound edit rejects the request even if its error is ignored.
 * The callback must not block, retain either view, or consume a request body.
 */
typedef rotmos_status (*rotmos_proxy_rewrite_fn)(
    const rotmos_proxy_inbound *inbound, rotmos_proxy_outbound *outbound,
    void *userdata, rotmos_error *error);

/** Return the inbound method; the view is borrowed for the callback. */
ROTMOS_API rotmos_http_method
rotmos_proxy_inbound_method(const rotmos_proxy_inbound *in);
/** Return the validated raw path, borrowed until the callback returns. */
ROTMOS_API const char *
rotmos_proxy_inbound_path(const rotmos_proxy_inbound *in);
/** Return the raw query, or NULL when absent; borrowed for the callback. */
ROTMOS_API const char *
rotmos_proxy_inbound_query(const rotmos_proxy_inbound *in);
/** Return the inbound Host value, borrowed until the callback returns. */
ROTMOS_API const char *
rotmos_proxy_inbound_host(const rotmos_proxy_inbound *in);
/** Return nonzero when this request is a validated WebSocket upgrade. */
ROTMOS_API int rotmos_proxy_inbound_websocket(const rotmos_proxy_inbound *in);
/** Return a borrowed route capture, or an empty view when unmatched. */
ROTMOS_API rotmos_bytes rotmos_proxy_inbound_path_param(
    const rotmos_proxy_inbound *in, const char *name);
/** Return the number of inbound request header fields, including repeats. */
ROTMOS_API size_t
rotmos_proxy_inbound_header_count(const rotmos_proxy_inbound *in);
/** Read one inbound header; output strings are borrowed for the callback. */
ROTMOS_API rotmos_status
rotmos_proxy_inbound_header_at(const rotmos_proxy_inbound *in, size_t index,
                               const char **name, const char **value);

/**
 * Select the configured target at index zero or an alternate in registration
 * order. An unconfigured index fails admission even if the error is ignored.
 */
ROTMOS_API rotmos_status rotmos_proxy_outbound_select_target(
    rotmos_proxy_outbound *out, size_t index, rotmos_error *error);
/** Set the forwarded method; WebSocket upgrades must retain GET. */
ROTMOS_API rotmos_status rotmos_proxy_outbound_set_method(
    rotmos_proxy_outbound *out, rotmos_http_method method, rotmos_error *error);
/** Copy a validated raw origin-form path for the upstream request. */
ROTMOS_API rotmos_status rotmos_proxy_outbound_set_path(
    rotmos_proxy_outbound *out, const char *raw_path, rotmos_error *error);
/** Copy the raw query; NULL removes it. Final target validation still applies.
 */
ROTMOS_API rotmos_status rotmos_proxy_outbound_set_query(
    rotmos_proxy_outbound *out, const char *raw_query, rotmos_error *error);
/** Copy an explicit Host value; NULL restores the selected target authority. */
ROTMOS_API rotmos_status rotmos_proxy_outbound_set_host(
    rotmos_proxy_outbound *out, const char *host, rotmos_error *error);
/** Append a copied end-to-end request header, preserving repeated fields. */
ROTMOS_API rotmos_status
rotmos_proxy_outbound_add_header(rotmos_proxy_outbound *out, const char *name,
                                 const char *value, rotmos_error *error);
/** Replace all fields with this name by one copied end-to-end header.
 * Name and value may be borrowed from header_at(); successful replacement
 * invalidates strings borrowed from the removed fields. */
ROTMOS_API rotmos_status
rotmos_proxy_outbound_set_header(rotmos_proxy_outbound *out, const char *name,
                                 const char *value, rotmos_error *error);
/** Remove all fields with this name, which may be borrowed from header_at().
 * Removed fields' borrowed strings become invalid. Transport, forwarding,
 * and handshake fields are owned by librotmos. */
ROTMOS_API rotmos_status rotmos_proxy_outbound_remove_header(
    rotmos_proxy_outbound *out, const char *name, rotmos_error *error);

/**
 * Synchronous final-response metadata hook, before downstream headers are
 * committed. Receives no body. Called for HTTP, SSE, and non-101 WebSocket
 * rejections; a validated 101 handshake bypasses it. The view is borrowed
 * only for this call and must not be retained or used to block the worker.
 * Returning an error or ignoring a failed edit rejects the upstream response
 * before commitment.
 */
typedef rotmos_status (*rotmos_proxy_modify_response_fn)(
    rotmos_proxy_response *response, void *userdata, rotmos_error *error);

/** Return the current downstream final status before headers are committed. */
ROTMOS_API int
rotmos_proxy_response_status(const rotmos_proxy_response *response);
/** Return the number of sanitized downstream response header fields. */
ROTMOS_API size_t
rotmos_proxy_response_header_count(const rotmos_proxy_response *response);
/** Read one response header; output strings are borrowed for the callback. */
ROTMOS_API rotmos_status rotmos_proxy_response_header_at(
    const rotmos_proxy_response *response, size_t index, const char **name,
    const char **value);

/**
 * Set a final non-101 status without changing whether the upstream body can be
 * sent. HEAD and 204/205/304 body rules remain in force. Failed edits are
 * sticky even if the response hook ignores their error.
 */
ROTMOS_API rotmos_status rotmos_proxy_response_set_status(
    rotmos_proxy_response *response, int status, rotmos_error *error);
/** Append a copied end-to-end response header, preserving repeated fields. */
ROTMOS_API rotmos_status rotmos_proxy_response_add_header(
    rotmos_proxy_response *response, const char *name, const char *value,
    rotmos_error *error);
/** Replace all fields with this name by one copied end-to-end header. */
ROTMOS_API rotmos_status rotmos_proxy_response_set_header(
    rotmos_proxy_response *response, const char *name, const char *value,
    rotmos_error *error);
/** Remove all fields with this name; transport, forwarding, and handshake
 * fields are owned by librotmos. */
ROTMOS_API rotmos_status rotmos_proxy_response_remove_header(
    rotmos_proxy_response *response, const char *name, rotmos_error *error);

/**
 * Registration input for one in-process reverse proxy route.
 *
 * Zero-initialize or call rotmos_proxy_config_init(). librotmos copies the
 * path and configured target URLs on successful registration. The route uses
 * existing librotmos path matching and registration order. Request and response
 * bodies flow through bounded chunk buffers; they are never materialized.
 */
typedef struct rotmos_proxy_config {
    /* Required librotmos route pattern; copied before registration returns. */
    const char *path;
    /* Zero selects all supported librotmos HTTP methods. WebSocket requires
     * GET. */
    rotmos_http_methods methods;
    /* Zero selects a literal path. Parameter and regex routes are supported. */
    rotmos_route_path_kind path_kind;
    /* Required HTTP or HTTPS base URL. Its authority is fixed for this route.
     */
    const char *target;
    /* Optional additional allowed base URLs. The director may select only a
     * configured target. Each string and the array are borrowed until this
     * registration call returns, then copied by librotmos. */
    const char *const *alternate_targets;
    /* Number of additional configured targets in alternate_targets. */
    size_t alternate_target_count;
    /* Zero selects ROTMOS_PROXY_HTTP_AUTO. */
    rotmos_proxy_http_version upstream_http_version;
    /* Zero uses a 10-second connection deadline; negative values are invalid.
     */
    long connect_timeout_ms;
    /* Zero uses a 60-second no-progress deadline; negative values are invalid.
     */
    long idle_timeout_ms;
    /* Zero disables the total deadline, including for SSE and WebSocket. */
    long total_timeout_ms;
    /* Zero uses a 16 KiB per-direction transport chunk limit. Allowed range is
     * 8 KiB through 1 MiB. Header limits are separate. */
    size_t buffer_limit_bytes;
    /* Optional PEM CA bundle for verifying HTTPS and WSS upstream peers.
     * NULL uses libcurl's configured default trust store. The PEM text is
     * borrowed until registration returns, then copied. It replaces libcurl's
     * default CA bundle for this route; peer and hostname checks remain on.
     * A nonempty bundle may contain at most 256 KiB of PEM text. */
    const char *tls_ca_pem;
    /* Optional PEM client certificate chain and private key for HTTPS/WSS
     * upstream authentication. Both must be set together. Each is borrowed
     * until registration returns, then copied, and may contain 1 to 256 KiB
     * of PEM text. No certificate or key file is created. */
    const char *tls_client_cert_pem;
    /* PEM key paired with tls_client_cert_pem; copied and cleansed on release.
     */
    const char *tls_client_key_pem;
    /* Optional synchronous rewrite hook and borrowed application context. */
    rotmos_proxy_rewrite_fn rewrite;
    /* Caller-owned context; retain it until router destruction completes. */
    void *rewrite_userdata;
    /* Optional headers-time admission hook and borrowed application context. */
    rotmos_proxy_preflight_fn preflight;
    /* Caller-owned context; retain it until router destruction completes. */
    void *preflight_userdata;
    /* Optional final response hook and borrowed application context. */
    rotmos_proxy_modify_response_fn modify_response;
    /* Caller-owned context; retain it until router destruction completes. */
    void *modify_response_userdata;
    /* Optional uncommitted gateway-error hook and borrowed context. */
    rotmos_proxy_on_error_fn on_error;
    /* Caller-owned context; retain it until router destruction completes. */
    void *on_error_userdata;
    rotmos_proxy_exchange_fn on_exchange;
    void *on_exchange_userdata;
} rotmos_proxy_config;

/** Set the documented zero-value defaults. NULL is ignored. */
ROTMOS_API void rotmos_proxy_config_init(rotmos_proxy_config *config);

/* Registration copies strings, targets, TLS material and callbacks. */
ROTMOS_API rotmos_status rotmos_router_proxy(rotmos_router *router,
                                             const rotmos_proxy_config *config,
                                             rotmos_route **route,
                                             rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif

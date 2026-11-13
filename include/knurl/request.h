#ifndef KNURL_REQUEST_H
#define KNURL_REQUEST_H

/** @file request.h Public request API. Expected failures return knurl_status
 * and optional bounded knurl_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <knurl/http.h>
#include <knurl/io.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Numeric address/port view borrowed from a request or callback snapshot. */
typedef struct knurl_endpoint {
    /** Numeric address byte view, borrowed from its owner. */
    knurl_bytes address;
    /** TCP port of this observed peer or local endpoint. */
    unsigned short port;
} knurl_endpoint;

/** Borrow the owning server; access only within the request's valid scope. */
KNURL_API knurl_server *knurl_request_server(const knurl_request *request);
/** Borrow the request's owner loop; never retain across fork/run boundaries. */
KNURL_API knurl_loop *knurl_request_loop(const knurl_request *request);
/** Read the parsed HTTP method enum; use method_token for the original bytes.
 */
KNURL_API knurl_http_method knurl_request_method(const knurl_request *request);
/** Borrow the original method token until request close. */
KNURL_API knurl_bytes knurl_request_method_token(const knurl_request *request);
/** Borrow the HTTP version token until request close. */
KNURL_API knurl_bytes knurl_request_version(const knurl_request *request);
/** Borrow the original request target, including its raw query, until request
 * close.
 */
KNURL_API knurl_bytes knurl_request_target(const knurl_request *request);
/** Borrow the undecoded request path until request close. */
KNURL_API knurl_bytes knurl_request_raw_path(const knurl_request *request);
/** Borrow the backend's request path used for routing until request close. */
KNURL_API knurl_bytes knurl_request_path(const knurl_request *request);
/** Borrow the raw query span until request close. */
KNURL_API knurl_bytes knurl_request_query(const knurl_request *request);
/** Borrow the inbound Host/authority span until request close. */
KNURL_API knurl_bytes knurl_request_authority(const knurl_request *request);
/** Return the zero-based listener index which accepted this request. */
KNURL_API size_t knurl_request_listener_index(const knurl_request *request);
/** Count ordered request headers, including repeated fields. */
KNURL_API size_t knurl_request_header_count(const knurl_request *request);
/** Count separate body trailers; zero before body completion. */
KNURL_API size_t knurl_request_trailer_count(const knurl_request *);
/** Trailer access requires completed input, otherwise STATE. Views expire
 * with the request; legal repeated fields retain order. */
KNURL_API knurl_status knurl_request_trailer_at(const knurl_request *, size_t,
                                                knurl_header *, knurl_error *);
/** First case-insensitive trailer, EOF if absent, STATE before completion. */
KNURL_API knurl_status knurl_request_trailer_first(const knurl_request *,
                                                   const char *, knurl_bytes *,
                                                   knurl_error *);
/** Borrow one ordered request header by zero-based index until request close.
 */
KNURL_API knurl_status knurl_request_header_at(const knurl_request *request,
                                               size_t index, knurl_header *out,
                                               knurl_error *error);
/** First lookups return EOF for absence and OK for a present empty value. */
KNURL_API knurl_status knurl_request_header_first(const knurl_request *request,
                                                  const char *name,
                                                  knurl_bytes *out,
                                                  knurl_error *error);
/** Count decoded query pairs, including repeated names and empty values. */
KNURL_API size_t knurl_request_query_count(const knurl_request *request);
/** Borrow a decoded query pair by zero-based index until request close. */
KNURL_API knurl_status knurl_request_query_at(const knurl_request *request,
                                              size_t index, knurl_header *out,
                                              knurl_error *error);
/** Borrow the first decoded query value; EOF means absent, OK includes a
 * present empty value.
 */
KNURL_API knurl_status knurl_request_query_first(const knurl_request *request,
                                                 const char *name,
                                                 knurl_bytes *out,
                                                 knurl_error *error);
/** Borrow a named decoded capture; EOF means no matching capture. */
KNURL_API knurl_status knurl_request_path_param(const knurl_request *request,
                                                const char *name,
                                                knurl_bytes *out,
                                                knurl_error *error);
/** Exact typed query/path lookups. EOF means absent; duplicate query names
 * are INVALID. Bounds are inclusive and failure preserves numeric output. */
KNURL_API knurl_status knurl_request_query_long(const knurl_request *,
                                                const char *, long minimum,
                                                long maximum, long *out,
                                                knurl_error *);
/** Exact signed-decimal path capture within inclusive native-long bounds;
 * EOF absent, INVALID syntax/bounds, OVERFLOW unrepresentable. */
KNURL_API knurl_status knurl_request_param_long(const knurl_request *,
                                                const char *, long minimum,
                                                long maximum, long *out,
                                                knurl_error *);
/** Exact unsigned-decimal query with inclusive portable bounds. Duplicate
 * names are INVALID, absence EOF; failure preserves output. */
KNURL_API knurl_status knurl_request_query_u64(const knurl_request *,
                                               const char *, knurl_u64 minimum,
                                               knurl_u64 maximum,
                                               knurl_u64 *out, knurl_error *);
/** Exact unsigned-decimal path capture with inclusive portable bounds;
 * EOF absent, INVALID syntax/bounds, OVERFLOW unrepresentable. */
KNURL_API knurl_status knurl_request_param_u64(const knurl_request *,
                                               const char *, knurl_u64 minimum,
                                               knurl_u64 maximum,
                                               knurl_u64 *out, knurl_error *);
/** Query accepts exactly true/false/1/0; duplicates INVALID, absence EOF.
 * Successful false writes zero; failure preserves output. */
KNURL_API knurl_status knurl_request_query_boolean(const knurl_request *,
                                                   const char *, int *out,
                                                   knurl_error *);
/** Path capture accepts exactly true/false/1/0; absence EOF, syntax INVALID.
 * Successful false writes zero; failure preserves output. */
KNURL_API knurl_status knurl_request_param_boolean(const knurl_request *,
                                                   const char *, int *out,
                                                   knurl_error *);
/** Borrow the real TCP peer endpoint, unaffected by forwarded-address policy,
 * until request close.
 */
KNURL_API knurl_endpoint knurl_request_peer(const knurl_request *request);
/** Borrow the accepted local endpoint until request close. */
KNURL_API knurl_endpoint knurl_request_local(const knurl_request *request);
/** Copied identity values, separate from the unmodified TCP peer. Correlation
 * is empty unless a resolver is configured. Views expire at request close. */
KNURL_API knurl_bytes knurl_request_effective_address(const knurl_request *);
/** Borrow the resolved correlation ID until request close; empty unless
 * configured.
 */
KNURL_API knurl_bytes knurl_request_correlation_id(const knurl_request *);
/** Emit a named application policy/observation event on the owner loop;
 * observers cannot reenter emission.
 */
KNURL_API knurl_status knurl_request_event(knurl_request *, const char *name,
                                           knurl_error *);
/** Return nonzero when the downstream connection uses TLS. */
KNURL_API int knurl_request_is_tls(const knurl_request *request);
/** Read framing metadata without consuming input; present distinguishes absence
 * from zero length.
 */
KNURL_API knurl_status
knurl_request_content_length(const knurl_request *request, int *present,
                             knurl_u64 *out, knurl_error *error);
/** Borrows a completed in-memory body; no read-all or disk fallback. */
KNURL_API knurl_status knurl_request_body_bytes(const knurl_request *request,
                                                knurl_bytes *out,
                                                knurl_error *error);
/** Read the caller-owned request association; independent of dispatch callback
 * state.
 */
KNURL_API void *knurl_request_userdata(const knurl_request *request);
/** Application association, independent of the dispatch plan's callback state.
 */
KNURL_API void knurl_request_set_userdata(knurl_request *request,
                                          void *userdata);

struct http_request;
struct knurl_body;
struct knurl_body_materialize_config;
struct knurl_body_result;
struct knurl_bytes;
struct knurl_channel;
struct knurl_channel_config;
struct knurl_endpoint;
struct knurl_error;
struct knurl_header;
struct knurl_loop;
struct knurl_response;
struct knurl_server;
struct knurl_u64;
struct knurl_websocket;
struct knurl_websocket_config;
struct knurl_xml_config;
struct lonejson;
struct lonejson_schema_view;
/** Library-owned request receiver. Pass this handle as the first
 * argument to its methods. Obtain it through the documented constructor or
 * borrowed accessor. Do not copy, initialize or modify the shell or impl.
 */
struct knurl_request {
    /** Typed lookups: exact lexical conversion; EOF absent, INVALID duplicate
     * queries or out-of-bounds values, OVERFLOW unrepresentable numbers. */
    knurl_status (*query_long)(const struct knurl_request *, const char *, long,
                               long, long *, struct knurl_error *);
    /** Exact signed-decimal path capture within inclusive bounds; EOF absent,
     * INVALID syntax/bounds, OVERFLOW unrepresentable. */
    knurl_status (*param_long)(const struct knurl_request *, const char *, long,
                               long, long *, struct knurl_error *);
    /** Exact unsigned query within inclusive bounds; duplicate names INVALID,
     * absence EOF, overflow OVERFLOW. Failure preserves output. */
    knurl_status (*query_u64)(const struct knurl_request *, const char *,
                              struct knurl_u64, struct knurl_u64,
                              struct knurl_u64 *, struct knurl_error *);
    /** Exact unsigned path capture within inclusive bounds; EOF absent,
     * INVALID syntax/bounds, OVERFLOW unrepresentable. */
    knurl_status (*param_u64)(const struct knurl_request *, const char *,
                              struct knurl_u64, struct knurl_u64,
                              struct knurl_u64 *, struct knurl_error *);
    /** Exactly true/false/1/0 query; duplicates INVALID, absence EOF. */
    knurl_status (*query_boolean)(const struct knurl_request *, const char *,
                                  int *, struct knurl_error *);
    /** Exactly true/false/1/0 capture; syntax INVALID, absence EOF. */
    knurl_status (*param_boolean)(const struct knurl_request *, const char *,
                                  int *, struct knurl_error *);
    /** Borrowed native view, tied to this packaged backend revision. No native
     * transport intervention is permitted while managed I/O owns the
     * connection. */
    struct http_request *(*kore_request)(struct knurl_request *request);
    /** Headers dispatch only. Success copies handshake fields and owns exactly
     * one close callback. The request expires before on_open; userdata is never
     * freed by the library. Failed accept retains no callback ownership. The
     * borrowed handle remains valid until on_close returns. Operations require
     * its owner loop; application callbacks run there without recursion. */
    knurl_status (*websocket_accept)(struct knurl_request *,
                                     const struct knurl_websocket_config *,
                                     struct knurl_websocket **,
                                     struct knurl_error *);
    /** Parse a completed request body into caller-initialized mapped storage;
     * rewind before and after. See knurl_xml_parse_source.
     */
    knurl_status (*xml_into)(struct knurl_request *,
                             const struct lonejson_schema_view *,
                             const struct knurl_xml_config *, void *,
                             struct knurl_error *);
    /** Completed BUFFERED/MATERIALIZED bodies only. Rewinds before and after,
     * including failure. The caller initializes and cleans up mapped storage.
     */
    knurl_status (*json_into)(struct knurl_request *request,
                              const struct lonejson_schema_view *schema,
                              void *record, struct knurl_error *error);
    /** Validate a completed body through LoneJSON; rewind before and after,
     * including failure.
     */
    knurl_status (*json_validate)(struct knurl_request *request,
                                  struct lonejson *runtime,
                                  struct knurl_error *error);
    /** Iterate a completed body using caller-owned item storage; rewind before
     * and after. See knurl_json_array_each_source.
     */
    knurl_status (*json_array_each)(struct knurl_request *,
                                    const char *selector,
                                    const struct lonejson_schema_view *,
                                    void *item, knurl_json_array_item_fn,
                                    void *userdata, struct knurl_error *);
    /** Borrowed until the request's terminal callback returns. Owner-loop only.
     */
    struct knurl_response *(*response)(struct knurl_request *request);
    /** Borrow the owning server; access only within the request's valid scope.
     */
    struct knurl_server *(*server)(const struct knurl_request *request);
    /** Borrow the request's owner loop; never retain across fork/run
     * boundaries. */
    struct knurl_loop *(*loop)(const struct knurl_request *request);
    /** Read the parsed HTTP method enum; use method_token for the original
     * bytes.
     */
    knurl_http_method (*method)(const struct knurl_request *request);
    /** Borrow the original method token until request close. */
    struct knurl_bytes (*method_token)(const struct knurl_request *request);
    /** Borrow the HTTP version token until request close. */
    struct knurl_bytes (*version)(const struct knurl_request *request);
    /** Borrow the original request target, including its raw query, until
     * request close.
     */
    struct knurl_bytes (*target)(const struct knurl_request *request);
    /** Borrow the undecoded request path until request close. */
    struct knurl_bytes (*raw_path)(const struct knurl_request *request);
    /** Borrow the backend's request path used for routing until request close.
     */
    struct knurl_bytes (*path)(const struct knurl_request *request);
    /** Borrow the raw query span until request close. */
    struct knurl_bytes (*query)(const struct knurl_request *request);
    /** Borrow the inbound Host/authority span until request close. */
    struct knurl_bytes (*authority)(const struct knurl_request *request);
    /** Return the zero-based listener index which accepted this request. */
    size_t (*listener_index)(const struct knurl_request *request);
    /** Count ordered request headers, including repeated fields. */
    size_t (*header_count)(const struct knurl_request *request);
    /** Separate trailers become visible after body completion. */
    size_t (*trailer_count)(const struct knurl_request *);
    /** Borrow ordered trailer by zero-based index; STATE before completion. */
    knurl_status (*trailer_at)(const struct knurl_request *, size_t,
                               struct knurl_header *, struct knurl_error *);
    /** First case-insensitive trailer; STATE before completion, EOF absent. */
    knurl_status (*trailer_first)(const struct knurl_request *, const char *,
                                  struct knurl_bytes *, struct knurl_error *);
    /** Borrow one ordered request header by zero-based index until request
     * close.
     */
    knurl_status (*header_at)(const struct knurl_request *request, size_t index,
                              struct knurl_header *out,
                              struct knurl_error *error);
    /** First lookups return EOF for absence and OK for a present empty value.
     */
    knurl_status (*header_first)(const struct knurl_request *request,
                                 const char *name, struct knurl_bytes *out,
                                 struct knurl_error *error);
    /** Count decoded query pairs, including repeated names and empty values. */
    size_t (*query_count)(const struct knurl_request *request);
    /** Borrow a decoded query pair by zero-based index until request close. */
    knurl_status (*query_at)(const struct knurl_request *request, size_t index,
                             struct knurl_header *out,
                             struct knurl_error *error);
    /** Borrow the first decoded query value; EOF means absent, OK includes a
     * present empty value.
     */
    knurl_status (*query_first)(const struct knurl_request *request,
                                const char *name, struct knurl_bytes *out,
                                struct knurl_error *error);
    /** Borrow a named decoded capture; EOF means no matching capture. */
    knurl_status (*path_param)(const struct knurl_request *request,
                               const char *name, struct knurl_bytes *out,
                               struct knurl_error *error);
    /** Borrow the real TCP peer endpoint, unaffected by forwarded-address
     * policy, until request close.
     */
    struct knurl_endpoint (*peer)(const struct knurl_request *request);
    /** Borrow the accepted local endpoint until request close. */
    struct knurl_endpoint (*local)(const struct knurl_request *request);
    /** Copied identity values, separate from the unmodified TCP peer.
     * Correlation is empty unless a resolver is configured. Views expire at
     * request close. */
    struct knurl_bytes (*effective_address)(const struct knurl_request *);
    /** Borrow the resolved correlation ID until request close; empty unless
     * configured.
     */
    struct knurl_bytes (*correlation_id)(const struct knurl_request *);
    /** Emit a named application policy/observation event on the owner loop;
     * observers cannot reenter emission.
     */
    knurl_status (*event)(struct knurl_request *, const char *name,
                          struct knurl_error *);
    /** Return nonzero when the downstream connection uses TLS. */
    int (*is_tls)(const struct knurl_request *request);
    /** Read framing metadata without consuming input; present distinguishes
     * absence from zero length.
     */
    knurl_status (*content_length)(const struct knurl_request *request,
                                   int *present, struct knurl_u64 *out,
                                   struct knurl_error *error);
    /** Borrows a completed in-memory body; no read-all or disk fallback. */
    knurl_status (*body_bytes)(const struct knurl_request *request,
                               struct knurl_bytes *out,
                               struct knurl_error *error);
    /** Read the caller-owned request association; independent of dispatch
     * callback state.
     */
    void *(*userdata)(const struct knurl_request *request);
    /** Application association, independent of the dispatch plan's callback
     * state.
     */
    void (*set_userdata)(struct knurl_request *request, void *userdata);
    /** Borrowed copied principal from the mount's successful root
     * authorization; empty outside a WebDAV request. Child visibility checks do
     * not replace it. */
    struct knurl_bytes (*webdav_principal)(const struct knurl_request *);
    /** Borrowed completed transport body; NONE/STREAM provide no reader/result.
     */
    struct knurl_body *(*body)(struct knurl_request *request);
    /** Explicit independent copy/materialization; success rewinds the transport
     * reader before and after copying. Failure leaves an empty result. */
    knurl_status (*body_materialize)(
        struct knurl_request *request,
        const struct knurl_body_materialize_config *config,
        struct knurl_body_result *out, struct knurl_error *error);
    /** Copy the completed body into caller storage; rewind before and after.
     * See knurl_request_body_materialize.
     */
    knurl_status (*body_copy)(struct knurl_request *request, void *buffer,
                              size_t capacity, struct knurl_body_result *out,
                              struct knurl_error *error);
    /** Copy the completed body to a caller-owned file artifact; never silently
     * buffer live input. See knurl_request_body_materialize.
     */
    knurl_status (*body_spill)(
        struct knurl_request *request,
        const struct knurl_body_materialize_config *config,
        struct knurl_body_result *out, struct knurl_error *error);
    /** Owner-loop only; resume schedules later delivery and coalesces. */
    knurl_status (*body_resume)(struct knurl_request *request,
                                struct knurl_error *error);
    /** Headers callback only. Success owns a borrowed channel until request
     * close. Zero capacities inherit server I/O bounds. Explicit send capacity
     * is independent; receive/tail capacities must fit server input bounds. */
    knurl_status (*takeover)(struct knurl_request *request,
                             const struct knurl_channel_config *config,
                             struct knurl_channel **out,
                             struct knurl_error *error);
    /** Private library state; applications must not access it. */
    struct knurl_request_impl *impl;
};

#ifdef __cplusplus
}
#endif
#endif

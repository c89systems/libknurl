#ifndef KNURL_REQUEST_H
#define KNURL_REQUEST_H

/** @file request.h Public request API. Expected failures return knurl_status
 * and optional bounded knurl_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <knurl/http.h>

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

#ifdef __cplusplus
}
#endif
#endif

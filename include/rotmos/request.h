#ifndef ROTMOS_REQUEST_H
#define ROTMOS_REQUEST_H

/** @file request.h Public request API. Expected failures return rotmos_status
 * and optional bounded rotmos_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <rotmos/http.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Numeric address/port view borrowed from a request or callback snapshot. */
typedef struct rotmos_endpoint {
    /** Numeric address byte view, borrowed from its owner. */
    rotmos_bytes address;
    /** TCP port; zero permits an ephemeral bound port where supported. */
    unsigned short port;
} rotmos_endpoint;

/** Borrow the owning server; access only within the request's valid scope. */
ROTMOS_API rotmos_server *rotmos_request_server(const rotmos_request *request);
/** Borrow the request's owner loop; never retain across fork/run boundaries. */
ROTMOS_API rotmos_loop *rotmos_request_loop(const rotmos_request *request);
/** Read the parsed HTTP method enum; use method_token for the original bytes.
 */
ROTMOS_API rotmos_http_method
rotmos_request_method(const rotmos_request *request);
/** Borrow the original method token until request close. */
ROTMOS_API rotmos_bytes
rotmos_request_method_token(const rotmos_request *request);
/** Borrow the HTTP version token until request close. */
ROTMOS_API rotmos_bytes rotmos_request_version(const rotmos_request *request);
/** Borrow the original request target, including its raw query, until request
 * close.
 */
ROTMOS_API rotmos_bytes rotmos_request_target(const rotmos_request *request);
/** Borrow the undecoded request path until request close. */
ROTMOS_API rotmos_bytes rotmos_request_raw_path(const rotmos_request *request);
/** Borrow the backend's request path used for routing until request close. */
ROTMOS_API rotmos_bytes rotmos_request_path(const rotmos_request *request);
/** Borrow the raw query span until request close. */
ROTMOS_API rotmos_bytes rotmos_request_query(const rotmos_request *request);
/** Borrow the inbound Host/authority span until request close. */
ROTMOS_API rotmos_bytes rotmos_request_authority(const rotmos_request *request);
/** Return the zero-based listener index which accepted this request. */
ROTMOS_API size_t rotmos_request_listener_index(const rotmos_request *request);
/** Count ordered request headers, including repeated fields. */
ROTMOS_API size_t rotmos_request_header_count(const rotmos_request *request);
/** Borrow one ordered request header by zero-based index until request close.
 */
ROTMOS_API rotmos_status rotmos_request_header_at(const rotmos_request *request,
                                                  size_t index,
                                                  rotmos_header *out,
                                                  rotmos_error *error);
/** First lookups return EOF for absence and OK for a present empty value. */
ROTMOS_API rotmos_status
rotmos_request_header_first(const rotmos_request *request, const char *name,
                            rotmos_bytes *out, rotmos_error *error);
/** Count decoded query pairs, including repeated names and empty values. */
ROTMOS_API size_t rotmos_request_query_count(const rotmos_request *request);
/** Borrow a decoded query pair by zero-based index until request close. */
ROTMOS_API rotmos_status rotmos_request_query_at(const rotmos_request *request,
                                                 size_t index,
                                                 rotmos_header *out,
                                                 rotmos_error *error);
/** Borrow the first decoded query value; EOF means absent, OK includes a
 * present empty value.
 */
ROTMOS_API rotmos_status
rotmos_request_query_first(const rotmos_request *request, const char *name,
                           rotmos_bytes *out, rotmos_error *error);
/** Borrow a named decoded capture; EOF means no matching capture. */
ROTMOS_API rotmos_status
rotmos_request_path_param(const rotmos_request *request, const char *name,
                          rotmos_bytes *out, rotmos_error *error);
/** Borrow the real TCP peer endpoint, unaffected by forwarded-address policy,
 * until request close.
 */
ROTMOS_API rotmos_endpoint rotmos_request_peer(const rotmos_request *request);
/** Borrow the accepted local endpoint until request close. */
ROTMOS_API rotmos_endpoint rotmos_request_local(const rotmos_request *request);
/** Copied identity values, separate from the unmodified TCP peer. Correlation
 * is empty unless a resolver is configured. Views expire at request close. */
ROTMOS_API rotmos_bytes
rotmos_request_effective_address(const rotmos_request *);
/** Borrow the resolved correlation ID until request close; empty unless
 * configured.
 */
ROTMOS_API rotmos_bytes rotmos_request_correlation_id(const rotmos_request *);
/** Emit a named application policy/observation event on the owner loop;
 * observers cannot reenter emission.
 */
ROTMOS_API rotmos_status rotmos_request_event(rotmos_request *,
                                              const char *name, rotmos_error *);
/** Return nonzero when the downstream connection uses TLS. */
ROTMOS_API int rotmos_request_is_tls(const rotmos_request *request);
/** Read framing metadata without consuming input; present distinguishes absence
 * from zero length.
 */
ROTMOS_API rotmos_status
rotmos_request_content_length(const rotmos_request *request, int *present,
                              rotmos_u64 *out, rotmos_error *error);
/** Borrows a completed in-memory body; no read-all or disk fallback. */
ROTMOS_API rotmos_status rotmos_request_body_bytes(
    const rotmos_request *request, rotmos_bytes *out, rotmos_error *error);
/** Read the caller-owned request association; independent of dispatch callback
 * state.
 */
ROTMOS_API void *rotmos_request_userdata(const rotmos_request *request);
/** Application association, independent of the dispatch plan's callback state.
 */
ROTMOS_API void rotmos_request_set_userdata(rotmos_request *request,
                                            void *userdata);

#ifdef __cplusplus
}
#endif
#endif

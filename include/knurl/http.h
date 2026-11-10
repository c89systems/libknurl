#ifndef KNURL_HTTP_H
#define KNURL_HTTP_H

/** @file http.h Public http API. Expected failures return knurl_status and
 * optional bounded knurl_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <knurl/common.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Values for http method. */
typedef enum knurl_http_method {
    /** Select all supported HTTP methods. */
    KNURL_HTTP_ANY = 0,
    /** HTTP GET method. */
    KNURL_HTTP_GET,
    /** HTTP POST method. */
    KNURL_HTTP_POST,
    /** HTTP PUT method. */
    KNURL_HTTP_PUT,
    /** HTTP PATCH method. */
    KNURL_HTTP_PATCH,
    /** HTTP DELETE method. */
    KNURL_HTTP_DELETE,
    /** HTTP HEAD method. */
    KNURL_HTTP_HEAD,
    /** HTTP OPTIONS method. */
    KNURL_HTTP_OPTIONS,
    /** HTTP PROPFIND method. */
    KNURL_HTTP_PROPFIND,
    /** HTTP MKCOL method. */
    KNURL_HTTP_MKCOL,
    /** HTTP COPY method. */
    KNURL_HTTP_COPY,
    /** HTTP MOVE method. */
    KNURL_HTTP_MOVE
} knurl_http_method;

/** Http methods. */
typedef unsigned int knurl_http_methods;
/** Convert a concrete knurl_http_method to its method-mask bit. */
#define KNURL_HTTP_METHOD_MASK(method) (1U << (unsigned int)(method))
/** Mask containing every supported concrete HTTP method. */
#define KNURL_HTTP_METHODS_ALL 0xffeU
/** Method-mask bit for GET. */
#define KNURL_HTTP_METHODS_GET KNURL_HTTP_METHOD_MASK(KNURL_HTTP_GET)
/** Method-mask bit for POST. */
#define KNURL_HTTP_METHODS_POST KNURL_HTTP_METHOD_MASK(KNURL_HTTP_POST)
/** Method-mask bit for PUT. */
#define KNURL_HTTP_METHODS_PUT KNURL_HTTP_METHOD_MASK(KNURL_HTTP_PUT)
/** Method-mask bit for PATCH. */
#define KNURL_HTTP_METHODS_PATCH KNURL_HTTP_METHOD_MASK(KNURL_HTTP_PATCH)
/** Method-mask bit for DELETE. */
#define KNURL_HTTP_METHODS_DELETE KNURL_HTTP_METHOD_MASK(KNURL_HTTP_DELETE)
/** Method-mask bit for HEAD. */
#define KNURL_HTTP_METHODS_HEAD KNURL_HTTP_METHOD_MASK(KNURL_HTTP_HEAD)
/** Method-mask bit for OPTIONS. */
#define KNURL_HTTP_METHODS_OPTIONS KNURL_HTTP_METHOD_MASK(KNURL_HTTP_OPTIONS)
/** Method-mask bit for PROPFIND. */
#define KNURL_HTTP_METHODS_PROPFIND KNURL_HTTP_METHOD_MASK(KNURL_HTTP_PROPFIND)
/** Method-mask bit for MKCOL. */
#define KNURL_HTTP_METHODS_MKCOL KNURL_HTTP_METHOD_MASK(KNURL_HTTP_MKCOL)
/** Method-mask bit for COPY. */
#define KNURL_HTTP_METHODS_COPY KNURL_HTTP_METHOD_MASK(KNURL_HTTP_COPY)
/** Method-mask bit for MOVE. */
#define KNURL_HTTP_METHODS_MOVE KNURL_HTTP_METHOD_MASK(KNURL_HTTP_MOVE)

/** Values for body mode. */
typedef enum knurl_body_mode {
    /** Do not collect a managed request body. */
    KNURL_BODY_NONE = 0,
    /** Completed JSON body policy. */
    KNURL_BODY_JSON,
    /** Collect an admitted body for ordinary buffered handling. */
    KNURL_BODY_BUFFERED,
    /** Live bounded input with upload/raw dispatch registrations; ordinary
     * staged routes instead receive a completed body under the baseline upload
     * storage policy.
     */
    KNURL_BODY_STREAMING_UPLOAD,
    /** Complete admitted transport input in memory or a request-scoped spool.
     */
    KNURL_BODY_MATERIALIZED,
    /** Deliver live admitted input without whole-body materialization. */
    KNURL_BODY_STREAM
} knurl_body_mode;

/** Input admission/storage policy. Zero max_bytes inherits the server limit; a
 * positive hard server limit still applies. STREAM never collects/spools;
 * STREAMING_UPLOAD is live only through upload/raw dispatch registrations.
 */
typedef struct knurl_body_policy {
    /** Selected policy mode; use the enclosing initializer for defaults. */
    knurl_body_mode mode;
    /** Maximum bytes for this operation; zero-inheritance/unlimited behavior is
     * operation-specific.
     */
    knurl_u64 max_bytes;
    /** Maximum request bytes retained in memory before permitted transport
     * spooling.
     */
    size_t memory_buffer_limit_bytes;
    /** Nonzero rejects request-body disk spooling. */
    int disk_spool_disabled;
} knurl_body_policy;

/** Initialize body policy with its documented defaults; call before applying
 * overrides.
 */
KNURL_API void knurl_body_policy_init(knurl_body_policy *policy);
/** Initialize body policy json with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_body_policy_init_json(knurl_body_policy *policy);
/** Initialize body policy buffered with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_body_policy_init_buffered(knurl_body_policy *policy);
/** Initialize body policy upload with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_body_policy_init_upload(knurl_body_policy *policy);

/** One borrowed SSE event. Presence flags distinguish omitted and empty fields.
 * has_frame selects preformatted bytes instead of individual fields.
 */
typedef struct knurl_sse_event {
    /** Optional SSE comment bytes; each line is emitted as a comment. */
    knurl_bytes comment;
    /** Optional SSE event ID; control/newline restrictions apply. */
    knurl_bytes id;
    /** Optional SSE event name; control/newline restrictions apply. */
    knurl_bytes event;
    /** Optional borrowed SSE data, emitted as one data field per line when
     * has_data is nonzero.
     */
    knurl_bytes data;
    /** Optional preformatted SSE frame, taking precedence over individual
     * fields.
     */
    knurl_bytes frame;
    /** Exact unsigned SSE reconnect delay in milliseconds. */
    knurl_u64 retry_ms;
    /** Nonzero includes comment, including an empty comment. */
    int has_comment;
    /** Nonzero includes id, including an empty ID. */
    int has_id;
    /** Nonzero includes event, including an empty event name. */
    int has_event;
    /** Nonzero includes data, including an empty data field. */
    int has_data;
    /** Nonzero selects the preformatted frame representation. */
    int has_frame;
    /** Nonzero includes retry_ms, including an explicit zero. */
    int has_retry;
} knurl_sse_event;

/** Initialize sse event with its documented defaults; call before applying
 * overrides.
 */
KNURL_API void knurl_sse_event_init(knurl_sse_event *event);
/** Encodes one event. CR, LF and CRLF delimit field lines, emitted with LF
 * endings and a prefix on every line. Preformatted frames are copied verbatim.
 * Inputs must not overlap the destination.
 */
KNURL_API knurl_status knurl_sse_encode(const knurl_sse_event *event,
                                        void *buffer, size_t capacity,
                                        size_t *written, knurl_error *error);

#ifdef __cplusplus
}
#endif
#endif

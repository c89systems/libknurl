#ifndef ROTMOS_HTTP_H
#define ROTMOS_HTTP_H

/** @file http.h Public http API. Expected failures return rotmos_status and
 * optional bounded rotmos_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <rotmos/common.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Values for http method. */
typedef enum rotmos_http_method {
    /** Select all supported HTTP methods. */
    ROTMOS_HTTP_ANY = 0,
    /** HTTP GET method. */
    ROTMOS_HTTP_GET,
    /** HTTP POST method. */
    ROTMOS_HTTP_POST,
    /** HTTP PUT method. */
    ROTMOS_HTTP_PUT,
    /** HTTP PATCH method. */
    ROTMOS_HTTP_PATCH,
    /** HTTP DELETE method. */
    ROTMOS_HTTP_DELETE,
    /** HTTP HEAD method. */
    ROTMOS_HTTP_HEAD,
    /** HTTP OPTIONS method. */
    ROTMOS_HTTP_OPTIONS,
    /** HTTP PROPFIND method. */
    ROTMOS_HTTP_PROPFIND,
    /** HTTP MKCOL method. */
    ROTMOS_HTTP_MKCOL,
    /** HTTP COPY method. */
    ROTMOS_HTTP_COPY,
    /** HTTP MOVE method. */
    ROTMOS_HTTP_MOVE
} rotmos_http_method;

/** Http methods. */
typedef unsigned int rotmos_http_methods;
/** Convert a concrete rotmos_http_method to its method-mask bit. */
#define ROTMOS_HTTP_METHOD_MASK(method) (1U << (unsigned int)(method))
/** Mask containing every supported concrete HTTP method. */
#define ROTMOS_HTTP_METHODS_ALL 0xffeU
/** Method-mask bit for GET. */
#define ROTMOS_HTTP_METHODS_GET ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_GET)
/** Method-mask bit for POST. */
#define ROTMOS_HTTP_METHODS_POST ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_POST)
/** Method-mask bit for PUT. */
#define ROTMOS_HTTP_METHODS_PUT ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_PUT)
/** Method-mask bit for PATCH. */
#define ROTMOS_HTTP_METHODS_PATCH ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_PATCH)
/** Method-mask bit for DELETE. */
#define ROTMOS_HTTP_METHODS_DELETE ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_DELETE)
/** Method-mask bit for HEAD. */
#define ROTMOS_HTTP_METHODS_HEAD ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_HEAD)
/** Method-mask bit for OPTIONS. */
#define ROTMOS_HTTP_METHODS_OPTIONS ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_OPTIONS)
/** Method-mask bit for PROPFIND. */
#define ROTMOS_HTTP_METHODS_PROPFIND                                           \
    ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_PROPFIND)
/** Method-mask bit for MKCOL. */
#define ROTMOS_HTTP_METHODS_MKCOL ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_MKCOL)
/** Method-mask bit for COPY. */
#define ROTMOS_HTTP_METHODS_COPY ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_COPY)
/** Method-mask bit for MOVE. */
#define ROTMOS_HTTP_METHODS_MOVE ROTMOS_HTTP_METHOD_MASK(ROTMOS_HTTP_MOVE)

/** Values for body mode. */
typedef enum rotmos_body_mode {
    /** Do not collect a managed request body. */
    ROTMOS_BODY_NONE = 0,
    /** Completed JSON body policy. */
    ROTMOS_BODY_JSON,
    /** Collect an admitted body for ordinary buffered handling. */
    ROTMOS_BODY_BUFFERED,
    /** Live bounded input with upload/raw dispatch registrations; ordinary
     * staged routes instead receive a completed body under the baseline upload
     * storage policy.
     */
    ROTMOS_BODY_STREAMING_UPLOAD,
    /** Complete admitted transport input in memory or a request-scoped spool.
     */
    ROTMOS_BODY_MATERIALIZED,
    /** Deliver live admitted input without whole-body materialization. */
    ROTMOS_BODY_STREAM
} rotmos_body_mode;

/** Input admission/storage policy. Zero max_bytes inherits the server limit; a
 * positive hard server limit still applies. STREAM never collects/spools;
 * STREAMING_UPLOAD is live only through upload/raw dispatch registrations.
 */
typedef struct rotmos_body_policy {
    /** Selected policy mode; use the enclosing initializer for defaults. */
    rotmos_body_mode mode;
    /** Maximum bytes for this operation; zero-inheritance/unlimited behavior is
     * operation-specific.
     */
    rotmos_u64 max_bytes;
    /** Maximum request bytes retained in memory before permitted transport
     * spooling.
     */
    size_t memory_buffer_limit_bytes;
    /** Nonzero rejects request-body disk spooling. */
    int disk_spool_disabled;
} rotmos_body_policy;

/** Initialize body policy with its documented defaults; call before applying
 * overrides.
 */
ROTMOS_API void rotmos_body_policy_init(rotmos_body_policy *policy);
/** Initialize body policy json with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void rotmos_body_policy_init_json(rotmos_body_policy *policy);
/** Initialize body policy buffered with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void rotmos_body_policy_init_buffered(rotmos_body_policy *policy);
/** Initialize body policy upload with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void rotmos_body_policy_init_upload(rotmos_body_policy *policy);

/** One borrowed SSE event. Presence flags distinguish omitted and empty fields.
 * has_frame selects preformatted bytes instead of individual fields.
 */
typedef struct rotmos_sse_event {
    /** Optional SSE comment bytes; each line is emitted as a comment. */
    rotmos_bytes comment;
    /** Optional SSE event ID; control/newline restrictions apply. */
    rotmos_bytes id;
    /** Optional SSE event name; control/newline restrictions apply. */
    rotmos_bytes event;
    /** Optional borrowed SSE data, emitted as one data field per line when
     * has_data is nonzero.
     */
    rotmos_bytes data;
    /** Optional preformatted SSE frame, taking precedence over individual
     * fields.
     */
    rotmos_bytes frame;
    /** Exact unsigned SSE reconnect delay in milliseconds. */
    rotmos_u64 retry_ms;
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
} rotmos_sse_event;

/** Initialize sse event with its documented defaults; call before applying
 * overrides.
 */
ROTMOS_API void rotmos_sse_event_init(rotmos_sse_event *event);
/** Encodes one event. CR, LF and CRLF delimit field lines, emitted with LF
 * endings and a prefix on every line. Preformatted frames are copied verbatim.
 * Inputs must not overlap the destination.
 */
ROTMOS_API rotmos_status rotmos_sse_encode(const rotmos_sse_event *event,
                                           void *buffer, size_t capacity,
                                           size_t *written,
                                           rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif

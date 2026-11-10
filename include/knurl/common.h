#ifndef KNURL_COMMON_H
#define KNURL_COMMON_H

/** @file common.h Public common API. Expected failures return knurl_status and
 * optional bounded knurl_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <stddef.h>

#if defined(__GNUC__) || defined(__clang__)
/** Export visibility for public library declarations. */
#define KNURL_API __attribute__((visibility("default")))
#else
/** Export visibility for public library declarations. */
#define KNURL_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Owned inert server declaration and run lifecycle; destroy only while
 * inactive.
 */
typedef struct knurl_server knurl_server;
/** One extension-to-Content-Type override, copied with its configuration.
 * Extension is ASCII letters/digits or +, -, _ with an optional leading dot;
 * matching is case-insensitive against the filename's final extension.
 * Duplicate normalized extensions in one declaration are INVALID.
 */
typedef struct knurl_mime_type {
    /** Extension such as "mp3" or ".MP3"; not a path or compound suffix. */
    const char *extension;
    /** RFC 9110 media-type with token or quoted parameters; malformed values,
     * wildcards, header controls/folding and unquoted non-ASCII are INVALID.
     */
    const char *content_type;
} knurl_mime_type;
/** Process role snapshot borrowed during host callbacks; worker_index is zero-
 * based.
 */
typedef struct knurl_process_context knurl_process_context;
/** Owned route registry; successful server_router_take transfers ownership. */
typedef struct knurl_router knurl_router;
/** Router-owned immutable route declaration; expires at router destruction. */
typedef struct knurl_route knurl_route;
/** Router-owned prefix/hook registration context; expires with its router. */
typedef struct knurl_route_group knurl_route_group;
/** Borrowed owner-loop request; expires after its terminal callback. */
typedef struct knurl_request knurl_request;
/** Headers-time request dispatch selection and callback state. */
typedef struct knurl_request_plan knurl_request_plan;
/** Borrowed completed transport body; expires at request close. */
typedef struct knurl_body knurl_body;
/** Borrowed takeover transport; valid through its close callback. */
typedef struct knurl_channel knurl_channel;
/** Borrowed connection handle, independent of HTTP request lifetime; expires
 * after on_close.
 */
typedef struct knurl_websocket knurl_websocket;
/** Borrowed live output handle; expires after the request terminal callback. */
typedef struct knurl_response knurl_response;
/** Staged response builder; mutations precede after-hook completion and
 * transport submission.
 */
typedef struct knurl_reply knurl_reply;
/** Owned bounded producer; successful take/producer calls transfer ownership.
 */
typedef struct knurl_source knurl_source;
/** Borrowed process/run-specific owner loop; cannot cross a fork/run boundary.
 */
typedef struct knurl_loop knurl_loop;
/** Owner-loop borrowed-descriptor registration; keep context until retirement.
 */
typedef struct knurl_watch knurl_watch;
/** Owner-loop timer registration; cancellation retires it asynchronously. */
typedef struct knurl_timer knurl_timer;
/** Cross-thread signal token; stop producers before owner-loop destruction. */
typedef struct knurl_waker knurl_waker;

/** Values for status. */
typedef enum knurl_status {
    /** Operation completed successfully. */
    KNURL_OK = 0,
    /** No progress is currently possible; retry only under the operation's
     * readiness/resume contract.
     */
    KNURL_AGAIN,
    /** End of input/listing, or absent lookup value as documented by the
     * operation.
     */
    KNURL_EOF,
    /** Invalid argument, declaration or input representation. */
    KNURL_INVALID,
    /** Allocation failed. */
    KNURL_NOMEM,
    /** Operation is invalid in the current phase, owner or lifetime. */
    KNURL_STATE,
    /** Registration/resource state conflicts with the requested operation. */
    KNURL_CONFLICT,
    /** Configured capacity or admission bound was exceeded. */
    KNURL_LIMIT,
    /** Exact value cannot be represented in the destination range. */
    KNURL_OVERFLOW,
    /** Configured deadline expired. */
    KNURL_TIMEOUT,
    /** System or transport I/O failed. */
    KNURL_IO,
    /** Requested operation/capability is unavailable. */
    KNURL_NOT_SUPPORTED,
    /** Operation was cancelled or replaced before successful completion. */
    KNURL_CANCELLED,
    /** Private backend failure reported through the portable API. */
    KNURL_BACKEND
} knurl_status;

/** Values for error source. */
typedef enum knurl_error_source {
    /** Diagnostic source: none. */
    KNURL_ERROR_NONE = 0,
    /** Diagnostic source: core. */
    KNURL_ERROR_CORE,
    /** Diagnostic source: system. */
    KNURL_ERROR_SYSTEM,
    /** Diagnostic source: backend. */
    KNURL_ERROR_BACKEND,
    /** Diagnostic source: lonejson. */
    KNURL_ERROR_LONEJSON,
    /** Diagnostic source: pslog. */
    KNURL_ERROR_PSLOG,
    /** Diagnostic source: xml. */
    KNURL_ERROR_XML
} knurl_error_source;

/** Copied bounded diagnostic. status is portable; source/native_code retain
 * dependency detail. Text fields are always NUL-terminated.
 */
typedef struct knurl_error {
    /** Portable Core status, distinct from HTTP status and dependency
     * native_code. */
    knurl_status status;
    /** Diagnostic source category; NONE when no error is present. */
    knurl_error_source source;
    /** Dependency/system error code, separate from the portable status. */
    long native_code;
    /** Bounded NUL-terminated operation name for diagnostics. */
    char operation[48];
    /** Bounded NUL-terminated primary diagnostic. */
    char message[256];
    /** Bounded NUL-terminated supplemental diagnostic. */
    char detail[256];
} knurl_error;

/** Borrowed binary span; length is authoritative and no NUL termination is
 * required. The producing operation defines its lifetime.
 */
typedef struct knurl_bytes {
    /** Byte storage; ownership and lifetime follow the enclosing view. */
    const void *data;
    /** Exact number of bytes; excludes any incidental trailing NUL. */
    size_t length;
} knurl_bytes;

/** Independent allocation returned by materialized codec/document operations.
 * Initialize empty and release with knurl_owned_bytes_cleanup.
 */
typedef struct knurl_owned_bytes {
    /** Allocation owned by this value; release with knurl_owned_bytes_cleanup.
     * NULL represents an initialized empty value.
     */
    void *data;
    /** Exact number of bytes; excludes any incidental trailing NUL. */
    size_t length;
} knurl_owned_bytes;

/** Initialize an empty owned byte value. This does not free an existing
 * allocation; call knurl_owned_bytes_cleanup before reinitializing a populated
 * value. NULL is ignored.
 */
KNURL_API void knurl_owned_bytes_init(knurl_owned_bytes *bytes);
/** Free the owned allocation and clear the view; safe on an initialized empty
 * value.
 */
KNURL_API void knurl_owned_bytes_cleanup(knurl_owned_bytes *bytes);

/** One ordered name/value byte pair. Repeated headers retain order; storage is
 * borrowed from the producing request/reply/view.
 */
typedef struct knurl_header {
    /** Name of this declaration, field or listed resource. */
    knurl_bytes name;
    /** Field value; byte views may contain embedded NUL where the operation
     * permits it.
     */
    knurl_bytes value;
} knurl_header;

/** Each member is a 32-bit word, including when unsigned long is wider. */
typedef struct knurl_u64 {
    /** High 32 bits of the exact unsigned value; higher bits must be zero. */
    unsigned long high;
    /** Low 32 bits of the exact unsigned value; higher bits must be zero. */
    unsigned long low;
} knurl_u64;

/** Return the library release string; static storage, never free it. */
KNURL_API const char *knurl_version(void);
/** Clear a diagnostic to OK/NONE with empty text; NULL is ignored. */
KNURL_API void knurl_error_clear(knurl_error *error);
/** Return a static human-readable status label; never free it. */
KNURL_API const char *knurl_status_string(knurl_status status);
/** Construct an exact unsigned count; reject words outside the 32-bit range. */
KNURL_API knurl_status knurl_u64_from_words(unsigned long high,
                                            unsigned long low, knurl_u64 *out,
                                            knurl_error *error);
/** Convert a platform size to an exact unsigned count. */
KNURL_API knurl_status knurl_u64_from_size(size_t value, knurl_u64 *out,
                                           knurl_error *error);
/** Convert to the target's size_t range; report OVERFLOW rather than truncate.
 */
KNURL_API knurl_status knurl_u64_to_size(knurl_u64 value, size_t *out,
                                         knurl_error *error);
/** Compare exact counts; write -1, 0 or 1 to order. */
KNURL_API knurl_status knurl_u64_compare(knurl_u64 left, knurl_u64 right,
                                         int *order, knurl_error *error);
/** Add exact counts; report OVERFLOW rather than wrap. */
KNURL_API knurl_status knurl_u64_add(knurl_u64 left, knurl_u64 right,
                                     knurl_u64 *out, knurl_error *error);
/** Parse a nonempty unsigned decimal byte span without rounding. */
KNURL_API knurl_status knurl_u64_from_decimal(const char *text, size_t length,
                                              knurl_u64 *out,
                                              knurl_error *error);
/** The destination includes a trailing NUL. Failure leaves it untouched. */
KNURL_API knurl_status knurl_u64_format_decimal(knurl_u64 value, char *buffer,
                                                size_t capacity,
                                                size_t *written,
                                                knurl_error *error);

#ifdef __cplusplus
}
#endif
#endif

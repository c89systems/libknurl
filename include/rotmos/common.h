#ifndef ROTMOS_COMMON_H
#define ROTMOS_COMMON_H

/** @file common.h Public common API. Expected failures return rotmos_status and
 * optional bounded rotmos_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <stddef.h>

#if defined(__GNUC__) || defined(__clang__)
/** Export visibility for public library declarations. */
#define ROTMOS_API __attribute__((visibility("default")))
#else
/** Export visibility for public library declarations. */
#define ROTMOS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Owned inert server declaration and run lifecycle; destroy only while
 * inactive.
 */
typedef struct rotmos_server rotmos_server;
/** Process role snapshot borrowed during host callbacks; worker_index is zero-
 * based.
 */
typedef struct rotmos_process_context rotmos_process_context;
/** Owned route registry; successful server_router_take transfers ownership. */
typedef struct rotmos_router rotmos_router;
/** Router-owned immutable route declaration; expires at router destruction. */
typedef struct rotmos_route rotmos_route;
/** Router-owned prefix/hook registration context; expires with its router. */
typedef struct rotmos_route_group rotmos_route_group;
/** Borrowed owner-loop request; expires after its terminal callback. */
typedef struct rotmos_request rotmos_request;
/** Headers-time request dispatch selection and callback state. */
typedef struct rotmos_request_plan rotmos_request_plan;
/** Borrowed completed transport body; expires at request close. */
typedef struct rotmos_body rotmos_body;
/** Borrowed takeover transport; valid through its close callback. */
typedef struct rotmos_channel rotmos_channel;
/** Borrowed connection handle, independent of HTTP request lifetime; expires
 * after on_close.
 */
typedef struct rotmos_websocket rotmos_websocket;
/** Borrowed live output handle; expires after the request terminal callback. */
typedef struct rotmos_response rotmos_response;
/** Staged response builder; mutations precede after-hook completion and
 * transport submission.
 */
typedef struct rotmos_reply rotmos_reply;
/** Owned bounded producer; successful take/producer calls transfer ownership.
 */
typedef struct rotmos_source rotmos_source;
/** Borrowed process/run-specific owner loop; cannot cross a fork/run boundary.
 */
typedef struct rotmos_loop rotmos_loop;
/** Owner-loop borrowed-descriptor registration; keep context until retirement.
 */
typedef struct rotmos_watch rotmos_watch;
/** Owner-loop timer registration; cancellation retires it asynchronously. */
typedef struct rotmos_timer rotmos_timer;
/** Cross-thread signal token; stop producers before owner-loop destruction. */
typedef struct rotmos_waker rotmos_waker;

/** Values for status. */
typedef enum rotmos_status {
    /** Operation completed successfully. */
    ROTMOS_OK = 0,
    /** No progress is currently possible; retry only under the operation's
     * readiness/resume contract.
     */
    ROTMOS_AGAIN,
    /** End of input/listing, or absent lookup value as documented by the
     * operation.
     */
    ROTMOS_EOF,
    /** Invalid argument, declaration or input representation. */
    ROTMOS_INVALID,
    /** Allocation failed. */
    ROTMOS_NOMEM,
    /** Operation is invalid in the current phase, owner or lifetime. */
    ROTMOS_STATE,
    /** Registration/resource state conflicts with the requested operation. */
    ROTMOS_CONFLICT,
    /** Configured capacity or admission bound was exceeded. */
    ROTMOS_LIMIT,
    /** Exact value cannot be represented in the destination range. */
    ROTMOS_OVERFLOW,
    /** Configured deadline expired. */
    ROTMOS_TIMEOUT,
    /** System or transport I/O failed. */
    ROTMOS_IO,
    /** Requested operation/capability is unavailable. */
    ROTMOS_NOT_SUPPORTED,
    /** Operation was cancelled or replaced before successful completion. */
    ROTMOS_CANCELLED,
    /** Private backend failure reported through the portable API. */
    ROTMOS_BACKEND
} rotmos_status;

/** Values for error source. */
typedef enum rotmos_error_source {
    /** Diagnostic source: none. */
    ROTMOS_ERROR_NONE = 0,
    /** Diagnostic source: core. */
    ROTMOS_ERROR_CORE,
    /** Diagnostic source: system. */
    ROTMOS_ERROR_SYSTEM,
    /** Diagnostic source: backend. */
    ROTMOS_ERROR_BACKEND,
    /** Diagnostic source: lonejson. */
    ROTMOS_ERROR_LONEJSON,
    /** Diagnostic source: pslog. */
    ROTMOS_ERROR_PSLOG,
    /** Diagnostic source: xml. */
    ROTMOS_ERROR_XML
} rotmos_error_source;

/** Copied bounded diagnostic. status is portable; source/native_code retain
 * dependency detail. Text fields are always NUL-terminated.
 */
typedef struct rotmos_error {
    /** Portable Core status, distinct from HTTP status and dependency
     * native_code. */
    rotmos_status status;
    /** Diagnostic source category; NONE when no error is present. */
    rotmos_error_source source;
    /** Dependency/system error code, separate from the portable status. */
    long native_code;
    /** Bounded NUL-terminated operation name for diagnostics. */
    char operation[48];
    /** Bounded NUL-terminated primary diagnostic. */
    char message[256];
    /** Bounded NUL-terminated supplemental diagnostic. */
    char detail[256];
} rotmos_error;

/** Borrowed binary span; length is authoritative and no NUL termination is
 * required. The producing operation defines its lifetime.
 */
typedef struct rotmos_bytes {
    /** Byte storage; ownership and lifetime follow the enclosing view. */
    const void *data;
    /** Exact number of bytes; excludes any incidental trailing NUL. */
    size_t length;
} rotmos_bytes;

/** Independent allocation returned by materialized codec/document operations.
 * Initialize empty and release with rotmos_owned_bytes_cleanup.
 */
typedef struct rotmos_owned_bytes {
    /** Byte storage; ownership and lifetime follow the enclosing view. */
    void *data;
    /** Exact number of bytes; excludes any incidental trailing NUL. */
    size_t length;
} rotmos_owned_bytes;

/** Initializes or releases an independently owned byte allocation. */
ROTMOS_API void rotmos_owned_bytes_init(rotmos_owned_bytes *bytes);
/** Free the owned allocation and clear the view; safe on an initialized empty
 * value.
 */
ROTMOS_API void rotmos_owned_bytes_cleanup(rotmos_owned_bytes *bytes);

/** One ordered name/value byte pair. Repeated headers retain order; storage is
 * borrowed from the producing request/reply/view.
 */
typedef struct rotmos_header {
    /** Name of this declaration, field or listed resource. */
    rotmos_bytes name;
    /** Field value; byte views may contain embedded NUL where the operation
     * permits it.
     */
    rotmos_bytes value;
} rotmos_header;

/** Each member is a 32-bit word, including when unsigned long is wider. */
typedef struct rotmos_u64 {
    /** High 32 bits of the exact unsigned value; higher bits must be zero. */
    unsigned long high;
    /** Low 32 bits of the exact unsigned value; higher bits must be zero. */
    unsigned long low;
} rotmos_u64;

/** Return the library release string; static storage, never free it. */
ROTMOS_API const char *rotmos_version(void);
/** Clear a diagnostic to OK/NONE with empty text; NULL is ignored. */
ROTMOS_API void rotmos_error_clear(rotmos_error *error);
/** Return a static human-readable status label; never free it. */
ROTMOS_API const char *rotmos_status_string(rotmos_status status);
/** Construct an exact unsigned count; reject words outside the 32-bit range. */
ROTMOS_API rotmos_status rotmos_u64_from_words(unsigned long high,
                                               unsigned long low,
                                               rotmos_u64 *out,
                                               rotmos_error *error);
/** Convert a platform size to an exact unsigned count. */
ROTMOS_API rotmos_status rotmos_u64_from_size(size_t value, rotmos_u64 *out,
                                              rotmos_error *error);
/** Convert to the target's size_t range; report OVERFLOW rather than truncate.
 */
ROTMOS_API rotmos_status rotmos_u64_to_size(rotmos_u64 value, size_t *out,
                                            rotmos_error *error);
/** Compare exact counts; write -1, 0 or 1 to order. */
ROTMOS_API rotmos_status rotmos_u64_compare(rotmos_u64 left, rotmos_u64 right,
                                            int *order, rotmos_error *error);
/** Add exact counts; report OVERFLOW rather than wrap. */
ROTMOS_API rotmos_status rotmos_u64_add(rotmos_u64 left, rotmos_u64 right,
                                        rotmos_u64 *out, rotmos_error *error);
/** Parse a nonempty unsigned decimal byte span without rounding. */
ROTMOS_API rotmos_status rotmos_u64_from_decimal(const char *text,
                                                 size_t length, rotmos_u64 *out,
                                                 rotmos_error *error);
/** The destination includes a trailing NUL. Failure leaves it untouched. */
ROTMOS_API rotmos_status rotmos_u64_format_decimal(rotmos_u64 value,
                                                   char *buffer,
                                                   size_t capacity,
                                                   size_t *written,
                                                   rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif

#ifndef KNURL_RESPONSE_H
#define KNURL_RESPONSE_H

/** @file response.h Public response API. Expected failures return knurl_status
 * and optional bounded knurl_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <knurl/io.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Values for response phase. */
typedef enum knurl_response_phase {
    /** Live response phase: building. */
    KNURL_RESPONSE_BUILDING = 0,
    /** Live response phase: active. */
    KNURL_RESPONSE_ACTIVE,
    /** Live response phase: finishing. */
    KNURL_RESPONSE_FINISHING,
    /** Live response phase: complete. */
    KNURL_RESPONSE_COMPLETE,
    /** Live response phase: aborted. */
    KNURL_RESPONSE_ABORTED
} knurl_response_phase;

/** Copied terminal response accounting; committed distinguishes failure before
 * versus after final output commitment.
 */
typedef struct knurl_response_outcome {
    /** Portable terminal status; OK indicates successful completion. */
    knurl_status cause;
    /** HTTP status code; zero means unknown/unset where documented by this
     * event/builder.
     */
    int status;
    /** Nonzero when final response output has crossed transport commitment.
     */
    int committed;
    /** Exact final response wire bytes accepted by transport. */
    knurl_u64 wire_bytes;
    /** Exact interim response wire bytes, separate from final output. */
    knurl_u64 interim_wire_bytes;
    /** Exact final response payload bytes. */
    knurl_u64 body_bytes;
    /** Exact monotonic operation duration in milliseconds. */
    knurl_u64 duration_ms;
} knurl_response_outcome;

/** Owner-loop live-output capacity notification; retry bounded writes without
 * recursion.
 */
typedef void (*knurl_response_writable_fn)(knurl_response *, void *);
/** Terminal live response callback with borrowed outcome; release callback
 * state according to the request lifetime.
 */
typedef void (*knurl_response_close_fn)(knurl_response *,
                                        const knurl_response_outcome *, void *);

/** Borrowed until the request's terminal callback returns. Owner-loop only. */
KNURL_API knurl_response *knurl_request_response(knurl_request *request);
/** Return the live response's current output phase on its owner loop. */
KNURL_API knurl_response_phase
knurl_response_state(const knurl_response *response);
/** Return nonzero once final response headers have reached transport
 * commitment.
 */
KNURL_API int knurl_response_is_committed(const knurl_response *response);
/** Return bytes currently retained in the bounded output queue. */
KNURL_API size_t knurl_response_queued_bytes(const knurl_response *response);
/** Install owner-loop writable/terminal callbacks; caller retains userdata
 * through terminal delivery.
 */
KNURL_API knurl_status knurl_response_callbacks(
    knurl_response *response, knurl_response_writable_fn writable,
    knurl_response_close_fn close, void *userdata, knurl_error *error);
/** Set a final status while BUILDING; this does not begin output. */
KNURL_API knurl_status knurl_response_status(knurl_response *response,
                                             int status, knurl_error *error);
/** Copy a header, replacing case-insensitive matches while BUILDING. */
KNURL_API knurl_status knurl_response_header_set(knurl_response *response,
                                                 const char *name,
                                                 const char *value,
                                                 knurl_error *error);
/** Append a copied repeated header while BUILDING. */
KNURL_API knurl_status knurl_response_header_add(knurl_response *response,
                                                 const char *name,
                                                 const char *value,
                                                 knurl_error *error);
/** Declare the exact output-body length while BUILDING; mismatch fails
 * delivery.
 */
KNURL_API knurl_status knurl_response_known_length(knurl_response *response,
                                                   knurl_u64 length,
                                                   knurl_error *error);
/** Declare copied trailer names before begin; end supplies their values. */
KNURL_API knurl_status knurl_response_trailers_enable(knurl_response *response,
                                                      const char *const *names,
                                                      size_t count,
                                                      knurl_error *error);
/** Begin explicit live output; subsequent writes follow bounded transport
 * backpressure.
 */
KNURL_API knurl_status knurl_response_begin(knurl_response *response,
                                            knurl_error *error);
/** BUILDING only, HTTP/1.1 100/103. Copies bounded fields into the ordered
 * output queue without committing the later final response. AGAIN takes
 * nothing. At most 16 interim blocks / 128 KiB per request. */
KNURL_API knurl_status knurl_response_interim(knurl_response *response,
                                              int status,
                                              const knurl_header *headers,
                                              size_t count, knurl_error *error);
/** Accepts a bounded prefix. AGAIN always accepts zero bytes. */
KNURL_API knurl_status knurl_response_write(knurl_response *response,
                                            const void *data, size_t length,
                                            size_t *accepted,
                                            knurl_error *error);
/** Finish live output with optional declared trailers; completion waits for
 * queued bytes to drain.
 */
KNURL_API knurl_status knurl_response_end(knurl_response *response,
                                          const knurl_header *trailers,
                                          size_t count, knurl_error *error);
/** Discard an uncommitted output selection so a replacement can be built; fails
 * after commitment.
 */
KNURL_API knurl_status knurl_response_replace_uncommitted(
    knurl_response *response, knurl_error *error);
/** Abort output with a terminal cause; retained producers retire through
 * acknowledgement/cleanup.
 */
KNURL_API knurl_status knurl_response_abort(knurl_response *response,
                                            knurl_status cause,
                                            knurl_error *error);
/** Complete reply conveniences require BUILDING; failure installs no reply. */
KNURL_API knurl_status knurl_response_bytes(knurl_response *response,
                                            int status,
                                            const char *content_type,
                                            knurl_bytes bytes,
                                            knurl_error *error);
/** Takes the regular-file FD only on success. Bounded pread on the owner loop
 * can block on filesystem I/O; this is not a native-reader executor. */
KNURL_API knurl_status knurl_response_file_take(knurl_response *response,
                                                int fd, knurl_u64 offset,
                                                knurl_u64 length,
                                                knurl_error *error);
/** Takes a source only on success. OWNER_LOOP reads are nonblocking;
 * NATIVE_READER callbacks run through the bounded worker executor. No implicit
 * reset. EOF alone indicates success; AGAIN pauses until an explicit later
 * resume. */
KNURL_API knurl_status knurl_response_producer(knurl_response *response,
                                               knurl_source *source,
                                               knurl_error *error);
/** Schedule another owner-loop producer pull after AGAIN; coalesces without
 * recursion.
 */
KNURL_API knurl_status knurl_response_producer_resume(knurl_response *response,
                                                      knurl_error *error);

#ifdef __cplusplus
}
#endif
#endif

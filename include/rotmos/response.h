#ifndef ROTMOS_RESPONSE_H
#define ROTMOS_RESPONSE_H

/** @file response.h Public response API. Expected failures return rotmos_status
 * and optional bounded rotmos_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <rotmos/io.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Values for response phase. */
typedef enum rotmos_response_phase {
    /** Live response phase: building. */
    ROTMOS_RESPONSE_BUILDING = 0,
    /** Live response phase: active. */
    ROTMOS_RESPONSE_ACTIVE,
    /** Live response phase: finishing. */
    ROTMOS_RESPONSE_FINISHING,
    /** Live response phase: complete. */
    ROTMOS_RESPONSE_COMPLETE,
    /** Live response phase: aborted. */
    ROTMOS_RESPONSE_ABORTED
} rotmos_response_phase;

/** Copied terminal response accounting; committed distinguishes failure before
 * versus after final output commitment.
 */
typedef struct rotmos_response_outcome {
    /** Portable terminal status; OK indicates successful completion. */
    rotmos_status cause;
    /** HTTP status code; zero means unknown/unset where documented by this
     * event/builder.
     */
    int status;
    /** Nonzero when output or a storage mutation has crossed its commitment
     * point.
     */
    int committed;
    /** Exact final response wire bytes accepted by transport. */
    rotmos_u64 wire_bytes;
    /** Exact interim response wire bytes, separate from final output. */
    rotmos_u64 interim_wire_bytes;
    /** Exact final response payload bytes. */
    rotmos_u64 body_bytes;
    /** Exact monotonic operation duration in milliseconds. */
    rotmos_u64 duration_ms;
} rotmos_response_outcome;

/** Owner-loop live-output capacity notification; retry bounded writes without
 * recursion.
 */
typedef void (*rotmos_response_writable_fn)(rotmos_response *, void *);
/** Terminal live response callback with borrowed outcome; release callback
 * state according to the request lifetime.
 */
typedef void (*rotmos_response_close_fn)(rotmos_response *,
                                         const rotmos_response_outcome *,
                                         void *);

/** Borrowed until the request's terminal callback returns. Owner-loop only. */
ROTMOS_API rotmos_response *rotmos_request_response(rotmos_request *request);
/** Return the live response's current output phase on its owner loop. */
ROTMOS_API rotmos_response_phase
rotmos_response_state(const rotmos_response *response);
/** Return nonzero once final response headers have reached transport
 * commitment.
 */
ROTMOS_API int rotmos_response_is_committed(const rotmos_response *response);
/** Return bytes currently retained in the bounded output queue. */
ROTMOS_API size_t rotmos_response_queued_bytes(const rotmos_response *response);
/** Install owner-loop writable/terminal callbacks; caller retains userdata
 * through terminal delivery.
 */
ROTMOS_API rotmos_status rotmos_response_callbacks(
    rotmos_response *response, rotmos_response_writable_fn writable,
    rotmos_response_close_fn close, void *userdata, rotmos_error *error);
/** Set a final status while BUILDING; this does not begin output. */
ROTMOS_API rotmos_status rotmos_response_status(rotmos_response *response,
                                                int status,
                                                rotmos_error *error);
/** Copy a header, replacing case-insensitive matches while BUILDING. */
ROTMOS_API rotmos_status rotmos_response_header_set(rotmos_response *response,
                                                    const char *name,
                                                    const char *value,
                                                    rotmos_error *error);
/** Append a copied repeated header while BUILDING. */
ROTMOS_API rotmos_status rotmos_response_header_add(rotmos_response *response,
                                                    const char *name,
                                                    const char *value,
                                                    rotmos_error *error);
/** Declare the exact output-body length while BUILDING; mismatch fails
 * delivery.
 */
ROTMOS_API rotmos_status rotmos_response_known_length(rotmos_response *response,
                                                      rotmos_u64 length,
                                                      rotmos_error *error);
/** Declare copied trailer names before begin; end supplies their values. */
ROTMOS_API rotmos_status rotmos_response_trailers_enable(
    rotmos_response *response, const char *const *names, size_t count,
    rotmos_error *error);
/** Begin explicit live output; subsequent writes follow bounded transport
 * backpressure.
 */
ROTMOS_API rotmos_status rotmos_response_begin(rotmos_response *response,
                                               rotmos_error *error);
/** BUILDING only, HTTP/1.1 100/103. Copies bounded fields into the ordered
 * output queue without committing the later final response. AGAIN takes
 * nothing. At most 16 interim blocks / 128 KiB per request. */
ROTMOS_API rotmos_status rotmos_response_interim(rotmos_response *response,
                                                 int status,
                                                 const rotmos_header *headers,
                                                 size_t count,
                                                 rotmos_error *error);
/** Accepts a bounded prefix. AGAIN always accepts zero bytes. */
ROTMOS_API rotmos_status rotmos_response_write(rotmos_response *response,
                                               const void *data, size_t length,
                                               size_t *accepted,
                                               rotmos_error *error);
/** Finish live output with optional declared trailers; completion waits for
 * queued bytes to drain.
 */
ROTMOS_API rotmos_status rotmos_response_end(rotmos_response *response,
                                             const rotmos_header *trailers,
                                             size_t count, rotmos_error *error);
/** Discard an uncommitted output selection so a replacement can be built; fails
 * after commitment.
 */
ROTMOS_API rotmos_status rotmos_response_replace_uncommitted(
    rotmos_response *response, rotmos_error *error);
/** Abort output with a terminal cause; retained producers retire through
 * acknowledgement/cleanup.
 */
ROTMOS_API rotmos_status rotmos_response_abort(rotmos_response *response,
                                               rotmos_status cause,
                                               rotmos_error *error);
/** Complete reply conveniences require BUILDING; failure installs no reply. */
ROTMOS_API rotmos_status rotmos_response_bytes(rotmos_response *response,
                                               int status,
                                               const char *content_type,
                                               rotmos_bytes bytes,
                                               rotmos_error *error);
/** Takes the regular-file FD only on success. Bounded pread on the owner loop
 * can block on filesystem I/O; this is not a native-reader executor. */
ROTMOS_API rotmos_status rotmos_response_file_take(rotmos_response *response,
                                                   int fd, rotmos_u64 offset,
                                                   rotmos_u64 length,
                                                   rotmos_error *error);
/** Takes a source only on success. OWNER_LOOP reads are nonblocking;
 * NATIVE_READER callbacks run through the bounded worker executor. No implicit
 * reset. EOF alone indicates success; AGAIN pauses until an explicit later
 * resume. */
ROTMOS_API rotmos_status rotmos_response_producer(rotmos_response *response,
                                                  rotmos_source *source,
                                                  rotmos_error *error);
/** Schedule another owner-loop producer pull after AGAIN; coalesces without
 * recursion.
 */
ROTMOS_API rotmos_status
rotmos_response_producer_resume(rotmos_response *response, rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif

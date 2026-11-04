#ifndef ROTMOS_RESPONSE_H
#define ROTMOS_RESPONSE_H

#include <rotmos/io.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rotmos_response_phase {
    ROTMOS_RESPONSE_BUILDING = 0,
    ROTMOS_RESPONSE_ACTIVE,
    ROTMOS_RESPONSE_FINISHING,
    ROTMOS_RESPONSE_COMPLETE,
    ROTMOS_RESPONSE_ABORTED
} rotmos_response_phase;

typedef struct rotmos_response_outcome {
    rotmos_status cause;
    int status;
    int committed;
    rotmos_u64 wire_bytes;
    rotmos_u64 interim_wire_bytes;
    rotmos_u64 body_bytes;
    rotmos_u64 duration_ms;
} rotmos_response_outcome;

typedef void (*rotmos_response_writable_fn)(rotmos_response *, void *);
typedef void (*rotmos_response_close_fn)(rotmos_response *,
                                         const rotmos_response_outcome *,
                                         void *);

/* Borrowed until the request's terminal callback returns. Owner-loop only. */
ROTMOS_API rotmos_response *rotmos_request_response(rotmos_request *request);
ROTMOS_API rotmos_response_phase
rotmos_response_state(const rotmos_response *response);
ROTMOS_API int rotmos_response_is_committed(const rotmos_response *response);
ROTMOS_API size_t rotmos_response_queued_bytes(const rotmos_response *response);
ROTMOS_API rotmos_status rotmos_response_callbacks(
    rotmos_response *response, rotmos_response_writable_fn writable,
    rotmos_response_close_fn close, void *userdata, rotmos_error *error);
ROTMOS_API rotmos_status rotmos_response_status(rotmos_response *response,
                                                int status,
                                                rotmos_error *error);
ROTMOS_API rotmos_status rotmos_response_header_set(rotmos_response *response,
                                                    const char *name,
                                                    const char *value,
                                                    rotmos_error *error);
ROTMOS_API rotmos_status rotmos_response_header_add(rotmos_response *response,
                                                    const char *name,
                                                    const char *value,
                                                    rotmos_error *error);
ROTMOS_API rotmos_status rotmos_response_known_length(rotmos_response *response,
                                                      rotmos_u64 length,
                                                      rotmos_error *error);
ROTMOS_API rotmos_status rotmos_response_trailers_enable(
    rotmos_response *response, const char *const *names, size_t count,
    rotmos_error *error);
ROTMOS_API rotmos_status rotmos_response_begin(rotmos_response *response,
                                               rotmos_error *error);
/* BUILDING only, HTTP/1.1 100/103. Copies bounded fields into the ordered
 * output queue without committing the later final response. AGAIN takes
 * nothing. At most 16 interim blocks / 128 KiB per request. */
ROTMOS_API rotmos_status rotmos_response_interim(rotmos_response *response,
                                                 int status,
                                                 const rotmos_header *headers,
                                                 size_t count,
                                                 rotmos_error *error);
/* Accepts a bounded prefix. AGAIN always accepts zero bytes. */
ROTMOS_API rotmos_status rotmos_response_write(rotmos_response *response,
                                               const void *data, size_t length,
                                               size_t *accepted,
                                               rotmos_error *error);
ROTMOS_API rotmos_status rotmos_response_end(rotmos_response *response,
                                             const rotmos_header *trailers,
                                             size_t count, rotmos_error *error);
ROTMOS_API rotmos_status rotmos_response_replace_uncommitted(
    rotmos_response *response, rotmos_error *error);
ROTMOS_API rotmos_status rotmos_response_abort(rotmos_response *response,
                                               rotmos_status cause,
                                               rotmos_error *error);
/* Complete reply conveniences require BUILDING; failure installs no reply. */
ROTMOS_API rotmos_status rotmos_response_bytes(rotmos_response *response,
                                               int status,
                                               const char *content_type,
                                               rotmos_bytes bytes,
                                               rotmos_error *error);
/* Takes the regular-file FD only on success. Bounded pread on the owner loop
 * can block on filesystem I/O; this is not a native-reader executor. */
ROTMOS_API rotmos_status rotmos_response_file_take(rotmos_response *response,
                                                   int fd, rotmos_u64 offset,
                                                   rotmos_u64 length,
                                                   rotmos_error *error);
/* Takes a nonblocking OWNER_LOOP source only on success. No implicit reset.
 * EOF alone indicates success; AGAIN pauses until an explicit later resume. */
ROTMOS_API rotmos_status rotmos_response_producer(rotmos_response *response,
                                                  rotmos_source *source,
                                                  rotmos_error *error);
ROTMOS_API rotmos_status
rotmos_response_producer_resume(rotmos_response *response, rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif

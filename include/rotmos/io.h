#ifndef ROTMOS_IO_H
#define ROTMOS_IO_H

#include <rotmos/common.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef rotmos_status (*rotmos_source_read_fn)(void *buffer, size_t capacity,
                                               size_t *produced, void *userdata,
                                               rotmos_error *error);
typedef rotmos_status (*rotmos_source_reset_fn)(void *userdata,
                                                rotmos_error *error);
typedef void (*rotmos_source_close_fn)(void *userdata, rotmos_status outcome);
/* Close receives OK on completed consumption, the failure cause, or CANCELLED
 * on replacement/destruction before completion. Reentrant mutation is rejected.
 * Native runtime reads receive only their independently retained userdata and
 * chunk, never live request/response handles. Retain userdata until close: an
 * interrupted native read may acknowledge after transport closure. Close runs
 * on the owner loop after that acknowledgement, exactly once.
 */

typedef enum rotmos_source_execution {
    ROTMOS_SOURCE_OWNER_LOOP = 0,
    ROTMOS_SOURCE_NATIVE_READER
} rotmos_source_execution;

typedef struct rotmos_source_config {
    rotmos_source_read_fn read;
    rotmos_source_reset_fn reset;
    rotmos_source_close_fn close;
    void *userdata;
    rotmos_source_execution execution;
    rotmos_u64 length;
    int has_length;
} rotmos_source_config;

ROTMOS_API void rotmos_source_config_init(rotmos_source_config *config);
/* Construction does not call callbacks. Success owns one close notification. */
ROTMOS_API rotmos_status rotmos_source_new(const rotmos_source_config *config,
                                           rotmos_source **out,
                                           rotmos_error *error);
/* A copied in-memory source; construction never borrows the input storage. */
ROTMOS_API rotmos_status rotmos_source_bytes(rotmos_bytes bytes,
                                             rotmos_source **out,
                                             rotmos_error *error);
/* Opens and pins a regular file at its current length. Reads/reset use an
 * independent cursor; close releases the FD and never unlinks the path.
 * Later growth is excluded; truncation before the declared end fails. */
ROTMOS_API rotmos_status rotmos_source_file(const char *path,
                                            rotmos_source **out,
                                            rotmos_error *error);
/* Manual access is available before ownership passes to a reply/runtime.
 * A native upload handler may also read its borrowed source as documented in
 * upload.h; that view rejects reset, destroy and ownership transfer.
 */
ROTMOS_API rotmos_status rotmos_source_read(rotmos_source *source, void *buffer,
                                            size_t capacity, size_t *produced,
                                            rotmos_error *error);
ROTMOS_API rotmos_status rotmos_source_reset(rotmos_source *source,
                                             rotmos_error *error);
/* Owner-loop readiness notification after runtime transfer; can be called
 * inside an owner-loop read callback and coalesces a later pull. A native
 * provider posts through a public waker. No response/request handle needed. */
ROTMOS_API rotmos_status rotmos_source_resume(rotmos_source *, rotmos_error *);
/* Cooperative query during a retained native read callback. Includes response
 * revocation and worker stopping. Close waits for read acknowledgement and
 * runs once on the owner loop; no HTTP callback joins a native reader. */
ROTMOS_API int rotmos_source_cancelled(const rotmos_source *);
ROTMOS_API rotmos_status rotmos_source_destroy(rotmos_source *source,
                                               rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif

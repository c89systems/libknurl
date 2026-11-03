#ifndef ROTMOS_IO_H
#define ROTMOS_IO_H

#include <rotmos/common.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rotmos_source rotmos_source;

typedef rotmos_status (*rotmos_source_read_fn)(void *buffer, size_t capacity,
                                               size_t *produced, void *userdata,
                                               rotmos_error *error);
typedef rotmos_status (*rotmos_source_reset_fn)(void *userdata,
                                                rotmos_error *error);
typedef void (*rotmos_source_close_fn)(void *userdata, rotmos_status outcome);
/* Close receives OK on completed consumption, the failure cause, or CANCELLED
 * on replacement/destruction before completion. Reentrant mutation is rejected.
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
/* Manual access is available only before ownership passes to a reply/runtime.
 */
ROTMOS_API rotmos_status rotmos_source_read(rotmos_source *source, void *buffer,
                                            size_t capacity, size_t *produced,
                                            rotmos_error *error);
ROTMOS_API rotmos_status rotmos_source_reset(rotmos_source *source,
                                             rotmos_error *error);
ROTMOS_API rotmos_status rotmos_source_destroy(rotmos_source *source,
                                               rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif

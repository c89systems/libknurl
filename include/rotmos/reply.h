#ifndef ROTMOS_REPLY_H
#define ROTMOS_REPLY_H

#include <rotmos/io.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rotmos_reply rotmos_reply;

typedef enum rotmos_reply_kind {
    ROTMOS_REPLY_EMPTY = 0,
    ROTMOS_REPLY_BYTES,
    ROTMOS_REPLY_FILE,
    ROTMOS_REPLY_SOURCE
} rotmos_reply_kind;

typedef struct rotmos_source_materialize_config {
    size_t max_bytes;
    size_t chunk_bytes;
    int reset_before;
    int reset_after;
} rotmos_source_materialize_config;

/* Standalone builders can be used by custom dispatchers and tests. */
ROTMOS_API rotmos_status rotmos_reply_new(rotmos_reply **out,
                                          rotmos_error *error);
ROTMOS_API rotmos_status rotmos_reply_destroy(rotmos_reply *reply,
                                              rotmos_error *error);
/* Status-only selection releases payload/type while retaining headers. */
ROTMOS_API rotmos_status rotmos_reply_status(rotmos_reply *reply, int status,
                                             rotmos_error *error);
ROTMOS_API rotmos_status rotmos_reply_header_add(rotmos_reply *reply,
                                                 const char *name,
                                                 const char *value,
                                                 rotmos_error *error);
/* Replaces all case-insensitive occurrences at their first position. */
ROTMOS_API rotmos_status rotmos_reply_header_set(rotmos_reply *reply,
                                                 const char *name,
                                                 const char *value,
                                                 rotmos_error *error);
ROTMOS_API rotmos_status rotmos_reply_text(rotmos_reply *reply, int status,
                                           const char *content_type,
                                           const char *text,
                                           rotmos_error *error);
ROTMOS_API rotmos_status rotmos_reply_bytes(rotmos_reply *reply, int status,
                                            const char *content_type,
                                            rotmos_bytes body,
                                            rotmos_error *error);
/* Opens/pins a regular file; no later path lookup is used for delivery. */
ROTMOS_API rotmos_status rotmos_reply_file(rotmos_reply *reply, int status,
                                           const char *content_type,
                                           const char *path,
                                           rotmos_error *error);
/* No read/reset occurs. Success transfers the source until terminal cleanup. */
ROTMOS_API rotmos_status rotmos_reply_source_take(rotmos_reply *reply,
                                                  int status,
                                                  const char *content_type,
                                                  rotmos_source *source,
                                                  rotmos_error *error);
ROTMOS_API void
rotmos_source_materialize_config_init(rotmos_source_materialize_config *config);
/* Synchronous, explicitly buffered operation. Borrows the source, never closes
 * it, and rejects AGAIN. Reset is performed only when explicitly requested.
 * Zero bounds select defaults (10 MiB / 8192 bytes). Failure preserves the
 * prior reply but may consume input unless reset_after is enabled. */
ROTMOS_API rotmos_status rotmos_reply_source_materialized(
    rotmos_reply *reply, int status, const char *content_type,
    rotmos_source *source, const rotmos_source_materialize_config *config,
    rotmos_error *error);

/* These snapshots borrow the builder's storage until its next mutation. */
ROTMOS_API int rotmos_reply_is_selected(const rotmos_reply *reply);
ROTMOS_API int rotmos_reply_status_code(const rotmos_reply *reply);
ROTMOS_API rotmos_reply_kind
rotmos_reply_payload_kind(const rotmos_reply *reply);
ROTMOS_API rotmos_bytes rotmos_reply_content_type(const rotmos_reply *reply);
ROTMOS_API rotmos_bytes rotmos_reply_body(const rotmos_reply *reply);
ROTMOS_API size_t rotmos_reply_header_count(const rotmos_reply *reply);
ROTMOS_API rotmos_status rotmos_reply_header_at(const rotmos_reply *reply,
                                                size_t index,
                                                rotmos_header *out,
                                                rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif

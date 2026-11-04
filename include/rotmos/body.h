#ifndef ROTMOS_BODY_H
#define ROTMOS_BODY_H

#include <rotmos/http.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rotmos_body_storage {
    ROTMOS_BODY_STORAGE_NONE = 0,
    ROTMOS_BODY_MEMORY,
    ROTMOS_BODY_FILE
} rotmos_body_storage;

typedef struct rotmos_body_materialize_config {
    void *buffer;
    size_t capacity;
    size_t memory_limit_bytes;
    const char *directory;
    const char *prefix;
} rotmos_body_materialize_config;

typedef struct rotmos_body_result {
    rotmos_body_storage kind;
    rotmos_u64 length;
    rotmos_bytes memory;
    int owns_memory;
    char *path;
} rotmos_body_result;

ROTMOS_API void
rotmos_body_materialize_config_init(rotmos_body_materialize_config *config);
ROTMOS_API void rotmos_body_result_init(rotmos_body_result *result);
/* Frees owned memory/path metadata; returned files remain caller artifacts. */
ROTMOS_API void rotmos_body_result_cleanup(rotmos_body_result *result);
ROTMOS_API void
rotmos_body_policy_init_materialized(rotmos_body_policy *policy);
ROTMOS_API void rotmos_body_policy_init_stream(rotmos_body_policy *policy);
/* Borrowed completed transport body; NONE/STREAM provide no reader/result. */
ROTMOS_API rotmos_body *rotmos_request_body(rotmos_request *request);
ROTMOS_API rotmos_body_storage rotmos_body_kind(const rotmos_body *body);
ROTMOS_API rotmos_status rotmos_body_length(const rotmos_body *body,
                                            rotmos_u64 *out,
                                            rotmos_error *error);
ROTMOS_API rotmos_status rotmos_body_bytes(const rotmos_body *body,
                                           rotmos_bytes *out,
                                           rotmos_error *error);
/* Borrowed FD/path; no transfer, close, unlink or position mutation by caller.
 */
ROTMOS_API rotmos_status rotmos_body_file(const rotmos_body *body, int *fd,
                                          const char **path,
                                          rotmos_error *error);
ROTMOS_API rotmos_status rotmos_body_read(rotmos_body *body, void *buffer,
                                          size_t capacity, size_t *produced,
                                          rotmos_error *error);
ROTMOS_API rotmos_status rotmos_body_rewind(rotmos_body *body,
                                            rotmos_error *error);
/* Explicit independent copy/materialization; success rewinds the transport
 * reader before and after copying. Failure leaves an empty result. */
ROTMOS_API rotmos_status rotmos_request_body_materialize(
    rotmos_request *request, const rotmos_body_materialize_config *config,
    rotmos_body_result *out, rotmos_error *error);
ROTMOS_API rotmos_status rotmos_request_body_copy(rotmos_request *request,
                                                  void *buffer, size_t capacity,
                                                  rotmos_body_result *out,
                                                  rotmos_error *error);
ROTMOS_API rotmos_status rotmos_request_body_spill(
    rotmos_request *request, const rotmos_body_materialize_config *config,
    rotmos_body_result *out, rotmos_error *error);

/* Memory readers borrow the result/buffer until source close. File readers
 * pin an independently opened FD. Neither removes returned files. */
ROTMOS_API rotmos_status rotmos_body_result_open_reader(
    const rotmos_body_result *result, rotmos_source **out, rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif

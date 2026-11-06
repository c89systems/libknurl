#ifndef ROTMOS_BODY_H
#define ROTMOS_BODY_H

/** @file body.h Public body API. Expected failures return rotmos_status and
 * optional bounded rotmos_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <rotmos/http.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Values for body storage. */
typedef enum rotmos_body_storage {
    /** Completed body storage: none. */
    ROTMOS_BODY_STORAGE_NONE = 0,
    /** Completed body storage: memory. */
    ROTMOS_BODY_MEMORY,
    /** Completed body storage: file. */
    ROTMOS_BODY_FILE
} rotmos_body_storage;

/** Configuration for body materialize. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct rotmos_body_materialize_config {
    /** Caller-owned byte storage; retain through the synchronous operation. */
    void *buffer;
    /** Caller-provided buffer size in bytes. */
    size_t capacity;
    /** Memory threshold for explicit body materialization; remaining data may
     * become a file artifact.
     */
    size_t memory_limit_bytes;
    /** Directory for explicitly requested file materialization; NULL uses the
     * documented default.
     */
    const char *directory;
    /** Group path prefix or explicit materialization filename prefix. */
    const char *prefix;
} rotmos_body_materialize_config;

/** Independent body-copy result. Cleanup releases owned memory/path metadata,
 * but returned files remain caller artifacts.
 */
typedef struct rotmos_body_result {
    /** Representation or event discriminator; inspect before interpreting other
     * fields.
     */
    rotmos_body_storage kind;
    /** Exact number of bytes; excludes any incidental trailing NUL. */
    rotmos_u64 length;
    /** Independent materialized bytes; may borrow the caller buffer when
     * owns_memory is zero.
     */
    rotmos_bytes memory;
    /** Nonzero when result cleanup frees the memory view's backing allocation.
     */
    int owns_memory;
    /** Resource or route path, interpreted according to the enclosing
     * operation.
     */
    char *path;
} rotmos_body_result;

/** Initialize body materialize config with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void
rotmos_body_materialize_config_init(rotmos_body_materialize_config *config);
/** Initialize body result with its documented defaults; call before applying
 * overrides.
 */
ROTMOS_API void rotmos_body_result_init(rotmos_body_result *result);
/** Frees owned memory/path metadata; returned files remain caller artifacts. */
ROTMOS_API void rotmos_body_result_cleanup(rotmos_body_result *result);
/** Initialize body policy materialized with its documented defaults; call
 * before applying overrides.
 */
ROTMOS_API void
rotmos_body_policy_init_materialized(rotmos_body_policy *policy);
/** Initialize body policy stream with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void rotmos_body_policy_init_stream(rotmos_body_policy *policy);
/** Borrowed completed transport body; NONE/STREAM provide no reader/result. */
ROTMOS_API rotmos_body *rotmos_request_body(rotmos_request *request);
/** Return the completed body's storage kind; the handle remains request-owned.
 */
ROTMOS_API rotmos_body_storage rotmos_body_kind(const rotmos_body *body);
/** Read the exact completed transport-body length without consuming it. */
ROTMOS_API rotmos_status rotmos_body_length(const rotmos_body *body,
                                            rotmos_u64 *out,
                                            rotmos_error *error);
/** Borrow in-memory body bytes until request close; file-backed bodies are not
 * read into memory.
 */
ROTMOS_API rotmos_status rotmos_body_bytes(const rotmos_body *body,
                                           rotmos_bytes *out,
                                           rotmos_error *error);
/** Borrowed FD/path; no transfer, close, unlink or position mutation by caller.
 */
ROTMOS_API rotmos_status rotmos_body_file(const rotmos_body *body, int *fd,
                                          const char **path,
                                          rotmos_error *error);
/** Read a bounded chunk from the completed body's cursor; EOF ends the reader.
 */
ROTMOS_API rotmos_status rotmos_body_read(rotmos_body *body, void *buffer,
                                          size_t capacity, size_t *produced,
                                          rotmos_error *error);
/** Reset the completed transport-body cursor to its beginning. */
ROTMOS_API rotmos_status rotmos_body_rewind(rotmos_body *body,
                                            rotmos_error *error);
/** Explicit independent copy/materialization; success rewinds the transport
 * reader before and after copying. Failure leaves an empty result. */
ROTMOS_API rotmos_status rotmos_request_body_materialize(
    rotmos_request *request, const rotmos_body_materialize_config *config,
    rotmos_body_result *out, rotmos_error *error);
/** Copy the completed body into caller storage; rewind before and after. See
 * rotmos_request_body_materialize.
 */
ROTMOS_API rotmos_status rotmos_request_body_copy(rotmos_request *request,
                                                  void *buffer, size_t capacity,
                                                  rotmos_body_result *out,
                                                  rotmos_error *error);
/** Copy the completed body to a caller-owned file artifact; never silently
 * buffer live input. See rotmos_request_body_materialize.
 */
ROTMOS_API rotmos_status rotmos_request_body_spill(
    rotmos_request *request, const rotmos_body_materialize_config *config,
    rotmos_body_result *out, rotmos_error *error);

/** Memory readers borrow the result/buffer until source close. File readers
 * pin an independently opened regular-file FD. Paths open with O_NONBLOCK
 * before type validation, so a FIFO does not wait for a writer. Neither
 * reader removes returned files. */
ROTMOS_API rotmos_status rotmos_body_result_open_reader(
    const rotmos_body_result *result, rotmos_source **out, rotmos_error *error);

#ifdef __cplusplus
}
#endif
#endif

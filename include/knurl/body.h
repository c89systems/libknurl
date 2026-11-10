#ifndef KNURL_BODY_H
#define KNURL_BODY_H

/** @file body.h Public body API. Expected failures return knurl_status and
 * optional bounded knurl_error diagnostics. Take operations transfer
 * ownership only on success; views remain borrowed for their documented scope.
 */

#include <knurl/http.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Values for body storage. */
typedef enum knurl_body_storage {
    /** Completed body storage: none. */
    KNURL_BODY_STORAGE_NONE = 0,
    /** Completed body storage: memory. */
    KNURL_BODY_MEMORY,
    /** Completed body storage: file. */
    KNURL_BODY_FILE
} knurl_body_storage;

/** Configuration for body materialize. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct knurl_body_materialize_config {
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
} knurl_body_materialize_config;

/** Independent body-copy result. Cleanup releases owned memory/path metadata,
 * but returned files remain caller artifacts.
 */
typedef struct knurl_body_result {
    /** Representation or event discriminator; inspect before interpreting other
     * fields.
     */
    knurl_body_storage kind;
    /** Exact number of bytes; excludes any incidental trailing NUL. */
    knurl_u64 length;
    /** Independent materialized bytes; may borrow the caller buffer when
     * owns_memory is zero.
     */
    knurl_bytes memory;
    /** Nonzero when result cleanup frees the memory view's backing allocation.
     */
    int owns_memory;
    /** Resource or route path, interpreted according to the enclosing
     * operation.
     */
    char *path;
} knurl_body_result;

/** Initialize body materialize config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void
knurl_body_materialize_config_init(knurl_body_materialize_config *config);
/** Initialize body result with its documented defaults; call before applying
 * overrides.
 */
KNURL_API void knurl_body_result_init(knurl_body_result *result);
/** Frees owned memory/path metadata; returned files remain caller artifacts. */
KNURL_API void knurl_body_result_cleanup(knurl_body_result *result);
/** Initialize body policy materialized with its documented defaults; call
 * before applying overrides.
 */
KNURL_API void knurl_body_policy_init_materialized(knurl_body_policy *policy);
/** Initialize body policy stream with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_body_policy_init_stream(knurl_body_policy *policy);
/** Borrowed completed transport body; NONE/STREAM provide no reader/result. */
KNURL_API knurl_body *knurl_request_body(knurl_request *request);
/** Return the completed body's storage kind; the handle remains request-owned.
 */
KNURL_API knurl_body_storage knurl_body_kind(const knurl_body *body);
/** Read the exact completed transport-body length without consuming it. */
KNURL_API knurl_status knurl_body_length(const knurl_body *body, knurl_u64 *out,
                                         knurl_error *error);
/** Borrow in-memory body bytes until request close; file-backed bodies are not
 * read into memory.
 */
KNURL_API knurl_status knurl_body_bytes(const knurl_body *body,
                                        knurl_bytes *out, knurl_error *error);
/** Borrowed FD/path; no transfer, close, unlink or position mutation by caller.
 */
KNURL_API knurl_status knurl_body_file(const knurl_body *body, int *fd,
                                       const char **path, knurl_error *error);
/** Read a bounded chunk from the completed body's cursor; EOF ends the reader.
 */
KNURL_API knurl_status knurl_body_read(knurl_body *body, void *buffer,
                                       size_t capacity, size_t *produced,
                                       knurl_error *error);
/** Reset the completed transport-body cursor to its beginning. */
KNURL_API knurl_status knurl_body_rewind(knurl_body *body, knurl_error *error);
/** Explicit independent copy/materialization; success rewinds the transport
 * reader before and after copying. Failure leaves an empty result. */
KNURL_API knurl_status knurl_request_body_materialize(
    knurl_request *request, const knurl_body_materialize_config *config,
    knurl_body_result *out, knurl_error *error);
/** Copy the completed body into caller storage; rewind before and after. See
 * knurl_request_body_materialize.
 */
KNURL_API knurl_status knurl_request_body_copy(knurl_request *request,
                                               void *buffer, size_t capacity,
                                               knurl_body_result *out,
                                               knurl_error *error);
/** Copy the completed body to a caller-owned file artifact; never silently
 * buffer live input. See knurl_request_body_materialize.
 */
KNURL_API knurl_status knurl_request_body_spill(
    knurl_request *request, const knurl_body_materialize_config *config,
    knurl_body_result *out, knurl_error *error);

/** Memory readers borrow the result/buffer until source close. File readers
 * pin an independently opened regular-file FD. Paths open with O_NONBLOCK
 * before type validation, so a FIFO does not wait for a writer. Neither
 * reader removes returned files. */
KNURL_API knurl_status knurl_body_result_open_reader(
    const knurl_body_result *result, knurl_source **out, knurl_error *error);

#ifdef __cplusplus
}
#endif
#endif

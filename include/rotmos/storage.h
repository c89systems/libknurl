#ifndef ROTMOS_STORAGE_H
#define ROTMOS_STORAGE_H
#include <rotmos/asset.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct rotmos_storage_provider rotmos_storage_provider;
typedef struct rotmos_storage_operation rotmos_storage_operation;
typedef struct rotmos_storage_resource rotmos_storage_resource;
typedef enum rotmos_storage_code {
    ROTMOS_STORAGE_OK = 0,
    ROTMOS_STORAGE_NOT_FOUND,
    ROTMOS_STORAGE_EXISTS,
    ROTMOS_STORAGE_INVALID,
    ROTMOS_STORAGE_LIMIT,
    ROTMOS_STORAGE_IO,
    ROTMOS_STORAGE_NOMEM,
    ROTMOS_STORAGE_CONFLICT,
    ROTMOS_STORAGE_TOMBSTONED,
    ROTMOS_STORAGE_PRECONDITION,
    ROTMOS_STORAGE_NOT_SUPPORTED,
    ROTMOS_STORAGE_END,
    ROTMOS_STORAGE_DENIED
} rotmos_storage_code;
typedef enum rotmos_storage_action {
    ROTMOS_STORAGE_STAT = 0,
    ROTMOS_STORAGE_OPEN_READ,
    ROTMOS_STORAGE_LIST_OPEN,
    ROTMOS_STORAGE_LIST_NEXT,
    ROTMOS_STORAGE_PUT_BEGIN,
    ROTMOS_STORAGE_PUT_WRITE,
    ROTMOS_STORAGE_PUT_FINISH,
    ROTMOS_STORAGE_PUT_ABORT,
    ROTMOS_STORAGE_DELETE,
    ROTMOS_STORAGE_MKCOL,
    ROTMOS_STORAGE_COPY,
    ROTMOS_STORAGE_MOVE
} rotmos_storage_action;
typedef enum rotmos_storage_resource_kind {
    ROTMOS_STORAGE_LIST = 0,
    ROTMOS_STORAGE_WRITE = 1
} rotmos_storage_resource_kind;
#define ROTMOS_STORAGE_CAN_WRITE 1u
#define ROTMOS_STORAGE_CAN_CONDITIONAL 2u
#define ROTMOS_STORAGE_CAN_COPY 4u
#define ROTMOS_STORAGE_CAN_MOVE 8u
#define ROTMOS_STORAGE_ATOMIC_CROSS_RESOURCE 16u
#define ROTMOS_STORAGE_DURABLE_COMMIT 32u
#define ROTMOS_STORAGE_CAN_SCOPE_CHECK 64u
#define ROTMOS_STORAGE_CHUNK_MAX 65536u
#define ROTMOS_STORAGE_CONDITION_MAX 65536u
typedef struct rotmos_storage_arguments {
    rotmos_storage_action action;
    const char *path, *destination, *if_match, *if_none_match;
    rotmos_storage_resource *resource;
    rotmos_bytes bytes;
    int overwrite, depth, atomic_cross_resource_required;
} rotmos_storage_arguments;
typedef struct rotmos_storage_result {
    rotmos_storage_code code;
    rotmos_error error;
    rotmos_asset_info info;
    rotmos_asset_entry entry;
    rotmos_asset *asset;
    rotmos_storage_resource *resource;
    size_t accepted;
    const char *recovery_path;
    /* Commit/partial outcomes survive interruption. Created distinguishes a
     * committed new destination from replacement without an unlocked stat. */
    int has_info, has_entry, committed, partial, created;
} rotmos_storage_result;
typedef struct rotmos_storage_outcome {
    rotmos_status cause;
    const rotmos_storage_result *result;
    int cancelled;
} rotmos_storage_outcome;
typedef rotmos_status (*rotmos_storage_invoke_fn)(
    rotmos_storage_operation *, const rotmos_storage_arguments *, void *,
    rotmos_error *);
typedef void (*rotmos_storage_provider_retire_fn)(
    rotmos_storage_operation *, const rotmos_storage_outcome *, void *);
typedef struct rotmos_storage_provider_config {
    unsigned int capabilities;
    const char *commit_description;
    rotmos_storage_invoke_fn invoke;
    rotmos_storage_invoke_fn cancel;
    rotmos_storage_provider_retire_fn retired;
    rotmos_retire_fn close;
    void *userdata;
} rotmos_storage_provider_config;
typedef void (*rotmos_storage_complete_fn)(rotmos_storage_operation *,
                                           const rotmos_storage_outcome *,
                                           void *);
typedef void (*rotmos_storage_interrupt_fn)(rotmos_storage_operation *,
                                            rotmos_status, void *);
typedef void (*rotmos_storage_retire_fn)(const rotmos_storage_outcome *,
                                         void *);
typedef rotmos_status (*rotmos_storage_scope_fn)(rotmos_storage_operation *,
                                                 const char *path,
                                                 const char *destination,
                                                 int *allowed, void *,
                                                 rotmos_error *);
typedef struct rotmos_storage_operation_config {
    rotmos_storage_complete_fn completed;
    rotmos_storage_interrupt_fn interrupted;
    rotmos_storage_retire_fn retired;
    rotmos_storage_scope_fn check_scope;
    void *userdata;
    long timeout_ms;
    int timeout_disabled;
} rotmos_storage_operation_config;
typedef struct rotmos_storage_resource_config {
    rotmos_storage_resource_kind kind;
    void *userdata;
    rotmos_retire_fn close;
} rotmos_storage_resource_config;
ROTMOS_API void
rotmos_storage_provider_config_init(rotmos_storage_provider_config *);
ROTMOS_API void
rotmos_storage_operation_config_init(rotmos_storage_operation_config *);
ROTMOS_API void rotmos_storage_arguments_init(rotmos_storage_arguments *);
ROTMOS_API void rotmos_storage_result_init(rotmos_storage_result *);
ROTMOS_API void
rotmos_storage_resource_config_init(rotmos_storage_resource_config *);
ROTMOS_API const char *rotmos_storage_code_string(rotmos_storage_code);
/* Inert declaration copy. Cancel is required: OK acknowledges cancellation,
 * AGAIN promises later acknowledgement through operation_complete. A cancel
 * error does not acknowledge or release pending arguments. Provider callbacks
 * run on one bound owner loop; resources may be closed on that thread after
 * the run. No reuse of bound providers across loop/run boundaries. */
ROTMOS_API rotmos_status
rotmos_storage_provider_new(const rotmos_storage_provider_config *,
                            rotmos_storage_provider **, rotmos_error *);
ROTMOS_API unsigned int
rotmos_storage_provider_capabilities(const rotmos_storage_provider *);
ROTMOS_API const char *
rotmos_storage_provider_commit_description(const rotmos_storage_provider *);
ROTMOS_API rotmos_status
rotmos_storage_provider_close(rotmos_storage_provider *, rotmos_error *);
/* Active HTTP worker loop only (on_worker_start or later). Success accepts
 * one operation and owns one terminal retirement, even when
 * invoke completes synchronously or reports a domain/protocol failure. Bad
 * declarations/capabilities/allocation failures before invoke take nothing.
 * Provider invoke returns OK synchronously or AGAIN with responsibility for
 * later completion. Staged results and callbacks are always delivered later.
 * Arguments/chunks are copied and pinned through provider acknowledgement.
 * Timeout zero selects 30000 ms, including queue/lock wait. */
ROTMOS_API rotmos_status rotmos_storage_operation_start(
    rotmos_storage_provider *, rotmos_loop *, const rotmos_storage_arguments *,
    const rotmos_storage_operation_config *, rotmos_storage_operation **,
    rotmos_error *);
/* Owner-loop only, including during invoke/cancel. Copies metadata and takes
 * asset/resource results only on success. Failure takes nothing and permits
 * retry. A successful completion acknowledges that no job still uses the
 * operation/arguments; completion occurs once. Native work publishes through
 * its owner-loop completion callback or a public waker, never directly. */
ROTMOS_API rotmos_status rotmos_storage_operation_complete(
    rotmos_storage_operation *, const rotmos_storage_result *, rotmos_error *);
ROTMOS_API rotmos_loop *
rotmos_storage_operation_loop(const rotmos_storage_operation *);
ROTMOS_API const rotmos_storage_arguments *
rotmos_storage_operation_arguments(const rotmos_storage_operation *);
ROTMOS_API void *
rotmos_storage_operation_userdata(const rotmos_storage_operation *);
ROTMOS_API rotmos_status rotmos_storage_operation_set_userdata(
    rotmos_storage_operation *, void *, rotmos_error *);
/* Immutable query may be used by retained native work. A configured checker
 * requires CAN_SCOPE_CHECK before accepting a mutation. Scope-capable providers
 * check every affected physical descendant, mapped destination and overwritten
 * destination descendant inside the same atomic mutation scope. */
ROTMOS_API int
rotmos_storage_operation_has_scope_check(const rotmos_storage_operation *);
/* Owner-loop only. Invokes check_scope synchronously; never yields. Retained
 * native jobs must marshal checks to this loop, never invoke policy directly.
 * Cancellation suppresses policy callbacks and denies the check. */
ROTMOS_API rotmos_status
rotmos_storage_operation_check_scope(rotmos_storage_operation *, const char *,
                                     const char *, int *, rotmos_error *);
/* Cooperative native-thread check. Explicit cancellation revokes consumer
 * callbacks immediately. Timeout/shutdown may first deliver interrupted on the
 * owner loop. Retirement still waits for acknowledgement and preserves actual
 * committed/partial/recovery results. No rollback is implied by cancellation.
 */
ROTMOS_API int
rotmos_storage_operation_cancelled(const rotmos_storage_operation *);
ROTMOS_API rotmos_status
rotmos_storage_operation_cancel(rotmos_storage_operation *, rotmos_error *);
ROTMOS_API rotmos_status rotmos_storage_operation_take_asset(
    rotmos_storage_operation *, rotmos_asset **, rotmos_error *);
ROTMOS_API rotmos_status rotmos_storage_operation_take_resource(
    rotmos_storage_operation *, rotmos_storage_resource **, rotmos_error *);
/* Provider-owned resource constructors run on the operation's loop. An open
 * operation retains its argument resource after its caller closes that owner.
 * Returned resources transfer through operation_complete/take_resource. */
ROTMOS_API rotmos_status rotmos_storage_resource_new(
    rotmos_storage_operation *, const rotmos_storage_resource_config *,
    rotmos_storage_resource **, rotmos_error *);
ROTMOS_API void *
rotmos_storage_resource_userdata(const rotmos_storage_resource *);
ROTMOS_API rotmos_storage_resource_kind
rotmos_storage_resource_type(const rotmos_storage_resource *);
ROTMOS_API rotmos_status
rotmos_storage_resource_close(rotmos_storage_resource *, rotmos_error *);
/* Read-only adapter over a borrowed asset provider. No new packed format or
 * Vectis object is introduced. Caller retains the asset provider through close.
 */
ROTMOS_API rotmos_status rotmos_storage_provider_assets(
    rotmos_asset_provider *, rotmos_storage_provider **, rotmos_error *);
typedef struct rotmos_storage_filesystem_config {
    const char *cache_dir, *site_id, *root_dir;
    size_t max_file_bytes, max_total_bytes, max_resources;
} rotmos_storage_filesystem_config;
ROTMOS_API void
rotmos_storage_filesystem_config_init(rotmos_storage_filesystem_config *);
/* Pure path calculation for host-owned asset extraction/hydration. Does not
 * create directories; output is cleared on failure. */
ROTMOS_API rotmos_status rotmos_storage_filesystem_content_directory(
    const rotmos_storage_filesystem_config *, char *, size_t, rotmos_error *);
/* Inert declaration over a borrowed server. Filesystem providers share one
 * bounded executor in that HTTP worker, created on its first operation.
 * Absolute cache/root paths; NULL root
 * selects cache_dir/webdav/site_id/content and the existing tombstone tree.
 * Zero quotas select 16 MiB/file, 128 MiB total and 4096 resources.
 * PUT transactions are capped by max_file_bytes and admitted by server limits;
 * native execution uses the worker's bounded queue with backpressure.
 * Conditions are rechecked with quota accounting under the mutation lock at
 * finish. No filesystem I/O or threads are started by construction. Server
 * outlives the provider; bound providers are not reusable across runs. */
ROTMOS_API rotmos_status rotmos_storage_filesystem_new(
    rotmos_server *, const rotmos_storage_filesystem_config *,
    rotmos_storage_provider **, rotmos_error *);
/* Synchronous filesystem calls lifted from Vectis's standalone WebDAV API.
 * No server, loop or executor is required. Configurations/conditions are
 * borrowed for the call. Reads return malloc-owned bytes (NULL for empty
 * files); callers free them. Listing stops successfully when callback returns
 * zero. Entry ETags are unquoted SHA-256 hex, populated by read; lookup leaves
 * them empty. COPY depth is 0 or -1; MOVE always moves the whole resource.
 * Conditions, quotas, tombstones and mutations share the HTTP engine's lock.
 * Failed direct MOVE can preserve a committed destination/recovery backup,
 * as documented for the asynchronous filesystem provider. */
#define ROTMOS_FILESYSTEM_PATH_MAX 1024u
#define ROTMOS_FILESYSTEM_STORAGE_PATH_MAX 4096u
typedef enum rotmos_filesystem_entry_kind {
    ROTMOS_FILESYSTEM_ENTRY_NONE = 0,
    ROTMOS_FILESYSTEM_ENTRY_FILE,
    ROTMOS_FILESYSTEM_ENTRY_TOMBSTONE,
    ROTMOS_FILESYSTEM_ENTRY_COLLECTION
} rotmos_filesystem_entry_kind;
typedef struct rotmos_filesystem_entry {
    rotmos_filesystem_entry_kind kind;
    size_t size;
    char storage_path[ROTMOS_FILESYSTEM_STORAGE_PATH_MAX];
    char etag[65];
} rotmos_filesystem_entry;
typedef int (*rotmos_filesystem_list_fn)(const char *,
                                         rotmos_filesystem_entry_kind, size_t,
                                         void *);
ROTMOS_API int rotmos_storage_filesystem_path_normalize(
    const char *path, char out[ROTMOS_FILESYSTEM_PATH_MAX + 1]);
ROTMOS_API rotmos_storage_code rotmos_storage_filesystem_lookup(
    const rotmos_storage_filesystem_config *config, const char *path,
    rotmos_filesystem_entry *entry);
ROTMOS_API rotmos_storage_code rotmos_storage_filesystem_read(
    const rotmos_storage_filesystem_config *config, const char *path,
    unsigned char **body, size_t *size, rotmos_filesystem_entry *entry);
ROTMOS_API rotmos_storage_code
rotmos_storage_filesystem_put(const rotmos_storage_filesystem_config *config,
                              const char *path, const void *body, size_t size,
                              const char *if_match, const char *if_none_match);
ROTMOS_API rotmos_storage_code rotmos_storage_filesystem_delete(
    const rotmos_storage_filesystem_config *config, const char *path,
    const char *if_match, const char *if_none_match);
ROTMOS_API rotmos_storage_code rotmos_storage_filesystem_mkcol(
    const rotmos_storage_filesystem_config *config, const char *path,
    const char *if_match, const char *if_none_match);
ROTMOS_API rotmos_storage_code rotmos_storage_filesystem_copy(
    const rotmos_storage_filesystem_config *config, const char *source,
    const char *destination, int overwrite, int depth, const char *if_match,
    const char *if_none_match);
ROTMOS_API rotmos_storage_code rotmos_storage_filesystem_move(
    const rotmos_storage_filesystem_config *config, const char *source,
    const char *destination, int overwrite, const char *if_match,
    const char *if_none_match);
ROTMOS_API rotmos_storage_code rotmos_storage_filesystem_list(
    const rotmos_storage_filesystem_config *config, const char *path,
    rotmos_filesystem_list_fn callback, void *userdata);
#ifdef __cplusplus
}
#endif
#endif

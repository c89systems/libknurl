#ifndef ROTMOS_STORAGE_H
#define ROTMOS_STORAGE_H
#include <rotmos/asset.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Owned storage provider bound to one worker loop/run after first use. */
typedef struct rotmos_storage_provider rotmos_storage_provider;
/** Acknowledged storage operation; borrowed until actual retirement. */
typedef struct rotmos_storage_operation rotmos_storage_operation;
/** Provider resource retained by every in-flight operation using it. */
typedef struct rotmos_storage_resource rotmos_storage_resource;
/** Values for storage code. */
typedef enum rotmos_storage_code {
    /** Storage domain result/action: ok. */
    ROTMOS_STORAGE_OK = 0,
    /** Storage domain result/action: not found. */
    ROTMOS_STORAGE_NOT_FOUND,
    /** Storage domain result/action: exists. */
    ROTMOS_STORAGE_EXISTS,
    /** Storage domain result/action: invalid. */
    ROTMOS_STORAGE_INVALID,
    /** Public bound for storage limit, in bytes or entries as named. */
    ROTMOS_STORAGE_LIMIT,
    /** Storage domain result/action: io. */
    ROTMOS_STORAGE_IO,
    /** Storage domain result/action: nomem. */
    ROTMOS_STORAGE_NOMEM,
    /** Storage domain result/action: conflict. */
    ROTMOS_STORAGE_CONFLICT,
    /** Storage domain result/action: tombstoned. */
    ROTMOS_STORAGE_TOMBSTONED,
    /** Storage domain result/action: precondition. */
    ROTMOS_STORAGE_PRECONDITION,
    /** Storage domain result/action: not supported. */
    ROTMOS_STORAGE_NOT_SUPPORTED,
    /** Storage domain result/action: end. */
    ROTMOS_STORAGE_END,
    /** Storage domain result/action: denied. */
    ROTMOS_STORAGE_DENIED
} rotmos_storage_code;
/** Values for storage action. */
typedef enum rotmos_storage_action {
    /** Storage domain result/action: stat. */
    ROTMOS_STORAGE_STAT = 0,
    /** Storage domain result/action: open read. */
    ROTMOS_STORAGE_OPEN_READ,
    /** Storage domain result/action: list open. */
    ROTMOS_STORAGE_LIST_OPEN,
    /** Storage domain result/action: list next. */
    ROTMOS_STORAGE_LIST_NEXT,
    /** Storage domain result/action: put begin. */
    ROTMOS_STORAGE_PUT_BEGIN,
    /** Storage domain result/action: put write. */
    ROTMOS_STORAGE_PUT_WRITE,
    /** Storage domain result/action: put finish. */
    ROTMOS_STORAGE_PUT_FINISH,
    /** Storage domain result/action: put abort. */
    ROTMOS_STORAGE_PUT_ABORT,
    /** Storage domain result/action: delete. */
    ROTMOS_STORAGE_DELETE,
    /** Storage domain result/action: mkcol. */
    ROTMOS_STORAGE_MKCOL,
    /** Storage domain result/action: copy. */
    ROTMOS_STORAGE_COPY,
    /** Storage domain result/action: move. */
    ROTMOS_STORAGE_MOVE
} rotmos_storage_action;
/** Values for storage resource kind. */
typedef enum rotmos_storage_resource_kind {
    /** Storage domain result/action: list. */
    ROTMOS_STORAGE_LIST = 0,
    /** Storage domain result/action: write. */
    ROTMOS_STORAGE_WRITE = 1
} rotmos_storage_resource_kind;
/** Provider capability bit: write. */
#define ROTMOS_STORAGE_CAN_WRITE 1u
/** Provider capability bit: conditional. */
#define ROTMOS_STORAGE_CAN_CONDITIONAL 2u
/** Provider capability bit: copy. */
#define ROTMOS_STORAGE_CAN_COPY 4u
/** Provider capability bit: move. */
#define ROTMOS_STORAGE_CAN_MOVE 8u
/** Provider guarantees source/destination atomicity for cross-resource
 * mutations.
 */
#define ROTMOS_STORAGE_ATOMIC_CROSS_RESOURCE 16u
/** Provider advertises durable commit semantics described by
 * commit_description.
 */
#define ROTMOS_STORAGE_DURABLE_COMMIT 32u
/** Provider checks every affected descendant/destination inside the mutation
 * scope.
 */
#define ROTMOS_STORAGE_CAN_SCOPE_CHECK 64u
/** Maximum copied storage operation chunk size in bytes. */
#define ROTMOS_STORAGE_CHUNK_MAX 65536u
/** Maximum copied storage conditional-header size in bytes. */
#define ROTMOS_STORAGE_CONDITION_MAX 65536u
/** One storage command; operation_start copies argument text/chunks and retains
 * resources through provider acknowledgement.
 */
typedef struct rotmos_storage_arguments {
    /** Requested operation or authorization decision discriminator. */
    rotmos_storage_action action;
    /** Resource or route path, interpreted according to the enclosing
     * operation.
     */
    const char *path;
    /** Normalized COPY/MOVE destination path. */
    const char *destination;
    /** Conditional ETag match expression; NULL omits this precondition. */
    const char *if_match;
    /** Conditional ETag exclusion expression; NULL omits this precondition. */
    const char *if_none_match;
    /** Provider listing/write resource, retained while referenced by an
     * operation.
     */
    rotmos_storage_resource *resource;
    /** Copied bounded operation payload, or TLS bytes copied by the server
     * declaration.
     */
    rotmos_bytes bytes;
    /** Nonzero permits replacement of the COPY/MOVE destination. */
    int overwrite;
    /** Collection COPY depth: 0 or -1; WebDAV PROPFIND supports finite 0/1. */
    int depth;
    /** Nonzero requires provider atomicity across source/destination resources.
     */
    int atomic_cross_resource_required;
} rotmos_storage_arguments;
/** Storage-domain outcome with explicit asset/resource transfer,
 * commit/partial/recovery state and optional metadata.
 */
typedef struct rotmos_storage_result {
    /** Storage-domain result, distinct from transport/cancellation status. */
    rotmos_storage_code code;
    /** Copied bounded diagnostic; OK/NONE when no error is attached. */
    rotmos_error error;
    /** Opened-resource metadata; valid only when the containing result supplies
     * metadata.
     */
    rotmos_asset_info info;
    /** One immediate listing child when has_entry is nonzero. */
    rotmos_asset_entry entry;
    /** Opened asset offered for successful ownership transfer. */
    rotmos_asset *asset;
    /** Provider listing/write resource, retained while referenced by an
     * operation.
     */
    rotmos_storage_resource *resource;
    /** Number of input bytes accepted by a PUT write operation. */
    size_t accepted;
    /** Copied recovery artifact path for a committed/partial failed mutation,
     * when available.
     */
    const char *recovery_path;
    /** Commit/partial outcomes survive interruption. Created distinguishes a
     * committed new destination from replacement without an unlocked stat. */
    int has_info;
    /** Nonzero when entry is populated. */
    int has_entry;
    /** Nonzero when output or a storage mutation has crossed its commitment
     * point.
     */
    int committed;
    /** Nonzero when a mutation left a partial committed/recovery outcome. */
    int partial;
    /** Nonzero distinguishes newly created destinations from replacement under
     * the mutation lock.
     */
    int created;
} rotmos_storage_result;
/** Terminal operation status plus provider result; interruption/cancellation
 * never implies rollback of a committed mutation.
 */
typedef struct rotmos_storage_outcome {
    /** Portable terminal status; OK indicates successful completion. */
    rotmos_status cause;
    /** Borrowed storage result retaining actual committed/partial outcomes
     * through retirement.
     */
    const rotmos_storage_result *result;
    /** Nonzero for cancellation; does not erase actual commit/partial outcomes.
     */
    int cancelled;
} rotmos_storage_outcome;
/** Owner-loop provider invoke/cancel entry. OK acknowledges synchronously;
 * AGAIN promises later operation_complete. A cancellation error does not
 * release arguments or acknowledge native work.
 */
typedef rotmos_status (*rotmos_storage_invoke_fn)(
    rotmos_storage_operation *, const rotmos_storage_arguments *, void *,
    rotmos_error *);
/** Provider-side final retirement after acknowledgement; operation/result are
 * borrowed for this call and no job may still use their arguments.
 */
typedef void (*rotmos_storage_provider_retire_fn)(
    rotmos_storage_operation *, const rotmos_storage_outcome *, void *);
/** Configuration for storage provider. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct rotmos_storage_provider_config {
    /** Bit mask of provider-supported storage guarantees and actions. */
    unsigned int capabilities;
    /** Copied provider durability/commit description. */
    const char *commit_description;
    /** Owner-loop provider entry point; OK completes synchronously, AGAIN
     * promises later acknowledgement.
     */
    rotmos_storage_invoke_fn invoke;
    /** Owner-loop cancellation callback; AGAIN retains arguments until later
     * acknowledgement.
     */
    rotmos_storage_invoke_fn cancel;
    /** Final owner-loop retirement notification, after provider/native
     * acknowledgement.
     */
    rotmos_storage_provider_retire_fn retired;
    /** Terminal cleanup callback; retain its context until actual retirement.
     */
    rotmos_retire_fn close;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} rotmos_storage_provider_config;
/** Owner-loop consumer completion delivered later even for synchronous
 * providers. Suppressed after explicit cancellation; take asset/resource
 * results only through public transfer helpers.
 */
typedef void (*rotmos_storage_complete_fn)(rotmos_storage_operation *,
                                           const rotmos_storage_outcome *,
                                           void *);
/** Owner-loop timeout/shutdown notification before provider acknowledgement.
 * Actual committed/partial outcomes arrive at retirement; no rollback is
 * implied.
 */
typedef void (*rotmos_storage_interrupt_fn)(rotmos_storage_operation *,
                                            rotmos_status, void *);
/** Final owner-loop consumer retirement after provider/native acknowledgement;
 * result preserves actual commit/partial/recovery state.
 */
typedef void (*rotmos_storage_retire_fn)(const rotmos_storage_outcome *,
                                         void *);
/** Synchronous owner-loop check of each affected source/destination inside the
 * provider mutation scope. Set allowed; must not yield or run on a native
 * worker.
 */
typedef rotmos_status (*rotmos_storage_scope_fn)(rotmos_storage_operation *,
                                                 const char *path,
                                                 const char *destination,
                                                 int *allowed, void *,
                                                 rotmos_error *);
/** Configuration for storage operation. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct rotmos_storage_operation_config {
    /** Consumer completion callback; suppressed after explicit cancellation. */
    rotmos_storage_complete_fn completed;
    /** Timeout/shutdown notification; does not imply provider acknowledgement
     * or rollback.
     */
    rotmos_storage_interrupt_fn interrupted;
    /** Final owner-loop retirement notification, after provider/native
     * acknowledgement.
     */
    rotmos_storage_retire_fn retired;
    /** Synchronous owner-loop authorization for every affected physical
     * resource/destination.
     */
    rotmos_storage_scope_fn check_scope;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
    /** Operation deadline in milliseconds; zero inherits 30000 unless
     * timeout_disabled.
     */
    long timeout_ms;
    /** Nonzero disables the extra operation deadline. */
    int timeout_disabled;
} rotmos_storage_operation_config;
/** Configuration for storage resource. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct rotmos_storage_resource_config {
    /** Representation or event discriminator; inspect before interpreting other
     * fields.
     */
    rotmos_storage_resource_kind kind;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
    /** Terminal cleanup callback; retain its context until actual retirement.
     */
    rotmos_retire_fn close;
} rotmos_storage_resource_config;
/** Initialize storage provider config with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void
rotmos_storage_provider_config_init(rotmos_storage_provider_config *);
/** Initialize storage operation config with its documented defaults; call
 * before applying overrides.
 */
ROTMOS_API void
rotmos_storage_operation_config_init(rotmos_storage_operation_config *);
/** Initialize storage arguments with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void rotmos_storage_arguments_init(rotmos_storage_arguments *);
/** Initialize storage result with its documented defaults; call before applying
 * overrides.
 */
ROTMOS_API void rotmos_storage_result_init(rotmos_storage_result *);
/** Initialize storage resource config with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void
rotmos_storage_resource_config_init(rotmos_storage_resource_config *);
/** Return a static human-readable storage-domain status label. */
ROTMOS_API const char *rotmos_storage_code_string(rotmos_storage_code);
/** Inert declaration copy. Cancel is required: OK acknowledges cancellation,
 * AGAIN promises later acknowledgement through operation_complete. A cancel
 * error does not acknowledge or release pending arguments. Provider callbacks
 * run on one bound owner loop; resources may be closed on that thread after
 * the run. No reuse of bound providers across loop/run boundaries. */
ROTMOS_API rotmos_status
rotmos_storage_provider_new(const rotmos_storage_provider_config *,
                            rotmos_storage_provider **, rotmos_error *);
/** Read the provider's immutable capability flags. */
ROTMOS_API unsigned int
rotmos_storage_provider_capabilities(const rotmos_storage_provider *);
/** Borrow the provider's commit/durability description until provider close. */
ROTMOS_API const char *
rotmos_storage_provider_commit_description(const rotmos_storage_provider *);
/** Release the provider owner; pending operations/resources retain it until
 * retirement.
 */
ROTMOS_API rotmos_status
rotmos_storage_provider_close(rotmos_storage_provider *, rotmos_error *);
/** Active HTTP worker loop only (on_worker_start or later). Success accepts
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
/** Owner-loop only, including during invoke/cancel. Copies metadata and takes
 * asset/resource results only on success. Failure takes nothing and permits
 * retry. A successful completion acknowledges that no job still uses the
 * operation/arguments; completion occurs once. Native work publishes through
 * its owner-loop completion callback or a public waker, never directly. */
ROTMOS_API rotmos_status rotmos_storage_operation_complete(
    rotmos_storage_operation *, const rotmos_storage_result *, rotmos_error *);
/** Borrow the operation's owner loop through retirement. */
ROTMOS_API rotmos_loop *
rotmos_storage_operation_loop(const rotmos_storage_operation *);
/** Borrow the immutable copied operation arguments through provider
 * acknowledgement.
 */
ROTMOS_API const rotmos_storage_arguments *
rotmos_storage_operation_arguments(const rotmos_storage_operation *);
/** Read provider-associated operation state through retirement. */
ROTMOS_API void *
rotmos_storage_operation_userdata(const rotmos_storage_operation *);
/** Associate provider-owned state with an operation on its owner loop; does not
 * transfer allocation ownership.
 */
ROTMOS_API rotmos_status rotmos_storage_operation_set_userdata(
    rotmos_storage_operation *, void *, rotmos_error *);
/** Immutable query may be used by retained native work. A configured checker
 * requires CAN_SCOPE_CHECK before accepting a mutation. Scope-capable providers
 * check every affected physical descendant, mapped destination and overwritten
 * destination descendant inside the same atomic mutation scope. */
ROTMOS_API int
rotmos_storage_operation_has_scope_check(const rotmos_storage_operation *);
/** Owner-loop only. Invokes check_scope synchronously; never yields. Retained
 * native jobs must marshal checks to this loop, never invoke policy directly.
 * Cancellation suppresses policy callbacks and denies the check. */
ROTMOS_API rotmos_status
rotmos_storage_operation_check_scope(rotmos_storage_operation *, const char *,
                                     const char *, int *, rotmos_error *);
/** Cooperative native-thread check. Explicit cancellation revokes consumer
 * callbacks immediately. Timeout/shutdown may first deliver interrupted on the
 * owner loop. Retirement still waits for acknowledgement and preserves actual
 * committed/partial/recovery results. No rollback is implied by cancellation.
 */
ROTMOS_API int
rotmos_storage_operation_cancelled(const rotmos_storage_operation *);
/** Revoke consumer delivery and request provider cancellation; actual
 * retirement still waits for acknowledgement.
 */
ROTMOS_API rotmos_status
rotmos_storage_operation_cancel(rotmos_storage_operation *, rotmos_error *);
/** Transfer a completed opened asset to its consumer once; only succeeds for an
 * available asset result.
 */
ROTMOS_API rotmos_status rotmos_storage_operation_take_asset(
    rotmos_storage_operation *, rotmos_asset **, rotmos_error *);
/** Transfer a completed provider resource to its consumer once. */
ROTMOS_API rotmos_status rotmos_storage_operation_take_resource(
    rotmos_storage_operation *, rotmos_storage_resource **, rotmos_error *);
/** Provider-owned resource constructors run on the operation's loop. An open
 * operation retains its argument resource after its caller closes that owner.
 * Returned resources transfer through operation_complete/take_resource. */
ROTMOS_API rotmos_status rotmos_storage_resource_new(
    rotmos_storage_operation *, const rotmos_storage_resource_config *,
    rotmos_storage_resource **, rotmos_error *);
/** Read caller-supplied resource state while the resource remains alive. */
ROTMOS_API void *
rotmos_storage_resource_userdata(const rotmos_storage_resource *);
/** Inspect whether the resource is a listing cursor or write transaction. */
ROTMOS_API rotmos_storage_resource_kind
rotmos_storage_resource_type(const rotmos_storage_resource *);
/** Release the resource owner; operations already using it retain it through
 * acknowledgement.
 */
ROTMOS_API rotmos_status
rotmos_storage_resource_close(rotmos_storage_resource *, rotmos_error *);
/** Read-only adapter over a borrowed asset provider. No new packed format or
 * Vectis object is introduced. Caller retains the asset provider through close.
 */
ROTMOS_API rotmos_status rotmos_storage_provider_assets(
    rotmos_asset_provider *, rotmos_storage_provider **, rotmos_error *);
/** Configuration for storage filesystem. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct rotmos_storage_filesystem_config {
    /** Absolute managed-cache base directory. */
    const char *cache_dir;
    /** Managed WebDAV site identifier; selects the existing per-site
     * content/tombstone layout.
     */
    const char *site_id;
    /** Filesystem serving/storage root; confinement and direct-root policy
     * follow the owning operation.
     */
    const char *root_dir;
    /** Per-file quota; zero selects 16 MiB. */
    size_t max_file_bytes;
    /** Total stored-file byte quota; zero selects 128 MiB. */
    size_t max_total_bytes;
    /** Resource-count quota; zero selects 4096. */
    size_t max_resources;
} rotmos_storage_filesystem_config;
/** Initialize storage filesystem config with its documented defaults; call
 * before applying overrides.
 */
ROTMOS_API void
rotmos_storage_filesystem_config_init(rotmos_storage_filesystem_config *);
/** Pure path calculation for host-owned asset extraction/hydration. Does not
 * create directories; output is cleared on failure. */
ROTMOS_API rotmos_status rotmos_storage_filesystem_content_directory(
    const rotmos_storage_filesystem_config *, char *, size_t, rotmos_error *);
/** Inert declaration over a borrowed server. Filesystem providers share one
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
/** Maximum normalized standalone resource path bytes, excluding the trailing
 * NUL.
 */
#define ROTMOS_FILESYSTEM_PATH_MAX 1024u
/** Standalone resolved storage-path buffer size, including trailing NUL. */
#define ROTMOS_FILESYSTEM_STORAGE_PATH_MAX 4096u
/** Values for filesystem entry kind. */
typedef enum rotmos_filesystem_entry_kind {
    /** Standalone resource kind: none. */
    ROTMOS_FILESYSTEM_ENTRY_NONE = 0,
    /** Standalone resource kind: file. */
    ROTMOS_FILESYSTEM_ENTRY_FILE,
    /** Standalone resource kind: tombstone. */
    ROTMOS_FILESYSTEM_ENTRY_TOMBSTONE,
    /** Standalone resource kind: collection. */
    ROTMOS_FILESYSTEM_ENTRY_COLLECTION
} rotmos_filesystem_entry_kind;
/** Caller-owned standalone lookup/read result. lookup leaves etag empty; read
 * supplies an unquoted SHA-256 digest.
 */
typedef struct rotmos_filesystem_entry {
    /** Representation or event discriminator; inspect before interpreting other
     * fields.
     */
    rotmos_filesystem_entry_kind kind;
    /** Resource length in bytes. */
    size_t size;
    /** Resolved standalone filesystem path in caller-owned entry storage. */
    char storage_path[ROTMOS_FILESYSTEM_STORAGE_PATH_MAX];
    /** Entity tag; encoding/quoting follows the enclosing asset or standalone-
     * storage contract.
     */
    char etag[65];
} rotmos_filesystem_entry;
/** Synchronous immediate-child callback; name is borrowed for the call. Nonzero
 * continues, zero stops successfully.
 */
typedef int (*rotmos_filesystem_list_fn)(const char *,
                                         rotmos_filesystem_entry_kind, size_t,
                                         void *);
/** Normalize a standalone resource path into caller storage; return nonzero on
 * success, zero on invalid input.
 */
ROTMOS_API int rotmos_storage_filesystem_path_normalize(
    const char *path, char out[ROTMOS_FILESYSTEM_PATH_MAX + 1]);
/** Synchronously inspect a resource; does not read content or compute a file
 * ETag.
 */
ROTMOS_API rotmos_storage_code rotmos_storage_filesystem_lookup(
    const rotmos_storage_filesystem_config *config, const char *path,
    rotmos_filesystem_entry *entry);
/** Synchronously return malloc-owned file bytes and its SHA-256 ETag; free the
 * bytes, including after saving metadata.
 */
ROTMOS_API rotmos_storage_code rotmos_storage_filesystem_read(
    const rotmos_storage_filesystem_config *config, const char *path,
    unsigned char **body, size_t *size, rotmos_filesystem_entry *entry);
/** Synchronously write supplied bytes with conditions and quota accounting
 * under the shared mutation lock.
 */
ROTMOS_API rotmos_storage_code
rotmos_storage_filesystem_put(const rotmos_storage_filesystem_config *config,
                              const char *path, const void *body, size_t size,
                              const char *if_match, const char *if_none_match);
/** Synchronously delete a resource under conditions, preserving managed-store
 * tombstone semantics.
 */
ROTMOS_API rotmos_storage_code rotmos_storage_filesystem_delete(
    const rotmos_storage_filesystem_config *config, const char *path,
    const char *if_match, const char *if_none_match);
/** Synchronously create a collection under conditions and resource quotas. */
ROTMOS_API rotmos_storage_code rotmos_storage_filesystem_mkcol(
    const rotmos_storage_filesystem_config *config, const char *path,
    const char *if_match, const char *if_none_match);
/** Synchronously copy with overwrite/conditions; depth is 0 or -1. Shares the
 * HTTP storage engine.
 */
ROTMOS_API rotmos_storage_code rotmos_storage_filesystem_copy(
    const rotmos_storage_filesystem_config *config, const char *source,
    const char *destination, int overwrite, int depth, const char *if_match,
    const char *if_none_match);
/** Synchronously move the whole resource with overwrite/conditions; partial
 * recovery semantics match the HTTP engine.
 */
ROTMOS_API rotmos_storage_code rotmos_storage_filesystem_move(
    const rotmos_storage_filesystem_config *config, const char *source,
    const char *destination, int overwrite, const char *if_match,
    const char *if_none_match);
/** Synchronously enumerate immediate children; callback zero stops
 * successfully. No file content read is required.
 */
ROTMOS_API rotmos_storage_code rotmos_storage_filesystem_list(
    const rotmos_storage_filesystem_config *config, const char *path,
    rotmos_filesystem_list_fn callback, void *userdata);
#ifdef __cplusplus
}
#endif
#endif

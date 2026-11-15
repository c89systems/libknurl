#ifndef KNURL_STORAGE_H
#define KNURL_STORAGE_H
#include <knurl/asset.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Owned storage provider bound to one worker loop/run after first use. */
typedef struct knurl_storage_provider knurl_storage_provider;
/** Acknowledged storage operation; borrowed until actual retirement. */
typedef struct knurl_storage_operation knurl_storage_operation;
/** Provider resource retained by every in-flight operation using it. */
typedef struct knurl_storage_resource knurl_storage_resource;
/** Values for storage code. */
typedef enum knurl_storage_code {
    /** Storage domain result/action: ok. */
    KNURL_STORAGE_OK = 0,
    /** Storage domain result/action: not found. */
    KNURL_STORAGE_NOT_FOUND,
    /** Storage domain result/action: exists. */
    KNURL_STORAGE_EXISTS,
    /** Storage domain result/action: invalid. */
    KNURL_STORAGE_INVALID,
    /** Public bound for storage limit, in bytes or entries as named. */
    KNURL_STORAGE_LIMIT,
    /** Storage domain result/action: io. */
    KNURL_STORAGE_IO,
    /** Storage domain result/action: nomem. */
    KNURL_STORAGE_NOMEM,
    /** Storage domain result/action: conflict. */
    KNURL_STORAGE_CONFLICT,
    /** Storage domain result/action: tombstoned. */
    KNURL_STORAGE_TOMBSTONED,
    /** Storage domain result/action: precondition. */
    KNURL_STORAGE_PRECONDITION,
    /** Storage domain result/action: not supported. */
    KNURL_STORAGE_NOT_SUPPORTED,
    /** Storage domain result/action: end. */
    KNURL_STORAGE_END,
    /** Storage domain result/action: denied. */
    KNURL_STORAGE_DENIED
} knurl_storage_code;
/** Values for storage action. */
typedef enum knurl_storage_action {
    /** Storage domain result/action: stat. */
    KNURL_STORAGE_STAT = 0,
    /** Storage domain result/action: open read. */
    KNURL_STORAGE_OPEN_READ,
    /** Storage domain result/action: list open. */
    KNURL_STORAGE_LIST_OPEN,
    /** Storage domain result/action: list next. */
    KNURL_STORAGE_LIST_NEXT,
    /** Storage domain result/action: put begin. */
    KNURL_STORAGE_PUT_BEGIN,
    /** Storage domain result/action: put write. */
    KNURL_STORAGE_PUT_WRITE,
    /** Storage domain result/action: put finish. */
    KNURL_STORAGE_PUT_FINISH,
    /** Storage domain result/action: put abort. */
    KNURL_STORAGE_PUT_ABORT,
    /** Storage domain result/action: delete. */
    KNURL_STORAGE_DELETE,
    /** Storage domain result/action: mkcol. */
    KNURL_STORAGE_MKCOL,
    /** Storage domain result/action: copy. */
    KNURL_STORAGE_COPY,
    /** Storage domain result/action: move. */
    KNURL_STORAGE_MOVE
} knurl_storage_action;
/** Values for storage resource kind. */
typedef enum knurl_storage_resource_kind {
    /** Storage domain result/action: list. */
    KNURL_STORAGE_LIST = 0,
    /** Storage domain result/action: write. */
    KNURL_STORAGE_WRITE = 1
} knurl_storage_resource_kind;
/** Provider capability bit: write. */
#define KNURL_STORAGE_CAN_WRITE 1u
/** Provider capability bit: conditional. */
#define KNURL_STORAGE_CAN_CONDITIONAL 2u
/** Provider capability bit: copy. */
#define KNURL_STORAGE_CAN_COPY 4u
/** Provider capability bit: move. */
#define KNURL_STORAGE_CAN_MOVE 8u
/** Provider guarantees source/destination atomicity for cross-resource
 * mutations.
 */
#define KNURL_STORAGE_ATOMIC_CROSS_RESOURCE 16u
/** Provider advertises durable commit semantics described by
 * commit_description.
 */
#define KNURL_STORAGE_DURABLE_COMMIT 32u
/** Provider checks every affected descendant/destination inside the mutation
 * scope.
 */
#define KNURL_STORAGE_CAN_SCOPE_CHECK 64u
/** Maximum copied storage operation chunk size in bytes. */
#define KNURL_STORAGE_CHUNK_MAX 65536u
/** Maximum copied storage conditional-header size in bytes. */
#define KNURL_STORAGE_CONDITION_MAX 65536u
/** One storage command; operation_start copies argument text/chunks and retains
 * resources through provider acknowledgement.
 */
typedef struct knurl_storage_arguments {
    /** Requested operation or authorization decision discriminator. */
    knurl_storage_action action;
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
    knurl_storage_resource *resource;
    /** Copied bounded operation payload, or TLS bytes copied by the server
     * declaration.
     */
    knurl_bytes bytes;
    /** Nonzero permits replacement of the COPY/MOVE destination. */
    int overwrite;
    /** Collection COPY depth: 0 or -1; WebDAV PROPFIND supports finite 0/1. */
    int depth;
    /** Nonzero requires provider atomicity across source/destination resources.
     */
    int atomic_cross_resource_required;
} knurl_storage_arguments;
/** Storage-domain outcome with explicit asset/resource transfer,
 * commit/partial/recovery state and optional metadata.
 */
typedef struct knurl_storage_result {
    /** Storage-domain result, distinct from transport/cancellation status. */
    knurl_storage_code code;
    /** Copied bounded diagnostic; OK/NONE when no error is attached. */
    knurl_error error;
    /** Opened-resource metadata; valid only when the containing result supplies
     * metadata.
     */
    knurl_asset_info info;
    /** One immediate listing child when has_entry is nonzero. */
    knurl_asset_entry entry;
    /** Opened asset offered for successful ownership transfer. */
    knurl_asset *asset;
    /** Provider listing/write resource, retained while referenced by an
     * operation.
     */
    knurl_storage_resource *resource;
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
} knurl_storage_result;
/** Terminal operation status plus provider result; interruption/cancellation
 * never implies rollback of a committed mutation.
 */
typedef struct knurl_storage_outcome {
    /** Portable terminal status; OK indicates successful completion. */
    knurl_status cause;
    /** Borrowed storage result retaining actual committed/partial outcomes
     * through retirement.
     */
    const knurl_storage_result *result;
    /** Nonzero for cancellation; does not erase actual commit/partial outcomes.
     */
    int cancelled;
} knurl_storage_outcome;
/** Owner-loop provider invoke/cancel entry. OK acknowledges synchronously;
 * AGAIN promises later operation_complete. A cancellation error does not
 * release arguments or acknowledge native work.
 */
typedef knurl_status (*knurl_storage_invoke_fn)(knurl_storage_operation *,
                                                const knurl_storage_arguments *,
                                                void *, knurl_error *);
/** Provider-side final retirement after acknowledgement; operation/result are
 * borrowed for this call and no job may still use their arguments.
 */
typedef void (*knurl_storage_provider_retire_fn)(knurl_storage_operation *,
                                                 const knurl_storage_outcome *,
                                                 void *);
/** Configuration for storage provider. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct knurl_storage_provider_config {
    /** Bit mask of provider-supported storage guarantees and actions. */
    unsigned int capabilities;
    /** Copied provider durability/commit description. */
    const char *commit_description;
    /** Owner-loop provider entry point; OK completes synchronously, AGAIN
     * promises later acknowledgement.
     */
    knurl_storage_invoke_fn invoke;
    /** Owner-loop cancellation callback; AGAIN retains arguments until later
     * acknowledgement.
     */
    knurl_storage_invoke_fn cancel;
    /** Final owner-loop retirement notification, after provider/native
     * acknowledgement.
     */
    knurl_storage_provider_retire_fn retired;
    /** Terminal cleanup callback; retain its context until actual retirement.
     */
    knurl_retire_fn close;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} knurl_storage_provider_config;
/** Owner-loop consumer completion delivered later even for synchronous
 * providers. Suppressed after explicit cancellation; take asset/resource
 * results only through public transfer helpers.
 */
typedef void (*knurl_storage_complete_fn)(knurl_storage_operation *,
                                          const knurl_storage_outcome *,
                                          void *);
/** Owner-loop timeout/shutdown notification before provider acknowledgement.
 * Actual committed/partial outcomes arrive at retirement; no rollback is
 * implied.
 */
typedef void (*knurl_storage_interrupt_fn)(knurl_storage_operation *,
                                           knurl_status, void *);
/** Final owner-loop consumer retirement after provider/native acknowledgement;
 * result preserves actual commit/partial/recovery state.
 */
typedef void (*knurl_storage_retire_fn)(const knurl_storage_outcome *, void *);
/** Synchronous owner-loop check of each affected source/destination inside the
 * provider mutation scope. Set allowed; must not yield or run on a native
 * worker.
 */
typedef knurl_status (*knurl_storage_scope_fn)(knurl_storage_operation *,
                                               const char *path,
                                               const char *destination,
                                               int *allowed, void *,
                                               knurl_error *);
/** Configuration for storage operation. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct knurl_storage_operation_config {
    /** Consumer completion callback; suppressed after explicit cancellation. */
    knurl_storage_complete_fn completed;
    /** Timeout/shutdown notification; does not imply provider acknowledgement
     * or rollback.
     */
    knurl_storage_interrupt_fn interrupted;
    /** Final owner-loop retirement notification, after provider/native
     * acknowledgement.
     */
    knurl_storage_retire_fn retired;
    /** Synchronous owner-loop authorization for every affected physical
     * resource/destination.
     */
    knurl_storage_scope_fn check_scope;
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
} knurl_storage_operation_config;
/** Configuration for storage resource. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct knurl_storage_resource_config {
    /** Representation or event discriminator; inspect before interpreting other
     * fields.
     */
    knurl_storage_resource_kind kind;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
    /** Terminal cleanup callback; retain its context until actual retirement.
     */
    knurl_retire_fn close;
} knurl_storage_resource_config;
/** Initialize storage provider config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void
knurl_storage_provider_config_init(knurl_storage_provider_config *);
/** Initialize storage operation config with its documented defaults; call
 * before applying overrides.
 */
KNURL_API void
knurl_storage_operation_config_init(knurl_storage_operation_config *);
/** Initialize storage arguments with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_storage_arguments_init(knurl_storage_arguments *);
/** Initialize storage result with its documented defaults; call before applying
 * overrides.
 */
KNURL_API void knurl_storage_result_init(knurl_storage_result *);
/** Initialize storage resource config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void
knurl_storage_resource_config_init(knurl_storage_resource_config *);
/** Return a static human-readable storage-domain status label. */
KNURL_API const char *knurl_storage_code_string(knurl_storage_code);
/** Inert declaration copy. Cancel is required: OK acknowledges cancellation,
 * AGAIN promises later acknowledgement through operation_complete. A cancel
 * error does not acknowledge or release pending arguments. Provider callbacks
 * run on one bound owner loop; resources may be closed on that thread after
 * the run. No reuse of bound providers across loop/run boundaries. */
KNURL_API knurl_status
knurl_storage_provider_new(const knurl_storage_provider_config *,
                           knurl_storage_provider **, knurl_error *);
/** Read the provider's immutable capability flags. */
KNURL_API unsigned int
knurl_storage_provider_capabilities(const knurl_storage_provider *);
/** Borrow the provider's commit/durability description until provider close. */
KNURL_API const char *
knurl_storage_provider_commit_description(const knurl_storage_provider *);
/** Release the provider owner; pending operations/resources retain it until
 * retirement.
 */
KNURL_API knurl_status knurl_storage_provider_close(knurl_storage_provider *,
                                                    knurl_error *);
/** Active HTTP worker loop only (on_worker_start or later). Success accepts
 * one operation and owns one terminal retirement, even when
 * invoke completes synchronously or reports a domain/protocol failure. Bad
 * declarations/capabilities/allocation failures before invoke take nothing.
 * Provider invoke returns OK synchronously or AGAIN with responsibility for
 * later completion. Staged results and callbacks are always delivered later.
 * Arguments/chunks are copied and pinned through provider acknowledgement.
 * Timeout zero selects 30000 ms, including queue/lock wait. */
KNURL_API knurl_status knurl_storage_operation_start(
    knurl_storage_provider *, knurl_loop *, const knurl_storage_arguments *,
    const knurl_storage_operation_config *, knurl_storage_operation **,
    knurl_error *);
/** Owner-loop only, including during invoke/cancel. Copies metadata and takes
 * asset/resource results only on success. Failure takes nothing and permits
 * retry. A successful completion acknowledges that no job still uses the
 * operation/arguments; completion occurs once. Native work publishes through
 * its owner-loop completion callback or a public waker, never directly. */
KNURL_API knurl_status knurl_storage_operation_complete(
    knurl_storage_operation *, const knurl_storage_result *, knurl_error *);
/** Borrow the operation's owner loop through retirement. */
KNURL_API knurl_loop *
knurl_storage_operation_loop(const knurl_storage_operation *);
/** Borrow the immutable copied operation arguments through provider
 * acknowledgement.
 */
KNURL_API const knurl_storage_arguments *
knurl_storage_operation_arguments(const knurl_storage_operation *);
/** Read provider-associated operation state through retirement. */
KNURL_API void *
knurl_storage_operation_userdata(const knurl_storage_operation *);
/** Associate provider-owned state with an operation on its owner loop; does not
 * transfer allocation ownership.
 */
KNURL_API knurl_status knurl_storage_operation_set_userdata(
    knurl_storage_operation *, void *, knurl_error *);
/** Immutable query may be used by retained native work. A configured checker
 * requires CAN_SCOPE_CHECK before accepting a mutation. Scope-capable providers
 * check every affected physical descendant, mapped destination and overwritten
 * destination descendant inside the same atomic mutation scope. */
KNURL_API int
knurl_storage_operation_has_scope_check(const knurl_storage_operation *);
/** Owner-loop only. Invokes check_scope synchronously; never yields. Retained
 * native jobs must marshal checks to this loop, never invoke policy directly.
 * Cancellation suppresses policy callbacks and denies the check. */
KNURL_API knurl_status
knurl_storage_operation_check_scope(knurl_storage_operation *, const char *,
                                    const char *, int *, knurl_error *);
/** Cooperative native-thread check. Explicit cancellation revokes consumer
 * callbacks immediately. Timeout/shutdown may first deliver interrupted on the
 * owner loop. Retirement still waits for acknowledgement and preserves actual
 * committed/partial/recovery results. No rollback is implied by cancellation.
 */
KNURL_API int
knurl_storage_operation_cancelled(const knurl_storage_operation *);
/** Revoke consumer delivery and request provider cancellation; actual
 * retirement still waits for acknowledgement.
 */
KNURL_API knurl_status knurl_storage_operation_cancel(knurl_storage_operation *,
                                                      knurl_error *);
/** Transfer a completed opened asset to its consumer once; only succeeds for an
 * available asset result.
 */
KNURL_API knurl_status knurl_storage_operation_take_asset(
    knurl_storage_operation *, knurl_asset **, knurl_error *);
/** Transfer a completed provider resource to its consumer once. */
KNURL_API knurl_status knurl_storage_operation_take_resource(
    knurl_storage_operation *, knurl_storage_resource **, knurl_error *);
/** Provider-owned resource constructors run on the operation's loop. An open
 * operation retains its argument resource after its caller closes that owner.
 * Returned resources transfer through operation_complete/take_resource. */
KNURL_API knurl_status knurl_storage_resource_new(
    knurl_storage_operation *, const knurl_storage_resource_config *,
    knurl_storage_resource **, knurl_error *);
/** Read caller-supplied resource state while the resource remains alive. */
KNURL_API void *knurl_storage_resource_userdata(const knurl_storage_resource *);
/** Inspect whether the resource is a listing cursor or write transaction. */
KNURL_API knurl_storage_resource_kind
knurl_storage_resource_type(const knurl_storage_resource *);
/** Release the resource owner; operations already using it retain it through
 * acknowledgement.
 */
KNURL_API knurl_status knurl_storage_resource_close(knurl_storage_resource *,
                                                    knurl_error *);
/** Read-only adapter over a borrowed asset provider. No new packed format or
 * Vectis object is introduced. Caller retains the asset provider through close.
 */
KNURL_API knurl_status knurl_storage_provider_assets(knurl_asset_provider *,
                                                     knurl_storage_provider **,
                                                     knurl_error *);
/** Configuration for storage filesystem. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct knurl_storage_filesystem_config {
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
    /** Per-file quota. Provider construction resolves zero to 16 MiB;
     * standalone filesystem calls require a positive value.
     */
    size_t max_file_bytes;
    /** Total stored-file byte quota. Provider construction resolves zero to
     * 128 MiB; standalone filesystem calls require a positive value.
     */
    size_t max_total_bytes;
    /** Resource-count quota. Provider construction resolves zero to 4096;
     * standalone filesystem calls require a positive value.
     */
    size_t max_resources;
} knurl_storage_filesystem_config;
/** Initialize quotas to 16 MiB/file, 128 MiB total and 4096 resources. Call
 * before standalone overrides: those calls preserve quota values and reject
 * zero instead of inheriting provider-construction defaults.
 */
KNURL_API void
knurl_storage_filesystem_config_init(knurl_storage_filesystem_config *);
/** Pure path calculation for host-owned asset extraction/hydration. Does not
 * create directories; output is cleared on failure. */
KNURL_API knurl_status knurl_storage_filesystem_content_directory(
    const knurl_storage_filesystem_config *, char *, size_t, knurl_error *);
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
KNURL_API knurl_status knurl_storage_filesystem_new(
    knurl_server *, const knurl_storage_filesystem_config *,
    knurl_storage_provider **, knurl_error *);
/** Maximum normalized standalone resource path bytes, excluding the trailing
 * NUL.
 */
#define KNURL_FILESYSTEM_PATH_MAX 1024u
/** Standalone resolved storage-path buffer size, including trailing NUL. */
#define KNURL_FILESYSTEM_STORAGE_PATH_MAX 4096u
/** Values for filesystem entry kind. */
typedef enum knurl_filesystem_entry_kind {
    /** Standalone resource kind: none. */
    KNURL_FILESYSTEM_ENTRY_NONE = 0,
    /** Standalone resource kind: file. */
    KNURL_FILESYSTEM_ENTRY_FILE,
    /** Standalone resource kind: tombstone. */
    KNURL_FILESYSTEM_ENTRY_TOMBSTONE,
    /** Standalone resource kind: collection. */
    KNURL_FILESYSTEM_ENTRY_COLLECTION
} knurl_filesystem_entry_kind;
/** Caller-owned standalone lookup/read result. lookup leaves etag empty; read
 * supplies an unquoted SHA-256 digest.
 */
typedef struct knurl_filesystem_entry {
    /** Representation or event discriminator; inspect before interpreting other
     * fields.
     */
    knurl_filesystem_entry_kind kind;
    /** Resource length in bytes. */
    size_t size;
    /** Resolved standalone filesystem path in caller-owned entry storage. */
    char storage_path[KNURL_FILESYSTEM_STORAGE_PATH_MAX];
    /** Entity tag; encoding/quoting follows the enclosing asset or standalone-
     * storage contract.
     */
    char etag[65];
} knurl_filesystem_entry;
/** Synchronous immediate-child callback; name is borrowed for the call. Nonzero
 * continues, zero stops successfully.
 */
typedef int (*knurl_filesystem_list_fn)(const char *,
                                        knurl_filesystem_entry_kind, size_t,
                                        void *);
/** Normalize a standalone resource path into caller storage; return nonzero on
 * success, zero on invalid input.
 */
KNURL_API int knurl_storage_filesystem_path_normalize(
    const char *path, char out[KNURL_FILESYSTEM_PATH_MAX + 1]);
/** Synchronously inspect a resource; does not read content or compute a file
 * ETag.
 */
KNURL_API knurl_storage_code knurl_storage_filesystem_lookup(
    const knurl_storage_filesystem_config *config, const char *path,
    knurl_filesystem_entry *entry);
/** Synchronously return malloc-owned file bytes and its SHA-256 ETag; free the
 * bytes, including after saving metadata.
 */
KNURL_API knurl_storage_code knurl_storage_filesystem_read(
    const knurl_storage_filesystem_config *config, const char *path,
    unsigned char **body, size_t *size, knurl_filesystem_entry *entry);
/** Synchronously write supplied bytes with conditions and quota accounting
 * under the shared mutation lock.
 */
KNURL_API knurl_storage_code
knurl_storage_filesystem_put(const knurl_storage_filesystem_config *config,
                             const char *path, const void *body, size_t size,
                             const char *if_match, const char *if_none_match);
/** Synchronously delete a resource under conditions, preserving managed-store
 * tombstone semantics.
 */
KNURL_API knurl_storage_code knurl_storage_filesystem_delete(
    const knurl_storage_filesystem_config *config, const char *path,
    const char *if_match, const char *if_none_match);
/** Synchronously create a collection under conditions and resource quotas. */
KNURL_API knurl_storage_code knurl_storage_filesystem_mkcol(
    const knurl_storage_filesystem_config *config, const char *path,
    const char *if_match, const char *if_none_match);
/** Synchronously copy with overwrite/conditions; depth is 0 or -1. Shares the
 * HTTP storage engine.
 */
KNURL_API knurl_storage_code knurl_storage_filesystem_copy(
    const knurl_storage_filesystem_config *config, const char *source,
    const char *destination, int overwrite, int depth, const char *if_match,
    const char *if_none_match);
/** Synchronously move the whole resource with overwrite/conditions; partial
 * recovery semantics match the HTTP engine.
 */
KNURL_API knurl_storage_code knurl_storage_filesystem_move(
    const knurl_storage_filesystem_config *config, const char *source,
    const char *destination, int overwrite, const char *if_match,
    const char *if_none_match);
/** Synchronously enumerate immediate children; callback zero stops
 * successfully. No file content read is required.
 */
KNURL_API knurl_storage_code knurl_storage_filesystem_list(
    const knurl_storage_filesystem_config *config, const char *path,
    knurl_filesystem_list_fn callback, void *userdata);

struct knurl_error;
struct knurl_loop;
struct knurl_storage_arguments;
struct knurl_storage_operation;
struct knurl_storage_operation_config;
/** Library-owned storage provider receiver. Pass this handle as the first
 * argument to its methods. Obtain it through the documented constructor or
 * borrowed accessor. Do not copy, initialize or modify the shell or impl.
 */
struct knurl_storage_provider {
    /** Read the provider's immutable capability flags. */
    unsigned int (*capabilities)(const struct knurl_storage_provider *);
    /** Borrow the provider's commit/durability description until provider
     * close. */
    const char *(*commit_description)(const struct knurl_storage_provider *);
    /** Release the provider owner; pending operations/resources retain it until
     * retirement.
     */
    knurl_status (*close)(struct knurl_storage_provider *,
                          struct knurl_error *);
    /** Active HTTP worker loop only (on_worker_start or later). Success accepts
     * one operation and owns one terminal retirement, even when
     * invoke completes synchronously or reports a domain/protocol failure. Bad
     * declarations/capabilities/allocation failures before invoke take nothing.
     * Provider invoke returns OK synchronously or AGAIN with responsibility for
     * later completion. Staged results and callbacks are always delivered
     * later. Arguments/chunks are copied and pinned through provider
     * acknowledgement. Timeout zero selects 30000 ms, including queue/lock
     * wait. */
    knurl_status (*start)(struct knurl_storage_provider *, struct knurl_loop *,
                          const struct knurl_storage_arguments *,
                          const struct knurl_storage_operation_config *,
                          struct knurl_storage_operation **,
                          struct knurl_error *);
    /** Private library state; applications must not access it. */
    struct knurl_storage_provider_impl *impl;
};

struct knurl_asset;
struct knurl_error;
struct knurl_loop;
struct knurl_storage_arguments;
struct knurl_storage_resource;
struct knurl_storage_resource_config;
struct knurl_storage_result;
/** Library-owned storage operation receiver. Pass this handle as the first
 * argument to its methods. Obtain it through the documented constructor or
 * borrowed accessor. Do not copy, initialize or modify the shell or impl.
 */
struct knurl_storage_operation {
    /** Owner-loop only, including during invoke/cancel. Copies metadata and
     * takes asset/resource results only on success. Failure takes nothing and
     * permits retry. A successful completion acknowledges that no job still
     * uses the operation/arguments; completion occurs once. Native work
     * publishes through its owner-loop completion callback or a public waker,
     * never directly. */
    knurl_status (*complete)(struct knurl_storage_operation *,
                             const struct knurl_storage_result *,
                             struct knurl_error *);
    /** Borrow the operation's owner loop through retirement. */
    struct knurl_loop *(*loop)(const struct knurl_storage_operation *);
    /** Borrow the immutable copied operation arguments through provider
     * acknowledgement.
     */
    const struct knurl_storage_arguments *(*arguments)(
        const struct knurl_storage_operation *);
    /** Read provider-associated operation state through retirement. */
    void *(*userdata)(const struct knurl_storage_operation *);
    /** Associate provider-owned state with an operation on its owner loop; does
     * not transfer allocation ownership.
     */
    knurl_status (*set_userdata)(struct knurl_storage_operation *, void *,
                                 struct knurl_error *);
    /** Immutable query may be used by retained native work. A configured
     * checker requires CAN_SCOPE_CHECK before accepting a mutation.
     * Scope-capable providers check every affected physical descendant, mapped
     * destination and overwritten destination descendant inside the same atomic
     * mutation scope. */
    int (*has_scope_check)(const struct knurl_storage_operation *);
    /** Owner-loop only. Invokes check_scope synchronously; never yields.
     * Retained native jobs must marshal checks to this loop, never invoke
     * policy directly. Cancellation suppresses policy callbacks and denies the
     * check. */
    knurl_status (*check_scope)(struct knurl_storage_operation *, const char *,
                                const char *, int *, struct knurl_error *);
    /** Cooperative native-thread check. Explicit cancellation revokes consumer
     * callbacks immediately. Timeout/shutdown may first deliver interrupted on
     * the owner loop. Retirement still waits for acknowledgement and preserves
     * actual committed/partial/recovery results. No rollback is implied by
     * cancellation.
     */
    int (*cancelled)(const struct knurl_storage_operation *);
    /** Revoke consumer delivery and request provider cancellation; actual
     * retirement still waits for acknowledgement.
     */
    knurl_status (*cancel)(struct knurl_storage_operation *,
                           struct knurl_error *);
    /** Transfer a completed opened asset to its consumer once; only succeeds
     * for an available asset result.
     */
    knurl_status (*take_asset)(struct knurl_storage_operation *,
                               struct knurl_asset **, struct knurl_error *);
    /** Transfer a completed provider resource to its consumer once. */
    knurl_status (*take_resource)(struct knurl_storage_operation *,
                                  struct knurl_storage_resource **,
                                  struct knurl_error *);
    /** Provider-owned resource constructors run on the operation's loop. An
     * open operation retains its argument resource after its caller closes that
     * owner. Returned resources transfer through
     * operation_complete/take_resource. */
    knurl_status (*resource_new)(struct knurl_storage_operation *,
                                 const struct knurl_storage_resource_config *,
                                 struct knurl_storage_resource **,
                                 struct knurl_error *);
    /** Private library state; applications must not access it. */
    struct knurl_storage_operation_impl *impl;
};

struct knurl_error;
/** Library-owned storage resource receiver. Pass this handle as the first
 * argument to its methods. Obtain it through the documented constructor or
 * borrowed accessor. Do not copy, initialize or modify the shell or impl.
 */
struct knurl_storage_resource {
    /** Read caller-supplied resource state while the resource remains alive. */
    void *(*userdata)(const struct knurl_storage_resource *);
    /** Inspect whether the resource is a listing cursor or write transaction.
     */
    knurl_storage_resource_kind (*type)(const struct knurl_storage_resource *);
    /** Release the resource owner; operations already using it retain it
     * through acknowledgement.
     */
    knurl_status (*close)(struct knurl_storage_resource *,
                          struct knurl_error *);
    /** Private library state; applications must not access it. */
    struct knurl_storage_resource_impl *impl;
};

#ifdef __cplusplus
}
#endif
#endif

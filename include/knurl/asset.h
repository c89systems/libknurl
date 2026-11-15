#ifndef KNURL_ASSET_H
#define KNURL_ASSET_H
#include <knurl/io.h>
#include <knurl/loop.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Maximum normalized asset path size in bytes. */
#define KNURL_ASSET_PATH_MAX 4096u
/** Maximum asset entity-tag metadata storage in bytes. */
#define KNURL_ASSET_ETAG_MAX 256u
/** Maximum asset Content-Type metadata storage in bytes. */
#define KNURL_ASSET_TYPE_MAX 256u
/** Maximum asset HTTP-date metadata storage in bytes. */
#define KNURL_ASSET_DATE_MAX 64u
/** Owned opened read-only resource with immutable copied metadata. */
typedef struct knurl_asset knurl_asset;
/** Owned asset-opening provider; open assets retain it through close. */
typedef struct knurl_asset_provider knurl_asset_provider;
/** Values for asset kind. */
typedef enum knurl_asset_kind {
    /** Read-only resource kind: file. */
    KNURL_ASSET_FILE = 0,
    /** Read-only resource kind: directory. */
    KNURL_ASSET_DIRECTORY = 1
} knurl_asset_kind;
/** Opened immutable asset metadata copied by asset construction; string views
 * expire at asset close.
 */
typedef struct knurl_asset_info {
    /** Representation or event discriminator; inspect before interpreting other
     * fields.
     */
    knurl_asset_kind kind;
    /** Resource length in bytes. */
    knurl_u64 size;
    /** Entity tag; encoding/quoting follows the enclosing asset or standalone-
     * storage contract.
     */
    const char *etag;
    /** Optional RFC 9110 Content-Type, including valid parameters. Malformed
     * syntax is INVALID; NULL permits static extension inference.
     */
    const char *content_type;
    /** HTTP Last-Modified timestamp text, when available. */
    const char *last_modified;
    /** Nonzero asserts Last-Modified identifies this representation strongly
     * enough for date If-Range. Zero never promotes timestamp resolution alone.
     */
    int last_modified_strong;
    /** Optional precise Unix modification timestamp for sidecar freshness.
     * has_modification_time=0 leaves both values unspecified. */
    knurl_u64 modification_seconds;
    /** Subsecond modification component, 0..999999999 when present. */
    unsigned long modification_nanoseconds;
    /** Nonzero makes the precise timestamp available for freshness checks. */
    int has_modification_time;
} knurl_asset_info;
/** One immediate child name/metadata view; listing storage expires at the next
 * list call or asset close.
 */
typedef struct knurl_asset_entry {
    /** Name of this declaration, field or listed resource. */
    const char *name;
    /** Opened-resource metadata; valid only when the containing result supplies
     * metadata.
     */
    knurl_asset_info info;
} knurl_asset_entry;
/** Read the same opened asset at the supplied exact offset into caller storage.
 * AGAIN returns no bytes; do not block the HTTP owner loop or reenter the
 * asset.
 */
typedef knurl_status (*knurl_asset_read_fn)(knurl_u64, void *, size_t, size_t *,
                                            void *, knurl_error *);
/** Return one immediate child, EOF at end, or AGAIN without an entry. Entry
 * views last until the next call/close.
 */
typedef knurl_status (*knurl_asset_list_fn)(knurl_asset_entry *, void *,
                                            knurl_error *);
/** Configuration for asset. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_asset_config {
    /** Opened-resource metadata; valid only when the containing result supplies
     * metadata.
     */
    knurl_asset_info info;
    /** Bounded read callback; honor the enclosing offset/prefix/AGAIN contract.
     */
    knurl_asset_read_fn read;
    /** Incremental listing callback; EOF means no more children. */
    knurl_asset_list_fn next;
    /** Terminal cleanup callback; retain its context until actual retirement.
     */
    knurl_retire_fn close;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} knurl_asset_config;
/** Read-only opened object with immutable copied metadata. Reads are at
 * explicit offsets and must refer to this same opened object. Source/list I/O
 * may return AGAIN with no result. Open/close and metadata calls are
 * synchronous and must return promptly; long remote/native operations use the
 * storage job contract, not an asset callback that blocks the HTTP owner loop.
 * Caller serializes all access. No callback may longjmp or reenter its handle.
 */
KNURL_API void knurl_asset_config_init(knurl_asset_config *);
/** Copy metadata and callbacks into an owned opened asset; construction
 * performs no reads. See knurl_asset_config_init for callback constraints.
 */
KNURL_API knurl_status knurl_asset_new(const knurl_asset_config *,
                                       knurl_asset **, knurl_error *);
/** Borrowed bytes stay valid until the successful asset's close notification.
 */
KNURL_API knurl_status knurl_asset_memory(const knurl_asset_info *, knurl_bytes,
                                          knurl_retire_fn, void *,
                                          knurl_asset **, knurl_error *);
/** Borrow immutable copied metadata until the asset closes. */
KNURL_API const knurl_asset_info *knurl_asset_stat(const knurl_asset *);
/** Read at an explicit offset into caller storage; AGAIN produces no bytes.
 * Caller serializes access.
 */
KNURL_API knurl_status knurl_asset_read(knurl_asset *, knurl_u64, void *,
                                        size_t, size_t *, knurl_error *);
/** One immediate child per call. Entry metadata is borrowed through the next
 * list call/close. EOF has no entry; AGAIN means retry later. */
KNURL_API knurl_status knurl_asset_list_next(knurl_asset *, knurl_asset_entry *,
                                             knurl_error *);
/** Release an owned opened asset and deliver its close notification. */
KNURL_API knurl_status knurl_asset_close(knurl_asset *, knurl_error *);
/** Success transfers asset, failure takes nothing; emits only the selected
 * range, never reads/skips the preceding content. No read at construction. */
KNURL_API knurl_status knurl_asset_source_take(knurl_asset *, knurl_u64,
                                               knurl_u64, knurl_source **,
                                               knurl_error *);
/** Synchronously open one normalized resource path. OK transfers one opened
 * asset; EOF means missing. Return promptly without reentry.
 */
typedef knurl_status (*knurl_asset_open_fn)(const char *, knurl_asset **,
                                            void *, knurl_error *);
/** Configuration for asset provider. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct knurl_asset_provider_config {
    /** Resource/state opener; failed construction must clean its own partial
     * state.
     */
    knurl_asset_open_fn open;
    /** Terminal cleanup callback; retain its context until actual retirement.
     */
    knurl_retire_fn close;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} knurl_asset_provider_config;
/** Initialize asset provider config with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_asset_provider_config_init(knurl_asset_provider_config *);
/** Create an inert owned provider; retain callback userdata until provider
 * retirement.
 */
KNURL_API knurl_status
knurl_asset_provider_new(const knurl_asset_provider_config *,
                         knurl_asset_provider **, knurl_error *);
/** Paths are normalized absolute paths, without trailing slash except '/'.
 * Invalid/ambiguous segments, percent signs and backslashes are rejected.
 * EOF means missing. Success takes exactly one returned asset; already opened
 * assets pin their provider through terminal close, even after provider_close.
 */
KNURL_API knurl_status knurl_asset_provider_open(knurl_asset_provider *,
                                                 const char *, knurl_asset **,
                                                 knurl_error *);
/** Close the provider owner; opened assets keep it alive through their own
 * close.
 */
KNURL_API knurl_status knurl_asset_provider_close(knurl_asset_provider *,
                                                  knurl_error *);
/** Inert construction; first open pins the root with a component-by-component
 * O_NOFOLLOW walk. No symlinks or special files are served. Files are opened
 * once for metadata/read, listing is incremental. Relative roots are resolved
 * at first open. Regular filesystem syscalls may block in the OS. */
KNURL_API knurl_status knurl_asset_filesystem_new(const char *,
                                                  knurl_asset_provider **,
                                                  knurl_error *);

struct knurl_asset_entry;
struct knurl_asset_info;
struct knurl_error;
struct knurl_source;
struct knurl_u64;
/** Library-owned asset receiver. Pass this handle as the first
 * argument to its methods. Obtain it through the documented constructor or
 * borrowed accessor. Do not copy, initialize or modify the shell or impl.
 */
struct knurl_asset {
    /** Borrow immutable copied metadata until the asset closes. */
    const struct knurl_asset_info *(*stat)(const struct knurl_asset *);
    /** Read at an explicit offset into caller storage; AGAIN produces no bytes.
     * Caller serializes access.
     */
    knurl_status (*read)(struct knurl_asset *, struct knurl_u64, void *, size_t,
                         size_t *, struct knurl_error *);
    /** One immediate child per call. Entry metadata is borrowed through the
     * next list call/close. EOF has no entry; AGAIN means retry later. */
    knurl_status (*list_next)(struct knurl_asset *, struct knurl_asset_entry *,
                              struct knurl_error *);
    /** Release an owned opened asset and deliver its close notification. */
    knurl_status (*close)(struct knurl_asset *, struct knurl_error *);
    /** Success transfers asset, failure takes nothing; emits only the selected
     * range, never reads/skips the preceding content. No read at construction.
     */
    knurl_status (*source_take)(struct knurl_asset *, struct knurl_u64,
                                struct knurl_u64, struct knurl_source **,
                                struct knurl_error *);
    /** Private library state; applications must not access it. */
    struct knurl_asset_impl *impl;
};

struct knurl_asset;
struct knurl_error;
struct knurl_storage_provider;
/** Library-owned asset provider receiver. Pass this handle as the first
 * argument to its methods. Obtain it through the documented constructor or
 * borrowed accessor. Do not copy, initialize or modify the shell or impl.
 */
struct knurl_asset_provider {
    /** Paths are normalized absolute paths, without trailing slash except '/'.
     * Invalid/ambiguous segments, percent signs and backslashes are rejected.
     * EOF means missing. Success takes exactly one returned asset; already
     * opened assets pin their provider through terminal close, even after
     * provider_close.
     */
    knurl_status (*open)(struct knurl_asset_provider *, const char *,
                         struct knurl_asset **, struct knurl_error *);
    /** Close the provider owner; opened assets keep it alive through their own
     * close.
     */
    knurl_status (*close)(struct knurl_asset_provider *, struct knurl_error *);
    /** Read-only adapter over a borrowed asset provider. No new packed format
     * or Vectis object is introduced. Caller retains the asset provider through
     * close.
     */
    knurl_status (*storage_provider)(struct knurl_asset_provider *,
                                     struct knurl_storage_provider **,
                                     struct knurl_error *);
    /** Private library state; applications must not access it. */
    struct knurl_asset_provider_impl *impl;
};

#ifdef __cplusplus
}
#endif
#endif

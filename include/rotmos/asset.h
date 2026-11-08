#ifndef ROTMOS_ASSET_H
#define ROTMOS_ASSET_H
#include <rotmos/io.h>
#include <rotmos/loop.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Maximum normalized asset path size in bytes. */
#define ROTMOS_ASSET_PATH_MAX 4096u
/** Maximum asset entity-tag metadata storage in bytes. */
#define ROTMOS_ASSET_ETAG_MAX 256u
/** Maximum asset Content-Type metadata storage in bytes. */
#define ROTMOS_ASSET_TYPE_MAX 256u
/** Maximum asset HTTP-date metadata storage in bytes. */
#define ROTMOS_ASSET_DATE_MAX 64u
/** Owned opened read-only resource with immutable copied metadata. */
typedef struct rotmos_asset rotmos_asset;
/** Owned asset-opening provider; open assets retain it through close. */
typedef struct rotmos_asset_provider rotmos_asset_provider;
/** Values for asset kind. */
typedef enum rotmos_asset_kind {
    /** Read-only resource kind: file. */
    ROTMOS_ASSET_FILE = 0,
    /** Read-only resource kind: directory. */
    ROTMOS_ASSET_DIRECTORY = 1
} rotmos_asset_kind;
/** Opened immutable asset metadata copied by asset construction; string views
 * expire at asset close.
 */
typedef struct rotmos_asset_info {
    /** Representation or event discriminator; inspect before interpreting other
     * fields.
     */
    rotmos_asset_kind kind;
    /** Resource length in bytes. */
    rotmos_u64 size;
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
} rotmos_asset_info;
/** One immediate child name/metadata view; listing storage expires at the next
 * list call or asset close.
 */
typedef struct rotmos_asset_entry {
    /** Name of this declaration, field or listed resource. */
    const char *name;
    /** Opened-resource metadata; valid only when the containing result supplies
     * metadata.
     */
    rotmos_asset_info info;
} rotmos_asset_entry;
/** Read the same opened asset at the supplied exact offset into caller storage.
 * AGAIN returns no bytes; do not block the HTTP owner loop or reenter the
 * asset.
 */
typedef rotmos_status (*rotmos_asset_read_fn)(rotmos_u64, void *, size_t,
                                              size_t *, void *, rotmos_error *);
/** Return one immediate child, EOF at end, or AGAIN without an entry. Entry
 * views last until the next call/close.
 */
typedef rotmos_status (*rotmos_asset_list_fn)(rotmos_asset_entry *, void *,
                                              rotmos_error *);
/** Configuration for asset. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_asset_config {
    /** Opened-resource metadata; valid only when the containing result supplies
     * metadata.
     */
    rotmos_asset_info info;
    /** Bounded read callback; honor the enclosing offset/prefix/AGAIN contract.
     */
    rotmos_asset_read_fn read;
    /** Incremental listing callback; EOF means no more children. */
    rotmos_asset_list_fn next;
    /** Terminal cleanup callback; retain its context until actual retirement.
     */
    rotmos_retire_fn close;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} rotmos_asset_config;
/** Read-only opened object with immutable copied metadata. Reads are at
 * explicit offsets and must refer to this same opened object. Source/list I/O
 * may return AGAIN with no result. Open/close and metadata calls are
 * synchronous and must return promptly; long remote/native operations use the
 * storage job contract, not an asset callback that blocks the HTTP owner loop.
 * Caller serializes all access. No callback may longjmp or reenter its handle.
 */
ROTMOS_API void rotmos_asset_config_init(rotmos_asset_config *);
/** Copy metadata and callbacks into an owned opened asset; construction
 * performs no reads. See rotmos_asset_config_init for callback constraints.
 */
ROTMOS_API rotmos_status rotmos_asset_new(const rotmos_asset_config *,
                                          rotmos_asset **, rotmos_error *);
/** Borrowed bytes stay valid until the successful asset's close notification.
 */
ROTMOS_API rotmos_status rotmos_asset_memory(const rotmos_asset_info *,
                                             rotmos_bytes, rotmos_retire_fn,
                                             void *, rotmos_asset **,
                                             rotmos_error *);
/** Borrow immutable copied metadata until the asset closes. */
ROTMOS_API const rotmos_asset_info *rotmos_asset_stat(const rotmos_asset *);
/** Read at an explicit offset into caller storage; AGAIN produces no bytes.
 * Caller serializes access.
 */
ROTMOS_API rotmos_status rotmos_asset_read(rotmos_asset *, rotmos_u64, void *,
                                           size_t, size_t *, rotmos_error *);
/** One immediate child per call. Entry metadata is borrowed through the next
 * list call/close. EOF has no entry; AGAIN means retry later. */
ROTMOS_API rotmos_status rotmos_asset_list_next(rotmos_asset *,
                                                rotmos_asset_entry *,
                                                rotmos_error *);
/** Release an owned opened asset and deliver its close notification. */
ROTMOS_API rotmos_status rotmos_asset_close(rotmos_asset *, rotmos_error *);
/** Success transfers asset, failure takes nothing; emits only the selected
 * range, never reads/skips the preceding content. No read at construction. */
ROTMOS_API rotmos_status rotmos_asset_source_take(rotmos_asset *, rotmos_u64,
                                                  rotmos_u64, rotmos_source **,
                                                  rotmos_error *);
/** Synchronously open one normalized resource path. OK transfers one opened
 * asset; EOF means missing. Return promptly without reentry.
 */
typedef rotmos_status (*rotmos_asset_open_fn)(const char *, rotmos_asset **,
                                              void *, rotmos_error *);
/** Configuration for asset provider. Use the owning operation's initializer
 * before applying overrides.
 */
typedef struct rotmos_asset_provider_config {
    /** Resource/state opener; failed construction must clean its own partial
     * state.
     */
    rotmos_asset_open_fn open;
    /** Terminal cleanup callback; retain its context until actual retirement.
     */
    rotmos_retire_fn close;
    /** Caller-owned callback context; retain it for the enclosing
     * handle/callback lifetime.
     */
    void *userdata;
} rotmos_asset_provider_config;
/** Initialize asset provider config with its documented defaults; call before
 * applying overrides.
 */
ROTMOS_API void
rotmos_asset_provider_config_init(rotmos_asset_provider_config *);
/** Create an inert owned provider; retain callback userdata until provider
 * retirement.
 */
ROTMOS_API rotmos_status
rotmos_asset_provider_new(const rotmos_asset_provider_config *,
                          rotmos_asset_provider **, rotmos_error *);
/** Paths are normalized absolute paths, without trailing slash except '/'.
 * Invalid/ambiguous segments, percent signs and backslashes are rejected.
 * EOF means missing. Success takes exactly one returned asset; already opened
 * assets pin their provider through terminal close, even after provider_close.
 */
ROTMOS_API rotmos_status rotmos_asset_provider_open(rotmos_asset_provider *,
                                                    const char *,
                                                    rotmos_asset **,
                                                    rotmos_error *);
/** Close the provider owner; opened assets keep it alive through their own
 * close.
 */
ROTMOS_API rotmos_status rotmos_asset_provider_close(rotmos_asset_provider *,
                                                     rotmos_error *);
/** Inert construction; first open pins the root with a component-by-component
 * O_NOFOLLOW walk. No symlinks or special files are served. Files are opened
 * once for metadata/read, listing is incremental. Relative roots are resolved
 * at first open. Regular filesystem syscalls may block in the OS. */
ROTMOS_API rotmos_status rotmos_asset_filesystem_new(const char *,
                                                     rotmos_asset_provider **,
                                                     rotmos_error *);
#ifdef __cplusplus
}
#endif
#endif

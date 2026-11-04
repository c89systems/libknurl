#ifndef ROTMOS_ASSET_H
#define ROTMOS_ASSET_H
#include <rotmos/io.h>
#include <rotmos/loop.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ROTMOS_ASSET_PATH_MAX 4096u
#define ROTMOS_ASSET_ETAG_MAX 256u
#define ROTMOS_ASSET_TYPE_MAX 256u
#define ROTMOS_ASSET_DATE_MAX 64u
typedef struct rotmos_asset rotmos_asset;
typedef struct rotmos_asset_provider rotmos_asset_provider;
typedef enum rotmos_asset_kind {
    ROTMOS_ASSET_FILE = 0,
    ROTMOS_ASSET_DIRECTORY = 1
} rotmos_asset_kind;
typedef struct rotmos_asset_info {
    rotmos_asset_kind kind;
    rotmos_u64 size;
    const char *etag;
    const char *content_type;
    const char *last_modified;
} rotmos_asset_info;
typedef struct rotmos_asset_entry {
    const char *name;
    rotmos_asset_info info;
} rotmos_asset_entry;
typedef rotmos_status (*rotmos_asset_read_fn)(rotmos_u64, void *, size_t,
                                              size_t *, void *, rotmos_error *);
typedef rotmos_status (*rotmos_asset_list_fn)(rotmos_asset_entry *, void *,
                                              rotmos_error *);
typedef struct rotmos_asset_config {
    rotmos_asset_info info;
    rotmos_asset_read_fn read;
    rotmos_asset_list_fn next;
    rotmos_retire_fn close;
    void *userdata;
} rotmos_asset_config;
/* Read-only opened object with immutable copied metadata. Reads are at
 * explicit offsets and must refer to this same opened object. Source/list I/O
 * may return AGAIN with no result. Open/close and metadata calls are
 * synchronous and must return promptly; long remote/native operations use the
 * storage job contract, not an asset callback that blocks the HTTP owner loop.
 * Caller serializes all access. No callback may longjmp or reenter its handle.
 */
ROTMOS_API void rotmos_asset_config_init(rotmos_asset_config *);
ROTMOS_API rotmos_status rotmos_asset_new(const rotmos_asset_config *,
                                          rotmos_asset **, rotmos_error *);
/* Borrowed bytes stay valid until the successful asset's close notification. */
ROTMOS_API rotmos_status rotmos_asset_memory(const rotmos_asset_info *,
                                             rotmos_bytes, rotmos_retire_fn,
                                             void *, rotmos_asset **,
                                             rotmos_error *);
ROTMOS_API const rotmos_asset_info *rotmos_asset_stat(const rotmos_asset *);
ROTMOS_API rotmos_status rotmos_asset_read(rotmos_asset *, rotmos_u64, void *,
                                           size_t, size_t *, rotmos_error *);
/* One immediate child per call. Entry metadata is borrowed through the next
 * list call/close. EOF has no entry; AGAIN means retry later. */
ROTMOS_API rotmos_status rotmos_asset_list_next(rotmos_asset *,
                                                rotmos_asset_entry *,
                                                rotmos_error *);
ROTMOS_API rotmos_status rotmos_asset_close(rotmos_asset *, rotmos_error *);
/* Success transfers asset, failure takes nothing; emits only the selected
 * range, never reads/skips the preceding content. No read at construction. */
ROTMOS_API rotmos_status rotmos_asset_source_take(rotmos_asset *, rotmos_u64,
                                                  rotmos_u64, rotmos_source **,
                                                  rotmos_error *);
typedef rotmos_status (*rotmos_asset_open_fn)(const char *, rotmos_asset **,
                                              void *, rotmos_error *);
typedef struct rotmos_asset_provider_config {
    rotmos_asset_open_fn open;
    rotmos_retire_fn close;
    void *userdata;
} rotmos_asset_provider_config;
ROTMOS_API void
rotmos_asset_provider_config_init(rotmos_asset_provider_config *);
ROTMOS_API rotmos_status
rotmos_asset_provider_new(const rotmos_asset_provider_config *,
                          rotmos_asset_provider **, rotmos_error *);
/* Paths are normalized absolute paths, without trailing slash except '/'.
 * Invalid/ambiguous segments, percent signs and backslashes are rejected.
 * EOF means missing. Success takes exactly one returned asset; already opened
 * assets pin their provider through terminal close, even after provider_close.
 */
ROTMOS_API rotmos_status rotmos_asset_provider_open(rotmos_asset_provider *,
                                                    const char *,
                                                    rotmos_asset **,
                                                    rotmos_error *);
ROTMOS_API rotmos_status rotmos_asset_provider_close(rotmos_asset_provider *,
                                                     rotmos_error *);
/* Inert construction; first open pins the root with a component-by-component
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

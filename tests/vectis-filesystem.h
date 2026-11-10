#ifndef REFERENCE_FILESYSTEM_H
#define REFERENCE_FILESYSTEM_H
#include <knurl/storage.h>
/* Vectis's standalone declarations have the same value layouts. These
 * translations contain no disk, locking, quota or codec implementation. */
typedef knurl_storage_filesystem_config reference_webdav_config;
typedef knurl_filesystem_entry reference_webdav_entry;
typedef knurl_filesystem_entry_kind reference_webdav_entry_kind;
typedef knurl_storage_code reference_webdav_status;
#define REFERENCE_WEBDAV_OK KNURL_STORAGE_OK
#define REFERENCE_WEBDAV_NOT_FOUND KNURL_STORAGE_NOT_FOUND
#define REFERENCE_WEBDAV_EXISTS KNURL_STORAGE_EXISTS
#define REFERENCE_WEBDAV_INVALID KNURL_STORAGE_INVALID
#define REFERENCE_WEBDAV_LIMIT KNURL_STORAGE_LIMIT
#define REFERENCE_WEBDAV_IO KNURL_STORAGE_IO
#define REFERENCE_WEBDAV_NOMEM KNURL_STORAGE_NOMEM
#define REFERENCE_WEBDAV_CONFLICT KNURL_STORAGE_CONFLICT
#define REFERENCE_WEBDAV_PRECONDITION KNURL_STORAGE_PRECONDITION
#define REFERENCE_WEBDAV_ENTRY_FILE KNURL_FILESYSTEM_ENTRY_FILE
#define REFERENCE_WEBDAV_ENTRY_COLLECTION KNURL_FILESYSTEM_ENTRY_COLLECTION
#define REFERENCE_WEBDAV_ENTRY_TOMBSTONE KNURL_FILESYSTEM_ENTRY_TOMBSTONE
#define reference_webdav_config_init knurl_storage_filesystem_config_init
#define reference_webdav_path_normalize knurl_storage_filesystem_path_normalize
#define reference_webdav_content_dir(c, out, error)                            \
    knurl_storage_filesystem_content_directory(                                \
        c, out, KNURL_FILESYSTEM_STORAGE_PATH_MAX, error)
#define reference_webdav_lookup knurl_storage_filesystem_lookup
#define reference_webdav_read knurl_storage_filesystem_read
#define reference_webdav_put_conditional knurl_storage_filesystem_put
#define reference_webdav_delete_conditional knurl_storage_filesystem_delete
#define reference_webdav_mkcol_conditional knurl_storage_filesystem_mkcol
#define reference_webdav_copy_conditional knurl_storage_filesystem_copy
#define reference_webdav_move_conditional knurl_storage_filesystem_move
#define reference_webdav_list knurl_storage_filesystem_list
#define reference_webdav_put(c, p, b, n)                                       \
    knurl_storage_filesystem_put(c, p, b, n, NULL, NULL)
#define reference_webdav_delete(c, p)                                          \
    knurl_storage_filesystem_delete(c, p, NULL, NULL)
#define reference_webdav_mkcol(c, p)                                           \
    knurl_storage_filesystem_mkcol(c, p, NULL, NULL)
#define reference_webdav_copy(c, s, d, o)                                      \
    knurl_storage_filesystem_copy(c, s, d, o, -1, NULL, NULL)
#define reference_webdav_copy_depth(c, s, d, o, h)                             \
    knurl_storage_filesystem_copy(c, s, d, o, h, NULL, NULL)
#define reference_webdav_move(c, s, d, o)                                      \
    knurl_storage_filesystem_move(c, s, d, o, NULL, NULL)
#endif

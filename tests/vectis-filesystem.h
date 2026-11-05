#ifndef REFERENCE_FILESYSTEM_H
#define REFERENCE_FILESYSTEM_H
#include <rotmos/storage.h>
/* Vectis's standalone declarations have the same value layouts. These
 * translations contain no disk, locking, quota or codec implementation. */
typedef rotmos_storage_filesystem_config reference_webdav_config;
typedef rotmos_filesystem_entry reference_webdav_entry;
typedef rotmos_filesystem_entry_kind reference_webdav_entry_kind;
typedef rotmos_storage_code reference_webdav_status;
#define REFERENCE_WEBDAV_OK ROTMOS_STORAGE_OK
#define REFERENCE_WEBDAV_NOT_FOUND ROTMOS_STORAGE_NOT_FOUND
#define REFERENCE_WEBDAV_EXISTS ROTMOS_STORAGE_EXISTS
#define REFERENCE_WEBDAV_INVALID ROTMOS_STORAGE_INVALID
#define REFERENCE_WEBDAV_LIMIT ROTMOS_STORAGE_LIMIT
#define REFERENCE_WEBDAV_IO ROTMOS_STORAGE_IO
#define REFERENCE_WEBDAV_NOMEM ROTMOS_STORAGE_NOMEM
#define REFERENCE_WEBDAV_CONFLICT ROTMOS_STORAGE_CONFLICT
#define REFERENCE_WEBDAV_PRECONDITION ROTMOS_STORAGE_PRECONDITION
#define REFERENCE_WEBDAV_ENTRY_FILE ROTMOS_FILESYSTEM_ENTRY_FILE
#define REFERENCE_WEBDAV_ENTRY_COLLECTION ROTMOS_FILESYSTEM_ENTRY_COLLECTION
#define REFERENCE_WEBDAV_ENTRY_TOMBSTONE ROTMOS_FILESYSTEM_ENTRY_TOMBSTONE
#define reference_webdav_config_init rotmos_storage_filesystem_config_init
#define reference_webdav_path_normalize rotmos_storage_filesystem_path_normalize
#define reference_webdav_content_dir(c, out, error)                            \
    rotmos_storage_filesystem_content_directory(                               \
        c, out, ROTMOS_FILESYSTEM_STORAGE_PATH_MAX, error)
#define reference_webdav_lookup rotmos_storage_filesystem_lookup
#define reference_webdav_read rotmos_storage_filesystem_read
#define reference_webdav_put_conditional rotmos_storage_filesystem_put
#define reference_webdav_delete_conditional rotmos_storage_filesystem_delete
#define reference_webdav_mkcol_conditional rotmos_storage_filesystem_mkcol
#define reference_webdav_copy_conditional rotmos_storage_filesystem_copy
#define reference_webdav_move_conditional rotmos_storage_filesystem_move
#define reference_webdav_list rotmos_storage_filesystem_list
#define reference_webdav_put(c, p, b, n)                                       \
    rotmos_storage_filesystem_put(c, p, b, n, NULL, NULL)
#define reference_webdav_delete(c, p)                                          \
    rotmos_storage_filesystem_delete(c, p, NULL, NULL)
#define reference_webdav_mkcol(c, p)                                           \
    rotmos_storage_filesystem_mkcol(c, p, NULL, NULL)
#define reference_webdav_copy(c, s, d, o)                                      \
    rotmos_storage_filesystem_copy(c, s, d, o, -1, NULL, NULL)
#define reference_webdav_copy_depth(c, s, d, o, h)                             \
    rotmos_storage_filesystem_copy(c, s, d, o, h, NULL, NULL)
#define reference_webdav_move(c, s, d, o)                                      \
    rotmos_storage_filesystem_move(c, s, d, o, NULL, NULL)
#endif

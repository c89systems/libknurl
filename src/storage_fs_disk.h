#ifndef KNURL_STORAGE_FS_DISK_H
#define KNURL_STORAGE_FS_DISK_H
#include <knurl/storage.h>
#include "executor.h"
#include <sys/stat.h>
#include <stdint.h>
#define KNURL_FS_PATH_MAX 1024u
#define KNURL_FS_STORAGE_PATH_MAX 4096u
#define KNURL_FS_ETAG_LENGTH 64u
#define KNURL_FS_ENTRY_FILE 1
#define KNURL_FS_ENTRY_TOMBSTONE 2
#define KNURL_FS_ENTRY_COLLECTION 3
typedef struct knurl_fs_config {
    const char *cache_dir, *site_id, *root_dir;
    size_t max_file_bytes, max_total_bytes, max_resources;
    knurl_job *job;
    knurl_storage_result *result;
    char *recovery;
    knurl_storage_code (*scope)(const char *, const char *, void *);
    void *scope_data;
} knurl_fs_config;
typedef struct knurl_fs_entry {
    int kind;
    size_t size;
    char storage_path[KNURL_FS_STORAGE_PATH_MAX];
} knurl_fs_entry;
knurl_storage_code knurl_fs_scope(const knurl_fs_config *, const char *,
                                  const char *, int, int);
int knurl_fs_config_valid(const knurl_fs_config *);
int knurl_fs_cancelled(const knurl_job *);
int knurl_fs_path_normalize(const char *, char[KNURL_FS_PATH_MAX + 1]);
int knurl_fs_disk_path(const knurl_fs_config *, const char *, const char *,
                       char[KNURL_FS_STORAGE_PATH_MAX]);
int knurl_fs_file_regular(const char *, struct stat *);
int knurl_fs_file_directory(const char *);
int knurl_fs_lock(const knurl_fs_config *);
void knurl_fs_unlock(int);
knurl_storage_code knurl_fs_lookup(const knurl_fs_config *, const char *,
                                   knurl_fs_entry *);
knurl_storage_code knurl_fs_check_conditions(const knurl_fs_config *,
                                             const char *, const char *,
                                             const char *);
knurl_storage_code knurl_fs_check_parent(const knurl_fs_config *, const char *);
int knurl_fs_usage(const knurl_fs_config *, uint64_t *, uint64_t *);
int knurl_fs_subtract_tombstone_usage(const knurl_fs_config *, const char *,
                                      uint64_t *, uint64_t *);
int knurl_fs_missing_path_resources(const char *, const char *, int,
                                    uint64_t *);
int knurl_fs_resources_within_limit(const knurl_fs_config *, uint64_t,
                                    uint64_t);
int knurl_fs_ancestor_tombstone_exists(const knurl_fs_config *, const char *);
int knurl_fs_tombstone_exists(const knurl_fs_config *, const char *);
int knurl_fs_clear_tombstones(const knurl_fs_config *, const char *);
int knurl_fs_open_parent_fd(const knurl_fs_config *, const char *, int,
                            char[KNURL_FS_PATH_MAX + 1]);
int knurl_fs_direct_open_existing(const knurl_fs_config *, const char *, int,
                                  struct stat *);
int knurl_fs_etag_fd(int, char[67], knurl_job *);
int knurl_fs_tag_matches(const char *, const char *, int, int);
int knurl_fs_internal_entry(const char *);
knurl_storage_code knurl_fs_delete_conditional(const knurl_fs_config *,
                                               const char *, const char *,
                                               const char *);
knurl_storage_code knurl_fs_mkcol_conditional(const knurl_fs_config *,
                                              const char *, const char *,
                                              const char *);
knurl_storage_code knurl_fs_copy_or_move(const knurl_fs_config *, const char *,
                                         const char *, int, int, int,
                                         const char *, const char *, int *);
#endif

#ifndef ROTMOS_STORAGE_FS_DISK_H
#define ROTMOS_STORAGE_FS_DISK_H
#include <rotmos/storage.h>
#include <rotmos/executor.h>
#include <sys/stat.h>
#include <stdint.h>
#define ROTMOS_FS_PATH_MAX 1024u
#define ROTMOS_FS_STORAGE_PATH_MAX 4096u
#define ROTMOS_FS_ETAG_LENGTH 64u
#define ROTMOS_FS_ENTRY_FILE 1
#define ROTMOS_FS_ENTRY_TOMBSTONE 2
#define ROTMOS_FS_ENTRY_COLLECTION 3
typedef struct rotmos_fs_config {
    const char *cache_dir, *site_id, *root_dir;
    size_t max_file_bytes, max_total_bytes, max_resources;
    rotmos_job *job;
    rotmos_storage_result *result;
    char *recovery;
    rotmos_storage_code (*scope)(const char *, const char *, void *);
    void *scope_data;
} rotmos_fs_config;
typedef struct rotmos_fs_entry {
    int kind;
    size_t size;
    char storage_path[ROTMOS_FS_STORAGE_PATH_MAX];
} rotmos_fs_entry;
rotmos_storage_code rotmos_fs_scope(const rotmos_fs_config *, const char *,
                                    const char *, int, int);
int rotmos_fs_config_valid(const rotmos_fs_config *);
int rotmos_fs_path_normalize(const char *, char[ROTMOS_FS_PATH_MAX + 1]);
int rotmos_fs_disk_path(const rotmos_fs_config *, const char *, const char *,
                        char[ROTMOS_FS_STORAGE_PATH_MAX]);
int rotmos_fs_file_regular(const char *, struct stat *);
int rotmos_fs_file_directory(const char *);
int rotmos_fs_lock(const rotmos_fs_config *);
void rotmos_fs_unlock(int);
rotmos_storage_code rotmos_fs_lookup(const rotmos_fs_config *, const char *,
                                     rotmos_fs_entry *);
rotmos_storage_code rotmos_fs_check_conditions(const rotmos_fs_config *,
                                               const char *, const char *,
                                               const char *);
rotmos_storage_code rotmos_fs_check_parent(const rotmos_fs_config *,
                                           const char *);
int rotmos_fs_usage(const rotmos_fs_config *, uint64_t *, uint64_t *);
int rotmos_fs_subtract_tombstone_usage(const rotmos_fs_config *, const char *,
                                       uint64_t *, uint64_t *);
int rotmos_fs_missing_path_resources(const char *, const char *, int,
                                     uint64_t *);
int rotmos_fs_resources_within_limit(const rotmos_fs_config *, uint64_t,
                                     uint64_t);
int rotmos_fs_ancestor_tombstone_exists(const rotmos_fs_config *, const char *);
int rotmos_fs_clear_tombstones(const rotmos_fs_config *, const char *);
int rotmos_fs_open_parent_fd(const rotmos_fs_config *, const char *, int,
                             char[ROTMOS_FS_PATH_MAX + 1]);
int rotmos_fs_direct_open_existing(const rotmos_fs_config *, const char *, int,
                                   struct stat *);
int rotmos_fs_etag_fd(int, char[67], rotmos_job *);
int rotmos_fs_tag_matches(const char *, const char *, int, int);
int rotmos_fs_internal_entry(const char *);
rotmos_storage_code rotmos_fs_delete_conditional(const rotmos_fs_config *,
                                                 const char *, const char *,
                                                 const char *);
rotmos_storage_code rotmos_fs_mkcol_conditional(const rotmos_fs_config *,
                                                const char *, const char *,
                                                const char *);
rotmos_storage_code rotmos_fs_copy_or_move(const rotmos_fs_config *,
                                           const char *, const char *, int, int,
                                           int, const char *, const char *,
                                           int *);
#endif

/* Filesystem mutation mechanics lifted from Vectis (MIT); on-disk names
 * retained. */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "storage_fs_disk.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char knurl_fs_hex[] = "0123456789abcdef";
int knurl_fs_cancelled(const knurl_job *job)
{
    return job != NULL && knurl_job_cancelled(job);
}

static int knurl_fs_remove_tree(const char *path);
static int knurl_fs_disk_usage(const char *path, uint64_t *total,
                               uint64_t *resources);

static int knurl_fs_site_valid(const char *site_id)
{
    size_t i;

    if (site_id == NULL || site_id[0] == '\0' || strlen(site_id) > 63u) {
        return 0;
    }
    for (i = 0u; site_id[i] != '\0'; ++i) {
        if (!(isalnum((unsigned char)site_id[i]) || site_id[i] == '-' ||
              site_id[i] == '_')) {
            return 0;
        }
    }
    return 1;
}

int knurl_fs_config_valid(const knurl_fs_config *config)
{
    return config != NULL && config->cache_dir != NULL &&
           config->cache_dir[0] == '/' &&
           (config->root_dir == NULL || config->root_dir[0] == '/') &&
           knurl_fs_site_valid(config->site_id) &&
           config->max_file_bytes > 0u && config->max_total_bytes > 0u &&
           config->max_resources > 0u &&
           config->max_file_bytes <= config->max_total_bytes;
}

static int knurl_fs_direct_root(const knurl_fs_config *config)
{
    return config != NULL && config->root_dir != NULL &&
           config->root_dir[0] == '/';
}

static int knurl_fs_path_in_root(const char *root, const char *path,
                                 char out[KNURL_FS_STORAGE_PATH_MAX]);

static knurl_storage_code knurl_fs_direct_remove(const knurl_fs_config *config,
                                                 const char *normalized);
static knurl_storage_code knurl_fs_direct_mkcol(const knurl_fs_config *config,
                                                const char *normalized);
static knurl_storage_code
knurl_fs_direct_copy_or_move_fs(const knurl_fs_config *config,
                                const char *normalized_source,
                                const char *normalized_destination,
                                int overwrite, int remove_source, int shallow);
static int knurl_fs_open_root_fd(const knurl_fs_config *config,
                                 int create_missing);
static int knurl_fs_copy_segment(char *out, size_t out_size,
                                 const char **cursor);
static int knurl_fs_open_child_dir(int parent_fd, const char *name);

static int knurl_fs_path_append(char *out, size_t out_size, const char *left,
                                const char *right)
{
    int written;

    written = snprintf(out, out_size, "%s/%s", left, right);
    return written >= 0 && (size_t)written < out_size;
}

static int knurl_fs_base(const knurl_fs_config *config, char *out,
                         size_t out_size)
{
    int written;

    if (!knurl_fs_config_valid(config)) {
        return 0;
    }
    written = snprintf(out, out_size, "%s/webdav/%s", config->cache_dir,
                       config->site_id);
    return written >= 0 && (size_t)written < out_size;
}

static int knurl_fs_mkdir_p(const char *path)
{
    char *copy, *segment, *slash;
    int fd, next, saved;
    if (path == NULL || path[0] != '/' ||
        strlen(path) >= KNURL_FS_STORAGE_PATH_MAX)
        return 0;
    copy = strdup(path);
    if (copy == NULL)
        return 0;
    fd = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    segment = copy;
    while (fd >= 0) {
        while (*segment == '/')
            ++segment;
        if (*segment == 0) {
            free(copy);
            close(fd);
            return 1;
        }
        slash = strchr(segment, '/');
        if (slash != NULL)
            *slash = 0;
        /* These segments come from trusted absolute storage configuration;
         * request resource paths are normalized separately. Keep O_NOFOLLOW
         * on every component while preserving configured dot segments. */
        next = openat(fd, segment,
                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT) {
            if (mkdirat(fd, segment, 0700) != 0 && errno != EEXIST)
                break;
            next = openat(fd, segment,
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        saved = errno;
        close(fd);
        fd = next;
        errno = saved;
        if (fd < 0)
            break;
        if (slash == NULL) {
            free(copy);
            close(fd);
            return 1;
        }
        segment = slash + 1;
    }
    saved = errno;
    if (fd >= 0)
        close(fd);
    free(copy);
    errno = saved;
    return 0;
}

static int knurl_fs_parent_dir(const char *path, char *out, size_t out_size)
{
    char *slash;

    if (path == NULL || strlen(path) >= out_size) {
        return 0;
    }
    strcpy(out, path);
    slash = strrchr(out, '/');
    if (slash == NULL || slash == out) {
        return 0;
    }
    *slash = '\0';
    return 1;
}

static int knurl_fs_prepare(const knurl_fs_config *config)
{
    char base[KNURL_FS_STORAGE_PATH_MAX];
    char path[KNURL_FS_STORAGE_PATH_MAX];
    static const char *const directories[] = {"content", "tombstones"};
    size_t i;
    int root_fd;

    if (!knurl_fs_base(config, base, sizeof(base)) || !knurl_fs_mkdir_p(base)) {
        return 0;
    }
    if (knurl_fs_direct_root(config)) {
        root_fd = knurl_fs_open_root_fd(config, 1);
        if (root_fd < 0) {
            return 0;
        }
        (void)close(root_fd);
        return 1;
    }
    for (i = 0u; i < sizeof(directories) / sizeof(directories[0]); ++i) {
        if (!knurl_fs_path_append(path, sizeof(path), base, directories[i]) ||
            !knurl_fs_mkdir_p(path)) {
            return 0;
        }
    }
    return 1;
}

int knurl_fs_disk_path(const knurl_fs_config *config, const char *category,
                       const char *path, char out[KNURL_FS_STORAGE_PATH_MAX])
{
    char base[KNURL_FS_STORAGE_PATH_MAX];
    char normalized[KNURL_FS_PATH_MAX + 1u];
    const char *relative;
    int written;

    if (!knurl_fs_base(config, base, sizeof(base)) || category == NULL ||
        !knurl_fs_path_normalize(path, normalized)) {
        return 0;
    }
    if (strcmp(category, "content") == 0 && knurl_fs_direct_root(config)) {
        return knurl_fs_path_in_root(config->root_dir, normalized, out);
    }
    relative = normalized[1] == '\0' ? "" : normalized + 1;
    if (relative[0] == '\0') {
        written =
            snprintf(out, KNURL_FS_STORAGE_PATH_MAX, "%s/%s", base, category);
    } else {
        written = snprintf(out, KNURL_FS_STORAGE_PATH_MAX, "%s/%s/%s", base,
                           category, relative);
    }
    return written >= 0 && (size_t)written < KNURL_FS_STORAGE_PATH_MAX;
}

int knurl_fs_file_regular(const char *path, struct stat *st)
{
    struct stat local;

    if (path == NULL) {
        return 0;
    }
    if (st == NULL) {
        st = &local;
    }
    return lstat(path, st) == 0 && S_ISREG(st->st_mode);
}

int knurl_fs_file_directory(const char *path)
{
    struct stat st;

    return path != NULL && lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int knurl_fs_touch_atomic(const char *path)
{
    char parent[KNURL_FS_STORAGE_PATH_MAX];
    char temporary[KNURL_FS_STORAGE_PATH_MAX];
    int fd;
    int written;
    int close_result;

    if (!knurl_fs_parent_dir(path, parent, sizeof(parent)) ||
        !knurl_fs_mkdir_p(parent)) {
        return 0;
    }
    written =
        snprintf(temporary, sizeof(temporary), "%s/.vectis-tmp-XXXXXX", parent);
    if (written < 0 || (size_t)written >= sizeof(temporary)) {
        return 0;
    }
    fd = mkstemp(temporary);
    if (fd == -1) {
        return 0;
    }
    if (fchmod(fd, 0600) == -1 || fsync(fd) == -1) {
        (void)close(fd);
        (void)unlink(temporary);
        return 0;
    }
    close_result = close(fd);
    fd = -1;
    if (close_result == -1 || rename(temporary, path) == -1) {
        (void)unlink(temporary);
        return 0;
    }
    return 1;
}

int knurl_fs_lock(const knurl_fs_config *config)
{
    char base[KNURL_FS_STORAGE_PATH_MAX];
    char path[KNURL_FS_STORAGE_PATH_MAX];
    int fd;

    if (knurl_fs_cancelled(config->job))
        return -1;
    if (!knurl_fs_prepare(config) ||
        !knurl_fs_base(config, base, sizeof(base)) ||
        !knurl_fs_path_append(path, sizeof(path), base, ".lock")) {
        return -1;
    }
    fd = open(path, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd == -1) {
        return -1;
    }
    /* Separate open descriptions exclude threads as well as worker processes.
     */
    while (flock(fd, LOCK_EX | LOCK_NB) == -1) {
        if (knurl_fs_cancelled(config->job) ||
            (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN)) {
            (void)close(fd);
            return -1;
        }
        (void)poll(NULL, 0, 5);
    }
    if (knurl_fs_cancelled(config->job)) {
        close(fd);
        return -1;
    }
    return fd;
}

void knurl_fs_unlock(int fd)
{
    if (fd < 0) {
        return;
    }
    (void)flock(fd, LOCK_UN);
    (void)close(fd);
}

int knurl_fs_tombstone_exists(const knurl_fs_config *config, const char *path)
{
    char normalized[KNURL_FS_PATH_MAX + 1u];
    char candidate[KNURL_FS_PATH_MAX + 1u];
    char disk[KNURL_FS_STORAGE_PATH_MAX];
    size_t i;

    if (!knurl_fs_path_normalize(path, normalized)) {
        return 0;
    }
    if (knurl_fs_direct_root(config)) {
        return 0;
    }
    for (i = 1u; normalized[i] != '\0'; ++i) {
        if (normalized[i] != '/' && normalized[i + 1u] != '\0') {
            continue;
        }
        if (i >= sizeof(candidate)) {
            return 0;
        }
        memcpy(candidate, normalized, i + 1u);
        candidate[i + 1u] = '\0';
        if (candidate[i] == '/') {
            candidate[i] = '\0';
        }
        if (candidate[0] != '\0' &&
            knurl_fs_disk_path(config, "tombstones", candidate, disk) &&
            knurl_fs_file_regular(disk, NULL)) {
            return 1;
        }
    }
    return 0;
}

int knurl_fs_ancestor_tombstone_exists(const knurl_fs_config *config,
                                       const char *path)
{
    char normalized[KNURL_FS_PATH_MAX + 1u];
    char candidate[KNURL_FS_PATH_MAX + 1u];
    char disk[KNURL_FS_STORAGE_PATH_MAX];
    size_t i;

    if (!knurl_fs_path_normalize(path, normalized)) {
        return 0;
    }
    if (knurl_fs_direct_root(config)) {
        return 0;
    }
    for (i = 1u; normalized[i] != '\0'; ++i) {
        if (normalized[i] != '/') {
            continue;
        }
        memcpy(candidate, normalized, i);
        candidate[i] = '\0';
        if (knurl_fs_disk_path(config, "tombstones", candidate, disk) &&
            knurl_fs_file_regular(disk, NULL)) {
            return 1;
        }
    }
    return 0;
}

static int knurl_fs_path_in_root(const char *root, const char *path,
                                 char out[KNURL_FS_STORAGE_PATH_MAX])
{
    char normalized[KNURL_FS_PATH_MAX + 1u];
    int written;

    if (root == NULL || !knurl_fs_path_normalize(path, normalized)) {
        return 0;
    }
    if (normalized[1] == '\0') {
        written = snprintf(out, KNURL_FS_STORAGE_PATH_MAX, "%s", root);
    } else if (strcmp(root, "/") == 0) {
        written = snprintf(out, KNURL_FS_STORAGE_PATH_MAX, "%s", normalized);
    } else {
        written =
            snprintf(out, KNURL_FS_STORAGE_PATH_MAX, "%s%s", root, normalized);
    }
    return written >= 0 && (size_t)written < KNURL_FS_STORAGE_PATH_MAX;
}

static int knurl_fs_clear_tombstones_in_root(const char *root, const char *path)
{
    char normalized[KNURL_FS_PATH_MAX + 1u];
    char disk[KNURL_FS_STORAGE_PATH_MAX];

    if (!knurl_fs_path_normalize(path, normalized) ||
        !knurl_fs_path_in_root(root, normalized, disk)) {
        return 0;
    }
    return knurl_fs_remove_tree(disk);
}

int knurl_fs_clear_tombstones(const knurl_fs_config *config, const char *path)
{
    char root[KNURL_FS_STORAGE_PATH_MAX];

    if (knurl_fs_direct_root(config)) {
        return 1;
    }
    return knurl_fs_disk_path(config, "tombstones", "/", root) &&
           knurl_fs_clear_tombstones_in_root(root, path);
}

typedef struct knurl_fs_path_scan {
    struct knurl_fs_path_scan *parent;
    DIR *directory;
    const char *path;
    char child[KNURL_FS_STORAGE_PATH_MAX];
} knurl_fs_path_scan;

static knurl_fs_path_scan *knurl_fs_path_scan_new(const char *path)
{
    DIR *directory;
    knurl_fs_path_scan *scan;
    directory = opendir(path);
    if (directory == NULL)
        return NULL;
    scan = (knurl_fs_path_scan *)malloc(sizeof(*scan));
    if (scan == NULL) {
        (void)closedir(directory);
        errno = ENOMEM;
        return NULL;
    }
    scan->parent = NULL;
    scan->directory = directory;
    scan->path = path;
    return scan;
}

static void knurl_fs_path_scan_close(knurl_fs_path_scan *scan)
{
    knurl_fs_path_scan *parent;
    while (scan != NULL) {
        parent = scan->parent;
        (void)closedir(scan->directory);
        free(scan);
        scan = parent;
    }
}

static int knurl_fs_usage_path(const char *path, uint64_t *total,
                               uint64_t *resources, int ignore_unsupported)
{
    knurl_fs_path_scan *scan, *child, *parent;
    struct dirent *entry;
    struct stat st;
    int written;
    if (total == NULL || resources == NULL)
        return 0;
    scan = knurl_fs_path_scan_new(path);
    if (scan == NULL)
        return errno == ENOENT;
    while (scan != NULL) {
        errno = 0;
        entry = readdir(scan->directory);
        if (entry == NULL) {
            if (errno != 0)
                break;
            parent = scan->parent;
            (void)closedir(scan->directory);
            free(scan);
            scan = parent;
            continue;
        }
        if (knurl_fs_internal_entry(entry->d_name))
            continue;
        written = snprintf(scan->child, sizeof(scan->child), "%s/%s",
                           scan->path, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(scan->child) ||
            lstat(scan->child, &st) == -1)
            break;
        if (S_ISREG(st.st_mode)) {
            if (st.st_size < 0 ||
                ((uint64_t)-1) - *total < (uint64_t)st.st_size ||
                *resources == ((uint64_t)-1))
                break;
            *total += (uint64_t)st.st_size;
            (*resources)++;
        } else if (S_ISDIR(st.st_mode)) {
            if (*resources == ((uint64_t)-1))
                break;
            (*resources)++;
            child = knurl_fs_path_scan_new(scan->child);
            if (child == NULL)
                break;
            child->parent = scan;
            scan = child;
        } else if (!ignore_unsupported)
            break;
    }
    if (scan == NULL)
        return 1;
    knurl_fs_path_scan_close(scan);
    return 0;
}

int knurl_fs_usage(const knurl_fs_config *config, uint64_t *total,
                   uint64_t *resources)
{
    char content[KNURL_FS_STORAGE_PATH_MAX];
    char tombstones[KNURL_FS_STORAGE_PATH_MAX];

    if (total == NULL || resources == NULL ||
        !knurl_fs_disk_path(config, "content", "/", content)) {
        return 0;
    }
    *total = 0u;
    *resources = 0u;
    if (knurl_fs_direct_root(config)) {
        return knurl_fs_usage_path(content, total, resources, 1);
    }
    if (!knurl_fs_disk_path(config, "tombstones", "/", tombstones)) {
        return 0;
    }
    return knurl_fs_usage_path(content, total, resources, 0) &&
           knurl_fs_usage_path(tombstones, total, resources, 0);
}

int knurl_fs_resources_within_limit(const knurl_fs_config *config,
                                    uint64_t resources,
                                    uint64_t additional_resources)
{
    uint64_t limit;

    if (config == NULL) {
        return 0;
    }
    limit = (uint64_t)config->max_resources;
    return resources <= limit && additional_resources <= limit - resources;
}

static int knurl_fs_add_resources(uint64_t *resources,
                                  uint64_t additional_resources)
{
    if (resources == NULL ||
        ((uint64_t)-1) - *resources < additional_resources) {
        return 0;
    }
    *resources += additional_resources;
    return 1;
}

/* Account for the tombstone subtree removed after a replacement is installed.
 * The mutation lock must be held; this does not change the stored tombstones.
 */
int knurl_fs_subtract_tombstone_usage(const knurl_fs_config *config,
                                      const char *path, uint64_t *usage,
                                      uint64_t *resources)
{
    char disk[KNURL_FS_STORAGE_PATH_MAX];
    uint64_t removed_usage;
    uint64_t removed_resources;

    if (knurl_fs_direct_root(config))
        return 1;
    removed_usage = 0u;
    removed_resources = 0u;
    if (!knurl_fs_disk_path(config, "tombstones", path, disk) ||
        !knurl_fs_disk_usage(disk, &removed_usage, &removed_resources) ||
        *usage < removed_usage || *resources < removed_resources)
        return 0;
    *usage -= removed_usage;
    *resources -= removed_resources;
    return 1;
}

int knurl_fs_missing_path_resources(const char *root, const char *path,
                                    int include_path, uint64_t *resources)
{
    char candidate[KNURL_FS_STORAGE_PATH_MAX];
    char parent[KNURL_FS_STORAGE_PATH_MAX];
    struct stat st;
    size_t root_size;

    if (root == NULL || path == NULL || resources == NULL) {
        return 0;
    }
    root_size = strlen(root);
    if (strncmp(path, root, root_size) != 0 ||
        (path[root_size] != '\0' && path[root_size] != '/')) {
        return 0;
    }
    if (strlen(path) >= sizeof(candidate)) {
        return 0;
    }
    strcpy(candidate, path);
    if (!include_path) {
        if (!knurl_fs_parent_dir(candidate, parent, sizeof(parent))) {
            return 0;
        }
        strcpy(candidate, parent);
    }
    while (strcmp(candidate, root) != 0) {
        if (lstat(candidate, &st) == 0) {
            if (strcmp(candidate, path) == 0 && include_path) {
                return 1;
            }
            return S_ISDIR(st.st_mode);
        }
        if (errno != ENOENT || *resources == ((uint64_t)-1)) {
            return 0;
        }
        (*resources)++;
        if (!knurl_fs_parent_dir(candidate, parent, sizeof(parent))) {
            return 0;
        }
        strcpy(candidate, parent);
    }
    return knurl_fs_file_directory(root);
}

static void knurl_fs_fd_close(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static int knurl_fs_open_root_fd(const knurl_fs_config *config,
                                 int create_missing)
{
    char segment[KNURL_FS_PATH_MAX + 1u];
    const char *cursor;
    int current_fd;
    int next_fd;
    int saved_errno;

    if (!knurl_fs_direct_root(config)) {
        return -1;
    }
    current_fd = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (current_fd < 0) {
        return -1;
    }
    cursor = config->root_dir;
    for (;;) {
        while (*cursor == '/') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        if (!knurl_fs_copy_segment(segment, sizeof(segment), &cursor)) {
            knurl_fs_fd_close(&current_fd);
            return -1;
        }
        next_fd = knurl_fs_open_child_dir(current_fd, segment);
        saved_errno = errno;
        if (next_fd < 0 && create_missing && saved_errno == ENOENT) {
            if (mkdirat(current_fd, segment, 0700) != 0 && errno != EEXIST) {
                knurl_fs_fd_close(&current_fd);
                return -1;
            }
            next_fd = knurl_fs_open_child_dir(current_fd, segment);
        }
        if (next_fd < 0) {
            knurl_fs_fd_close(&current_fd);
            return -1;
        }
        knurl_fs_fd_close(&current_fd);
        current_fd = next_fd;
    }
    return current_fd;
}

static int knurl_fs_copy_segment(char *out, size_t out_size,
                                 const char **cursor)
{
    const char *start;
    size_t size;

    if (out == NULL || out_size == 0u || cursor == NULL || *cursor == NULL) {
        return 0;
    }
    while (**cursor == '/') {
        (*cursor)++;
    }
    start = *cursor;
    while (**cursor != '\0' && **cursor != '/') {
        (*cursor)++;
    }
    size = (size_t)(*cursor - start);
    if (size == 0u || size >= out_size) {
        return 0;
    }
    memcpy(out, start, size);
    out[size] = '\0';
    return 1;
}

static int knurl_fs_open_child_dir(int parent_fd, const char *name)
{
    struct stat st;
    int fd;

    fd = openat(parent_fd, name,
                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

int knurl_fs_open_parent_fd(const knurl_fs_config *config,
                            const char *normalized, int create_missing,
                            char leaf[KNURL_FS_PATH_MAX + 1u])
{
    char segment[KNURL_FS_PATH_MAX + 1u];
    const char *cursor;
    int current_fd;
    int next_fd;

    if (leaf == NULL || normalized == NULL || normalized[0] != '/' ||
        normalized[1] == '\0') {
        return -1;
    }
    current_fd = knurl_fs_open_root_fd(config, 0);
    if (current_fd < 0) {
        return -1;
    }
    cursor = normalized + 1;
    while (1) {
        if (!knurl_fs_copy_segment(segment, sizeof(segment), &cursor)) {
            knurl_fs_fd_close(&current_fd);
            return -1;
        }
        while (*cursor == '/') {
            cursor++;
        }
        if (*cursor == '\0') {
            strcpy(leaf, segment);
            return current_fd;
        }
        next_fd = knurl_fs_open_child_dir(current_fd, segment);
        if (next_fd < 0 && create_missing && errno == ENOENT) {
            if (mkdirat(current_fd, segment, 0700) != 0) {
                knurl_fs_fd_close(&current_fd);
                return -1;
            }
            next_fd = knurl_fs_open_child_dir(current_fd, segment);
        }
        if (next_fd < 0) {
            knurl_fs_fd_close(&current_fd);
            return -1;
        }
        knurl_fs_fd_close(&current_fd);
        current_fd = next_fd;
    }
}

int knurl_fs_direct_open_existing(const knurl_fs_config *config,
                                  const char *normalized, int want_dir,
                                  struct stat *out_st)
{
    char leaf[KNURL_FS_PATH_MAX + 1u];
    struct stat st;
    int parent_fd;
    int fd;

    if (normalized == NULL || normalized[0] != '/') {
        return -1;
    }
    if (normalized[1] == '\0') {
        fd = knurl_fs_open_root_fd(config, 0);
    } else {
        parent_fd = knurl_fs_open_parent_fd(config, normalized, 0, leaf);
        if (parent_fd < 0) {
            return -1;
        }
        fd = openat(parent_fd, leaf,
                    O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC |
                        (want_dir ? O_DIRECTORY : 0));
        knurl_fs_fd_close(&parent_fd);
    }
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) != 0 || (want_dir && !S_ISDIR(st.st_mode)) ||
        (!want_dir && !S_ISREG(st.st_mode))) {
        knurl_fs_fd_close(&fd);
        return -1;
    }
    if (out_st != NULL) {
        *out_st = st;
    }
    return fd;
}

typedef struct knurl_fs_fd_scan {
    struct knurl_fs_fd_scan *parent;
    DIR *directory;
    int parent_fd, destination_fd;
    char name[KNURL_FS_PATH_MAX + 1u];
} knurl_fs_fd_scan;

static knurl_fs_fd_scan *knurl_fs_fd_scan_new(int parent_fd, const char *name)
{
    knurl_fs_fd_scan *scan;
    int fd;
    if (strlen(name) > KNURL_FS_PATH_MAX)
        return NULL;
    fd = knurl_fs_open_child_dir(parent_fd, name);
    if (fd < 0)
        return NULL;
    scan = (knurl_fs_fd_scan *)malloc(sizeof(*scan));
    if (scan == NULL) {
        (void)close(fd);
        return NULL;
    }
    scan->directory = fdopendir(fd);
    if (scan->directory == NULL) {
        (void)close(fd);
        free(scan);
        return NULL;
    }
    scan->parent = NULL;
    scan->parent_fd = parent_fd;
    scan->destination_fd = -1;
    strcpy(scan->name, name);
    return scan;
}

static void knurl_fs_fd_scan_close(knurl_fs_fd_scan *scan)
{
    knurl_fs_fd_scan *parent;
    while (scan != NULL) {
        parent = scan->parent;
        (void)closedir(scan->directory);
        knurl_fs_fd_close(&scan->destination_fd);
        free(scan);
        scan = parent;
    }
}

static int knurl_fs_direct_remove_tree_at(int parent_fd, const char *name)
{
    knurl_fs_fd_scan *scan, *child, *parent;
    struct dirent *entry;
    struct stat st;
    int ok;
    if (parent_fd < 0 || name == NULL ||
        fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT;
    if (S_ISREG(st.st_mode))
        return unlinkat(parent_fd, name, 0) == 0;
    if (!S_ISDIR(st.st_mode))
        return 0;
    scan = knurl_fs_fd_scan_new(parent_fd, name);
    if (scan == NULL)
        return 0;
    while (scan != NULL) {
        errno = 0;
        entry = readdir(scan->directory);
        if (entry == NULL) {
            if (errno != 0)
                break;
            parent = scan->parent;
            (void)closedir(scan->directory);
            ok = unlinkat(scan->parent_fd, scan->name, AT_REMOVEDIR) == 0;
            free(scan);
            scan = parent;
            if (!ok) {
                knurl_fs_fd_scan_close(scan);
                return 0;
            }
            continue;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (fstatat(dirfd(scan->directory), entry->d_name, &st,
                    AT_SYMLINK_NOFOLLOW) != 0)
            break;
        if (S_ISREG(st.st_mode)) {
            if (unlinkat(dirfd(scan->directory), entry->d_name, 0) != 0)
                break;
        } else if (S_ISDIR(st.st_mode)) {
            child = knurl_fs_fd_scan_new(dirfd(scan->directory), entry->d_name);
            if (child == NULL)
                break;
            child->parent = scan;
            scan = child;
        } else
            break;
    }
    if (scan == NULL)
        return 1;
    knurl_fs_fd_scan_close(scan);
    return 0;
}

static int knurl_fs_direct_reserve_dir_at(int parent_fd, const char *prefix,
                                          char out[KNURL_FS_PATH_MAX + 1u])
{
    int i;
    int written;

    if (parent_fd < 0 || prefix == NULL || prefix[0] == '\0' || out == NULL) {
        return 0;
    }
    for (i = 0; i < 1000; ++i) {
        written = snprintf(out, KNURL_FS_PATH_MAX + 1u, "%s-%lu-%d", prefix,
                           (unsigned long)getpid(), i);
        if (written <= 0 || (size_t)written >= KNURL_FS_PATH_MAX + 1u) {
            return 0;
        }
        if (mkdirat(parent_fd, out, 0700) == 0) {
            return 1;
        }
        if (errno != EEXIST) {
            return 0;
        }
    }
    return 0;
}

static knurl_storage_code knurl_fs_direct_remove(const knurl_fs_config *config,
                                                 const char *normalized)
{
    char leaf[KNURL_FS_PATH_MAX + 1u];
    int parent_fd;
    int ok;

    parent_fd = knurl_fs_open_parent_fd(config, normalized, 0, leaf);
    if (parent_fd < 0) {
        return errno == ENOENT || errno == ENOTDIR ? KNURL_STORAGE_NOT_FOUND
                                                   : KNURL_STORAGE_IO;
    }
    ok = knurl_fs_direct_remove_tree_at(parent_fd, leaf);
    knurl_fs_fd_close(&parent_fd);
    return ok ? KNURL_STORAGE_OK : KNURL_STORAGE_IO;
}

static knurl_storage_code knurl_fs_direct_mkcol(const knurl_fs_config *config,
                                                const char *normalized)
{
    char leaf[KNURL_FS_PATH_MAX + 1u];
    struct stat st;
    int parent_fd;
    int exists;
    int ok;

    parent_fd = knurl_fs_open_parent_fd(config, normalized, 0, leaf);
    if (parent_fd < 0) {
        return errno == ENOENT || errno == ENOTDIR ? KNURL_STORAGE_CONFLICT
                                                   : KNURL_STORAGE_IO;
    }
    exists = fstatat(parent_fd, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0;
    if (exists) {
        knurl_fs_fd_close(&parent_fd);
        return S_ISREG(st.st_mode) || S_ISDIR(st.st_mode) ? KNURL_STORAGE_EXISTS
                                                          : KNURL_STORAGE_IO;
    }
    if (errno != ENOENT) {
        knurl_fs_fd_close(&parent_fd);
        return KNURL_STORAGE_IO;
    }
    ok = mkdirat(parent_fd, leaf, 0700) == 0;
    knurl_fs_fd_close(&parent_fd);
    return ok ? KNURL_STORAGE_OK : KNURL_STORAGE_IO;
}

static int knurl_fs_copy_fd(int source, int destination, knurl_job *job)
{
    unsigned char bytes[2048];
    ssize_t n, written;
    size_t offset;
    for (;;) {
        if (knurl_fs_cancelled(job))
            return 0;
        do {
            n = read(source, bytes, sizeof(bytes));
        } while (n < 0 && errno == EINTR);
        if (n <= 0)
            return n == 0 && fsync(destination) == 0;
        offset = 0;
        while (offset < (size_t)n) {
            written = write(destination, bytes + offset, (size_t)n - offset);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                return 0;
            offset += (size_t)written;
        }
    }
}

static int knurl_fs_direct_copy_file_at(int source_parent_fd,
                                        const char *source_name,
                                        int destination_parent_fd,
                                        const char *destination_name,
                                        size_t max_file_bytes, knurl_job *job)
{
    int source, destination, ok;
    struct stat st;
    source = openat(source_parent_fd, source_name,
                    O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (source < 0)
        return 0;
    if (fstat(source, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close(source);
        return 0;
    }
    if ((uint64_t)st.st_size > max_file_bytes) {
        close(source);
        errno = EFBIG;
        return 0;
    }
    destination =
        openat(destination_parent_fd, destination_name,
               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    ok = destination >= 0 && knurl_fs_copy_fd(source, destination, job);
    if (destination >= 0 && close(destination) != 0)
        ok = 0;
    close(source);
    if (!ok && destination >= 0)
        (void)unlinkat(destination_parent_fd, destination_name, 0);
    return ok;
}

static int knurl_fs_direct_copy_tree_at(int source_parent_fd,
                                        const char *source_name,
                                        int destination_parent_fd,
                                        const char *destination_name,
                                        size_t max_file_bytes, int shallow,
                                        knurl_job *job)
{
    knurl_fs_fd_scan *scan, *child, *parent;
    struct dirent *entry;
    struct stat st;
    if (fstatat(source_parent_fd, source_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return 0;
    if (S_ISREG(st.st_mode))
        return knurl_fs_direct_copy_file_at(
            source_parent_fd, source_name, destination_parent_fd,
            destination_name, max_file_bytes, job);
    if (!S_ISDIR(st.st_mode) ||
        mkdirat(destination_parent_fd, destination_name, 0700) != 0)
        return 0;
    if (shallow)
        return 1;
    scan = knurl_fs_fd_scan_new(source_parent_fd, source_name);
    if (scan != NULL)
        scan->destination_fd =
            knurl_fs_open_child_dir(destination_parent_fd, destination_name);
    if (scan == NULL || scan->destination_fd < 0)
        goto failed;
    while (scan != NULL) {
        if (knurl_fs_cancelled(job))
            goto failed;
        errno = 0;
        entry = readdir(scan->directory);
        if (entry == NULL) {
            if (errno != 0)
                goto failed;
            parent = scan->parent;
            (void)closedir(scan->directory);
            knurl_fs_fd_close(&scan->destination_fd);
            free(scan);
            scan = parent;
            continue;
        }
        if (knurl_fs_internal_entry(entry->d_name))
            continue;
        if (fstatat(dirfd(scan->directory), entry->d_name, &st,
                    AT_SYMLINK_NOFOLLOW) != 0)
            goto failed;
        if (S_ISREG(st.st_mode)) {
            if (!knurl_fs_direct_copy_file_at(
                    dirfd(scan->directory), entry->d_name, scan->destination_fd,
                    entry->d_name, max_file_bytes, job))
                goto failed;
        } else if (S_ISDIR(st.st_mode)) {
            if (mkdirat(scan->destination_fd, entry->d_name, 0700) != 0)
                goto failed;
            child = knurl_fs_fd_scan_new(dirfd(scan->directory), entry->d_name);
            if (child == NULL)
                goto failed;
            child->parent = scan;
            child->destination_fd =
                knurl_fs_open_child_dir(scan->destination_fd, entry->d_name);
            scan = child;
            if (scan->destination_fd < 0)
                goto failed;
        } else
            goto failed;
    }
    return 1;
failed:
    {
        int native_error = errno;
        knurl_fs_fd_scan_close(scan);
        (void)knurl_fs_direct_remove_tree_at(destination_parent_fd,
                                             destination_name);
        errno = native_error;
        return 0;
    }
}

static void knurl_fs_direct_recovery(const knurl_fs_config *config,
                                     const char *destination,
                                     const char *transaction)
{
    const char *slash = strrchr(destination, '/');
    size_t prefix = (size_t)(slash - destination);
    config->result->partial = 1;
    if (prefix + strlen(transaction) + 2 < KNURL_FS_STORAGE_PATH_MAX) {
        memcpy(config->recovery, destination, prefix);
        config->recovery[prefix] = '/';
        strcpy(config->recovery + prefix + 1, transaction);
        config->result->recovery_path = config->recovery;
    }
}

static knurl_storage_code
knurl_fs_direct_copy_or_move_fs(const knurl_fs_config *config,
                                const char *normalized_source,
                                const char *normalized_destination,
                                int overwrite, int remove_source, int shallow)
{
    char backup_leaf[KNURL_FS_PATH_MAX + 1u];
    char destination_leaf[KNURL_FS_PATH_MAX + 1u];
    char source_leaf[KNURL_FS_PATH_MAX + 1u];
    char stage_leaf[KNURL_FS_PATH_MAX + 1u];
    char txn_leaf[KNURL_FS_PATH_MAX + 1u];
    struct stat st;
    int backup_created;
    int destination_exists;
    int destination_parent_fd;
    int source_parent_fd;
    int txn_fd;
    int ok;
    knurl_storage_code failure = KNURL_STORAGE_IO;

    source_parent_fd =
        knurl_fs_open_parent_fd(config, normalized_source, 0, source_leaf);
    if (source_parent_fd < 0) {
        return errno == ENOENT ? KNURL_STORAGE_NOT_FOUND : KNURL_STORAGE_IO;
    }
    if (fstatat(source_parent_fd, source_leaf, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        knurl_fs_fd_close(&source_parent_fd);
        return errno == ENOENT ? KNURL_STORAGE_NOT_FOUND : KNURL_STORAGE_IO;
    }
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
        knurl_fs_fd_close(&source_parent_fd);
        return KNURL_STORAGE_IO;
    }
    destination_parent_fd = knurl_fs_open_parent_fd(
        config, normalized_destination, 0, destination_leaf);
    if (destination_parent_fd < 0) {
        knurl_fs_fd_close(&source_parent_fd);
        return KNURL_STORAGE_IO;
    }
    destination_exists = fstatat(destination_parent_fd, destination_leaf, &st,
                                 AT_SYMLINK_NOFOLLOW) == 0;
    if (destination_exists) {
        if (!overwrite) {
            knurl_fs_fd_close(&source_parent_fd);
            knurl_fs_fd_close(&destination_parent_fd);
            return KNURL_STORAGE_EXISTS;
        }
    } else if (errno != ENOENT) {
        knurl_fs_fd_close(&source_parent_fd);
        knurl_fs_fd_close(&destination_parent_fd);
        return KNURL_STORAGE_IO;
    }
    if (!overwrite && !destination_exists) {
        ok = knurl_fs_direct_copy_tree_at(
            source_parent_fd, source_leaf, destination_parent_fd,
            destination_leaf, config->max_file_bytes, shallow, config->job);
        if (!ok && errno == EFBIG)
            failure = KNURL_STORAGE_LIMIT;
        if (ok) {
            config->result->committed = 1;
            config->result->created = 1;
        }
        if (ok && remove_source) {
            ok = knurl_fs_direct_remove_tree_at(source_parent_fd, source_leaf);
            if (!ok) {
                config->result->committed = 1;
                config->result->partial = 1;
            }
            /* Removal may already have deleted source children. Keep the copy.
             */
        } else if (!ok) {
            (void)knurl_fs_direct_remove_tree_at(destination_parent_fd,
                                                 destination_leaf);
        }
        knurl_fs_fd_close(&source_parent_fd);
        knurl_fs_fd_close(&destination_parent_fd);
        return ok ? KNURL_STORAGE_OK : failure;
    }

    if (!knurl_fs_direct_reserve_dir_at(destination_parent_fd, ".vectis-txn",
                                        txn_leaf)) {
        knurl_fs_fd_close(&source_parent_fd);
        knurl_fs_fd_close(&destination_parent_fd);
        return KNURL_STORAGE_IO;
    }
    txn_fd = knurl_fs_open_child_dir(destination_parent_fd, txn_leaf);
    if (txn_fd < 0) {
        (void)knurl_fs_direct_remove_tree_at(destination_parent_fd, txn_leaf);
        knurl_fs_fd_close(&source_parent_fd);
        knurl_fs_fd_close(&destination_parent_fd);
        return KNURL_STORAGE_IO;
    }
    (void)snprintf(stage_leaf, sizeof(stage_leaf), "stage");
    (void)snprintf(backup_leaf, sizeof(backup_leaf), "backup");
    backup_created = 0;
    ok = knurl_fs_direct_copy_tree_at(source_parent_fd, source_leaf, txn_fd,
                                      stage_leaf, config->max_file_bytes,
                                      shallow, config->job);
    if (!ok) {
        if (errno == EFBIG)
            failure = KNURL_STORAGE_LIMIT;
        (void)knurl_fs_direct_remove_tree_at(destination_parent_fd, txn_leaf);
        knurl_fs_fd_close(&txn_fd);
        knurl_fs_fd_close(&source_parent_fd);
        knurl_fs_fd_close(&destination_parent_fd);
        return failure;
    }
    if (destination_exists) {
        if (renameat(destination_parent_fd, destination_leaf, txn_fd,
                     backup_leaf) != 0) {
            (void)knurl_fs_direct_remove_tree_at(destination_parent_fd,
                                                 txn_leaf);
            knurl_fs_fd_close(&txn_fd);
            knurl_fs_fd_close(&source_parent_fd);
            knurl_fs_fd_close(&destination_parent_fd);
            return KNURL_STORAGE_IO;
        }
        backup_created = 1;
    }
    if (renameat(txn_fd, stage_leaf, destination_parent_fd, destination_leaf) !=
        0) {
        if (backup_created &&
            renameat(txn_fd, backup_leaf, destination_parent_fd,
                     destination_leaf) != 0) {
            const char *slash = strrchr(normalized_destination, '/');
            size_t prefix = (size_t)(slash - normalized_destination);
            config->result->partial = 1;
            if (prefix + strlen(txn_leaf) + 2 < KNURL_FS_STORAGE_PATH_MAX) {
                memcpy(config->recovery, normalized_destination, prefix);
                config->recovery[prefix] = '/';
                strcpy(config->recovery + prefix + 1, txn_leaf);
                config->result->recovery_path = config->recovery;
            }
        } else
            (void)knurl_fs_direct_remove_tree_at(destination_parent_fd,
                                                 txn_leaf);
        knurl_fs_fd_close(&txn_fd);
        knurl_fs_fd_close(&source_parent_fd);
        knurl_fs_fd_close(&destination_parent_fd);
        return KNURL_STORAGE_IO;
    }
    config->result->committed = 1;
    config->result->created = !destination_exists;
    if (ok && remove_source) {
        ok = knurl_fs_direct_remove_tree_at(source_parent_fd, source_leaf);
        if (!ok) {
            config->result->committed = 1;
            config->result->partial = 1;
            if (backup_created) {
                const char *slash = strrchr(normalized_destination, '/');
                size_t prefix = (size_t)(slash - normalized_destination);
                if (prefix + strlen(txn_leaf) + 2 < KNURL_FS_STORAGE_PATH_MAX) {
                    memcpy(config->recovery, normalized_destination, prefix);
                    config->recovery[prefix] = '/';
                    strcpy(config->recovery + prefix + 1, txn_leaf);
                    config->result->recovery_path = config->recovery;
                }
            }
            /* The source may be partially removed. Preserve the complete new
             * destination and any overwritten destination in txn/backup for
             * recovery; neither copy is safe to discard or roll back here. */
            knurl_fs_fd_close(&txn_fd);
            if (!backup_created) {
                (void)knurl_fs_direct_remove_tree_at(destination_parent_fd,
                                                     txn_leaf);
            }
            knurl_fs_fd_close(&source_parent_fd);
            knurl_fs_fd_close(&destination_parent_fd);
            return KNURL_STORAGE_IO;
        }
    }
    config->result->committed = 1;
    config->result->created = !destination_exists;
    if (backup_created &&
        !knurl_fs_direct_remove_tree_at(txn_fd, backup_leaf)) {
        knurl_fs_direct_recovery(config, normalized_destination, txn_leaf);
        knurl_fs_fd_close(&txn_fd);
        knurl_fs_fd_close(&source_parent_fd);
        knurl_fs_fd_close(&destination_parent_fd);
        return KNURL_STORAGE_IO;
    }
    knurl_fs_fd_close(&txn_fd);
    (void)knurl_fs_direct_remove_tree_at(destination_parent_fd, txn_leaf);
    knurl_fs_fd_close(&source_parent_fd);
    knurl_fs_fd_close(&destination_parent_fd);
    return ok ? KNURL_STORAGE_OK : failure;
}

int knurl_fs_path_normalize(const char *path, char out[KNURL_FS_PATH_MAX + 1u])
{
    size_t input;
    size_t output;
    size_t segment_start;

    if (path == NULL || out == NULL || path[0] != '/') {
        return 0;
    }
    input = 1u;
    output = 1u;
    out[0] = '/';
    while (path[input] != '\0') {
        while (path[input] == '/') {
            input++;
        }
        if (path[input] == '\0') {
            break;
        }
        segment_start = input;
        while (path[input] != '\0' && path[input] != '/') {
            unsigned char value;

            value = (unsigned char)path[input];
            if (!(isalnum(value) || value == '.' || value == '_' ||
                  value == '-')) {
                return 0;
            }
            input++;
        }
        if (input == segment_start ||
            (input - segment_start == 1u && path[segment_start] == '.') ||
            (input - segment_start == 2u && path[segment_start] == '.' &&
             path[segment_start + 1u] == '.')) {
            return 0;
        }
        if (output > 1u) {
            if (output + 1u >= KNURL_FS_PATH_MAX + 1u) {
                return 0;
            }
            out[output++] = '/';
        }
        if (output + input - segment_start >= KNURL_FS_PATH_MAX + 1u) {
            return 0;
        }
        memcpy(out + output, path + segment_start, input - segment_start);
        output += input - segment_start;
    }
    out[output] = '\0';
    return 1;
}

knurl_storage_code knurl_fs_lookup(const knurl_fs_config *config,
                                   const char *path, knurl_fs_entry *entry)
{
    char normalized[KNURL_FS_PATH_MAX + 1u];
    char disk[KNURL_FS_STORAGE_PATH_MAX];
    struct stat st;

    if (entry == NULL || !knurl_fs_path_normalize(path, normalized) ||
        !knurl_fs_config_valid(config)) {
        return KNURL_STORAGE_INVALID;
    }
    memset(entry, 0, sizeof(*entry));
    if (knurl_fs_tombstone_exists(config, normalized)) {
        entry->kind = KNURL_FS_ENTRY_TOMBSTONE;
        return KNURL_STORAGE_OK;
    }
    if (!knurl_fs_disk_path(config, "content", normalized, disk)) {
        return KNURL_STORAGE_INVALID;
    }
    if (knurl_fs_direct_root(config)) {
        int fd;

        fd = knurl_fs_direct_open_existing(config, normalized, 0, &st);
        if (fd >= 0) {
            knurl_fs_fd_close(&fd);
            entry->kind = KNURL_FS_ENTRY_FILE;
            entry->size = (size_t)st.st_size;
            strcpy(entry->storage_path, disk);
            return KNURL_STORAGE_OK;
        }
        fd = knurl_fs_direct_open_existing(config, normalized, 1, NULL);
        if (fd >= 0) {
            knurl_fs_fd_close(&fd);
            entry->kind = KNURL_FS_ENTRY_COLLECTION;
            return KNURL_STORAGE_OK;
        }
        return errno == ENOENT || errno == ENOTDIR ? KNURL_STORAGE_NOT_FOUND
                                                   : KNURL_STORAGE_IO;
    }
    if (knurl_fs_file_regular(disk, &st)) {
        entry->kind = KNURL_FS_ENTRY_FILE;
        entry->size = (size_t)st.st_size;
        strcpy(entry->storage_path, disk);
        return KNURL_STORAGE_OK;
    }
    if (knurl_fs_file_directory(disk)) {
        entry->kind = KNURL_FS_ENTRY_COLLECTION;
        return KNURL_STORAGE_OK;
    }
    return KNURL_STORAGE_NOT_FOUND;
}

/* Call with the mutation lock held, before staging or creating content. */
knurl_storage_code knurl_fs_check_parent(const knurl_fs_config *config,
                                         const char *normalized)
{
    char parent[KNURL_FS_PATH_MAX + 1u];
    char *slash;
    knurl_fs_entry entry;
    knurl_storage_code status;

    strcpy(parent, normalized);
    slash = strrchr(parent, '/');
    slash[slash == parent ? 1 : 0] = '\0';
    status = knurl_fs_lookup(config, parent, &entry);
    if (status == KNURL_STORAGE_NOT_FOUND ||
        (status == KNURL_STORAGE_OK &&
         entry.kind != KNURL_FS_ENTRY_COLLECTION)) {
        return KNURL_STORAGE_CONFLICT;
    }
    return status;
}

int knurl_fs_etag_fd(int fd, char out[67], knurl_job *job)
{
    EVP_MD_CTX *ctx;
    unsigned char buffer[2048], digest[32];
    unsigned int size;
    size_t i;
    off_t offset;
    ssize_t count;
    int ok;
    ctx = EVP_MD_CTX_new();
    if (ctx == NULL)
        return 0;
    ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1;
    offset = 0;
    while (ok) {
        if (knurl_fs_cancelled(job)) {
            ok = 0;
            break;
        }
        count = pread(fd, buffer, sizeof(buffer), offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            ok = count == 0;
            break;
        }
        ok = EVP_DigestUpdate(ctx, buffer, (size_t)count) == 1;
        offset += count;
    }
    if (ok)
        ok = EVP_DigestFinal_ex(ctx, digest, &size) == 1 && size == 32u;
    EVP_MD_CTX_free(ctx);
    if (!ok)
        return 0;
    out[0] = '"';
    for (i = 0; i < 32u; ++i) {
        out[1u + i * 2u] = knurl_fs_hex[digest[i] >> 4u];
        out[2u + i * 2u] = knurl_fs_hex[digest[i] & 15u];
    }
    out[65] = '"';
    out[66] = '\0';
    return 1;
}

/* Parse complete lists even after a match, so malformed tails fail closed. */
int knurl_fs_tag_matches(const char *p, const char *etag, int exists,
                         int strong)
{
    const char *start;
    size_t length;
    int matched, weak;
    matched = 0;
    while (*p == ' ' || *p == '\t')
        ++p;
    if (*p == '*') {
        ++p;
        while (*p == ' ' || *p == '\t')
            ++p;
        return *p == '\0' ? exists : -1;
    }
    for (;;) {
        weak = p[0] == 'W' && p[1] == '/';
        if (weak)
            p += 2;
        if (*p != '"')
            return -1;
        start = p++;
        while (*p != '"') {
            if ((unsigned char)*p < 0x21u || (unsigned char)*p == 0x7fu)
                return -1;
            ++p;
        }
        ++p;
        length = (size_t)(p - start);
        if (exists && (!strong || !weak) && strlen(etag) == length &&
            memcmp(start, etag, length) == 0)
            matched = 1;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p == '\0')
            return matched;
        if (*p++ != ',')
            return -1;
        while (*p == ' ' || *p == '\t')
            ++p;
    }
}

knurl_storage_code knurl_fs_check_conditions(const knurl_fs_config *config,
                                             const char *path,
                                             const char *if_match,
                                             const char *if_none_match)
{
    knurl_fs_entry entry;
    knurl_storage_code status;
    char etag[67];
    int exists, fd, match, none;
    if (if_match == NULL && if_none_match == NULL)
        return KNURL_STORAGE_OK;
    etag[0] = '\0';
    status = knurl_fs_lookup(config, path, &entry);
    if (status != KNURL_STORAGE_OK && status != KNURL_STORAGE_NOT_FOUND)
        return status;
    exists =
        status == KNURL_STORAGE_OK && (entry.kind == KNURL_FS_ENTRY_FILE ||
                                       entry.kind == KNURL_FS_ENTRY_COLLECTION);
    if (exists && entry.kind == KNURL_FS_ENTRY_FILE) {
        fd = knurl_fs_direct_root(config)
                 ? knurl_fs_direct_open_existing(config, path, 0, NULL)
                 : open(entry.storage_path,
                        O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            return KNURL_STORAGE_IO;
        match = knurl_fs_etag_fd(fd, etag, config->job);
        (void)close(fd);
        if (!match)
            return KNURL_STORAGE_IO;
    }
    match =
        if_match == NULL ? 1 : knurl_fs_tag_matches(if_match, etag, exists, 1);
    none = if_none_match == NULL
               ? 0
               : knurl_fs_tag_matches(if_none_match, etag, exists, 0);
    if (match < 0 || none < 0)
        return KNURL_STORAGE_INVALID;
    return !match || none ? KNURL_STORAGE_PRECONDITION : KNURL_STORAGE_OK;
}

static int knurl_fs_remove_tree(const char *path)
{
    knurl_fs_path_scan *scan, *child, *parent;
    struct dirent *entry;
    struct stat st;
    int written, ok;
    if (path == NULL)
        return 0;
    if (lstat(path, &st) == -1)
        return errno == ENOENT;
    if (S_ISREG(st.st_mode))
        return unlink(path) == 0;
    if (!S_ISDIR(st.st_mode))
        return 0;
    scan = knurl_fs_path_scan_new(path);
    if (scan == NULL)
        return 0;
    while (scan != NULL) {
        errno = 0;
        entry = readdir(scan->directory);
        if (entry == NULL) {
            if (errno != 0)
                break;
            parent = scan->parent;
            (void)closedir(scan->directory);
            ok = rmdir(scan->path) == 0;
            free(scan);
            scan = parent;
            if (!ok) {
                knurl_fs_path_scan_close(scan);
                return 0;
            }
            continue;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        written = snprintf(scan->child, sizeof(scan->child), "%s/%s",
                           scan->path, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(scan->child) ||
            lstat(scan->child, &st) == -1)
            break;
        if (S_ISREG(st.st_mode)) {
            if (unlink(scan->child) != 0)
                break;
        } else if (S_ISDIR(st.st_mode)) {
            child = knurl_fs_path_scan_new(scan->child);
            if (child == NULL)
                break;
            child->parent = scan;
            scan = child;
        } else
            break;
    }
    if (scan == NULL)
        return 1;
    knurl_fs_path_scan_close(scan);
    return 0;
}

static int knurl_fs_disk_usage(const char *path, uint64_t *total,
                               uint64_t *resources)
{
    struct stat st;

    if (path == NULL || total == NULL || resources == NULL ||
        lstat(path, &st) == -1) {
        return errno == ENOENT;
    }
    if (S_ISREG(st.st_mode)) {
        if (st.st_size < 0 || ((uint64_t)-1) - *total < (uint64_t)st.st_size ||
            *resources == ((uint64_t)-1)) {
            return 0;
        }
        *total += (uint64_t)st.st_size;
        (*resources)++;
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (*resources == ((uint64_t)-1)) {
            return 0;
        }
        (*resources)++;
        return knurl_fs_usage_path(path, total, resources, 0);
    }
    return 0;
}

static int knurl_fs_copy_file(const char *source_path,
                              const char *destination_path, knurl_job *job,
                              size_t max_file_bytes)
{
    int source, destination, ok;
    struct stat st;
    source = open(source_path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (source < 0)
        return 0;
    if (fstat(source, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close(source);
        return 0;
    }
    if ((uint64_t)st.st_size > max_file_bytes) {
        close(source);
        errno = EFBIG;
        return 0;
    }
    destination =
        open(destination_path,
             O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    ok = destination >= 0 && knurl_fs_copy_fd(source, destination, job);
    if (destination >= 0 && close(destination) != 0)
        ok = 0;
    close(source);
    if (!ok && destination >= 0)
        (void)unlink(destination_path);
    return ok;
}

typedef struct knurl_fs_copy_scan {
    struct knurl_fs_copy_scan *parent;
    DIR *directory;
    const char *source, *destination;
    char source_child[KNURL_FS_STORAGE_PATH_MAX];
    char destination_child[KNURL_FS_STORAGE_PATH_MAX];
} knurl_fs_copy_scan;

static knurl_fs_copy_scan *knurl_fs_copy_scan_new(const char *source,
                                                  const char *destination)
{
    DIR *directory;
    knurl_fs_copy_scan *scan;
    directory = opendir(source);
    if (directory == NULL)
        return NULL;
    scan = (knurl_fs_copy_scan *)malloc(sizeof(*scan));
    if (scan == NULL) {
        (void)closedir(directory);
        return NULL;
    }
    scan->parent = NULL;
    scan->directory = directory;
    scan->source = source;
    scan->destination = destination;
    return scan;
}

static int knurl_fs_copy_tree(const char *source, const char *destination,
                              knurl_job *job, size_t max_file_bytes)
{
    knurl_fs_copy_scan *scan, *child, *parent;
    struct dirent *entry;
    struct stat st;
    int a, b, native_error;
    if (source == NULL || destination == NULL || lstat(source, &st) == -1)
        return 0;
    if (S_ISREG(st.st_mode))
        return knurl_fs_copy_file(source, destination, job, max_file_bytes);
    if (!S_ISDIR(st.st_mode) || mkdir(destination, 0700) == -1)
        return 0;
    scan = knurl_fs_copy_scan_new(source, destination);
    if (scan == NULL) {
        (void)rmdir(destination);
        return 0;
    }
    while (scan != NULL) {
        if (knurl_fs_cancelled(job))
            break;
        errno = 0;
        entry = readdir(scan->directory);
        if (entry == NULL) {
            if (errno != 0)
                break;
            parent = scan->parent;
            (void)closedir(scan->directory);
            free(scan);
            scan = parent;
            continue;
        }
        if (knurl_fs_internal_entry(entry->d_name))
            continue;
        a = snprintf(scan->source_child, sizeof(scan->source_child), "%s/%s",
                     scan->source, entry->d_name);
        b = snprintf(scan->destination_child, sizeof(scan->destination_child),
                     "%s/%s", scan->destination, entry->d_name);
        if (a < 0 || (size_t)a >= sizeof(scan->source_child) || b < 0 ||
            (size_t)b >= sizeof(scan->destination_child) ||
            lstat(scan->source_child, &st) == -1)
            break;
        if (S_ISREG(st.st_mode)) {
            if (!knurl_fs_copy_file(scan->source_child, scan->destination_child,
                                    job, max_file_bytes))
                break;
        } else if (S_ISDIR(st.st_mode)) {
            if (mkdir(scan->destination_child, 0700) == -1)
                break;
            child = knurl_fs_copy_scan_new(scan->source_child,
                                           scan->destination_child);
            if (child == NULL)
                break;
            child->parent = scan;
            scan = child;
        } else
            break;
    }
    if (scan == NULL)
        return 1;
    native_error = errno;
    while (scan != NULL) {
        parent = scan->parent;
        (void)closedir(scan->directory);
        free(scan);
        scan = parent;
    }
    (void)knurl_fs_remove_tree(destination);
    errno = native_error;
    return 0;
}

static int knurl_fs_transaction_dir(const knurl_fs_config *config,
                                    char out[KNURL_FS_STORAGE_PATH_MAX])
{
    char base[KNURL_FS_STORAGE_PATH_MAX];
    int written;

    if (!knurl_fs_base(config, base, sizeof(base))) {
        return 0;
    }
    written = snprintf(out, KNURL_FS_STORAGE_PATH_MAX, "%s/.transaction-XXXXXX",
                       base);
    return written >= 0 && (size_t)written < KNURL_FS_STORAGE_PATH_MAX &&
           mkdtemp(out) != NULL;
}

static int knurl_fs_transaction_path(const char *transaction, const char *name,
                                     char out[KNURL_FS_STORAGE_PATH_MAX])
{
    int written;

    if (transaction == NULL || name == NULL) {
        return 0;
    }
    written =
        snprintf(out, KNURL_FS_STORAGE_PATH_MAX, "%s/%s", transaction, name);
    return written >= 0 && (size_t)written < KNURL_FS_STORAGE_PATH_MAX;
}

static int knurl_fs_move_to_backup(const char *live, const char *backup,
                                   int *moved)
{
    struct stat st;

    if (live == NULL || backup == NULL || moved == NULL) {
        return 0;
    }
    *moved = 0;
    if (lstat(live, &st) == -1) {
        return errno == ENOENT;
    }
    if (rename(live, backup) == -1) {
        return 0;
    }
    *moved = 1;
    return 1;
}

static int knurl_fs_restore_backup(const char *live, const char *backup,
                                   int moved)
{
    if (live == NULL || backup == NULL || !knurl_fs_remove_tree(live))
        return 0;
    return !moved || rename(backup, live) == 0;
}

static int
knurl_fs_create_destination_parent(const char *content_root,
                                   const char *destination,
                                   char created[KNURL_FS_STORAGE_PATH_MAX])
{
    char candidate[KNURL_FS_STORAGE_PATH_MAX];
    char parent[KNURL_FS_STORAGE_PATH_MAX];
    struct stat st;

    if (content_root == NULL || destination == NULL || created == NULL ||
        !knurl_fs_parent_dir(destination, candidate, sizeof(candidate))) {
        return 0;
    }
    created[0] = '\0';
    while (strcmp(candidate, content_root) != 0) {
        if (lstat(candidate, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) {
                return 0;
            }
            break;
        }
        if (errno != ENOENT) {
            return 0;
        }
        strcpy(created, candidate);
        if (!knurl_fs_parent_dir(candidate, parent, sizeof(parent))) {
            return 0;
        }
        strcpy(candidate, parent);
    }
    if (strcmp(candidate, content_root) == 0 &&
        !knurl_fs_file_directory(content_root)) {
        return 0;
    }
    return created[0] == '\0' || knurl_fs_mkdir_p(created);
}

knurl_storage_code knurl_fs_delete_conditional(const knurl_fs_config *config,
                                               const char *path,
                                               const char *if_match,
                                               const char *if_none_match)
{
    char normalized[KNURL_FS_PATH_MAX + 1u];
    char content[KNURL_FS_STORAGE_PATH_MAX];
    char tombstone[KNURL_FS_STORAGE_PATH_MAX];
    char tombstone_root[KNURL_FS_STORAGE_PATH_MAX];
    uint64_t usage;
    uint64_t resources;
    uint64_t additional_resources;
    uint64_t removed_resources;
    uint64_t removed_usage;
    knurl_storage_code status;
    int lock_fd;

    if (!knurl_fs_config_valid(config) ||
        !knurl_fs_path_normalize(path, normalized) || normalized[1] == '\0') {
        return KNURL_STORAGE_INVALID;
    }
    lock_fd = knurl_fs_lock(config);
    if (lock_fd < 0) {
        return KNURL_STORAGE_IO;
    }
    status =
        knurl_fs_check_conditions(config, normalized, if_match, if_none_match);
    if (status == KNURL_STORAGE_OK)
        status = knurl_fs_scope(config, normalized, NULL, 1, 0);
    if (status != KNURL_STORAGE_OK) {
        knurl_fs_unlock(lock_fd);
        return status;
    }
    if (knurl_fs_ancestor_tombstone_exists(config, normalized)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_CONFLICT;
    }
    if (!knurl_fs_disk_path(config, "content", normalized, content) ||
        !knurl_fs_usage(config, &usage, &resources)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    if (knurl_fs_direct_root(config)) {
        (void)usage;
        (void)resources;
        status = knurl_fs_direct_remove(config, normalized);
        if (status == KNURL_STORAGE_IO)
            config->result->partial = 1;
        if (status != KNURL_STORAGE_OK) {
            knurl_fs_unlock(lock_fd);
            return status;
        }
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_OK;
    }
    if (!knurl_fs_disk_path(config, "tombstones", normalized, tombstone) ||
        !knurl_fs_disk_path(config, "tombstones", "/", tombstone_root)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    removed_usage = 0u;
    removed_resources = 0u;
    if (!knurl_fs_disk_usage(content, &removed_usage, &removed_resources) ||
        resources < removed_resources) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    resources -= removed_resources;
    removed_usage = 0u;
    removed_resources = 0u;
    if (!knurl_fs_disk_usage(tombstone, &removed_usage, &removed_resources) ||
        resources < removed_resources) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    resources -= removed_resources;
    additional_resources = 1u; /* Replacement tombstone plus missing parents. */
    if (!knurl_fs_missing_path_resources(tombstone_root, tombstone, 0,
                                         &additional_resources)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    if (!knurl_fs_resources_within_limit(config, resources,
                                         additional_resources)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_LIMIT;
    }
    if (!knurl_fs_clear_tombstones(config, normalized) ||
        !knurl_fs_touch_atomic(tombstone)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    config->result->committed = 1;
    if (!knurl_fs_remove_tree(content)) {
        config->result->partial = 1;
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    knurl_fs_unlock(lock_fd);
    return KNURL_STORAGE_OK;
}

knurl_storage_code knurl_fs_mkcol_conditional(const knurl_fs_config *config,
                                              const char *path,
                                              const char *if_match,
                                              const char *if_none_match)
{
    char normalized[KNURL_FS_PATH_MAX + 1u];
    char disk[KNURL_FS_STORAGE_PATH_MAX];
    uint64_t usage;
    uint64_t resources;
    knurl_storage_code status;
    int lock_fd;

    if (!knurl_fs_config_valid(config) ||
        !knurl_fs_path_normalize(path, normalized) || normalized[1] == '\0') {
        return KNURL_STORAGE_INVALID;
    }
    lock_fd = knurl_fs_lock(config);
    if (lock_fd < 0) {
        return KNURL_STORAGE_IO;
    }
    status =
        knurl_fs_check_conditions(config, normalized, if_match, if_none_match);
    if (status != KNURL_STORAGE_OK) {
        knurl_fs_unlock(lock_fd);
        return status;
    }
    if (knurl_fs_ancestor_tombstone_exists(config, normalized)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_CONFLICT;
    }
    status = knurl_fs_check_parent(config, normalized);
    if (status != KNURL_STORAGE_OK) {
        knurl_fs_unlock(lock_fd);
        return status;
    }
    if (!knurl_fs_disk_path(config, "content", normalized, disk) ||
        !knurl_fs_usage(config, &usage, &resources) ||
        !knurl_fs_subtract_tombstone_usage(config, normalized, &usage,
                                           &resources)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    if (knurl_fs_file_regular(disk, NULL) || knurl_fs_file_directory(disk)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_EXISTS;
    }
    if (!knurl_fs_resources_within_limit(config, resources, 1u)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_LIMIT;
    }
    if (knurl_fs_direct_root(config)) {
        status = knurl_fs_direct_mkcol(config, normalized);
        if (status != KNURL_STORAGE_OK) {
            knurl_fs_unlock(lock_fd);
            return status;
        }
    } else if (mkdir(disk, 0700) != 0 ||
               !knurl_fs_clear_tombstones(config, normalized)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    knurl_fs_unlock(lock_fd);
    return KNURL_STORAGE_OK;
}

typedef struct knurl_fs_copy_or_move_direct_scratch {
    knurl_fs_entry source_entry;
    knurl_fs_entry destination_entry;
    char normalized_source[KNURL_FS_PATH_MAX + 1u];
    char normalized_destination[KNURL_FS_PATH_MAX + 1u];
    char source_disk[KNURL_FS_STORAGE_PATH_MAX];
    char destination_disk[KNURL_FS_STORAGE_PATH_MAX];
    char content_root[KNURL_FS_STORAGE_PATH_MAX];
} knurl_fs_copy_or_move_direct_scratch;

static knurl_storage_code knurl_fs_copy_or_move_direct_with_scratch(
    const knurl_fs_config *config, const char *source, const char *destination,
    int overwrite, int remove_source, int shallow, const char *if_match,
    const char *if_none_match, int *created,
    knurl_fs_copy_or_move_direct_scratch *scratch)
{

    uint64_t usage;
    uint64_t resources;
    uint64_t source_usage;
    uint64_t destination_usage;
    uint64_t source_resources;
    uint64_t destination_resources;
    uint64_t additional_resources;
    knurl_storage_code status;
    int lock_fd;
    int is_new;

    if (created != NULL)
        *created = 0;

    if (!knurl_fs_config_valid(config) || !knurl_fs_direct_root(config) ||
        !knurl_fs_path_normalize(source, scratch->normalized_source) ||
        !knurl_fs_path_normalize(destination,
                                 scratch->normalized_destination) ||
        scratch->normalized_source[1] == '\0' ||
        scratch->normalized_destination[1] == '\0' ||
        strcmp(scratch->normalized_source, scratch->normalized_destination) ==
            0 ||
        (strncmp(scratch->normalized_source, scratch->normalized_destination,
                 strlen(scratch->normalized_destination)) == 0 &&
         scratch->normalized_source[strlen(scratch->normalized_destination)] ==
             '/')) {
        return KNURL_STORAGE_INVALID;
    }
    lock_fd = knurl_fs_lock(config);
    if (lock_fd < 0) {
        return KNURL_STORAGE_IO;
    }
    status = knurl_fs_check_conditions(config, scratch->normalized_source,
                                       if_match, if_none_match);
    if (status == KNURL_STORAGE_OK)
        status = knurl_fs_check_parent(config, scratch->normalized_destination);
    if (status == KNURL_STORAGE_OK)
        status = knurl_fs_scope(config, scratch->normalized_source,
                                scratch->normalized_destination,
                                remove_source || !shallow, overwrite);
    if (status != KNURL_STORAGE_OK) {
        knurl_fs_unlock(lock_fd);
        return status;
    }
    status = knurl_fs_lookup(config, scratch->normalized_source,
                             &scratch->source_entry);
    if (status != KNURL_STORAGE_OK ||
        (scratch->source_entry.kind != KNURL_FS_ENTRY_FILE &&
         scratch->source_entry.kind != KNURL_FS_ENTRY_COLLECTION)) {
        knurl_fs_unlock(lock_fd);
        return status == KNURL_STORAGE_OK ? KNURL_STORAGE_TOMBSTONED : status;
    }
    if (remove_source && shallow &&
        scratch->source_entry.kind == KNURL_FS_ENTRY_COLLECTION) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_INVALID;
    }
    if (scratch->source_entry.kind == KNURL_FS_ENTRY_COLLECTION &&
        strncmp(scratch->normalized_destination, scratch->normalized_source,
                strlen(scratch->normalized_source)) == 0 &&
        scratch->normalized_destination[strlen(scratch->normalized_source)] ==
            '/') {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_INVALID;
    }
    status = knurl_fs_lookup(config, scratch->normalized_destination,
                             &scratch->destination_entry);
    is_new = (status == KNURL_STORAGE_NOT_FOUND) ||
             (status == KNURL_STORAGE_OK &&
              scratch->destination_entry.kind == KNURL_FS_ENTRY_TOMBSTONE);
    if (!overwrite &&
        ((status == KNURL_STORAGE_OK &&
          scratch->destination_entry.kind != KNURL_FS_ENTRY_TOMBSTONE))) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_EXISTS;
    }
    if (status == KNURL_STORAGE_OK &&
        scratch->destination_entry.kind == KNURL_FS_ENTRY_COLLECTION &&
        scratch->source_entry.kind == KNURL_FS_ENTRY_FILE) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_CONFLICT;
    }
    if (status != KNURL_STORAGE_OK && status != KNURL_STORAGE_NOT_FOUND) {
        knurl_fs_unlock(lock_fd);
        return status;
    }
    if (!knurl_fs_disk_path(config, "content", scratch->normalized_source,
                            scratch->source_disk) ||
        !knurl_fs_disk_path(config, "content", scratch->normalized_destination,
                            scratch->destination_disk) ||
        !knurl_fs_disk_path(config, "content", "/", scratch->content_root) ||
        !knurl_fs_usage(config, &usage, &resources)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    source_usage = 0u;
    destination_usage = 0u;
    shallow =
        shallow && scratch->source_entry.kind == KNURL_FS_ENTRY_COLLECTION;
    source_resources = shallow ? 1u : 0u;
    destination_resources = 0u;
    if ((!shallow && !knurl_fs_disk_usage(scratch->source_disk, &source_usage,
                                          &source_resources)) ||
        !knurl_fs_disk_usage(scratch->destination_disk, &destination_usage,
                             &destination_resources) ||
        usage < destination_usage ||
        (!remove_source &&
         (usage - destination_usage > config->max_total_bytes ||
          source_usage >
              config->max_total_bytes - (usage - destination_usage)))) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_LIMIT;
    }
    if (resources < destination_resources) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    resources -= destination_resources;
    additional_resources = 0u;
    if (!knurl_fs_missing_path_resources(scratch->content_root,
                                         scratch->destination_disk, 0,
                                         &additional_resources)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    if (!remove_source &&
        !knurl_fs_add_resources(&additional_resources, source_resources)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_LIMIT;
    }
    if (!knurl_fs_resources_within_limit(config, resources,
                                         additional_resources)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_LIMIT;
    }
    status = knurl_fs_direct_copy_or_move_fs(config, scratch->normalized_source,
                                             scratch->normalized_destination,
                                             overwrite, remove_source, shallow);
    if (status == KNURL_STORAGE_OK && created != NULL)
        *created = is_new;
    knurl_fs_unlock(lock_fd);
    return status;
}

static knurl_storage_code knurl_fs_copy_or_move_direct(
    const knurl_fs_config *config, const char *source, const char *destination,
    int overwrite, int remove_source, int shallow, const char *if_match,
    const char *if_none_match, int *created)
{
    knurl_fs_copy_or_move_direct_scratch *scratch;
    knurl_storage_code result;
    scratch = (knurl_fs_copy_or_move_direct_scratch *)malloc(sizeof(*scratch));
    if (scratch == NULL) {
        if (created != NULL)
            *created = 0;
        return KNURL_STORAGE_NOMEM;
    }
    result = knurl_fs_copy_or_move_direct_with_scratch(
        config, source, destination, overwrite, remove_source, shallow,
        if_match, if_none_match, created, scratch);
    free(scratch);
    return result;
}

typedef struct knurl_fs_copy_or_move_scratch {
    knurl_fs_entry source_entry;
    knurl_fs_entry destination_entry;
    char normalized_source[KNURL_FS_PATH_MAX + 1u];
    char normalized_destination[KNURL_FS_PATH_MAX + 1u];
    char source_disk[KNURL_FS_STORAGE_PATH_MAX];
    char destination_disk[KNURL_FS_STORAGE_PATH_MAX];
    char content_root[KNURL_FS_STORAGE_PATH_MAX];
    char tombstone_root[KNURL_FS_STORAGE_PATH_MAX];
    char source_tombstone[KNURL_FS_STORAGE_PATH_MAX];
    char destination_tombstone[KNURL_FS_STORAGE_PATH_MAX];
    char transaction[KNURL_FS_STORAGE_PATH_MAX];
    char staged_content[KNURL_FS_STORAGE_PATH_MAX];
    char staged_tombstones[KNURL_FS_STORAGE_PATH_MAX];
    char staged_source_tombstone[KNURL_FS_STORAGE_PATH_MAX];
    char staged_destination_tombstone[KNURL_FS_STORAGE_PATH_MAX];
    char staged_destination_tombstone_parent[KNURL_FS_STORAGE_PATH_MAX];
    char backup_destination[KNURL_FS_STORAGE_PATH_MAX];
    char backup_source[KNURL_FS_STORAGE_PATH_MAX];
    char backup_tombstones[KNURL_FS_STORAGE_PATH_MAX];
    char created_destination_parent[KNURL_FS_STORAGE_PATH_MAX];
} knurl_fs_copy_or_move_scratch;

static knurl_storage_code knurl_fs_copy_or_move_with_scratch(
    const knurl_fs_config *config, const char *source, const char *destination,
    int overwrite, int remove_source, int shallow, const char *if_match,
    const char *if_none_match, int *created,
    knurl_fs_copy_or_move_scratch *scratch)
{

    struct stat source_tombstone_stat;
    uint64_t usage;
    uint64_t resources;
    uint64_t source_usage;
    uint64_t destination_usage;
    uint64_t source_resources;
    uint64_t destination_resources;
    uint64_t source_tombstone_usage;
    uint64_t source_tombstone_resources;
    uint64_t destination_tombstone_usage;
    uint64_t destination_tombstone_resources;
    uint64_t additional_resources;
    knurl_storage_code status;
    int destination_installed;
    int destination_moved;
    int source_moved;
    int source_tombstone_present;
    int tombstones_moved;
    int recovery_needed = 0;
    int lock_fd;
    int is_new;

    if (created != NULL)
        *created = 0;

    if (!knurl_fs_config_valid(config) ||
        !knurl_fs_path_normalize(source, scratch->normalized_source) ||
        !knurl_fs_path_normalize(destination,
                                 scratch->normalized_destination) ||
        scratch->normalized_source[1] == '\0' ||
        scratch->normalized_destination[1] == '\0' ||
        strcmp(scratch->normalized_source, scratch->normalized_destination) ==
            0 ||
        (strncmp(scratch->normalized_source, scratch->normalized_destination,
                 strlen(scratch->normalized_destination)) == 0 &&
         scratch->normalized_source[strlen(scratch->normalized_destination)] ==
             '/')) {
        return KNURL_STORAGE_INVALID;
    }
    if (knurl_fs_direct_root(config)) {
        return knurl_fs_copy_or_move_direct(config, source, destination,
                                            overwrite, remove_source, shallow,
                                            if_match, if_none_match, created);
    }
    lock_fd = knurl_fs_lock(config);
    if (lock_fd < 0) {
        return KNURL_STORAGE_IO;
    }
    status = knurl_fs_check_conditions(config, scratch->normalized_source,
                                       if_match, if_none_match);
    if (status == KNURL_STORAGE_OK)
        status = knurl_fs_check_parent(config, scratch->normalized_destination);
    if (status == KNURL_STORAGE_OK)
        status = knurl_fs_scope(config, scratch->normalized_source,
                                scratch->normalized_destination,
                                remove_source || !shallow, overwrite);
    if (status != KNURL_STORAGE_OK) {
        knurl_fs_unlock(lock_fd);
        return status;
    }
    if (knurl_fs_ancestor_tombstone_exists(config,
                                           scratch->normalized_source)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_TOMBSTONED;
    }
    if (knurl_fs_ancestor_tombstone_exists(config,
                                           scratch->normalized_destination)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_CONFLICT;
    }
    status = knurl_fs_lookup(config, scratch->normalized_source,
                             &scratch->source_entry);
    if (status != KNURL_STORAGE_OK ||
        (scratch->source_entry.kind != KNURL_FS_ENTRY_FILE &&
         scratch->source_entry.kind != KNURL_FS_ENTRY_COLLECTION)) {
        knurl_fs_unlock(lock_fd);
        return status == KNURL_STORAGE_OK ? KNURL_STORAGE_TOMBSTONED : status;
    }
    if (remove_source && shallow &&
        scratch->source_entry.kind == KNURL_FS_ENTRY_COLLECTION) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_INVALID;
    }
    if (scratch->source_entry.kind == KNURL_FS_ENTRY_COLLECTION &&
        strncmp(scratch->normalized_destination, scratch->normalized_source,
                strlen(scratch->normalized_source)) == 0 &&
        scratch->normalized_destination[strlen(scratch->normalized_source)] ==
            '/') {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_INVALID;
    }
    status = knurl_fs_lookup(config, scratch->normalized_destination,
                             &scratch->destination_entry);
    is_new = (status == KNURL_STORAGE_NOT_FOUND) ||
             (status == KNURL_STORAGE_OK &&
              scratch->destination_entry.kind == KNURL_FS_ENTRY_TOMBSTONE);
    if (!overwrite &&
        ((status == KNURL_STORAGE_OK &&
          scratch->destination_entry.kind != KNURL_FS_ENTRY_TOMBSTONE))) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_EXISTS;
    }
    if (status == KNURL_STORAGE_OK &&
        scratch->destination_entry.kind == KNURL_FS_ENTRY_COLLECTION &&
        scratch->source_entry.kind == KNURL_FS_ENTRY_FILE) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_CONFLICT;
    }
    if (status != KNURL_STORAGE_OK && status != KNURL_STORAGE_NOT_FOUND) {
        knurl_fs_unlock(lock_fd);
        return status;
    }
    if (!knurl_fs_disk_path(config, "content", scratch->normalized_source,
                            scratch->source_disk) ||
        !knurl_fs_disk_path(config, "content", scratch->normalized_destination,
                            scratch->destination_disk) ||
        !knurl_fs_disk_path(config, "content", "/", scratch->content_root) ||
        !knurl_fs_disk_path(config, "tombstones", "/",
                            scratch->tombstone_root) ||
        !knurl_fs_disk_path(config, "tombstones", scratch->normalized_source,
                            scratch->source_tombstone) ||
        !knurl_fs_disk_path(config, "tombstones",
                            scratch->normalized_destination,
                            scratch->destination_tombstone) ||
        !knurl_fs_usage(config, &usage, &resources)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    source_usage = 0u;
    destination_usage = 0u;
    shallow =
        shallow && scratch->source_entry.kind == KNURL_FS_ENTRY_COLLECTION;
    source_resources = shallow ? 1u : 0u;
    destination_resources = 0u;
    source_tombstone_usage = 0u;
    source_tombstone_resources = 0u;
    destination_tombstone_usage = 0u;
    destination_tombstone_resources = 0u;
    if ((!shallow && !knurl_fs_disk_usage(scratch->source_disk, &source_usage,
                                          &source_resources)) ||
        !knurl_fs_disk_usage(scratch->destination_disk, &destination_usage,
                             &destination_resources) ||
        usage < destination_usage ||
        ((!remove_source) &&
         (usage - destination_usage > config->max_total_bytes ||
          source_usage >
              config->max_total_bytes - (usage - destination_usage)))) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_LIMIT;
    }
    if (resources < destination_resources) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    resources -= destination_resources;
    additional_resources = 0u;
    if (!knurl_fs_missing_path_resources(scratch->content_root,
                                         scratch->destination_disk, 0,
                                         &additional_resources)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    if (!shallow && scratch->source_entry.kind == KNURL_FS_ENTRY_COLLECTION &&
        !knurl_fs_disk_usage(scratch->source_tombstone, &source_tombstone_usage,
                             &source_tombstone_resources)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    if (!knurl_fs_disk_usage(scratch->destination_tombstone,
                             &destination_tombstone_usage,
                             &destination_tombstone_resources) ||
        resources < destination_tombstone_resources) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    resources -= destination_tombstone_resources;
    if ((!remove_source) &&
        !knurl_fs_add_resources(&additional_resources, source_resources)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_LIMIT;
    }
    if (!remove_source &&
        scratch->source_entry.kind == KNURL_FS_ENTRY_COLLECTION &&
        !knurl_fs_add_resources(&additional_resources,
                                source_tombstone_resources)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_LIMIT;
    }
    if (source_tombstone_resources != 0u &&
        !knurl_fs_missing_path_resources(scratch->tombstone_root,
                                         scratch->destination_tombstone, 0,
                                         &additional_resources)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    /* A moved tombstone subtree stays counted at its destination. */
    if (remove_source) {
        if (source_tombstone_resources != 0u) {
            if (!knurl_fs_add_resources(&additional_resources, 1u)) {
                knurl_fs_unlock(lock_fd);
                return KNURL_STORAGE_LIMIT;
            }
        } else if (!knurl_fs_missing_path_resources(scratch->tombstone_root,
                                                    scratch->source_tombstone,
                                                    1, &additional_resources)) {
            knurl_fs_unlock(lock_fd);
            return KNURL_STORAGE_IO;
        }
    }
    if (!knurl_fs_resources_within_limit(config, resources,
                                         additional_resources)) {
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_LIMIT;
    }
    scratch->transaction[0] = '\0';
    scratch->created_destination_parent[0] = '\0';
    destination_installed = 0;
    destination_moved = 0;
    source_moved = 0;
    tombstones_moved = 0;
    if (!knurl_fs_transaction_dir(config, scratch->transaction) ||
        !knurl_fs_transaction_path(scratch->transaction, "content",
                                   scratch->staged_content) ||
        !knurl_fs_transaction_path(scratch->transaction, "tombstones",
                                   scratch->staged_tombstones) ||
        !knurl_fs_transaction_path(scratch->transaction, "old-destination",
                                   scratch->backup_destination) ||
        !knurl_fs_transaction_path(scratch->transaction, "old-source",
                                   scratch->backup_source) ||
        !knurl_fs_transaction_path(scratch->transaction, "old-tombstones",
                                   scratch->backup_tombstones) ||
        !(shallow ? mkdir(scratch->staged_content, 0700) == 0
                  : knurl_fs_copy_tree(scratch->source_disk,
                                       scratch->staged_content, config->job,
                                       config->max_file_bytes)) ||
        !knurl_fs_copy_tree(scratch->tombstone_root, scratch->staged_tombstones,
                            config->job, config->max_file_bytes) ||
        !knurl_fs_path_in_root(scratch->staged_tombstones,
                               scratch->normalized_source,
                               scratch->staged_source_tombstone) ||
        !knurl_fs_path_in_root(scratch->staged_tombstones,
                               scratch->normalized_destination,
                               scratch->staged_destination_tombstone)) {
        status = errno == EFBIG ? KNURL_STORAGE_LIMIT : KNURL_STORAGE_IO;
        goto cleanup;
    }
    source_tombstone_present = 0;
    if (!shallow && scratch->source_entry.kind == KNURL_FS_ENTRY_COLLECTION) {
        if (lstat(scratch->staged_source_tombstone, &source_tombstone_stat) ==
            0) {
            if (!S_ISREG(source_tombstone_stat.st_mode) &&
                !S_ISDIR(source_tombstone_stat.st_mode)) {
                status = KNURL_STORAGE_IO;
                goto cleanup;
            }
            source_tombstone_present = 1;
        } else if (errno != ENOENT) {
            status = KNURL_STORAGE_IO;
            goto cleanup;
        }
    }
    if (!knurl_fs_clear_tombstones_in_root(scratch->staged_tombstones,
                                           scratch->normalized_destination) ||
        (source_tombstone_present &&
         (!knurl_fs_parent_dir(
              scratch->staged_destination_tombstone,
              scratch->staged_destination_tombstone_parent,
              sizeof(scratch->staged_destination_tombstone_parent)) ||
          !knurl_fs_mkdir_p(scratch->staged_destination_tombstone_parent) ||
          !knurl_fs_copy_tree(scratch->staged_source_tombstone,
                              scratch->staged_destination_tombstone,
                              config->job, config->max_file_bytes))) ||
        (remove_source &&
         (!knurl_fs_clear_tombstones_in_root(scratch->staged_tombstones,
                                             scratch->normalized_source) ||
          !knurl_fs_touch_atomic(scratch->staged_source_tombstone)))) {
        status = KNURL_STORAGE_IO;
        goto cleanup;
    }
    if (!knurl_fs_create_destination_parent(
            scratch->content_root, scratch->destination_disk,
            scratch->created_destination_parent)) {
        status = KNURL_STORAGE_IO;
        goto cleanup;
    }
    if (!knurl_fs_move_to_backup(scratch->destination_disk,
                                 scratch->backup_destination,
                                 &destination_moved) ||
        (remove_source &&
         !knurl_fs_move_to_backup(scratch->source_disk, scratch->backup_source,
                                  &source_moved)) ||
        rename(scratch->staged_content, scratch->destination_disk) == -1) {
        status = KNURL_STORAGE_IO;
        goto rollback_content;
    }
    destination_installed = 1;
    if (!knurl_fs_move_to_backup(scratch->tombstone_root,
                                 scratch->backup_tombstones,
                                 &tombstones_moved) ||
        rename(scratch->staged_tombstones, scratch->tombstone_root) == -1) {
        status = KNURL_STORAGE_IO;
        goto rollback_tombstones;
    }
    config->result->committed = 1;
    config->result->created = is_new;
    if (!knurl_fs_remove_tree(scratch->transaction)) {
        config->result->partial = 1;
        strcpy(config->recovery, scratch->transaction);
        config->result->recovery_path = config->recovery;
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    if (created != NULL)
        *created = is_new;
    knurl_fs_unlock(lock_fd);
    return KNURL_STORAGE_OK;

rollback_tombstones:
    if (tombstones_moved) {
        if (!knurl_fs_restore_backup(scratch->tombstone_root,
                                     scratch->backup_tombstones, 1))
            recovery_needed = 1;
    }
rollback_content:
    if (destination_installed || destination_moved) {
        if (!knurl_fs_restore_backup(scratch->destination_disk,
                                     scratch->backup_destination,
                                     destination_moved))
            recovery_needed = 1;
    }
    if (source_moved) {
        if (!knurl_fs_restore_backup(scratch->source_disk,
                                     scratch->backup_source, 1))
            recovery_needed = 1;
    }
cleanup:
    if (recovery_needed) {
        config->result->partial = 1;
        strcpy(config->recovery, scratch->transaction);
        config->result->recovery_path = config->recovery;
        knurl_fs_unlock(lock_fd);
        return KNURL_STORAGE_IO;
    }
    if (scratch->created_destination_parent[0] != '\0') {
        (void)knurl_fs_remove_tree(scratch->created_destination_parent);
    }
    if (scratch->transaction[0] != '\0') {
        (void)knurl_fs_remove_tree(scratch->transaction);
    }
    knurl_fs_unlock(lock_fd);
    return status;
}

knurl_storage_code
knurl_fs_copy_or_move(const knurl_fs_config *config, const char *source,
                      const char *destination, int overwrite, int remove_source,
                      int shallow, const char *if_match,
                      const char *if_none_match, int *created)
{
    knurl_fs_copy_or_move_scratch *scratch;
    knurl_storage_code result;
    scratch = (knurl_fs_copy_or_move_scratch *)malloc(sizeof(*scratch));
    if (scratch == NULL) {
        if (created != NULL)
            *created = 0;
        return KNURL_STORAGE_NOMEM;
    }
    result = knurl_fs_copy_or_move_with_scratch(
        config, source, destination, overwrite, remove_source, shallow,
        if_match, if_none_match, created, scratch);
    free(scratch);
    return result;
}

int knurl_fs_internal_entry(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
           strncmp(name, ".vectis-tmp-", sizeof(".vectis-tmp-") - 1u) == 0 ||
           strncmp(name, ".vectis-txn-", sizeof(".vectis-txn-") - 1u) == 0;
}

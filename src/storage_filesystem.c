#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "storage_fs_disk.h"
#include "body_internal.h"
#include "loop_internal.h"
#include <rotmos/request.h>
#include <dirent.h>
#include <openssl/rand.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct filesystem {
    rotmos_server *server;
    rotmos_fs_config config;
    unsigned int writes;
};
struct disk_resource {
    struct filesystem *filesystem;
    int fd, parent, ended, writing, failed;
    DIR *directory;
    char *path, *if_match, *if_none_match;
    char temporary[ROTMOS_FS_PATH_MAX + 1];
    size_t written;
};
struct disk_operation {
    struct filesystem *filesystem;
    rotmos_storage_operation *operation;
    const rotmos_storage_arguments *arguments;
    struct disk_resource *resource, *created;
    rotmos_job *job, *native_job;
    rotmos_timer *admission;
    rotmos_waker *scope_waker;
    unsigned int scope_ready;
    int scope_allowed, scope_destination;
    rotmos_status scope_status;
    rotmos_error scope_error;
    char scope_path[ROTMOS_FS_STORAGE_PATH_MAX],
        scope_target[ROTMOS_FS_STORAGE_PATH_MAX];
    size_t references;
    rotmos_storage_result result;
    int fd;
    char etag[67], modified[32], child[ROTMOS_FS_PATH_MAX + 1];
    char recovery[ROTMOS_FS_STORAGE_PATH_MAX];
};
struct opened_file {
    int fd;
};
static void filesystem_free(void *data)
{
    struct filesystem *f = data;
    free((void *)f->config.cache_dir);
    free((void *)f->config.site_id);
    free((void *)f->config.root_dir);
    free(f);
}
static void executor_retired(void *data)
{
    rotmos_loop *loop = data;
    loop->storage_executor = NULL;
}
static void resource_close(void *data)
{
    struct disk_resource *r = data;
    if (r->directory != NULL)
        closedir(r->directory);
    if (r->fd >= 0)
        close(r->fd);
    if (r->parent >= 0) {
        if (r->temporary[0] != 0)
            (void)unlinkat(r->parent, r->temporary, 0);
        close(r->parent);
    }
    if (r->writing)
        __atomic_sub_fetch(&r->filesystem->writes, 1u, __ATOMIC_RELAXED);
    free(r->path);
    free(r->if_match);
    free(r->if_none_match);
    free(r);
}
static rotmos_storage_code errno_code(void)
{
    if (errno == ENOENT || errno == ENOTDIR)
        return ROTMOS_STORAGE_NOT_FOUND;
    if (errno == ENOMEM)
        return ROTMOS_STORAGE_NOMEM;
    if (errno == EEXIST)
        return ROTMOS_STORAGE_EXISTS;
    if (errno == ENOSPC || errno == EDQUOT || errno == EFBIG)
        return ROTMOS_STORAGE_LIMIT;
    return ROTMOS_STORAGE_IO;
}
static int content_config(const rotmos_fs_config *config,
                          rotmos_fs_config *content, char *root)
{
    *content = *config;
    if (config->root_dir == NULL) {
        if (!rotmos_fs_disk_path(config, "content", "/", root))
            return 0;
        content->root_dir = root;
    }
    return 1;
}
/* Check existing components before path-based managed-cache helpers. External
 * writers must coordinate with the mutation lock, as in Vectis. */
static int path_confined(const rotmos_fs_config *config, const char *path)
{
    rotmos_fs_config content;
    char *root, *leaf;
    struct stat st;
    int fd, ok;
    root = malloc(ROTMOS_FS_STORAGE_PATH_MAX);
    leaf = malloc(ROTMOS_FS_PATH_MAX + 1);
    if (root == NULL || leaf == NULL) {
        free(root);
        free(leaf);
        return 0;
    }
    if (!content_config(config, &content, root)) {
        free(root);
        free(leaf);
        return 0;
    }
    if (strcmp(path, "/") == 0) {
        fd = rotmos_fs_direct_open_existing(&content, path, 1, NULL);
        ok = fd >= 0;
    } else {
        fd = rotmos_fs_open_parent_fd(&content, path, 0, leaf);
        if (fd < 0) {
            /* Missing components are confined too. Managed DELETE must be
             * able to tombstone a resource supplied by a host packed overlay.
             */
            ok = errno == ENOENT;
            free(root);
            free(leaf);
            return ok;
        }
        ok = 1;
        if (fstatat(fd, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0)
            ok = S_ISREG(st.st_mode) || S_ISDIR(st.st_mode);
        else if (ok)
            ok = errno == ENOENT;
    }
    if (fd >= 0)
        close(fd);
    free(root);
    free(leaf);
    return ok;
}
static int metadata(struct disk_operation *op, int fd, rotmos_asset_info *info,
                    int hash, rotmos_job *job)
{
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed",
                                 "Thu", "Fri", "Sat"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct stat st;
    struct tm utc;
    if (fstat(fd, &st) != 0 || st.st_size < 0 ||
        (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)))
        return 0;
    memset(info, 0, sizeof(*info));
    info->kind =
        S_ISDIR(st.st_mode) ? ROTMOS_ASSET_DIRECTORY : ROTMOS_ASSET_FILE;
    if (S_ISREG(st.st_mode)) {
        info->size.high = (unsigned long)((st.st_size >> 16) >> 16);
        info->size.low = (unsigned long)st.st_size & 0xffffffffUL;
        if (hash) {
            if (!rotmos_fs_etag_fd(fd, op->etag, job))
                return 0;
            info->etag = op->etag;
        }
    }
    if (gmtime_r(&st.st_mtime, &utc) != NULL && utc.tm_year >= -299 &&
        utc.tm_year <= 8099) {
        snprintf(op->modified, sizeof(op->modified),
                 "%s, %02d %s %04d %02d:%02d:%02d GMT", days[utc.tm_wday],
                 utc.tm_mday, months[utc.tm_mon], utc.tm_year + 1900,
                 utc.tm_hour, utc.tm_min, utc.tm_sec);
        info->last_modified = op->modified;
    }
    return 1;
}
static rotmos_storage_code open_read(struct disk_operation *op,
                                     const rotmos_fs_config *config)
{
    rotmos_fs_entry *entry;
    rotmos_fs_config content;
    char *root;
    rotmos_storage_code code;
    int fd;
    entry = malloc(sizeof(*entry));
    root = malloc(ROTMOS_FS_STORAGE_PATH_MAX);
    if (entry == NULL || root == NULL) {
        free(entry);
        free(root);
        return ROTMOS_STORAGE_NOMEM;
    }
    code = rotmos_fs_lookup(config, op->arguments->path, entry);
    if (code != ROTMOS_STORAGE_OK)
        goto done;
    if (entry->kind == ROTMOS_FS_ENTRY_TOMBSTONE) {
        code = ROTMOS_STORAGE_TOMBSTONED;
        goto done;
    }
    if (op->arguments->action == ROTMOS_STORAGE_OPEN_READ &&
        entry->kind != ROTMOS_FS_ENTRY_FILE) {
        code = ROTMOS_STORAGE_CONFLICT;
        goto done;
    }
    if (op->arguments->action == ROTMOS_STORAGE_LIST_OPEN &&
        entry->kind != ROTMOS_FS_ENTRY_COLLECTION) {
        code = ROTMOS_STORAGE_CONFLICT;
        goto done;
    }
    if (!content_config(config, &content, root)) {
        code = ROTMOS_STORAGE_INVALID;
        goto done;
    }
    fd = rotmos_fs_direct_open_existing(
        &content, op->arguments->path,
        entry->kind == ROTMOS_FS_ENTRY_COLLECTION, NULL);
    if (fd < 0) {
        code = errno_code();
        goto done;
    }
    if (!metadata(op, fd, &op->result.info, 1, config->job)) {
        close(fd);
        code = ROTMOS_STORAGE_IO;
        goto done;
    }
    op->result.has_info = 1;
    if (op->arguments->action == ROTMOS_STORAGE_STAT)
        close(fd);
    else if (op->arguments->action == ROTMOS_STORAGE_OPEN_READ)
        op->fd = fd;
    else {
        struct disk_resource *r = calloc(1, sizeof(*r));
        if (r == NULL) {
            close(fd);
            code = ROTMOS_STORAGE_NOMEM;
            goto done;
        }
        r->fd = r->parent = -1;
        r->filesystem = op->filesystem;
        r->path = strdup(op->arguments->path);
        r->directory = fdopendir(fd);
        if (r->path == NULL || r->directory == NULL) {
            if (r->directory == NULL)
                close(fd);
            resource_close(r);
            code = ROTMOS_STORAGE_NOMEM;
            goto done;
        }
        op->created = r;
    }
done:
    free(root);
    free(entry);
    return code;
}
static rotmos_storage_code list_next(struct disk_operation *op,
                                     const rotmos_fs_config *config)
{
    struct disk_resource *r = op->resource;
    struct dirent *entry;
    struct stat st;
    char *path;
    rotmos_fs_entry *visible;
    rotmos_storage_code code = ROTMOS_STORAGE_END;
    int count, fd, n;
    if (r->ended)
        return ROTMOS_STORAGE_END;
    path = malloc(ROTMOS_FS_PATH_MAX + 1);
    visible = malloc(sizeof(*visible));
    if (path == NULL || visible == NULL) {
        free(path);
        free(visible);
        return ROTMOS_STORAGE_NOMEM;
    }
    for (count = 0; count < 64; ++count) {
        if (rotmos_job_cancelled(config->job)) {
            code = ROTMOS_STORAGE_IO;
            break;
        }
        errno = 0;
        entry = readdir(r->directory);
        if (entry == NULL) {
            code = errno == 0 ? ROTMOS_STORAGE_END : ROTMOS_STORAGE_IO;
            r->ended = code == ROTMOS_STORAGE_END;
            break;
        }
        if (rotmos_fs_internal_entry(entry->d_name))
            continue;
        n = snprintf(path, ROTMOS_FS_PATH_MAX + 1, "%s%s%s", r->path,
                     strcmp(r->path, "/") == 0 ? "" : "/", entry->d_name);
        if (n < 0 || n > (int)ROTMOS_FS_PATH_MAX) {
            code = ROTMOS_STORAGE_INVALID;
            break;
        }
        code = rotmos_fs_lookup(config, path, visible);
        if (code == ROTMOS_STORAGE_NOT_FOUND ||
            (code == ROTMOS_STORAGE_OK &&
             visible->kind == ROTMOS_FS_ENTRY_TOMBSTONE))
            continue;
        if (code != ROTMOS_STORAGE_OK)
            break;
        if (fstatat(dirfd(r->directory), entry->d_name, &st,
                    AT_SYMLINK_NOFOLLOW) != 0) {
            code = errno_code();
            break;
        }
        if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
            continue;
        fd = openat(dirfd(r->directory), entry->d_name,
                    O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC |
                        (S_ISDIR(st.st_mode) ? O_DIRECTORY : 0));
        if (fd < 0) {
            code = errno_code();
            break;
        }
        if (!metadata(op, fd, &op->result.entry.info, 1, config->job)) {
            close(fd);
            code = ROTMOS_STORAGE_IO;
            break;
        }
        close(fd);
        strncpy(op->child, entry->d_name, sizeof(op->child) - 1);
        op->result.entry.name = op->child;
        op->result.has_entry = 1;
        code = ROTMOS_STORAGE_OK;
        break;
    }
    /* A batch with only hidden entries may continue in this native job. No
     * directory vector is accumulated and cancellation is checked per item. */
    if (count == 64)
        code = ROTMOS_STORAGE_CONFLICT;
    free(path);
    free(visible);
    return code;
}
static rotmos_storage_code put_begin(struct disk_operation *op,
                                     const rotmos_fs_config *config)
{
    struct disk_resource *r;
    rotmos_fs_config content;
    rotmos_storage_code code;
    char *root, *leaf;
    int lock, i;
    if (strcmp(op->arguments->path, "/") == 0)
        return ROTMOS_STORAGE_INVALID;
    lock = rotmos_fs_lock(config);
    if (lock < 0)
        return ROTMOS_STORAGE_IO;
    code = rotmos_fs_check_conditions(config, op->arguments->path,
                                      op->arguments->if_match,
                                      op->arguments->if_none_match);
    if (code == ROTMOS_STORAGE_OK)
        code = rotmos_fs_check_parent(config, op->arguments->path);
    if (code != ROTMOS_STORAGE_OK) {
        rotmos_fs_unlock(lock);
        return code;
    }
    if (__atomic_add_fetch(&op->filesystem->writes, 1u, __ATOMIC_RELAXED) >
        16u) {
        __atomic_sub_fetch(&op->filesystem->writes, 1u, __ATOMIC_RELAXED);
        rotmos_fs_unlock(lock);
        return ROTMOS_STORAGE_LIMIT;
    }
    r = calloc(1, sizeof(*r));
    root = malloc(ROTMOS_FS_STORAGE_PATH_MAX);
    leaf = malloc(ROTMOS_FS_PATH_MAX + 1);
    if (r == NULL || root == NULL || leaf == NULL) {
        free(r);
        free(root);
        free(leaf);
        __atomic_sub_fetch(&op->filesystem->writes, 1u, __ATOMIC_RELAXED);
        rotmos_fs_unlock(lock);
        return ROTMOS_STORAGE_NOMEM;
    }
    r->filesystem = op->filesystem;
    r->writing = 1;
    r->parent = r->fd = -1;
    r->path = strdup(op->arguments->path);
    if (op->arguments->if_match != NULL)
        r->if_match = strdup(op->arguments->if_match);
    if (op->arguments->if_none_match != NULL)
        r->if_none_match = strdup(op->arguments->if_none_match);
    if (r->path == NULL ||
        (op->arguments->if_match != NULL && r->if_match == NULL) ||
        (op->arguments->if_none_match != NULL && r->if_none_match == NULL)) {
        code = ROTMOS_STORAGE_NOMEM;
        goto failed;
    }
    if (!content_config(config, &content, root)) {
        code = ROTMOS_STORAGE_INVALID;
        goto failed;
    }
    r->parent = rotmos_fs_open_parent_fd(&content, r->path, 0, leaf);
    if (r->parent < 0) {
        code = errno_code();
        goto failed;
    }
    for (i = 0; i < 1000; ++i) {
        unsigned char nonce[16];
        size_t j;
        static const char hex[] = "0123456789abcdef";
        if (RAND_bytes(nonce, sizeof(nonce)) != 1) {
            errno = EIO;
            break;
        }
        strcpy(r->temporary, ".vectis-tmp-");
        for (j = 0; j < sizeof(nonce); ++j) {
            r->temporary[12 + 2 * j] = hex[nonce[j] >> 4];
            r->temporary[13 + 2 * j] = hex[nonce[j] & 15];
        }
        r->temporary[44] = 0;
        r->fd =
            openat(r->parent, r->temporary,
                   O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (r->fd >= 0)
            break;
        if (errno != EEXIST)
            break;
    }
    if (r->fd < 0) {
        r->temporary[0] = 0;
        code = errno_code();
        goto failed;
    }
    op->created = r;
    free(root);
    free(leaf);
    rotmos_fs_unlock(lock);
    return ROTMOS_STORAGE_OK;
failed:
    resource_close(r);
    free(root);
    free(leaf);
    rotmos_fs_unlock(lock);
    return code;
}
static rotmos_storage_code put_write(struct disk_operation *op,
                                     const rotmos_fs_config *config)
{
    struct disk_resource *r = op->resource;
    const unsigned char *data = op->arguments->bytes.data;
    size_t offset = 0, size = op->arguments->bytes.length, offered;
    ssize_t n;
    if (r->ended || r->failed)
        return ROTMOS_STORAGE_CONFLICT;
    if (size > config->max_file_bytes - r->written) {
        r->failed = 1;
        return ROTMOS_STORAGE_LIMIT;
    }
    while (offset < size) {
        if (rotmos_job_cancelled(config->job)) {
            r->failed = 1;
            return ROTMOS_STORAGE_IO;
        }
        offered = size - offset;
        if (offered > 2048)
            offered = 2048;
        n = write(r->fd, data + offset, offered);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            r->failed = 1;
            return errno_code();
        }
        offset += (size_t)n;
        r->written += (size_t)n;
    }
    op->result.accepted = size;
    return ROTMOS_STORAGE_OK;
}
static rotmos_storage_code put_finish(struct disk_operation *op,
                                      const rotmos_fs_config *config)
{
    struct disk_resource *r = op->resource;
    rotmos_fs_config content;
    rotmos_storage_code code;
    struct stat old, opened, current;
    uint64_t usage, resources, extra = 0, old_size = 0;
    char *root, *disk, *leaf;
    int lock, parent = -1;
    if (r->ended || r->failed)
        return ROTMOS_STORAGE_CONFLICT;
    root = malloc(ROTMOS_FS_STORAGE_PATH_MAX);
    disk = malloc(ROTMOS_FS_STORAGE_PATH_MAX);
    leaf = malloc(ROTMOS_FS_PATH_MAX + 1);
    if (root == NULL || disk == NULL || leaf == NULL) {
        free(root);
        free(disk);
        free(leaf);
        return ROTMOS_STORAGE_NOMEM;
    }
    lock = rotmos_fs_lock(config);
    code = ROTMOS_STORAGE_IO;
    if (lock < 0)
        goto done;
    code = rotmos_fs_check_conditions(config, r->path, r->if_match,
                                      r->if_none_match);
    if (code != ROTMOS_STORAGE_OK)
        goto done;
    code = rotmos_fs_check_parent(config, r->path);
    if (code != ROTMOS_STORAGE_OK)
        goto done;
    if (rotmos_fs_ancestor_tombstone_exists(config, r->path)) {
        code = ROTMOS_STORAGE_CONFLICT;
        goto done;
    }
    if (!content_config(config, &content, root) ||
        !rotmos_fs_disk_path(config, "content", r->path, disk)) {
        code = ROTMOS_STORAGE_INVALID;
        goto done;
    }
    if (config->root_dir != NULL)
        strcpy(root, config->root_dir);
    parent = rotmos_fs_open_parent_fd(&content, r->path, 0, leaf);
    if (parent < 0 || fstat(parent, &current) != 0 ||
        fstat(r->parent, &opened) != 0 || current.st_dev != opened.st_dev ||
        current.st_ino != opened.st_ino) {
        code = ROTMOS_STORAGE_CONFLICT;
        goto done;
    }
    if (fstatat(parent, leaf, &old, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(old.st_mode)) {
            code = ROTMOS_STORAGE_CONFLICT;
            goto done;
        }
        old_size = (uint64_t)old.st_size;
    } else if (errno != ENOENT) {
        code = ROTMOS_STORAGE_IO;
        goto done;
    } else
        extra = 1;
    if (!rotmos_fs_usage(config, &usage, &resources) ||
        !rotmos_fs_subtract_tombstone_usage(config, r->path, &usage,
                                            &resources)) {
        code = ROTMOS_STORAGE_IO;
        goto done;
    }
    if (usage < old_size || usage - old_size > config->max_total_bytes ||
        r->written > config->max_total_bytes - (usage - old_size) ||
        !rotmos_fs_resources_within_limit(config, resources, extra)) {
        code = ROTMOS_STORAGE_LIMIT;
        goto done;
    }
    if (rotmos_job_cancelled(config->job)) {
        code = ROTMOS_STORAGE_IO;
        goto done;
    }
    if (fsync(r->fd) != 0 ||
        renameat(r->parent, r->temporary, parent, leaf) != 0) {
        code = errno_code();
        goto done;
    }
    r->temporary[0] = 0;
    r->ended = 1;
    op->result.committed = 1;
    op->result.created = extra != 0;
    code = ROTMOS_STORAGE_OK;
    if (!rotmos_fs_clear_tombstones(config, r->path)) {
        code = ROTMOS_STORAGE_IO;
        op->result.partial = 1;
    }
    if (metadata(op, r->fd, &op->result.info, 1, config->job))
        op->result.has_info = 1;
done:
    if (parent >= 0)
        close(parent);
    if (lock >= 0)
        rotmos_fs_unlock(lock);
    free(root);
    free(disk);
    free(leaf);
    return code;
}
static rotmos_storage_code scope_request(const char *path,
                                         const char *destination, void *data)
{
    struct disk_operation *op = data;
    strcpy(op->scope_path, path);
    op->scope_destination = destination != NULL;
    if (destination != NULL)
        strcpy(op->scope_target, destination);
    __atomic_store_n(&op->scope_ready, 1u, __ATOMIC_RELEASE);
    (void)rotmos_waker_signal(op->scope_waker, NULL);
    while (__atomic_load_n(&op->scope_ready, __ATOMIC_ACQUIRE) != 2u) {
        if (rotmos_job_cancelled(op->native_job))
            return ROTMOS_STORAGE_IO;
        (void)poll(NULL, 0, 1);
    }
    if (op->scope_status != ROTMOS_OK)
        return op->scope_status == ROTMOS_NOMEM ? ROTMOS_STORAGE_NOMEM
                                                : ROTMOS_STORAGE_IO;
    return op->scope_allowed ? ROTMOS_STORAGE_OK : ROTMOS_STORAGE_DENIED;
}
static void scope_ready(rotmos_loop *loop, void *data)
{
    struct disk_operation *op = data;
    (void)loop;
    if (op->operation == NULL ||
        __atomic_load_n(&op->scope_ready, __ATOMIC_ACQUIRE) != 1u)
        return;
    op->scope_allowed = 0;
    op->scope_status = rotmos_storage_operation_check_scope(
        op->operation, op->scope_path,
        op->scope_destination ? op->scope_target : NULL, &op->scope_allowed,
        &op->scope_error);
    __atomic_store_n(&op->scope_ready, 2u, __ATOMIC_RELEASE);
}
static rotmos_status native_work(rotmos_job *job, rotmos_bytes arguments,
                                 void *data, rotmos_error *error)
{
    struct disk_operation *op = data;
    rotmos_fs_config config = op->filesystem->config;
    rotmos_storage_action action = op->arguments->action;
    const rotmos_storage_arguments *a = op->arguments;
    char normalized[ROTMOS_FS_PATH_MAX + 1];
    int lock;
    (void)arguments;
    (void)error;
    op->native_job = job;
    config.job = job;
    if (op->scope_waker != NULL) {
        config.scope = scope_request;
        config.scope_data = op;
    }
    config.result = &op->result;
    config.recovery = op->recovery;
    op->result.code = ROTMOS_STORAGE_IO;
    if (rotmos_job_cancelled(job))
        return ROTMOS_OK;
    if (op->resource == NULL &&
        (!rotmos_fs_path_normalize(a->path, normalized) ||
         strcmp(normalized, a->path) != 0)) {
        op->result.code = ROTMOS_STORAGE_INVALID;
        return ROTMOS_OK;
    }
    if (op->resource == NULL &&
        (strstr(a->path, "/.vectis-tmp-") != NULL ||
         strstr(a->path, "/.vectis-txn-") != NULL ||
         (a->destination != NULL &&
          (strstr(a->destination, "/.vectis-tmp-") != NULL ||
           strstr(a->destination, "/.vectis-txn-") != NULL)))) {
        op->result.code = ROTMOS_STORAGE_INVALID;
        return ROTMOS_OK;
    }
    /* Preparation and component checks occur under the shared mutation lock.
     * Mutations reacquire it for their complete condition/quota/commit span. */
    lock = rotmos_fs_lock(&config);
    if (lock < 0)
        return ROTMOS_OK;
    if (op->resource == NULL && action > ROTMOS_STORAGE_LIST_NEXT &&
        (!path_confined(&config, a->path) ||
         (a->destination != NULL && !path_confined(&config, a->destination)))) {
        rotmos_fs_unlock(lock);
        op->result.code = ROTMOS_STORAGE_CONFLICT;
        return ROTMOS_OK;
    }
    if (action <= ROTMOS_STORAGE_LIST_OPEN) {
        op->result.code = open_read(op, &config);
        rotmos_fs_unlock(lock);
        return ROTMOS_OK;
    }
    if (action == ROTMOS_STORAGE_LIST_NEXT) {
        do {
            op->result.code = list_next(op, &config);
        } while (op->result.code == ROTMOS_STORAGE_CONFLICT &&
                 !rotmos_job_cancelled(job));
        rotmos_fs_unlock(lock);
        return ROTMOS_OK;
    }
    rotmos_fs_unlock(lock);
    if (action == ROTMOS_STORAGE_PUT_BEGIN)
        op->result.code = put_begin(op, &config);
    else if (action == ROTMOS_STORAGE_PUT_WRITE)
        op->result.code = put_write(op, &config);
    else if (action == ROTMOS_STORAGE_PUT_FINISH)
        op->result.code = put_finish(op, &config);
    else if (action == ROTMOS_STORAGE_PUT_ABORT) {
        struct disk_resource *r = op->resource;
        if (!r->ended && r->temporary[0] != 0 &&
            unlinkat(r->parent, r->temporary, 0) != 0 && errno != ENOENT)
            op->result.code = errno_code();
        else {
            r->temporary[0] = 0;
            r->ended = 1;
            op->result.code = ROTMOS_STORAGE_OK;
        }
    } else if (action == ROTMOS_STORAGE_DELETE)
        op->result.code = rotmos_fs_delete_conditional(
            &config, a->path, a->if_match, a->if_none_match);
    else if (action == ROTMOS_STORAGE_MKCOL)
        op->result.code = rotmos_fs_mkcol_conditional(
            &config, a->path, a->if_match, a->if_none_match);
    else if (action == ROTMOS_STORAGE_COPY || action == ROTMOS_STORAGE_MOVE) {
        if (!rotmos_fs_path_normalize(a->destination, normalized) ||
            strcmp(normalized, a->destination) != 0)
            op->result.code = ROTMOS_STORAGE_INVALID;
        else
            op->result.code = rotmos_fs_copy_or_move(
                &config, a->path, a->destination, a->overwrite,
                action == ROTMOS_STORAGE_MOVE,
                action == ROTMOS_STORAGE_COPY && a->depth == 0, a->if_match,
                a->if_none_match, &op->result.created);
    }
    if (op->result.code == ROTMOS_STORAGE_OK &&
        action >= ROTMOS_STORAGE_DELETE) {
        op->result.committed = 1;
        if (action == ROTMOS_STORAGE_MKCOL)
            op->result.created = 1;
    }
    return ROTMOS_OK;
}
static void file_close(void *data)
{
    struct opened_file *file = data;
    close(file->fd);
    free(file);
}
static rotmos_status file_read(rotmos_u64 position, void *bytes,
                               size_t capacity, size_t *produced, void *data,
                               rotmos_error *error)
{
    struct opened_file *file = data;
    off_t offset;
    ssize_t n;
    *produced = 0;
    if (!rotmos_file_offset(position, &offset))
        return rotmos_fail(error, ROTMOS_OVERFLOW, "storage_filesystem",
                           "file offset exceeds representation");
    if (capacity > 2048)
        capacity = 2048;
    do {
        n = pread(file->fd, bytes, capacity, offset);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return rotmos_fail(error, ROTMOS_IO, "storage_filesystem",
                           "reading opened file");
    *produced = (size_t)n;
    return n == 0 ? ROTMOS_EOF : ROTMOS_OK;
}
static void native_retired(const rotmos_job_outcome *outcome, void *data)
{
    struct disk_operation *op = data;
    rotmos_status status = ROTMOS_OK;
    rotmos_error error;
    op->job = NULL;
    if (!outcome->started || outcome->work_status != ROTMOS_OK)
        op->result.code = ROTMOS_STORAGE_IO;
    if (op->result.code == ROTMOS_STORAGE_OK && op->fd >= 0) {
        struct opened_file *file = malloc(sizeof(*file));
        rotmos_asset_config config;
        if (file == NULL)
            status = ROTMOS_NOMEM;
        else {
            rotmos_asset_config_init(&config);
            config.info = op->result.info;
            file->fd = op->fd;
            config.read = file_read;
            config.close = file_close;
            config.userdata = file;
            status = rotmos_asset_new(&config, &op->result.asset, &error);
            if (status == ROTMOS_OK)
                op->fd = -1;
            else
                free(file);
        }
    }
    if (op->result.code == ROTMOS_STORAGE_OK && op->created != NULL) {
        rotmos_storage_resource_config config;
        rotmos_storage_resource_config_init(&config);
        config.kind =
            op->created->writing ? ROTMOS_STORAGE_WRITE : ROTMOS_STORAGE_LIST;
        config.userdata = op->created;
        config.close = resource_close;
        status = rotmos_storage_resource_new(op->operation, &config,
                                             &op->result.resource, &error);
        if (status == ROTMOS_OK)
            op->created = NULL;
    }
    if (status != ROTMOS_OK)
        op->result.code =
            status == ROTMOS_NOMEM ? ROTMOS_STORAGE_NOMEM : ROTMOS_STORAGE_IO;
    if (op->scope_status != ROTMOS_OK && op->scope_error.status != ROTMOS_OK)
        op->result.error = op->scope_error;
    if (op->result.code != ROTMOS_STORAGE_OK &&
        op->result.code != ROTMOS_STORAGE_END &&
        op->result.error.status == ROTMOS_OK)
        (void)rotmos_fail(
            &op->result.error,
            op->result.code == ROTMOS_STORAGE_NOMEM
                ? ROTMOS_NOMEM
                : (op->result.code == ROTMOS_STORAGE_LIMIT ? ROTMOS_LIMIT
                                                           : ROTMOS_IO),
            "storage_filesystem", rotmos_storage_code_string(op->result.code));
    status =
        rotmos_storage_operation_complete(op->operation, &op->result, &error);
    if (status == ROTMOS_OK) {
        op->result.asset = NULL;
        op->result.resource = NULL;
    } else {
        rotmos_storage_result fallback;
        rotmos_storage_result_init(&fallback);
        fallback.code = ROTMOS_STORAGE_NOMEM;
        fallback.committed = op->result.committed;
        fallback.partial = op->result.partial;
        fallback.created = op->result.created;
        (void)rotmos_storage_operation_complete(op->operation, &fallback, NULL);
    }
}
static void operation_unref(struct disk_operation *op)
{
    if (--op->references == 0)
        free(op);
}
static void scope_retired(void *data)
{
    struct disk_operation *op = data;
    op->scope_waker = NULL;
    operation_unref(op);
}
static void admission_retired(void *data)
{
    struct disk_operation *op = data;
    op->admission = NULL;
    operation_unref(op);
}
static rotmos_status submit(struct disk_operation *op, rotmos_error *error)
{
    rotmos_job_config config;
    rotmos_loop *loop = rotmos_storage_operation_loop(op->operation);
    rotmos_job_config_init(&config);
    config.work = native_work;
    config.retired = native_retired;
    config.userdata = op;
    config.timeout_disabled = 1;
    return rotmos_executor_submit(loop->storage_executor, &config, &op->job,
                                  error);
}
static void admission_ready(rotmos_timer *timer, void *data)
{
    struct disk_operation *op = data;
    rotmos_error error;
    rotmos_status status;
    if (op->operation == NULL || op->job != NULL ||
        rotmos_storage_operation_cancelled(op->operation)) {
        rotmos_timer_cancel(timer);
        return;
    }
    status = submit(op, &error);
    if (status == ROTMOS_AGAIN)
        return;
    rotmos_timer_cancel(timer);
    if (status == ROTMOS_OK)
        return;
    op->result.code =
        status == ROTMOS_NOMEM ? ROTMOS_STORAGE_NOMEM : ROTMOS_STORAGE_IO;
    op->result.error = error;
    (void)rotmos_storage_operation_complete(op->operation, &op->result, NULL);
}
static rotmos_status invoke(rotmos_storage_operation *operation,
                            const rotmos_storage_arguments *arguments,
                            void *data, rotmos_error *error)
{
    struct filesystem *f = data;
    struct disk_operation *op;
    rotmos_executor_config pool;
    rotmos_loop *loop = rotmos_storage_operation_loop(operation);
    rotmos_status status;
    if (rotmos_server_loop(f->server) != loop)
        return rotmos_fail(error, ROTMOS_STATE, "storage_filesystem",
                           "provider server and operation loop differ");
    if (loop->storage_executor == NULL) {
        rotmos_executor_config_init(&pool);
        pool.retired = executor_retired;
        pool.userdata = loop;
        status = rotmos_executor_new(f->server, &pool, &loop->storage_executor,
                                     error);
        if (status != ROTMOS_OK)
            return status;
    }
    op = calloc(1, sizeof(*op));
    if (op == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "storage_filesystem",
                           "allocating operation");
    op->filesystem = f;
    op->references = 1;
    op->operation = operation;
    op->arguments = arguments;
    op->fd = -1;
    rotmos_storage_result_init(&op->result);
    op->result.code = ROTMOS_STORAGE_IO;
    if (arguments->resource != NULL)
        op->resource = rotmos_storage_resource_userdata(arguments->resource);
    status = rotmos_storage_operation_set_userdata(operation, op, error);
    if (status != ROTMOS_OK) {
        free(op);
        return status;
    }
    if (rotmos_storage_operation_has_scope_check(operation)) {
        status = rotmos_waker_new(loop, scope_ready, scope_retired, op,
                                  &op->scope_waker, error);
        if (status != ROTMOS_OK)
            return status;
        ++op->references;
    }
    status = submit(op, error);
    if (status == ROTMOS_OK)
        return ROTMOS_AGAIN;
    if (status == ROTMOS_AGAIN) {
        status =
            rotmos_loop_timer(loop, 1, 1, admission_ready, admission_retired,
                              op, &op->admission, error);
        if (status == ROTMOS_OK) {
            ++op->references;
            return ROTMOS_AGAIN;
        }
    }
    return status;
}
static rotmos_status cancel(rotmos_storage_operation *operation,
                            const rotmos_storage_arguments *arguments,
                            void *data, rotmos_error *error)
{
    struct disk_operation *op = rotmos_storage_operation_userdata(operation);
    (void)arguments;
    (void)data;
    if (op == NULL)
        return ROTMOS_OK;
    if (op->admission != NULL)
        rotmos_timer_cancel(op->admission);
    if (op->job == NULL)
        return ROTMOS_OK;
    (void)rotmos_job_cancel(op->job, error);
    return ROTMOS_AGAIN;
}
static void retired(rotmos_storage_operation *operation,
                    const rotmos_storage_outcome *outcome, void *data)
{
    struct disk_operation *op = rotmos_storage_operation_userdata(operation);
    (void)outcome;
    (void)data;
    if (op == NULL)
        return;
    op->operation = NULL;
    if (op->scope_waker != NULL)
        (void)rotmos_waker_destroy(op->scope_waker, NULL);
    if (op->admission != NULL)
        rotmos_timer_cancel(op->admission);
    if (op->fd >= 0)
        close(op->fd);
    if (op->created != NULL)
        resource_close(op->created);
    if (op->result.asset != NULL)
        rotmos_asset_close(op->result.asset, NULL);
    if (op->result.resource != NULL)
        rotmos_storage_resource_close(op->result.resource, NULL);
    operation_unref(op);
}
static int absolute_path(const char *path)
{
    const char *p, *end;
    size_t n;
    if (path == NULL || path[0] != '/' || strlen(path) > 3500)
        return 0;
    p = path;
    while (*p != 0) {
        while (*p == '/')
            ++p;
        if (*p == 0)
            break;
        end = strchr(p, '/');
        n = end != NULL ? (size_t)(end - p) : strlen(p);
        if ((n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
            return 0;
        p += n;
    }
    return 1;
}
rotmos_status rotmos_storage_filesystem_content_directory(
    const rotmos_storage_filesystem_config *config, char *out, size_t capacity,
    rotmos_error *error)
{
    int n;
    if (out != NULL && capacity != 0)
        out[0] = 0;
    if (out == NULL || capacity == 0 || config == NULL ||
        !absolute_path(config->cache_dir) ||
        (config->root_dir != NULL && !absolute_path(config->root_dir)))
        return rotmos_fail(error, ROTMOS_INVALID, "storage_filesystem",
                           "provide configuration with absolute confined roots "
                           "and output capacity");
    if (config->root_dir != NULL)
        n = snprintf(out, capacity, "%s", config->root_dir);
    else {
        rotmos_fs_config check;
        memset(&check, 0, sizeof(check));
        check.cache_dir = config->cache_dir;
        check.site_id = config->site_id;
        check.max_file_bytes = check.max_total_bytes = check.max_resources = 1;
        if (!rotmos_fs_config_valid(&check))
            return rotmos_fail(error, ROTMOS_INVALID, "storage_filesystem",
                               "provide a site identifier of at most 63 "
                               "letters, digits, hyphens or underscores");
        n = snprintf(out, capacity, "%s/webdav/%s/content", config->cache_dir,
                     config->site_id);
    }
    if (n < 0 || (size_t)n >= capacity) {
        out[0] = 0;
        return rotmos_fail(error, ROTMOS_OVERFLOW, "storage_filesystem",
                           "content directory exceeds output capacity");
    }
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
void rotmos_storage_filesystem_config_init(
    rotmos_storage_filesystem_config *config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    config->max_file_bytes = 16u * 1024u * 1024u;
    config->max_total_bytes = 128u * 1024u * 1024u;
    config->max_resources = 4096;
}
rotmos_status rotmos_storage_filesystem_new(
    rotmos_server *server, const rotmos_storage_filesystem_config *config,
    rotmos_storage_provider **out, rotmos_error *error)
{
    struct filesystem *f;
    rotmos_storage_provider_config provider;
    rotmos_storage_filesystem_config effective;
    rotmos_status status;
    if (out != NULL)
        *out = NULL;
    if (server == NULL || config == NULL || out == NULL)
        return rotmos_fail(
            error, ROTMOS_INVALID, "storage_filesystem",
            "provide server, filesystem configuration and output");
    effective = *config;
    if (effective.max_file_bytes == 0)
        effective.max_file_bytes = 16u * 1024u * 1024u;
    if (effective.max_total_bytes == 0)
        effective.max_total_bytes = 128u * 1024u * 1024u;
    if (effective.max_resources == 0)
        effective.max_resources = 4096;
    f = calloc(1, sizeof(*f));
    if (f == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "storage_filesystem",
                           "allocating provider");
    f->server = server;
    f->config.cache_dir = rotmos_copy_text(effective.cache_dir);
    f->config.site_id = rotmos_copy_text(effective.site_id);
    f->config.root_dir = rotmos_copy_text(effective.root_dir);
    f->config.max_file_bytes = effective.max_file_bytes;
    f->config.max_total_bytes = effective.max_total_bytes;
    f->config.max_resources = effective.max_resources;
    if ((effective.cache_dir != NULL && f->config.cache_dir == NULL) ||
        (effective.site_id != NULL && f->config.site_id == NULL) ||
        (effective.root_dir != NULL && f->config.root_dir == NULL)) {
        filesystem_free(f);
        return ROTMOS_NOMEM;
    }
    if (!rotmos_fs_config_valid(&f->config) ||
        !absolute_path(f->config.cache_dir) ||
        (f->config.root_dir != NULL && !absolute_path(f->config.root_dir))) {
        filesystem_free(f);
        return rotmos_fail(error, ROTMOS_INVALID, "storage_filesystem",
                           "absolute roots, site identifier and finite nonzero "
                           "quotas required");
    }
    rotmos_storage_provider_config_init(&provider);
    provider.capabilities = ROTMOS_STORAGE_CAN_WRITE |
                            ROTMOS_STORAGE_CAN_CONDITIONAL |
                            ROTMOS_STORAGE_CAN_COPY | ROTMOS_STORAGE_CAN_MOVE |
                            ROTMOS_STORAGE_CAN_SCOPE_CHECK;
    provider.commit_description =
        "Mutation lock covers conditions, quotas and replacement; PUT fsyncs "
        "data before rename. Direct MOVE may leave committed destination and "
        "partial source; no cross-resource atomicity or directory-fsync crash "
        "durability promise.";
    provider.invoke = invoke;
    provider.cancel = cancel;
    provider.retired = retired;
    provider.close = filesystem_free;
    provider.userdata = f;
    status = rotmos_storage_provider_new(&provider, out, error);
    if (status != ROTMOS_OK)
        filesystem_free(f);
    return status;
}

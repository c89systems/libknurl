#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "storage_fs_disk.h"
#include "body_internal.h"
#include "loop_internal.h"
#include <knurl/request.h>
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
    knurl_server *server;
    knurl_fs_config config;
};
struct disk_resource {
    int fd, parent, ended, writing, failed;
    DIR *directory;
    char *path, *if_match, *if_none_match;
    char temporary[KNURL_FS_PATH_MAX + 1];
    size_t written;
};
struct disk_operation {
    struct filesystem *filesystem;
    knurl_storage_operation *operation;
    const knurl_storage_arguments *arguments;
    struct disk_resource *resource, *created;
    knurl_job *job, *native_job;
    knurl_timer *admission;
    knurl_waker *scope_waker;
    unsigned int scope_ready;
    int scope_allowed, scope_destination;
    knurl_status scope_status;
    knurl_error scope_error;
    char scope_path[KNURL_FS_STORAGE_PATH_MAX],
        scope_target[KNURL_FS_STORAGE_PATH_MAX];
    size_t references;
    knurl_storage_result result;
    int fd;
    char etag[67], modified[32], child[KNURL_FS_PATH_MAX + 1];
    char recovery[KNURL_FS_STORAGE_PATH_MAX];
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
    knurl_loop *loop = data;
    loop->impl->storage_executor = NULL;
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
    free(r->path);
    free(r->if_match);
    free(r->if_none_match);
    free(r);
}
static knurl_storage_code errno_code(void)
{
    if (errno == ENOENT || errno == ENOTDIR)
        return KNURL_STORAGE_NOT_FOUND;
    if (errno == ENOMEM)
        return KNURL_STORAGE_NOMEM;
    if (errno == EEXIST)
        return KNURL_STORAGE_EXISTS;
    return KNURL_STORAGE_IO;
}
static int content_config(const knurl_fs_config *config,
                          knurl_fs_config *content, char *root)
{
    *content = *config;
    if (config->root_dir == NULL) {
        if (!knurl_fs_disk_path(config, "content", "/", root))
            return 0;
        content->root_dir = root;
    }
    return 1;
}
/* Check existing components before path-based managed-cache helpers. External
 * writers must coordinate with the mutation lock, as in Vectis. */
static knurl_storage_code path_confined(const knurl_fs_config *config,
                                        const char *path)
{
    knurl_fs_config content;
    char *root, *leaf;
    struct stat st;
    int fd, ok;
    root = malloc(KNURL_FS_STORAGE_PATH_MAX);
    leaf = malloc(KNURL_FS_PATH_MAX + 1);
    if (root == NULL || leaf == NULL) {
        free(root);
        free(leaf);
        return KNURL_STORAGE_NOMEM;
    }
    if (!content_config(config, &content, root)) {
        free(root);
        free(leaf);
        return KNURL_STORAGE_INVALID;
    }
    if (strcmp(path, "/") == 0) {
        fd = knurl_fs_direct_open_existing(&content, path, 1, NULL);
        ok = fd >= 0;
    } else {
        fd = knurl_fs_open_parent_fd(&content, path, 0, leaf);
        if (fd < 0) {
            /* Missing components are confined too. Managed DELETE must be
             * able to tombstone a resource supplied by a host packed overlay.
             */
            ok = errno == ENOENT;
            free(root);
            free(leaf);
            return ok ? KNURL_STORAGE_OK : KNURL_STORAGE_CONFLICT;
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
    return ok ? KNURL_STORAGE_OK : KNURL_STORAGE_CONFLICT;
}
static int metadata_stat(struct disk_operation *op, const struct stat *st,
                         knurl_asset_info *info)
{
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed",
                                 "Thu", "Fri", "Sat"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm utc;
    if (st->st_size < 0 || (!S_ISREG(st->st_mode) && !S_ISDIR(st->st_mode)))
        return 0;
    memset(info, 0, sizeof(*info));
    info->kind =
        S_ISDIR(st->st_mode) ? KNURL_ASSET_DIRECTORY : KNURL_ASSET_FILE;
    if (S_ISREG(st->st_mode)) {
        info->size.high = (unsigned long)((st->st_size >> 16) >> 16);
        info->size.low = (unsigned long)st->st_size & 0xffffffffUL;
    }
    if (gmtime_r(&st->st_mtime, &utc) != NULL && utc.tm_year >= -299 &&
        utc.tm_year <= 8099) {
        snprintf(op->modified, sizeof(op->modified),
                 "%s, %02d %s %04d %02d:%02d:%02d GMT", days[utc.tm_wday],
                 utc.tm_mday, months[utc.tm_mon], utc.tm_year + 1900,
                 utc.tm_hour, utc.tm_min, utc.tm_sec);
        info->last_modified = op->modified;
    }
    return 1;
}
static int metadata(struct disk_operation *op, int fd, knurl_asset_info *info,
                    int hash, knurl_job *job)
{
    struct stat st;
    if (fstat(fd, &st) != 0 || !metadata_stat(op, &st, info))
        return 0;
    if (hash && S_ISREG(st.st_mode)) {
        if (!knurl_fs_etag_fd(fd, op->etag, job))
            return 0;
        info->etag = op->etag;
    }
    return 1;
}
static knurl_storage_code directory_cursor(struct disk_operation *op, int fd)
{
    struct disk_resource *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        close(fd);
        return KNURL_STORAGE_NOMEM;
    }
    r->fd = r->parent = -1;
    r->path = strdup(op->arguments->path);
    r->directory = fdopendir(fd);
    if (r->path == NULL || r->directory == NULL) {
        if (r->directory == NULL)
            close(fd);
        resource_close(r);
        return KNURL_STORAGE_NOMEM;
    }
    op->created = r;
    return KNURL_STORAGE_OK;
}
static knurl_storage_code open_read(struct disk_operation *op,
                                    const knurl_fs_config *config)
{
    knurl_fs_entry *entry;
    knurl_fs_config content;
    char *root;
    knurl_storage_code code;
    int fd;
    entry = malloc(sizeof(*entry));
    root = malloc(KNURL_FS_STORAGE_PATH_MAX);
    if (entry == NULL || root == NULL) {
        free(entry);
        free(root);
        return KNURL_STORAGE_NOMEM;
    }
    code = knurl_fs_lookup(config, op->arguments->path, entry);
    if (code != KNURL_STORAGE_OK)
        goto done;
    if (entry->kind == KNURL_FS_ENTRY_TOMBSTONE) {
        code = KNURL_STORAGE_TOMBSTONED;
        goto done;
    }
    if (op->arguments->action == KNURL_STORAGE_OPEN_READ &&
        entry->kind != KNURL_FS_ENTRY_FILE) {
        code = KNURL_STORAGE_CONFLICT;
        goto done;
    }
    if (op->arguments->action == KNURL_STORAGE_LIST_OPEN &&
        entry->kind != KNURL_FS_ENTRY_COLLECTION) {
        code = KNURL_STORAGE_CONFLICT;
        goto done;
    }
    if (!content_config(config, &content, root)) {
        code = KNURL_STORAGE_INVALID;
        goto done;
    }
    fd = knurl_fs_direct_open_existing(&content, op->arguments->path,
                                       entry->kind == KNURL_FS_ENTRY_COLLECTION,
                                       NULL);
    if (fd < 0) {
        code = errno_code();
        goto done;
    }
    if (!metadata(op, fd, &op->result.info, 1, config->job)) {
        close(fd);
        code = KNURL_STORAGE_IO;
        goto done;
    }
    op->result.has_info = 1;
    if (op->arguments->action == KNURL_STORAGE_STAT)
        close(fd);
    else if (op->arguments->action == KNURL_STORAGE_OPEN_READ)
        op->fd = fd;
    else
        code = directory_cursor(op, fd);
done:
    free(root);
    free(entry);
    return code;
}
static knurl_storage_code list_next(struct disk_operation *op,
                                    const knurl_fs_config *config)
{
    struct disk_resource *r = op->resource;
    struct dirent *entry;
    struct stat st;
    char *path;
    knurl_storage_code code = KNURL_STORAGE_END;
    int count, n;
    if (r->ended)
        return KNURL_STORAGE_END;
    path = malloc(KNURL_FS_PATH_MAX + 1);
    if (path == NULL) {
        return KNURL_STORAGE_NOMEM;
    }
    for (count = 0; count < 64; ++count) {
        if (knurl_fs_cancelled(config->job)) {
            code = KNURL_STORAGE_IO;
            break;
        }
        errno = 0;
        entry = readdir(r->directory);
        if (entry == NULL) {
            code = errno == 0 ? KNURL_STORAGE_END : KNURL_STORAGE_IO;
            r->ended = code == KNURL_STORAGE_END;
            break;
        }
        if (knurl_fs_internal_entry(entry->d_name))
            continue;
        n = snprintf(path, KNURL_FS_PATH_MAX + 1, "%s%s%s", r->path,
                     strcmp(r->path, "/") == 0 ? "" : "/", entry->d_name);
        if (n < 0 || n > (int)KNURL_FS_PATH_MAX) {
            code = KNURL_STORAGE_IO;
            break;
        }
        /* Vectis lists metadata without opening or hashing child contents.
         * Directory entry names need not satisfy request-path spelling rules.
         */
        if (knurl_fs_tombstone_exists(config, path))
            continue;
        if (fstatat(dirfd(r->directory), entry->d_name, &st,
                    AT_SYMLINK_NOFOLLOW) != 0) {
            code = errno_code();
            break;
        }
        if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
            continue;
        if (!metadata_stat(op, &st, &op->result.entry.info)) {
            code = KNURL_STORAGE_IO;
            break;
        }
        strncpy(op->child, entry->d_name, sizeof(op->child) - 1);
        op->result.entry.name = op->child;
        op->result.has_entry = 1;
        code = KNURL_STORAGE_OK;
        break;
    }
    /* A batch with only hidden entries may continue in this native job. No
     * directory vector is accumulated and cancellation is checked per item. */
    if (count == 64)
        code = KNURL_STORAGE_CONFLICT;
    free(path);
    return code;
}
static knurl_storage_code put_begin(struct disk_operation *op,
                                    const knurl_fs_config *config)
{
    struct disk_resource *r;
    knurl_fs_config content;
    knurl_storage_code code;
    char *root, *leaf;
    int lock, i;
    if (strcmp(op->arguments->path, "/") == 0)
        return KNURL_STORAGE_INVALID;
    lock = knurl_fs_lock(config);
    if (lock < 0)
        return KNURL_STORAGE_IO;
    code = knurl_fs_check_conditions(config, op->arguments->path,
                                     op->arguments->if_match,
                                     op->arguments->if_none_match);
    if (code == KNURL_STORAGE_OK)
        code = knurl_fs_check_parent(config, op->arguments->path);
    if (code != KNURL_STORAGE_OK) {
        knurl_fs_unlock(lock);
        return code;
    }
    r = calloc(1, sizeof(*r));
    root = malloc(KNURL_FS_STORAGE_PATH_MAX);
    leaf = malloc(KNURL_FS_PATH_MAX + 1);
    if (r == NULL || root == NULL || leaf == NULL) {
        free(r);
        free(root);
        free(leaf);
        knurl_fs_unlock(lock);
        return KNURL_STORAGE_NOMEM;
    }
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
        code = KNURL_STORAGE_NOMEM;
        goto failed;
    }
    if (!content_config(config, &content, root)) {
        code = KNURL_STORAGE_INVALID;
        goto failed;
    }
    r->parent = knurl_fs_open_parent_fd(&content, r->path, 0, leaf);
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
    knurl_fs_unlock(lock);
    return KNURL_STORAGE_OK;
failed:
    resource_close(r);
    free(root);
    free(leaf);
    knurl_fs_unlock(lock);
    return code;
}
static knurl_storage_code put_write(struct disk_operation *op,
                                    const knurl_fs_config *config)
{
    struct disk_resource *r = op->resource;
    const unsigned char *data = op->arguments->bytes.data;
    size_t offset = 0, size = op->arguments->bytes.length, offered;
    ssize_t n;
    if (r->ended || r->failed)
        return KNURL_STORAGE_CONFLICT;
    if (size > config->max_file_bytes - r->written) {
        r->failed = 1;
        return KNURL_STORAGE_LIMIT;
    }
    while (offset < size) {
        if (knurl_fs_cancelled(config->job)) {
            r->failed = 1;
            return KNURL_STORAGE_IO;
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
    return KNURL_STORAGE_OK;
}
static knurl_storage_code put_finish(struct disk_operation *op,
                                     const knurl_fs_config *config)
{
    struct disk_resource *r = op->resource;
    knurl_fs_config content;
    knurl_storage_code code;
    struct stat old, opened, current;
    uint64_t usage, resources, extra = 0, old_size = 0;
    char *root, *disk, *leaf;
    int lock, close_result, parent = -1;
    if (r->ended || r->failed)
        return KNURL_STORAGE_CONFLICT;
    root = malloc(KNURL_FS_STORAGE_PATH_MAX);
    disk = malloc(KNURL_FS_STORAGE_PATH_MAX);
    leaf = malloc(KNURL_FS_PATH_MAX + 1);
    if (root == NULL || disk == NULL || leaf == NULL) {
        free(root);
        free(disk);
        free(leaf);
        return KNURL_STORAGE_NOMEM;
    }
    lock = knurl_fs_lock(config);
    code = KNURL_STORAGE_IO;
    if (lock < 0)
        goto done;
    code = knurl_fs_check_conditions(config, r->path, r->if_match,
                                     r->if_none_match);
    if (code != KNURL_STORAGE_OK)
        goto done;
    code = knurl_fs_check_parent(config, r->path);
    if (code != KNURL_STORAGE_OK)
        goto done;
    if (knurl_fs_ancestor_tombstone_exists(config, r->path)) {
        code = KNURL_STORAGE_CONFLICT;
        goto done;
    }
    if (!content_config(config, &content, root) ||
        !knurl_fs_disk_path(config, "content", r->path, disk)) {
        code = KNURL_STORAGE_INVALID;
        goto done;
    }
    if (config->root_dir != NULL)
        strcpy(root, config->root_dir);
    parent = knurl_fs_open_parent_fd(&content, r->path, 0, leaf);
    if (parent < 0 || fstat(parent, &current) != 0 ||
        fstat(r->parent, &opened) != 0 || current.st_dev != opened.st_dev ||
        current.st_ino != opened.st_ino) {
        code = KNURL_STORAGE_CONFLICT;
        goto done;
    }
    if (fstatat(parent, leaf, &old, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(old.st_mode)) {
            code = KNURL_STORAGE_CONFLICT;
            goto done;
        }
        old_size = (uint64_t)old.st_size;
    } else if (errno != ENOENT) {
        code = KNURL_STORAGE_IO;
        goto done;
    } else
        extra = 1;
    if (!knurl_fs_usage(config, &usage, &resources) ||
        !knurl_fs_subtract_tombstone_usage(config, r->path, &usage,
                                           &resources)) {
        code = KNURL_STORAGE_IO;
        goto done;
    }
    if (usage < old_size || usage - old_size > config->max_total_bytes ||
        r->written > config->max_total_bytes - (usage - old_size) ||
        !knurl_fs_resources_within_limit(config, resources, extra)) {
        code = KNURL_STORAGE_LIMIT;
        goto done;
    }
    if (knurl_fs_cancelled(config->job)) {
        code = KNURL_STORAGE_IO;
        goto done;
    }
    if (fsync(r->fd) != 0) {
        code = errno_code();
        goto done;
    }
    if (!metadata(op, r->fd, &op->result.info, 1, config->job)) {
        code = KNURL_STORAGE_IO;
        goto done;
    }
    close_result = close(r->fd);
    r->fd = -1;
    if (close_result || renameat(r->parent, r->temporary, parent, leaf)) {
        r->failed = 1;
        code = errno_code();
        goto done;
    }
    r->temporary[0] = 0;
    r->ended = 1;
    op->result.committed = 1;
    op->result.created = extra != 0;
    code = KNURL_STORAGE_OK;
    if (!knurl_fs_clear_tombstones(config, r->path)) {
        code = KNURL_STORAGE_IO;
        op->result.partial = 1;
    }
    op->result.has_info = 1;
done:
    if (parent >= 0)
        close(parent);
    if (lock >= 0)
        knurl_fs_unlock(lock);
    free(root);
    free(disk);
    free(leaf);
    return code;
}
static knurl_storage_code scope_request(const char *path,
                                        const char *destination, void *data)
{
    struct disk_operation *op = data;
    strcpy(op->scope_path, path);
    op->scope_destination = destination != NULL;
    if (destination != NULL)
        strcpy(op->scope_target, destination);
    __atomic_store_n(&op->scope_ready, 1u, __ATOMIC_RELEASE);
    (void)knurl_waker_signal(op->scope_waker, NULL);
    while (__atomic_load_n(&op->scope_ready, __ATOMIC_ACQUIRE) != 2u) {
        if (knurl_fs_cancelled(op->native_job))
            return KNURL_STORAGE_IO;
        (void)poll(NULL, 0, 1);
    }
    if (op->scope_status != KNURL_OK)
        return op->scope_status == KNURL_NOMEM ? KNURL_STORAGE_NOMEM
                                               : KNURL_STORAGE_IO;
    return op->scope_allowed ? KNURL_STORAGE_OK : KNURL_STORAGE_DENIED;
}
static void scope_ready(knurl_loop *loop, void *data)
{
    struct disk_operation *op = data;
    (void)loop;
    if (op->operation == NULL ||
        __atomic_load_n(&op->scope_ready, __ATOMIC_ACQUIRE) != 1u)
        return;
    op->scope_allowed = 0;
    op->scope_status = knurl_storage_operation_check_scope(
        op->operation, op->scope_path,
        op->scope_destination ? op->scope_target : NULL, &op->scope_allowed,
        &op->scope_error);
    __atomic_store_n(&op->scope_ready, 2u, __ATOMIC_RELEASE);
}
static knurl_status native_work(knurl_job *job, knurl_bytes arguments,
                                void *data, knurl_error *error)
{
    struct disk_operation *op = data;
    knurl_fs_config config = op->filesystem->config;
    knurl_storage_action action = op->arguments->action;
    const knurl_storage_arguments *a = op->arguments;
    char normalized[KNURL_FS_PATH_MAX + 1];
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
    op->result.code = KNURL_STORAGE_IO;
    if (knurl_fs_cancelled(job))
        return KNURL_OK;
    if (op->resource == NULL &&
        (!knurl_fs_path_normalize(a->path, normalized) ||
         strcmp(normalized, a->path) != 0)) {
        op->result.code = KNURL_STORAGE_INVALID;
        return KNURL_OK;
    }
    if (op->resource == NULL &&
        (strstr(a->path, "/.vectis-tmp-") != NULL ||
         strstr(a->path, "/.vectis-txn-") != NULL ||
         (a->destination != NULL &&
          (strstr(a->destination, "/.vectis-tmp-") != NULL ||
           strstr(a->destination, "/.vectis-txn-") != NULL)))) {
        op->result.code = KNURL_STORAGE_INVALID;
        return KNURL_OK;
    }
    /* Preparation and component checks occur under the shared mutation lock.
     * Mutations reacquire it for their complete condition/quota/commit span. */
    lock = knurl_fs_lock(&config);
    if (lock < 0)
        return KNURL_OK;
    if (op->resource == NULL && action > KNURL_STORAGE_LIST_NEXT) {
        op->result.code = path_confined(&config, a->path);
        if (op->result.code == KNURL_STORAGE_OK && a->destination != NULL)
            op->result.code = path_confined(&config, a->destination);
        if (op->result.code != KNURL_STORAGE_OK) {
            knurl_fs_unlock(lock);
            return KNURL_OK;
        }
    }
    if (action <= KNURL_STORAGE_LIST_OPEN) {
        op->result.code = open_read(op, &config);
        knurl_fs_unlock(lock);
        return KNURL_OK;
    }
    if (action == KNURL_STORAGE_LIST_NEXT) {
        do {
            op->result.code = list_next(op, &config);
        } while (op->result.code == KNURL_STORAGE_CONFLICT &&
                 !knurl_fs_cancelled(job));
        knurl_fs_unlock(lock);
        return KNURL_OK;
    }
    knurl_fs_unlock(lock);
    if (action == KNURL_STORAGE_PUT_BEGIN)
        op->result.code = put_begin(op, &config);
    else if (action == KNURL_STORAGE_PUT_WRITE)
        op->result.code = put_write(op, &config);
    else if (action == KNURL_STORAGE_PUT_FINISH)
        op->result.code = put_finish(op, &config);
    else if (action == KNURL_STORAGE_PUT_ABORT) {
        struct disk_resource *r = op->resource;
        if (!r->ended && r->temporary[0] != 0 &&
            unlinkat(r->parent, r->temporary, 0) != 0 && errno != ENOENT)
            op->result.code = errno_code();
        else {
            r->temporary[0] = 0;
            r->ended = 1;
            op->result.code = KNURL_STORAGE_OK;
        }
    } else if (action == KNURL_STORAGE_DELETE)
        op->result.code = knurl_fs_delete_conditional(
            &config, a->path, a->if_match, a->if_none_match);
    else if (action == KNURL_STORAGE_MKCOL)
        op->result.code = knurl_fs_mkcol_conditional(
            &config, a->path, a->if_match, a->if_none_match);
    else if (action == KNURL_STORAGE_COPY || action == KNURL_STORAGE_MOVE) {
        if (!knurl_fs_path_normalize(a->destination, normalized) ||
            strcmp(normalized, a->destination) != 0)
            op->result.code = KNURL_STORAGE_INVALID;
        else
            op->result.code = knurl_fs_copy_or_move(
                &config, a->path, a->destination, a->overwrite,
                action == KNURL_STORAGE_MOVE,
                action == KNURL_STORAGE_COPY && a->depth == 0, a->if_match,
                a->if_none_match, &op->result.created);
    }
    if (op->result.code == KNURL_STORAGE_OK && action >= KNURL_STORAGE_DELETE) {
        op->result.committed = 1;
        if (action == KNURL_STORAGE_MKCOL)
            op->result.created = 1;
    }
    return KNURL_OK;
}
static void file_close(void *data)
{
    struct opened_file *file = data;
    close(file->fd);
    free(file);
}
static knurl_status file_read(knurl_u64 position, void *bytes, size_t capacity,
                              size_t *produced, void *data, knurl_error *error)
{
    struct opened_file *file = data;
    off_t offset;
    ssize_t n;
    *produced = 0;
    if (!knurl_file_offset(position, &offset))
        return knurl_fail(error, KNURL_OVERFLOW, "storage_filesystem",
                          "file offset exceeds representation");
    if (capacity > 2048)
        capacity = 2048;
    do {
        n = pread(file->fd, bytes, capacity, offset);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return knurl_fail(error, KNURL_IO, "storage_filesystem",
                          "reading opened file");
    *produced = (size_t)n;
    return n == 0 ? KNURL_EOF : KNURL_OK;
}
static void native_retired(const knurl_job_outcome *outcome, void *data)
{
    struct disk_operation *op = data;
    knurl_status status = KNURL_OK;
    knurl_error error;
    op->job = NULL;
    if (!outcome->started || outcome->work_status != KNURL_OK)
        op->result.code = KNURL_STORAGE_IO;
    if (op->result.code == KNURL_STORAGE_OK && op->fd >= 0) {
        struct opened_file *file = malloc(sizeof(*file));
        knurl_asset_config config;
        if (file == NULL)
            status = KNURL_NOMEM;
        else {
            knurl_asset_config_init(&config);
            config.info = op->result.info;
            file->fd = op->fd;
            config.read = file_read;
            config.close = file_close;
            config.userdata = file;
            status = knurl_asset_new(&config, &op->result.asset, &error);
            if (status == KNURL_OK)
                op->fd = -1;
            else
                free(file);
        }
    }
    if (op->result.code == KNURL_STORAGE_OK && op->created != NULL) {
        knurl_storage_resource_config config;
        knurl_storage_resource_config_init(&config);
        config.kind =
            op->created->writing ? KNURL_STORAGE_WRITE : KNURL_STORAGE_LIST;
        config.userdata = op->created;
        config.close = resource_close;
        status = knurl_storage_resource_new(op->operation, &config,
                                            &op->result.resource, &error);
        if (status == KNURL_OK)
            op->created = NULL;
    }
    if (status != KNURL_OK)
        op->result.code =
            status == KNURL_NOMEM ? KNURL_STORAGE_NOMEM : KNURL_STORAGE_IO;
    if (op->scope_status != KNURL_OK && op->scope_error.status != KNURL_OK)
        op->result.error = op->scope_error;
    if (op->result.code != KNURL_STORAGE_OK &&
        op->result.code != KNURL_STORAGE_END &&
        op->result.error.status == KNURL_OK)
        (void)knurl_fail(
            &op->result.error,
            op->result.code == KNURL_STORAGE_NOMEM
                ? KNURL_NOMEM
                : (op->result.code == KNURL_STORAGE_LIMIT ? KNURL_LIMIT
                                                          : KNURL_IO),
            "storage_filesystem", knurl_storage_code_string(op->result.code));
    status =
        knurl_storage_operation_complete(op->operation, &op->result, &error);
    if (status == KNURL_OK) {
        op->result.asset = NULL;
        op->result.resource = NULL;
    } else {
        knurl_storage_result fallback;
        knurl_storage_result_init(&fallback);
        fallback.code = KNURL_STORAGE_NOMEM;
        fallback.committed = op->result.committed;
        fallback.partial = op->result.partial;
        fallback.created = op->result.created;
        (void)knurl_storage_operation_complete(op->operation, &fallback, NULL);
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
static knurl_status submit(struct disk_operation *op, knurl_error *error)
{
    knurl_job_config config;
    knurl_loop *loop = knurl_storage_operation_loop(op->operation);
    knurl_job_config_init(&config);
    config.work = native_work;
    config.retired = native_retired;
    config.userdata = op;
    config.timeout_disabled = 1;
    return knurl_executor_submit(loop->impl->storage_executor, &config,
                                 &op->job, error);
}
static void admission_ready(knurl_timer *timer, void *data)
{
    struct disk_operation *op = data;
    knurl_error error;
    knurl_status status;
    if (op->operation == NULL || op->job != NULL ||
        knurl_storage_operation_cancelled(op->operation)) {
        knurl_timer_cancel(timer);
        return;
    }
    status = submit(op, &error);
    if (status == KNURL_AGAIN)
        return;
    knurl_timer_cancel(timer);
    if (status == KNURL_OK)
        return;
    op->result.code =
        status == KNURL_NOMEM ? KNURL_STORAGE_NOMEM : KNURL_STORAGE_IO;
    op->result.error = error;
    (void)knurl_storage_operation_complete(op->operation, &op->result, NULL);
}
static knurl_status invoke(knurl_storage_operation *operation,
                           const knurl_storage_arguments *arguments, void *data,
                           knurl_error *error)
{
    struct filesystem *f = data;
    struct disk_operation *op;
    knurl_executor_config pool;
    knurl_loop *loop = knurl_storage_operation_loop(operation);
    knurl_status status;
    if (knurl_server_loop(f->server) != loop)
        return knurl_fail(error, KNURL_STATE, "storage_filesystem",
                          "provider server and operation loop differ");
    if (loop->impl->storage_executor == NULL) {
        knurl_executor_config_init(&pool);
        pool.retired = executor_retired;
        pool.userdata = loop;
        status = knurl_executor_new(f->server, &pool,
                                    &loop->impl->storage_executor, error);
        if (status != KNURL_OK)
            return status;
    }
    op = calloc(1, sizeof(*op));
    if (op == NULL)
        return knurl_fail(error, KNURL_NOMEM, "storage_filesystem",
                          "allocating operation");
    op->filesystem = f;
    op->references = 1;
    op->operation = operation;
    op->arguments = arguments;
    op->fd = -1;
    knurl_storage_result_init(&op->result);
    op->result.code = KNURL_STORAGE_IO;
    if (arguments->resource != NULL)
        op->resource = knurl_storage_resource_userdata(arguments->resource);
    status = knurl_storage_operation_set_userdata(operation, op, error);
    if (status != KNURL_OK) {
        free(op);
        return status;
    }
    if (knurl_storage_operation_has_scope_check(operation)) {
        status = knurl_waker_new(loop, scope_ready, scope_retired, op,
                                 &op->scope_waker, error);
        if (status != KNURL_OK)
            return status;
        ++op->references;
    }
    status = submit(op, error);
    if (status == KNURL_OK)
        return KNURL_AGAIN;
    if (status == KNURL_AGAIN) {
        status = knurl_loop_timer(loop, 1, 1, admission_ready,
                                  admission_retired, op, &op->admission, error);
        if (status == KNURL_OK) {
            ++op->references;
            return KNURL_AGAIN;
        }
    }
    return status;
}
static knurl_status cancel(knurl_storage_operation *operation,
                           const knurl_storage_arguments *arguments, void *data,
                           knurl_error *error)
{
    struct disk_operation *op = knurl_storage_operation_userdata(operation);
    (void)arguments;
    (void)data;
    if (op == NULL)
        return KNURL_OK;
    if (op->admission != NULL)
        knurl_timer_cancel(op->admission);
    if (op->job == NULL)
        return KNURL_OK;
    (void)knurl_job_cancel(op->job, error);
    return KNURL_AGAIN;
}
static void retired(knurl_storage_operation *operation,
                    const knurl_storage_outcome *outcome, void *data)
{
    struct disk_operation *op = knurl_storage_operation_userdata(operation);
    (void)outcome;
    (void)data;
    if (op == NULL)
        return;
    op->operation = NULL;
    if (op->scope_waker != NULL)
        (void)knurl_waker_destroy(op->scope_waker, NULL);
    if (op->admission != NULL)
        knurl_timer_cancel(op->admission);
    if (op->fd >= 0)
        close(op->fd);
    if (op->created != NULL)
        resource_close(op->created);
    if (op->result.asset != NULL)
        knurl_asset_close(op->result.asset, NULL);
    if (op->result.resource != NULL)
        knurl_storage_resource_close(op->result.resource, NULL);
    operation_unref(op);
}
knurl_status knurl_storage_filesystem_content_directory(
    const knurl_storage_filesystem_config *config, char *out, size_t capacity,
    knurl_error *error)
{
    int n;
    if (out != NULL && capacity != 0)
        out[0] = 0;
    if (out == NULL || capacity == 0 || config == NULL)
        return knurl_fail(error, KNURL_INVALID, "storage_filesystem",
                          "provide configuration and output capacity");
    if (config->root_dir != NULL) {
        if (config->root_dir[0] != '/' ||
            strlen(config->root_dir) >= KNURL_FS_STORAGE_PATH_MAX)
            return knurl_fail(error, KNURL_INVALID, "storage_filesystem",
                              "provide an absolute root within path capacity");
        n = snprintf(out, capacity, "%s", config->root_dir);
    } else {
        knurl_fs_config check;
        memset(&check, 0, sizeof(check));
        check.cache_dir = config->cache_dir;
        check.site_id = config->site_id;
        check.max_file_bytes = config->max_file_bytes;
        check.max_total_bytes = config->max_total_bytes;
        check.max_resources = config->max_resources;
        if (!knurl_fs_config_valid(&check))
            return knurl_fail(error, KNURL_INVALID, "storage_filesystem",
                              "provide an absolute cache, valid site "
                              "identifier and finite nonzero quotas");
        n = snprintf(out, capacity, "%s/webdav/%s/content", config->cache_dir,
                     config->site_id);
    }
    if (n < 0 || (size_t)n >= capacity ||
        (size_t)n >= KNURL_FS_STORAGE_PATH_MAX) {
        out[0] = 0;
        return knurl_fail(error, KNURL_OVERFLOW, "storage_filesystem",
                          "content directory exceeds output capacity");
    }
    knurl_error_clear(error);
    return KNURL_OK;
}
void knurl_storage_filesystem_config_init(
    knurl_storage_filesystem_config *config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    config->max_file_bytes = 16u * 1024u * 1024u;
    config->max_total_bytes = 128u * 1024u * 1024u;
    config->max_resources = 4096;
}
/* Standalone callers use the same disk dispatcher as executor jobs. All
 * multi-KiB scratch state remains on the heap on default musl worker stacks. */
struct synchronous_disk {
    struct filesystem filesystem;
    struct disk_operation operation;
    knurl_storage_arguments arguments;
    knurl_fs_entry entry;
    char path[KNURL_FS_PATH_MAX + 1], destination[KNURL_FS_PATH_MAX + 1];
};
static struct synchronous_disk *
synchronous_new(const knurl_storage_filesystem_config *config, const char *path,
                const char *destination, knurl_storage_code *code)
{
    struct synchronous_disk *s;
    *code = KNURL_STORAGE_INVALID;
    if (config == NULL || path == NULL)
        return NULL;
    s = calloc(1, sizeof(*s));
    if (s == NULL) {
        *code = KNURL_STORAGE_NOMEM;
        return NULL;
    }
    s->filesystem.config.cache_dir = config->cache_dir;
    s->filesystem.config.site_id = config->site_id;
    s->filesystem.config.root_dir = config->root_dir;
    s->filesystem.config.max_file_bytes = config->max_file_bytes;
    s->filesystem.config.max_total_bytes = config->max_total_bytes;
    s->filesystem.config.max_resources = config->max_resources;
    if (!knurl_fs_config_valid(&s->filesystem.config) ||
        !knurl_fs_path_normalize(path, s->path) ||
        (destination &&
         !knurl_fs_path_normalize(destination, s->destination))) {
        free(s);
        return NULL;
    }
    s->arguments.path = s->path;
    s->arguments.destination = destination ? s->destination : NULL;
    s->operation.filesystem = &s->filesystem;
    s->operation.arguments = &s->arguments;
    s->operation.fd = -1;
    knurl_storage_result_init(&s->operation.result);
    *code = KNURL_STORAGE_OK;
    return s;
}
static void synchronous_free(struct synchronous_disk *s)
{
    if (s->operation.fd >= 0)
        close(s->operation.fd);
    if (s->operation.created != NULL)
        resource_close(s->operation.created);
    if (s->operation.resource != NULL)
        resource_close(s->operation.resource);
    free(s);
}
static knurl_storage_code synchronous_run(struct synchronous_disk *s,
                                          knurl_storage_action action)
{
    knurl_bytes arguments = {0};
    s->arguments.action = action;
    (void)native_work(NULL, arguments, &s->operation, NULL);
    return s->operation.result.code;
}
int knurl_storage_filesystem_path_normalize(
    const char *path, char out[KNURL_FILESYSTEM_PATH_MAX + 1])
{
    return out != NULL && knurl_fs_path_normalize(path, out);
}
static knurl_storage_code synchronous_lookup(struct synchronous_disk *s,
                                             knurl_filesystem_entry *out)
{
    knurl_storage_code code;
    memset(out, 0, sizeof(*out));
    code = knurl_fs_lookup(&s->filesystem.config, s->path, &s->entry);
    if (code == KNURL_STORAGE_OK) {
        out->kind = (knurl_filesystem_entry_kind)s->entry.kind;
        out->size = s->entry.size;
        strcpy(out->storage_path, s->entry.storage_path);
    }
    return code;
}
knurl_storage_code
knurl_storage_filesystem_lookup(const knurl_storage_filesystem_config *config,
                                const char *path, knurl_filesystem_entry *entry)
{
    struct synchronous_disk *s;
    knurl_storage_code code;
    if (entry == NULL)
        return KNURL_STORAGE_INVALID;
    memset(entry, 0, sizeof(*entry));
    s = synchronous_new(config, path, NULL, &code);
    if (s == NULL)
        return code;
    code = synchronous_lookup(s, entry);
    synchronous_free(s);
    return code;
}
knurl_storage_code
knurl_storage_filesystem_read(const knurl_storage_filesystem_config *config,
                              const char *path, unsigned char **body,
                              size_t *size, knurl_filesystem_entry *entry)
{
    struct synchronous_disk *s;
    knurl_storage_code code;
    unsigned char *bytes = NULL;
    size_t total = 0, offset = 0;
    ssize_t n;
    knurl_fs_config content;
    int lock = -1;
    if (body == NULL || size == NULL || entry == NULL)
        return KNURL_STORAGE_INVALID;
    *body = NULL;
    *size = 0;
    s = synchronous_new(config, path, NULL, &code);
    if (s == NULL)
        return code;
    code = synchronous_lookup(s, entry);
    if (code != KNURL_STORAGE_OK || entry->kind != KNURL_FILESYSTEM_ENTRY_FILE)
        goto done;
    lock = knurl_fs_lock(&s->filesystem.config);
    if (lock < 0) {
        code = KNURL_STORAGE_IO;
        goto done;
    }
    code = synchronous_lookup(s, entry);
    if (code != KNURL_STORAGE_OK || entry->kind != KNURL_FILESYSTEM_ENTRY_FILE)
        goto done;
    if (!content_config(&s->filesystem.config, &content,
                        s->operation.recovery)) {
        code = KNURL_STORAGE_INVALID;
        goto done;
    }
    s->operation.fd = knurl_fs_direct_open_existing(&content, s->path, 0, NULL);
    if (s->operation.fd < 0 || !metadata(&s->operation, s->operation.fd,
                                         &s->operation.result.info, 1, NULL)) {
        code = KNURL_STORAGE_IO;
        goto done;
    }
    if (knurl_u64_to_size(s->operation.result.info.size, &total, NULL) !=
            KNURL_OK ||
        (config->root_dir != NULL && total > config->max_file_bytes) ||
        (total && (bytes = malloc(total)) == NULL)) {
        code = KNURL_STORAGE_IO;
        goto done;
    }
    while (offset < total) {
        n = read(s->operation.fd, bytes + offset, total - offset);
        if (n <= 0) {
            code = KNURL_STORAGE_IO;
            goto done;
        }
        offset += (size_t)n;
    }
    if (close(s->operation.fd))
        code = KNURL_STORAGE_IO;
    s->operation.fd = -1;
    if (code == KNURL_STORAGE_OK) {
        memcpy(entry->etag, s->operation.etag + 1, 64);
        entry->etag[64] = 0;
        entry->size = *size = total;
        *body = bytes;
        bytes = NULL;
    }
done:
    free(bytes);
    if (s->operation.fd >= 0) {
        close(s->operation.fd);
        s->operation.fd = -1;
    }
    if (lock >= 0)
        knurl_fs_unlock(lock);
    synchronous_free(s);
    return code;
}
knurl_storage_code
knurl_storage_filesystem_put(const knurl_storage_filesystem_config *config,
                             const char *path, const void *body, size_t size,
                             const char *match, const char *none)
{
    struct synchronous_disk *s;
    knurl_storage_code code;
    if (size && body == NULL)
        return KNURL_STORAGE_INVALID;
    s = synchronous_new(config, path, NULL, &code);
    if (s == NULL)
        return code;
    s->arguments.if_match = match;
    s->arguments.if_none_match = none;
    code = synchronous_run(s, KNURL_STORAGE_PUT_BEGIN);
    if (code == KNURL_STORAGE_OK) {
        s->operation.resource = s->operation.created;
        s->operation.created = NULL;
        s->arguments.bytes.data = body;
        s->arguments.bytes.length = size;
        code = synchronous_run(s, KNURL_STORAGE_PUT_WRITE);
        if (code == KNURL_STORAGE_OK)
            code = synchronous_run(s, KNURL_STORAGE_PUT_FINISH);
    }
    synchronous_free(s);
    return code;
}
static knurl_storage_code
synchronous_mutate(const knurl_storage_filesystem_config *config,
                   const char *path, const char *destination, int overwrite,
                   int depth, const char *match, const char *none,
                   knurl_storage_action action)
{
    struct synchronous_disk *s;
    knurl_storage_code code;
    if (action == KNURL_STORAGE_COPY && depth != 0 && depth != -1)
        return KNURL_STORAGE_INVALID;
    s = synchronous_new(config, path, destination, &code);
    if (s == NULL)
        return code;
    s->arguments.if_match = match;
    s->arguments.if_none_match = none;
    s->arguments.overwrite = overwrite;
    s->arguments.depth = depth;
    code = synchronous_run(s, action);
    synchronous_free(s);
    return code;
}
knurl_storage_code
knurl_storage_filesystem_delete(const knurl_storage_filesystem_config *config,
                                const char *path, const char *match,
                                const char *none)
{
    return synchronous_mutate(config, path, NULL, 0, -1, match, none,
                              KNURL_STORAGE_DELETE);
}
knurl_storage_code
knurl_storage_filesystem_mkcol(const knurl_storage_filesystem_config *config,
                               const char *path, const char *match,
                               const char *none)
{
    return synchronous_mutate(config, path, NULL, 0, -1, match, none,
                              KNURL_STORAGE_MKCOL);
}
knurl_storage_code
knurl_storage_filesystem_copy(const knurl_storage_filesystem_config *config,
                              const char *path, const char *destination,
                              int overwrite, int depth, const char *match,
                              const char *none)
{
    return synchronous_mutate(config, path, destination, overwrite, depth,
                              match, none, KNURL_STORAGE_COPY);
}
knurl_storage_code knurl_storage_filesystem_move(
    const knurl_storage_filesystem_config *config, const char *path,
    const char *destination, int overwrite, const char *match, const char *none)
{
    return synchronous_mutate(config, path, destination, overwrite, -1, match,
                              none, KNURL_STORAGE_MOVE);
}
knurl_storage_code
knurl_storage_filesystem_list(const knurl_storage_filesystem_config *config,
                              const char *path,
                              knurl_filesystem_list_fn callback, void *data)
{
    struct synchronous_disk *s;
    knurl_storage_code code;
    size_t size;
    int n, fd;
    knurl_fs_config content;
    if (callback == NULL)
        return KNURL_STORAGE_INVALID;
    s = synchronous_new(config, path, NULL, &code);
    if (s == NULL)
        return code;
    if (!content_config(&s->filesystem.config, &content,
                        s->operation.recovery)) {
        synchronous_free(s);
        return KNURL_STORAGE_INVALID;
    }
    fd = knurl_fs_direct_open_existing(&content, s->path, 1, NULL);
    code = fd < 0
               ? (errno == ENOENT ? KNURL_STORAGE_NOT_FOUND : KNURL_STORAGE_IO)
               : directory_cursor(&s->operation, fd);
    if (code == KNURL_STORAGE_OK) {
        s->operation.resource = s->operation.created;
        s->operation.created = NULL;
        for (;;) {
            knurl_asset_entry *entry = &s->operation.result.entry;
            code = list_next(&s->operation, &s->filesystem.config);
            if (code == KNURL_STORAGE_CONFLICT)
                continue;
            if (code != KNURL_STORAGE_OK)
                break;
            n = snprintf(s->destination, sizeof(s->destination), "%s%s%s",
                         s->path, !strcmp(s->path, "/") ? "" : "/",
                         entry->name);
            if (n < 0 || (size_t)n >= sizeof(s->destination) ||
                knurl_u64_to_size(entry->info.size, &size, NULL) != KNURL_OK) {
                code = KNURL_STORAGE_IO;
                break;
            }
            if (!callback(s->destination,
                          entry->info.kind == KNURL_ASSET_FILE
                              ? KNURL_FILESYSTEM_ENTRY_FILE
                              : KNURL_FILESYSTEM_ENTRY_COLLECTION,
                          size, data))
                break;
        }
        if (code == KNURL_STORAGE_END)
            code = KNURL_STORAGE_OK;
    }
    synchronous_free(s);
    return code;
}
knurl_status
knurl_storage_filesystem_new(knurl_server *server,
                             const knurl_storage_filesystem_config *config,
                             knurl_storage_provider **out, knurl_error *error)
{
    struct filesystem *f;
    knurl_storage_provider_config provider;
    knurl_storage_filesystem_config effective;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (server == NULL || config == NULL || out == NULL)
        return knurl_fail(
            error, KNURL_INVALID, "storage_filesystem",
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
        return knurl_fail(error, KNURL_NOMEM, "storage_filesystem",
                          "allocating provider");
    f->server = server;
    f->config.cache_dir = knurl_copy_text(effective.cache_dir);
    f->config.site_id = knurl_copy_text(effective.site_id);
    f->config.root_dir = knurl_copy_text(effective.root_dir);
    f->config.max_file_bytes = effective.max_file_bytes;
    f->config.max_total_bytes = effective.max_total_bytes;
    f->config.max_resources = effective.max_resources;
    if ((effective.cache_dir != NULL && f->config.cache_dir == NULL) ||
        (effective.site_id != NULL && f->config.site_id == NULL) ||
        (effective.root_dir != NULL && f->config.root_dir == NULL)) {
        filesystem_free(f);
        return KNURL_NOMEM;
    }
    if (!knurl_fs_config_valid(&f->config)) {
        filesystem_free(f);
        return knurl_fail(error, KNURL_INVALID, "storage_filesystem",
                          "absolute roots, site identifier and finite nonzero "
                          "quotas required");
    }
    knurl_storage_provider_config_init(&provider);
    provider.capabilities = KNURL_STORAGE_CAN_WRITE |
                            KNURL_STORAGE_CAN_CONDITIONAL |
                            KNURL_STORAGE_CAN_COPY | KNURL_STORAGE_CAN_MOVE |
                            KNURL_STORAGE_CAN_SCOPE_CHECK;
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
    status = knurl_storage_provider_new(&provider, out, error);
    if (status != KNURL_OK)
        filesystem_free(f);
    return status;
}

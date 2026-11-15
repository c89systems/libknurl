#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "asset_internal.h"
#include "body_internal.h"
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct filesystem {
    char *root;
    int fd;
};
struct file_asset {
    int fd;
    DIR *directory;
    char etag[192];
    char modified[32];
};
static knurl_status system_error(knurl_error *error, const char *message)
{
    int code = errno;
    knurl_status status;
    if (code == ENOENT || code == ENOTDIR || code == ELOOP || code == EACCES) {
        knurl_error_clear(error);
        return KNURL_EOF;
    }
    status = knurl_fail(error, code == ENOMEM ? KNURL_NOMEM : KNURL_IO,
                        "asset_filesystem", message);
    if (error != NULL) {
        error->source = KNURL_ERROR_SYSTEM;
        error->native_code = code;
    }
    return status;
}
static int open_directory(int parent, const char *name)
{
    return openat(parent, name,
                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}
/* Lift Vectis's no-symlink component walk, retaining the opened root. */
static int root_open(const char *path)
{
    int fd, next;
    char *copy, *segment, *slash;
    fd = open(path[0] == '/' ? "/" : ".",
              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return -1;
    copy = knurl_copy_text(path);
    if (copy == NULL) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }
    segment = copy;
    while (*segment == '/')
        ++segment;
    while (*segment != 0) {
        slash = strchr(segment, '/');
        if (slash != NULL)
            *slash = 0;
        next = open_directory(fd, segment);
        if (next < 0) {
            int code = errno;
            close(fd);
            free(copy);
            errno = code;
            return -1;
        }
        close(fd);
        fd = next;
        if (slash == NULL)
            break;
        segment = slash + 1;
        while (*segment == '/')
            ++segment;
    }
    free(copy);
    return fd;
}
static int relative_open(int root, const char *path)
{
    int fd, next;
    char *copy, *segment, *slash;
    fd = open_directory(root, ".");
    if (fd < 0)
        return -1;
    if (path[1] == 0)
        return fd;
    copy = knurl_copy_text(path + 1);
    if (copy == NULL) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }
    segment = copy;
    for (;;) {
        slash = strchr(segment, '/');
        if (slash != NULL)
            *slash = 0;
        next = slash != NULL
                   ? open_directory(fd, segment)
                   : openat(fd, segment,
                            O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (next < 0) {
            int code = errno;
            close(fd);
            free(copy);
            errno = code;
            return -1;
        }
        close(fd);
        fd = next;
        if (slash == NULL)
            break;
        segment = slash + 1;
    }
    free(copy);
    return fd;
}
static void metadata(const struct stat *s, knurl_asset_info *info, char *etag,
                     size_t capacity, char *modified, size_t date_capacity)
{
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed",
                                 "Thu", "Fri", "Sat"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm utc;
    long nanos;
    memset(info, 0, sizeof(*info));
    info->kind = S_ISDIR(s->st_mode) ? KNURL_ASSET_DIRECTORY : KNURL_ASSET_FILE;
    if (info->kind == KNURL_ASSET_FILE) {
        info->size.low = (unsigned long)s->st_size & 0xffffffffUL;
        info->size.high = (unsigned long)((s->st_size >> 16) >> 16);
    }
#ifdef __APPLE__
    nanos = s->st_mtimespec.tv_nsec;
#else
    nanos = s->st_mtim.tv_nsec;
#endif
    /* Metadata validators are deliberately weak, not content-hash claims. */
    snprintf(etag, capacity, "W/\"%lx-%lx-%lx-%lx-%lx-%lx-%lx-%lx\"",
             (unsigned long)s->st_dev, (unsigned long)((s->st_ino >> 16) >> 16),
             (unsigned long)s->st_ino & 0xffffffffUL, info->size.high,
             info->size.low, (unsigned long)((s->st_mtime >> 16) >> 16),
             (unsigned long)s->st_mtime & 0xffffffffUL, (unsigned long)nanos);
    info->etag = etag;
    if (s->st_mtime >= 0 && nanos >= 0 && nanos < 1000000000L) {
        info->has_modification_time = 1;
        info->modification_seconds.high =
            (unsigned long)((s->st_mtime >> 16) >> 16);
        info->modification_seconds.low =
            (unsigned long)s->st_mtime & 0xffffffffUL;
        info->modification_nanoseconds = (unsigned long)nanos;
    }
    if (gmtime_r(&s->st_mtime, &utc) != NULL && utc.tm_year >= -299 &&
        utc.tm_year <= 8099) {
        snprintf(modified, date_capacity, "%s, %02d %s %04d %02d:%02d:%02d GMT",
                 days[utc.tm_wday], utc.tm_mday, months[utc.tm_mon],
                 utc.tm_year + 1900, utc.tm_hour, utc.tm_min, utc.tm_sec);
        info->last_modified = modified;
    }
}
static knurl_status file_read(knurl_u64 position, void *bytes, size_t capacity,
                              size_t *produced, void *data, knurl_error *error)
{
    struct file_asset *file = data;
    off_t offset;
    ssize_t count;
    *produced = 0;
    if (!knurl_file_offset(position, &offset))
        return knurl_fail(error, KNURL_OVERFLOW, "asset_read",
                          "asset offset exceeds file representation");
    if (capacity > 65536u)
        capacity = 65536u;
    do {
        count = pread(file->fd, bytes, capacity, offset);
    } while (count < 0 && errno == EINTR);
    if (count < 0)
        return system_error(error, "reading opened asset file");
    if (count == 0)
        return KNURL_EOF;
    *produced = (size_t)count;
    return KNURL_OK;
}
static knurl_status file_next(knurl_asset_entry *out, void *data,
                              knurl_error *error)
{
    struct file_asset *file = data;
    struct dirent *entry;
    struct stat s;
    size_t skipped;
    for (skipped = 0; skipped < 64; ++skipped) {
        errno = 0;
        entry = readdir(file->directory);
        if (entry == NULL)
            return errno != 0 ? system_error(error, "reading asset directory")
                              : KNURL_EOF;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (fstatat(file->fd, entry->d_name, &s, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT)
                continue;
            return system_error(error, "inspecting asset directory child");
        }
        if ((!S_ISREG(s.st_mode) && !S_ISDIR(s.st_mode)) || s.st_size < 0)
            continue;
        out->name = entry->d_name;
        metadata(&s, &out->info, file->etag, sizeof(file->etag), file->modified,
                 sizeof(file->modified));
        return KNURL_OK;
    }
    return KNURL_AGAIN;
}
static void file_close(void *data)
{
    struct file_asset *file = data;
    if (file->directory != NULL)
        closedir(file->directory);
    close(file->fd);
    free(file);
}
static knurl_status filesystem_open(const char *path, knurl_asset **out,
                                    void *data, knurl_error *error)
{
    struct filesystem *filesystem = data;
    struct file_asset *file;
    struct stat s;
    knurl_asset_config config;
    knurl_status status;
    int fd, list;
    if (filesystem->fd < 0) {
        filesystem->fd = root_open(filesystem->root);
        if (filesystem->fd < 0)
            return system_error(error, "pinning asset root");
    }
    fd = relative_open(filesystem->fd, path);
    if (fd < 0)
        return system_error(error, "opening confined asset");
    if (fstat(fd, &s) != 0) {
        int code = errno;
        close(fd);
        errno = code;
        return system_error(error, "inspecting opened asset");
    }
    if ((!S_ISREG(s.st_mode) && !S_ISDIR(s.st_mode)) || s.st_size < 0) {
        close(fd);
        return KNURL_EOF;
    }
    file = calloc(1, sizeof(*file));
    if (file == NULL) {
        close(fd);
        return knurl_fail(error, KNURL_NOMEM, "asset_filesystem",
                          "allocating opened asset");
    }
    file->fd = fd;
    knurl_asset_config_init(&config);
    metadata(&s, &config.info, file->etag, sizeof(file->etag), file->modified,
             sizeof(file->modified));
    if (S_ISDIR(s.st_mode)) {
        list = open_directory(fd, ".");
        if (list >= 0)
            file->directory = fdopendir(list);
        if (file->directory == NULL) {
            int code = errno;
            if (list >= 0)
                close(list);
            file_close(file);
            errno = code;
            return system_error(error, "opening asset directory iterator");
        }
        config.next = file_next;
    } else
        config.read = file_read;
    config.close = file_close;
    config.userdata = file;
    status = knurl_asset_new(&config, out, error);
    if (status != KNURL_OK)
        file_close(file);
    return status;
}
static void filesystem_close(void *data)
{
    struct filesystem *filesystem = data;
    if (filesystem->fd >= 0)
        close(filesystem->fd);
    free(filesystem->root);
    free(filesystem);
}
knurl_status knurl_asset_filesystem_new(const char *root,
                                        knurl_asset_provider **out,
                                        knurl_error *error)
{
    struct filesystem *filesystem;
    knurl_asset_provider_config config;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (root == NULL || root[0] == 0 || out == NULL)
        return knurl_fail(error, KNURL_INVALID, "asset_filesystem",
                          "provide root directory and provider output");
    filesystem = calloc(1, sizeof(*filesystem));
    if (filesystem == NULL)
        return knurl_fail(error, KNURL_NOMEM, "asset_filesystem",
                          "allocating filesystem provider");
    filesystem->fd = -1;
    filesystem->root = knurl_copy_text(root);
    if (filesystem->root == NULL) {
        filesystem_close(filesystem);
        return knurl_fail(error, KNURL_NOMEM, "asset_filesystem",
                          "copying root directory");
    }
    knurl_asset_provider_config_init(&config);
    config.open = filesystem_open;
    config.close = filesystem_close;
    config.userdata = filesystem;
    status = knurl_asset_provider_new(&config, out, error);
    if (status != KNURL_OK)
        filesystem_close(filesystem);
    return status;
}

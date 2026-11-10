#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "access_log_file.h"
#include "file_internal.h"
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct knurl_access_file {
    const char *path;
    char *from, *to;
    size_t capacity, files, next_backup;
    off_t size, limit;
    mode_t mode;
    int fd, disabled, rotating;
};

static knurl_status fail(knurl_error *error, const char *operation,
                         const char *message, int code)
{
    knurl_fail(error, KNURL_IO, operation, message);
    if (error != NULL) {
        error->source = KNURL_ERROR_SYSTEM;
        error->native_code = code;
    }
    return KNURL_IO;
}

static knurl_status open_active(struct knurl_access_file *file,
                                knurl_error *error)
{
    struct stat st;
    int code;
    file->fd = open(file->path,
                    O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC | O_NONBLOCK |
                        O_NOFOLLOW,
                    file->mode);
    if (file->fd < 0)
        return fail(error, "access_log_open", "cannot open access.log", errno);
    if (fstat(file->fd, &st) == -1) {
        code = errno;
        close(file->fd);
        file->fd = -1;
        return fail(error, "access_log_open", "cannot inspect access.log",
                    code);
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0) {
        close(file->fd);
        file->fd = -1;
        return fail(error, "access_log_open",
                    "access.log must be a regular file", EINVAL);
    }
    file->size = st.st_size;
    file->mode = st.st_mode & 0777;
    return KNURL_OK;
}

knurl_status knurl_access_file_open(const knurl_http_config *config,
                                    struct knurl_access_file **out,
                                    knurl_error *error)
{
    struct knurl_access_file *file;
    size_t length = strlen(config->access_log_path);
    knurl_status status;
    *out = NULL;
    file = calloc(1, sizeof(*file));
    if (file == NULL)
        return knurl_fail(error, KNURL_NOMEM, "access_log_open",
                          "allocating access.log state");
    file->fd = -1;
    file->mode = 0644;
    file->path = config->access_log_path;
    file->files = config->access_log_file_count;
    file->disabled = config->access_log_rotation_disabled;
    if (file->files == 0 ||
        !knurl_file_offset(config->access_log_max_bytes, &file->limit) ||
        file->limit <= 0 || length > (size_t)-1 - sizeof(size_t) * 3 - 2) {
        knurl_access_file_close(file);
        return knurl_fail(error, KNURL_INVALID, "access_log_open",
                          "invalid access.log rotation declaration");
    }
    file->capacity = length + sizeof(size_t) * 3 + 2;
    file->from = malloc(file->capacity);
    file->to = malloc(file->capacity);
    if (file->from == NULL || file->to == NULL) {
        knurl_access_file_close(file);
        return knurl_fail(error, KNURL_NOMEM, "access_log_open",
                          "allocating access.log paths");
    }
    status = open_active(file, error);
    if (status != KNURL_OK) {
        knurl_access_file_close(file);
        return status;
    }
    *out = file;
    knurl_error_clear(error);
    return KNURL_OK;
}

static knurl_status rotate(struct knurl_access_file *file, knurl_error *error)
{
    size_t index;
    int code = 0;
    if (file->files == 1) {
        if (ftruncate(file->fd, 0) == -1)
            return fail(error, "access_log_rotate",
                        "cannot truncate access.log", errno);
        file->size = 0;
        return KNURL_OK;
    }
    if (!file->rotating) {
        file->rotating = 1;
        file->next_backup = file->files;
    }
    /* Resume a failed rename chain at its failed step. Repeating completed
     * shifts would evict history on every retry without accepting a record. */
    if (file->next_backup == file->files) {
        snprintf(file->to, file->capacity, "%s.%lu", file->path,
                 (unsigned long)(file->files - 1));
        if (unlink(file->to) == -1 && errno != ENOENT)
            return fail(error, "access_log_rotate", "cannot evict oldest log",
                        errno);
        --file->next_backup;
    }
    while ((index = file->next_backup) != 0) {
        snprintf(file->to, file->capacity, "%s.%lu", file->path,
                 (unsigned long)index);
        if (index == 1)
            snprintf(file->from, file->capacity, "%s", file->path);
        else
            snprintf(file->from, file->capacity, "%s.%lu", file->path,
                     (unsigned long)(index - 1));
        if (rename(file->from, file->to) == -1 && errno != ENOENT)
            return fail(error, "access_log_rotate", "cannot shift log history",
                        errno);
        --file->next_backup;
    }
    if (close(file->fd) == -1)
        code = errno;
    file->fd = -1;
    file->size = 0;
    file->rotating = 0;
    if (code != 0)
        return fail(error, "access_log_rotate", "cannot close rotated log",
                    code);
    return open_active(file, error);
}

knurl_status knurl_access_file_write(struct knurl_access_file *file,
                                     const void *record, size_t length,
                                     knurl_error *error)
{
    const unsigned char *bytes = record;
    knurl_u64 count;
    off_t incoming, before;
    size_t used = 0;
    ssize_t written;
    int code;
    knurl_status status;
    if (length == 0) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    if (file->fd < 0 && open_active(file, error) != KNURL_OK)
        return error != NULL ? error->status : KNURL_IO;
    knurl_u64_from_size(length, &count, NULL);
    if (!knurl_file_offset(count, &incoming))
        return fail(error, "access_log_write", "access record is too large",
                    EOVERFLOW);
    if (!file->disabled &&
        (file->rotating ||
         (file->size != 0 && (file->size >= file->limit ||
                              incoming > file->limit - file->size)))) {
        status = rotate(file, error);
        if (status != KNURL_OK)
            return status;
    }
    before = file->size;
    while (used < length) {
        written = write(file->fd, bytes + used, length - used);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            code = written == 0 ? EIO : errno;
            /* Remove a partially appended record when the filesystem permits.
             * The sink owns this file exclusively; other writers are
             * unsupported. */
            if (used != 0 && ftruncate(file->fd, before) == 0)
                file->size = before;
            return fail(error, "access_log_write",
                        "cannot append access record", code);
        }
        used += (size_t)written;
        file->size += (off_t)written;
    }
    knurl_error_clear(error);
    return KNURL_OK;
}

int knurl_access_file_close(struct knurl_access_file *file)
{
    int code = 0;
    if (file != NULL) {
        if (file->fd >= 0 && close(file->fd) == -1)
            code = errno;
        free(file->from);
        free(file->to);
        free(file);
    }
    return code;
}

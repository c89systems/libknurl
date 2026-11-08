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

struct rotmos_access_file {
    const char *path;
    char *from, *to;
    size_t capacity, files, next_backup;
    off_t size, limit;
    mode_t mode;
    int fd, disabled, rotating;
};

static rotmos_status fail(rotmos_error *error, const char *operation,
                          const char *message, int code)
{
    rotmos_fail(error, ROTMOS_IO, operation, message);
    if (error != NULL) {
        error->source = ROTMOS_ERROR_SYSTEM;
        error->native_code = code;
    }
    return ROTMOS_IO;
}

static rotmos_status open_active(struct rotmos_access_file *file,
                                 rotmos_error *error)
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
    return ROTMOS_OK;
}

rotmos_status rotmos_access_file_open(const rotmos_http_config *config,
                                      struct rotmos_access_file **out,
                                      rotmos_error *error)
{
    struct rotmos_access_file *file;
    size_t length = strlen(config->access_log_path);
    rotmos_status status;
    *out = NULL;
    file = calloc(1, sizeof(*file));
    if (file == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "access_log_open",
                           "allocating access.log state");
    file->fd = -1;
    file->mode = 0644;
    file->path = config->access_log_path;
    file->files = config->access_log_file_count;
    file->disabled = config->access_log_rotation_disabled;
    if (file->files == 0 ||
        !rotmos_file_offset(config->access_log_max_bytes, &file->limit) ||
        file->limit <= 0 || length > (size_t)-1 - sizeof(size_t) * 3 - 2) {
        rotmos_access_file_close(file);
        return rotmos_fail(error, ROTMOS_INVALID, "access_log_open",
                           "invalid access.log rotation declaration");
    }
    file->capacity = length + sizeof(size_t) * 3 + 2;
    file->from = malloc(file->capacity);
    file->to = malloc(file->capacity);
    if (file->from == NULL || file->to == NULL) {
        rotmos_access_file_close(file);
        return rotmos_fail(error, ROTMOS_NOMEM, "access_log_open",
                           "allocating access.log paths");
    }
    status = open_active(file, error);
    if (status != ROTMOS_OK) {
        rotmos_access_file_close(file);
        return status;
    }
    *out = file;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

static rotmos_status rotate(struct rotmos_access_file *file,
                            rotmos_error *error)
{
    size_t index;
    int code = 0;
    if (file->files == 1) {
        if (ftruncate(file->fd, 0) == -1)
            return fail(error, "access_log_rotate",
                        "cannot truncate access.log", errno);
        file->size = 0;
        return ROTMOS_OK;
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

rotmos_status rotmos_access_file_write(struct rotmos_access_file *file,
                                       const void *record, size_t length,
                                       rotmos_error *error)
{
    const unsigned char *bytes = record;
    rotmos_u64 count;
    off_t incoming, before;
    size_t used = 0;
    ssize_t written;
    int code;
    rotmos_status status;
    if (length == 0) {
        rotmos_error_clear(error);
        return ROTMOS_OK;
    }
    if (file->fd < 0 && open_active(file, error) != ROTMOS_OK)
        return error != NULL ? error->status : ROTMOS_IO;
    rotmos_u64_from_size(length, &count, NULL);
    if (!rotmos_file_offset(count, &incoming))
        return fail(error, "access_log_write", "access record is too large",
                    EOVERFLOW);
    if (!file->disabled &&
        (file->rotating ||
         (file->size != 0 && (file->size >= file->limit ||
                              incoming > file->limit - file->size)))) {
        status = rotate(file, error);
        if (status != ROTMOS_OK)
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
    rotmos_error_clear(error);
    return ROTMOS_OK;
}

int rotmos_access_file_close(struct rotmos_access_file *file)
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

#define _POSIX_C_SOURCE 200809L
#include "acme_internal.h"

#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static rotmos_status io_error(rotmos_error *error, const char *message)
{
    int saved;
    saved = errno;
    (void)rotmos_fail(error, ROTMOS_IO, "acme_files", message);
    if (error != NULL) {
        error->source = ROTMOS_ERROR_SYSTEM;
        error->native_code = saved;
    }
    return ROTMOS_IO;
}

char *rotmos_acme_path(const char *directory, const char *leaf)
{
    size_t a;
    size_t b;
    char *result;
    a = strlen(directory);
    b = strlen(leaf);
    if (a > (size_t)-1 - b - 2)
        return NULL;
    result = malloc(a + b + 2);
    if (result != NULL) {
        memcpy(result, directory, a);
        result[a] = '/';
        memcpy(result + a + 1, leaf, b + 1);
    }
    return result;
}

rotmos_status rotmos_acme_sync(int descriptor, rotmos_error *error)
{
    int result;
    do {
        result = fsync(descriptor);
    } while (result < 0 && errno == EINTR);
    return result == 0 ? ROTMOS_OK
                       : io_error(error, "cannot synchronize ACME state");
}

rotmos_status rotmos_acme_directory(const char *path, int create,
                                    int private_mode, rotmos_error *error)
{
    struct stat info;
    if (create && mkdir(path, 0700) != 0 && errno != EEXIST)
        return io_error(error, "cannot create private ACME directory");
    if (lstat(path, &info) != 0)
        return io_error(error, "cannot inspect prepared ACME directory");
    if (!S_ISDIR(info.st_mode) || (private_mode && (info.st_uid != geteuid() ||
                                                    (info.st_mode & 077) != 0)))
        return rotmos_fail(error, ROTMOS_INVALID, "acme_directory",
                           "ACME directory must be a real directory; private "
                           "roots require owner-only access");
    return ROTMOS_OK;
}

static int erase(int parent, const char *name, unsigned int depth)
{
    struct stat info;
    struct dirent *entry;
    DIR *directory;
    int fd;
    int okay;
    if (depth > 4)
        return 0;
    if (fstatat(parent, name, &info, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(info.st_mode))
        return unlinkat(parent, name, 0) == 0;
    fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return 0;
    directory = fdopendir(fd);
    if (directory == NULL) {
        (void)close(fd);
        return 0;
    }
    okay = 1;
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (!erase(fd, entry->d_name, depth + 1)) {
            okay = 0;
            break;
        }
        errno = 0;
    }
    if (errno != 0)
        okay = 0;
    if (closedir(directory) != 0)
        okay = 0;
    return okay && unlinkat(parent, name, AT_REMOVEDIR) == 0;
}

static int generated(const char *name, const char *prefix)
{
    size_t i;
    size_t length;
    unsigned char byte;
    length = strlen(prefix);
    if (strlen(name) != length + 6 || memcmp(name, prefix, length))
        return 0;
    for (i = length; i < length + 6; ++i) {
        byte = (unsigned char)name[i];
        if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
              (byte >= '0' && byte <= '9')))
            return 0;
    }
    return 1;
}

int rotmos_acme_remove(struct rotmos_acme_store *store, const char *path)
{
    const char *name;
    size_t length;
    length = strlen(store->directory);
    if (strncmp(path, store->directory, length) || path[length] != '/')
        return 0;
    name = path + length + 1;
    if (!generated(name, "generation-") && !generated(name, "work-"))
        return 0;
    return erase(store->descriptor, name, 0);
}

rotmos_status rotmos_acme_selected(struct rotmos_acme_store *store,
                                   const char *name, char **out,
                                   rotmos_error *error)
{
    char target[64];
    ssize_t length;
    *out = NULL;
    length = readlinkat(store->descriptor, name, target, sizeof(target) - 1);
    if (length < 0) {
        if (errno == ENOENT)
            return ROTMOS_EOF;
        return io_error(error, "ACME selection must be a generation symlink");
    }
    target[length] = '\0';
    if (!generated(target, "generation-"))
        return rotmos_fail(
            error, ROTMOS_INVALID, "acme_selection",
            "ACME selection must reference a private generation basename");
    *out = rotmos_acme_path(store->directory, target);
    if (*out == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "acme_selection",
                           "cannot copy generation selection");
    return rotmos_acme_directory(*out, 0, 1, error);
}

rotmos_status rotmos_acme_select(struct rotmos_acme_store *store,
                                 const char *name, const char *generation,
                                 rotmos_error *error)
{
    const char *target;
    char *temporary;
    char *selected;
    int fd;
    rotmos_status status;
    target = strrchr(generation, '/');
    if (target == NULL || !generated(target + 1, "generation-"))
        return rotmos_fail(error, ROTMOS_INVALID, "acme_selection",
                           "invalid generation selection");
    temporary = rotmos_acme_path(store->directory, "selection-XXXXXX");
    selected = rotmos_acme_path(store->directory, name);
    if (temporary == NULL || selected == NULL) {
        free(temporary);
        free(selected);
        return rotmos_fail(error, ROTMOS_NOMEM, "acme_selection",
                           "cannot allocate selection paths");
    }
    fd = mkstemp(temporary);
    status = ROTMOS_OK;
    if (fd < 0)
        status = io_error(error, "cannot create ACME selection temporary");
    else {
        (void)close(fd);
        if (unlink(temporary) != 0 || symlink(target + 1, temporary) != 0 ||
            rename(temporary, selected) != 0)
            status =
                io_error(error, "cannot atomically select ACME generation");
        (void)unlink(temporary);
    }
    free(temporary);
    free(selected);
    return status;
}

static rotmos_status copy_file(const char *source, const char *destination,
                               size_t maximum, rotmos_error *error)
{
    struct stat info;
    unsigned char *buffer;
    size_t total;
    size_t offset;
    ssize_t count;
    ssize_t written;
    int input;
    int output;
    rotmos_status status;
    input = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (input < 0)
        return errno == ENOENT
                   ? ROTMOS_EOF
                   : io_error(error, "cannot open ACME state input");
    status = ROTMOS_OK;
    if (fstat(input, &info) != 0)
        status = io_error(error, "cannot inspect ACME state input");
    else if (!S_ISREG(info.st_mode) || info.st_size <= 0)
        status =
            rotmos_fail(error, ROTMOS_INVALID, "acme_snapshot",
                        "ACME state input must be a nonempty regular file");
    else if (info.st_size > (off_t)maximum)
        status = rotmos_fail(error, ROTMOS_LIMIT, "acme_snapshot",
                             "ACME state input exceeds max_material_bytes");
    output = -1;
    buffer = status == ROTMOS_OK ? malloc(8192) : NULL;
    if (status == ROTMOS_OK && buffer == NULL)
        status = rotmos_fail(error, ROTMOS_NOMEM, "acme_snapshot",
                             "cannot allocate bounded snapshot buffer");
    if (status == ROTMOS_OK) {
        output =
            open(destination,
                 O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (output < 0)
            status = io_error(error, "cannot create ACME snapshot file");
    }
    total = 0;
    while (status == ROTMOS_OK) {
        do {
            count = read(input, buffer, 8192);
        } while (count < 0 && errno == EINTR);
        if (count == 0)
            break;
        if (count < 0) {
            status = io_error(error, "cannot read ACME state input");
            break;
        }
        if ((size_t)count > maximum - total) {
            status =
                rotmos_fail(error, ROTMOS_LIMIT, "acme_snapshot",
                            "growing ACME input exceeds max_material_bytes");
            break;
        }
        total += (size_t)count;
        offset = 0;
        while (offset < (size_t)count) {
            do {
                written =
                    write(output, buffer + offset, (size_t)count - offset);
            } while (written < 0 && errno == EINTR);
            if (written <= 0) {
                status = io_error(error, "cannot write ACME snapshot");
                break;
            }
            offset += (size_t)written;
        }
    }
    if (status == ROTMOS_OK)
        status = rotmos_acme_sync(output, error);
    if (output >= 0 && close(output) != 0 && status == ROTMOS_OK)
        status = io_error(error, "cannot close ACME snapshot");
    (void)close(input);
    if (buffer != NULL) {
        OPENSSL_cleanse(buffer, 8192);
        free(buffer);
    }
    return status;
}

static rotmos_status copy_pair(struct rotmos_acme_store *store,
                               const char *source, const char *destination,
                               rotmos_error *error)
{
    static const char *leaves[] = {"key.pem", "fullchain.pem"};
    char *input;
    char *output;
    rotmos_status status;
    size_t i;
    int fd;
    status = ROTMOS_OK;
    for (i = 0; i < 2 && status == ROTMOS_OK; ++i) {
        input = rotmos_acme_path(source, leaves[i]);
        output = rotmos_acme_path(destination, leaves[i]);
        if (input == NULL || output == NULL)
            status = rotmos_fail(error, ROTMOS_NOMEM, "acme_snapshot",
                                 "cannot allocate identity snapshot paths");
        else {
            status = copy_file(input, output, store->config->max_material_bytes,
                               error);
            if (status == ROTMOS_EOF)
                status = ROTMOS_OK;
        }
        free(input);
        free(output);
    }
    fd =
        status == ROTMOS_OK
            ? open(destination, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)
            : -1;
    if (fd >= 0) {
        status = rotmos_acme_sync(fd, error);
        (void)close(fd);
    } else if (status == ROTMOS_OK)
        status = io_error(error, "cannot open identity snapshot directory");
    return status;
}

rotmos_status rotmos_acme_snapshot(struct rotmos_acme_store *store,
                                   const char *source, const char *prefix,
                                   char **out, rotmos_error *error)
{
    char *path;
    char *from;
    char *to;
    char *source_certs;
    char *destination_certs;
    struct stat info;
    rotmos_status status;
    size_t i;
    int fd;
    *out = NULL;
    path = rotmos_acme_path(store->directory, prefix);
    if (path == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "acme_snapshot",
                           "cannot allocate snapshot directory");
    if (mkdtemp(path) == NULL) {
        free(path);
        return io_error(error, "cannot create ACME snapshot directory");
    }
    from = rotmos_acme_path(source, "account-key.pem");
    to = rotmos_acme_path(path, "account-key.pem");
    status =
        from == NULL || to == NULL
            ? rotmos_fail(error, ROTMOS_NOMEM, "acme_snapshot",
                          "cannot allocate account snapshot paths")
            : copy_file(from, to, store->config->max_material_bytes, error);
    if (status == ROTMOS_EOF)
        status = ROTMOS_OK;
    free(from);
    free(to);
    source_certs = rotmos_acme_path(source, "certificates");
    destination_certs = rotmos_acme_path(path, "certificates");
    if (source_certs == NULL || destination_certs == NULL)
        status = rotmos_fail(error, ROTMOS_NOMEM, "acme_snapshot",
                             "cannot allocate certificate snapshot paths");
    if (status == ROTMOS_OK)
        status = rotmos_acme_directory(destination_certs, 1, 1, error);
    if (status == ROTMOS_OK && lstat(source_certs, &info) == 0)
        status = rotmos_acme_directory(source_certs, 0, 0, error);
    else if (status == ROTMOS_OK && errno != ENOENT)
        status = io_error(error, "cannot inspect certificate source directory");
    for (i = 0; i < store->domain_count && status == ROTMOS_OK; ++i) {
        from = rotmos_acme_path(source_certs, store->domains[i]);
        to = rotmos_acme_path(destination_certs, store->domains[i]);
        if (from == NULL || to == NULL)
            status = rotmos_fail(error, ROTMOS_NOMEM, "acme_snapshot",
                                 "cannot allocate domain snapshot paths");
        else {
            status = rotmos_acme_directory(to, 1, 1, error);
            if (status == ROTMOS_OK && lstat(from, &info) == 0)
                status = rotmos_acme_directory(from, 0, 0, error);
            else if (status == ROTMOS_OK && errno != ENOENT)
                status =
                    io_error(error, "cannot inspect domain source directory");
            if (status == ROTMOS_OK)
                status = copy_pair(store, from, to, error);
        }
        free(from);
        free(to);
    }
    fd = status == ROTMOS_OK
             ? open(destination_certs,
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)
             : -1;
    if (fd >= 0) {
        status = rotmos_acme_sync(fd, error);
        (void)close(fd);
    } else if (status == ROTMOS_OK)
        status = io_error(error, "cannot open certificate snapshot directory");
    fd = status == ROTMOS_OK
             ? open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)
             : -1;
    if (fd >= 0) {
        status = rotmos_acme_sync(fd, error);
        (void)close(fd);
    } else if (status == ROTMOS_OK)
        status = io_error(error, "cannot open snapshot directory");
    free(source_certs);
    free(destination_certs);
    if (status != ROTMOS_OK) {
        (void)rotmos_acme_remove(store, path);
        free(path);
        return status;
    }
    *out = path;
    return ROTMOS_OK;
}

rotmos_status rotmos_acme_sweep(struct rotmos_acme_store *store,
                                rotmos_error *error)
{
    DIR *directory;
    struct dirent *entry;
    char *active;
    char *pending;
    char *path;
    rotmos_status status;
    int fd;
    active = pending = NULL;
    status = rotmos_acme_selected(store, "active", &active, error);
    if (status == ROTMOS_EOF)
        status = ROTMOS_OK;
    if (status == ROTMOS_OK) {
        status = rotmos_acme_selected(store, "pending", &pending, error);
        if (status == ROTMOS_EOF)
            status = ROTMOS_OK;
    }
    fd = status == ROTMOS_OK
             ? open(store->directory,
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)
             : -1;
    directory = fd >= 0 ? fdopendir(fd) : NULL;
    if (directory == NULL && status == ROTMOS_OK) {
        if (fd >= 0)
            (void)close(fd);
        status = io_error(error, "cannot inspect private ACME generations");
    }
    errno = 0;
    while (directory != NULL && status == ROTMOS_OK &&
           (entry = readdir(directory)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
            !strcmp(entry->d_name, "active") ||
            !strcmp(entry->d_name, "pending"))
            continue;
        if (!generated(entry->d_name, "generation-") &&
            !generated(entry->d_name, "work-") &&
            !generated(entry->d_name, "selection-")) {
            status =
                rotmos_fail(error, ROTMOS_INVALID, "acme_sweep",
                            "unknown entry in library-owned ACME directory");
            break;
        }
        path = rotmos_acme_path(store->directory, entry->d_name);
        if (path == NULL)
            status = rotmos_fail(error, ROTMOS_NOMEM, "acme_sweep",
                                 "cannot allocate cleanup path");
        else if ((!active || strcmp(active, path)) &&
                 (!pending || strcmp(pending, path)) &&
                 (!store->work || strcmp(store->work, path)) &&
                 !erase(fd, entry->d_name, 0))
            status =
                io_error(error, "cannot remove unselected ACME generation");
        free(path);
        errno = 0;
    }
    if (directory != NULL) {
        if (status == ROTMOS_OK && errno != 0)
            status = io_error(error, "cannot read private ACME directory");
        if (closedir(directory) != 0 && status == ROTMOS_OK)
            status = io_error(error, "cannot close private ACME directory");
    }
    free(active);
    free(pending);
    return status;
}

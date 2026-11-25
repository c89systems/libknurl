#define _POSIX_C_SOURCE 200809L
#include "auth_json.h"
#include "json_internal.h"
#include <openssl/crypto.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const lonejson_field fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(struct auth_record, subject, "subject",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(struct auth_record, email, "email",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(struct auth_record, secret, "secret",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(struct auth_record, seed, "seed",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(struct auth_record, revision, "revision",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64(struct auth_record, expires, "expires"),
    LONEJSON_FIELD_I64(struct auth_record, updated, "updated"),
    LONEJSON_FIELD_I64(struct auth_record, last_step, "last_step"),
    LONEJSON_FIELD_I64(struct auth_record, attempts, "attempts"),
    LONEJSON_FIELD_BOOL(struct auth_record, disabled, "disabled")};
LONEJSON_MAP_DEFINE(record_map, struct auth_record, fields);

static knurl_status io(knurl_error *error, const char *message)
{
    return knurl_fail(error, KNURL_IO, "auth_json", message);
}

static int private_fd(int fd, int directory)
{
    struct stat info;
    return fstat(fd, &info) == 0 && info.st_uid == geteuid() &&
           (info.st_mode & 077) == 0 &&
           (directory ? S_ISDIR(info.st_mode)
                      : S_ISREG(info.st_mode) && info.st_nlink == 1);
}

static int open_directory(const char *path, knurl_error *error)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0 && private_fd(fd, 1))
        return fd;
    if (fd >= 0)
        close(fd);
    io(error, "state directory must exist and be owned and accessible only by "
              "the current user");
    return -1;
}

int knurl_auth_json_lock(knurl_auth_json *json, knurl_error *error)
{
    static const struct timespec interval = {0, 10000000L};
    int count, fd = openat(json->directory, "lock",
                           O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd >= 0 && private_fd(fd, 0)) {
        for (count = 0; count < 100; ++count) {
            if (flock(fd, LOCK_EX | LOCK_NB) == 0)
                return fd;
            if (errno != EWOULDBLOCK && errno != EINTR)
                break;
            (void)nanosleep(&interval, NULL);
        }
    }
    if (fd >= 0)
        close(fd);
    io(error, "cannot acquire the private state lock");
    return -1;
}

static int save(int directory, const char *name, const char *data, size_t size)
{
    char temp[40];
    int fd, ok;
    FILE *file;
    memcpy(temp, ".new-", 5);
    if (!knurl_auth_random_hex(temp + 5, 16))
        return 0;
    fd = openat(directory, temp,
                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return 0;
    file = fdopen(fd, "wb");
    if (file == NULL) {
        close(fd);
        ok = 0;
    } else {
        ok = fwrite(data, 1, size, file) == size && fflush(file) == 0 &&
             fsync(fd) == 0;
        if (fclose(file) != 0)
            ok = 0;
    }
    if (ok)
        ok = renameat(directory, temp, directory, name) == 0 &&
             fsync(directory) == 0;
    if (!ok)
        (void)unlinkat(directory, temp, 0);
    return ok;
}

static knurl_status read_key(knurl_auth_json *json, knurl_error *error)
{
    char data[66];
    ssize_t count;
    int fd = openat(json->directory, "key",
                    O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? KNURL_EOF
                               : io(error, "cannot open the state key");
    count = private_fd(fd, 0) ? read(fd, data, sizeof(data)) : -1;
    close(fd);
    if (count != 64) {
        OPENSSL_cleanse(data, sizeof(data));
        return io(error,
                  "state key must be an owner-only file with 64 hex digits");
    }
    data[64] = 0;
    count = knurl_auth_unhex(data, json->key, sizeof(json->key));
    OPENSSL_cleanse(data, sizeof(data));
    return count ? KNURL_OK : io(error, "state key is malformed");
}

static knurl_status scan(knurl_auth_json *json, int prune, knurl_error *error)
{
    int fd = openat(json->directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *entries = fd >= 0 ? fdopendir(fd) : NULL;
    struct dirent *entry;
    struct auth_record record;
    knurl_status status = KNURL_OK;
    const char *name, *kinds = prune ? "sape" : "ucsape";
    lonejson_int64 now = (lonejson_int64)time(NULL);
    int changed = 0;
    if (entries == NULL || now < 0) {
        if (entries != NULL)
            closedir(entries);
        else if (fd >= 0)
            close(fd);
        return io(error, "cannot inspect state directory");
    }
    while (status == KNURL_OK) {
        errno = 0;
        entry = readdir(entries);
        if (entry == NULL) {
            if (errno != 0)
                status = io(error, "cannot inspect state directory");
            break;
        }
        name = entry->d_name;
        if (strlen(name) != 71 || strchr(kinds, name[0]) == NULL ||
            name[1] != '-' || strspn(name + 2, "0123456789abcdef") != 64 ||
            strcmp(name + 66, ".json") != 0)
            continue;
        if (!prune) {
            status = knurl_fail(error, KNURL_STATE, "auth_init",
                                "state records exist but the key is missing; "
                                "restore the original key");
            break;
        }
        status = knurl_auth_json_read(json, name, &record, error);
        if (status == KNURL_EOF)
            status = KNURL_OK;
        else if (status == KNURL_OK && record.expires <= now) {
            if (unlinkat(json->directory, name, 0) != 0)
                status = io(error, "cannot remove expired state record");
            else
                changed = 1;
        }
    }
    closedir(entries);
    if (changed && fsync(json->directory) != 0 && status == KNURL_OK)
        status = io(error, "cannot sync state directory");
    return status;
}

knurl_status knurl_auth_json_init(const char *path, knurl_error *error)
{
    knurl_auth_json json;
    knurl_status status;
    int lock, fd;
    char key[65];
    if (path == NULL || *path == 0)
        return knurl_fail(error, KNURL_INVALID, "auth_init",
                          "state directory is required");
    if (mkdir(path, 0700) != 0 && errno != EEXIST)
        return io(error, "cannot create state directory");
    json.directory = open_directory(path, error);
    if (json.directory < 0)
        return KNURL_IO;
    fd = openat(json.directory, "lock",
                O_RDONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd >= 0)
        close(fd);
    lock = knurl_auth_json_lock(&json, error);
    status = lock >= 0 ? read_key(&json, error) : KNURL_IO;
    if (status == KNURL_EOF) {
        status = scan(&json, 0, error);
        if (status == KNURL_OK && (!knurl_auth_random_hex(key, 32) ||
                                   !save(json.directory, "key", key, 64)))
            status = io(error, "cannot create the state key");
        OPENSSL_cleanse(key, sizeof(key));
    }
    OPENSSL_cleanse(json.key, sizeof(json.key));
    if (lock >= 0)
        close(lock);
    close(json.directory);
    if (status == KNURL_OK)
        knurl_error_clear(error);
    return status;
}

knurl_status knurl_auth_json_open(const char *path, knurl_auth_json **out,
                                  knurl_error *error)
{
    knurl_auth_json *json;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (path == NULL || out == NULL)
        return knurl_fail(error, KNURL_INVALID, "auth_open",
                          "state directory and output are required");
    json = calloc(1, sizeof(*json));
    if (json == NULL)
        return knurl_fail(error, KNURL_NOMEM, "auth_open",
                          "allocating state handle");
    json->references = 1;
    json->directory = open_directory(path, error);
    status = json->directory >= 0 ? read_key(json, error) : KNURL_IO;
    if (status != KNURL_OK) {
        knurl_auth_json_close(json);
        return status == KNURL_EOF
                   ? io(error, "state key missing; initialize the store first")
                   : status;
    }
    *out = json;
    knurl_error_clear(error);
    return KNURL_OK;
}

void knurl_auth_json_retain(knurl_auth_json *json)
{
    if (json != NULL)
        ++json->references;
}

void knurl_auth_json_close(knurl_auth_json *json)
{
    if (json == NULL || --json->references != 0)
        return;
    if (json->directory >= 0)
        close(json->directory);
    OPENSSL_cleanse(json->key, sizeof(json->key));
    free(json);
}

knurl_status knurl_auth_json_read(knurl_auth_json *json, const char *name,
                                  struct auth_record *record,
                                  knurl_error *error)
{
    char data[4096];
    int fd, owned;
    FILE *file;
    size_t length;
    lonejson *runtime;
    lonejson_status code;
    knurl_status result;
    memset(record, 0, sizeof(*record));
    fd = openat(json->directory, name,
                O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? KNURL_EOF
                               : io(error, "cannot open state record");
    file = private_fd(fd, 0) ? fdopen(fd, "rb") : NULL;
    if (file == NULL) {
        close(fd);
        return io(error, "state record must be a private regular file");
    }
    length = fread(data, 1, sizeof(data), file);
    result = feof(file) && !ferror(file) && length > 0 ? KNURL_OK : KNURL_IO;
    fclose(file);
    if (result != KNURL_OK)
        return io(error, "unreadable or oversized state record");
    result = knurl_json_runtime(NULL, &runtime, &owned, error);
    if (result != KNURL_OK)
        return result;
    code =
        lonejson_parse_buffer(runtime, &record_map, record, data, length, NULL);
    lonejson_free(runtime);
    return code == LONEJSON_STATUS_OK ? KNURL_OK
                                      : io(error, "invalid state JSON");
}

knurl_status knurl_auth_json_write(knurl_auth_json *json, const char *name,
                                   const struct auth_record *record,
                                   knurl_error *error)
{
    char data[4096];
    size_t length;
    int owned;
    lonejson *runtime;
    lonejson_status code;
    knurl_status result;
    result = knurl_json_runtime(NULL, &runtime, &owned, error);
    if (result != KNURL_OK)
        return result;
    code = lonejson_serialize_buffer(runtime, &record_map, record, data,
                                     sizeof(data), &length, NULL);
    lonejson_free(runtime);
    if (code != LONEJSON_STATUS_OK)
        return io(error, "cannot serialize state JSON");
    if (!save(json->directory, name, data, strlen(data)))
        return io(error, "cannot replace and sync state record");
    return KNURL_OK;
}

knurl_status knurl_auth_json_delete(knurl_auth_json *json, const char *name,
                                    knurl_error *error)
{
    if (unlinkat(json->directory, name, 0) != 0)
        return errno == ENOENT ? KNURL_OK
                               : io(error, "cannot remove state record");
    if (fsync(json->directory) != 0)
        return io(error, "cannot sync state directory");
    return KNURL_OK;
}

knurl_status knurl_auth_json_prune(knurl_auth_json *json, knurl_error *error)
{
    knurl_status status;
    int lock;
    if (json == NULL)
        return knurl_fail(error, KNURL_INVALID, "auth_prune",
                          "state handle is required");
    lock = knurl_auth_json_lock(json, error);
    if (lock < 0)
        return KNURL_IO;
    status = scan(json, 1, error);
    close(lock);
    if (status == KNURL_OK)
        knurl_error_clear(error);
    return status;
}

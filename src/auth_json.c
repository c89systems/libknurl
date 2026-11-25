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
    LONEJSON_FIELD_STRING_FIXED_REQ(struct auth_record, key, "key",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(struct auth_record, subject, "subject",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(struct auth_record, policy, "policy",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(struct auth_record, email, "email",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(struct auth_record, secret, "secret",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(struct auth_record, seed, "seed",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(struct auth_record, revision, "revision",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(struct auth_record, enrollment,
                                    "enrollment", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64(struct auth_record, expires, "expires"),
    LONEJSON_FIELD_I64(struct auth_record, updated, "updated"),
    LONEJSON_FIELD_I64(struct auth_record, last_step, "last_step"),
    LONEJSON_FIELD_I64(struct auth_record, attempts, "attempts"),
    LONEJSON_FIELD_BOOL(struct auth_record, disabled, "disabled")};
LONEJSON_MAP_DEFINE(record_map, struct auth_record, fields);

static const lonejson_field document_fields[] = {LONEJSON_FIELD_OBJECT_ARRAY(
    struct auth_document, records, "records", struct auth_record, &record_map,
    LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(document_map, struct auth_document, document_fields);

static const char *const filenames[AUTH_COLLECTIONS] = {
    "accounts.json", "sessions.json", "email.json", "totp.json",
    "attempts.json"};

#define AUTH_MAX_RECORDS 8192U
#define AUTH_MAX_FILE_BYTES (8UL * 1024UL * 1024UL)

static knurl_status io(knurl_error *error, const char *message)
{
    return knurl_fail(error, KNURL_IO, "auth_json", message);
}

static int private_fd(int fd, int directory, int snapshot)
{
    struct stat info;
    return fstat(fd, &info) == 0 && info.st_uid == geteuid() &&
           (info.st_mode & 077) == 0 &&
           (directory
                ? S_ISDIR(info.st_mode)
                : S_ISREG(info.st_mode) &&
                      (info.st_nlink == 1 || (snapshot && info.st_nlink == 0)));
}

static int open_directory(const char *path, knurl_error *error)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0 && private_fd(fd, 1, 0))
        return fd;
    if (fd >= 0)
        close(fd);
    io(error, "state directory must exist and be owned and accessible only by "
              "the current user");
    return -1;
}

static int lock_store(knurl_auth_json *json, knurl_error *error)
{
    static const struct timespec interval = {0, 10000000L};
    int count, fd = openat(json->directory, "lock",
                           O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd >= 0 && private_fd(fd, 0, 0)) {
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
    count = private_fd(fd, 0, 0) ? read(fd, data, sizeof(data)) : -1;
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

static knurl_status empty_directory(knurl_auth_json *json, knurl_error *error)
{
    int fd = openat(json->directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *entries = fd >= 0 ? fdopendir(fd) : NULL;
    struct dirent *entry;
    knurl_status status = KNURL_OK;
    size_t length;
    if (entries == NULL) {
        if (fd >= 0)
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
        length = strlen(entry->d_name);
        if (length >= 5 && strcmp(entry->d_name + length - 5, ".json") == 0) {
            status = knurl_fail(error, KNURL_STATE, "auth_init",
                                "auth records already exist; restore the store "
                                "or initialize a new directory");
            break;
        }
    }
    closedir(entries);
    return status;
}

static knurl_status accounts_file(knurl_auth_json *json, knurl_error *error)
{
    knurl_status status;
    int fd = openat(json->directory, filenames[AUTH_ACCOUNTS],
                    O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? KNURL_EOF
                               : io(error, "cannot open accounts.json");
    status = private_fd(fd, 0, 1)
                 ? KNURL_OK
                 : io(error, "accounts.json must be a private regular file");
    close(fd);
    return status;
}

knurl_status knurl_auth_json_init(const char *path, knurl_error *error)
{
    knurl_auth_json json;
    knurl_status status;
    int lock, fd;
    char key[65];
    static const char empty[] = "{\"records\":[]}\n";
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
    lock = lock_store(&json, error);
    status = lock >= 0 ? read_key(&json, error) : KNURL_IO;
    if (status == KNURL_EOF) {
        status = empty_directory(&json, error);
        if (status == KNURL_OK && (!knurl_auth_random_hex(key, 32) ||
                                   !save(json.directory, "key", key, 64)))
            status = io(error, "cannot create the state key");
        OPENSSL_cleanse(key, sizeof(key));
    }
    if (status == KNURL_OK)
        status = accounts_file(&json, error);
    if (status == KNURL_EOF) {
        status = empty_directory(&json, error);
        if (status == KNURL_OK &&
            !save(json.directory, filenames[AUTH_ACCOUNTS], empty,
                  sizeof(empty) - 1))
            status = io(error, "cannot create accounts.json");
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
    if (status == KNURL_OK)
        status = accounts_file(json, error);
    if (status != KNURL_OK) {
        knurl_auth_json_close(json);
        return status == KNURL_EOF
                   ? io(error, "store key or accounts.json missing; initialize "
                               "a new store first")
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

static enum auth_collection collection(const char *key)
{
    switch (key[0]) {
    case 'u':
    case 'c':
        return AUTH_ACCOUNTS;
    case 's':
        return AUTH_SESSIONS;
    case 'e':
    case 'v':
        return AUTH_EMAIL_CODES;
    case 't':
    case 'w':
        return AUTH_TOTP_STEPS;
    default:
        return AUTH_ATTEMPTS;
    }
}

static knurl_status load_document(struct auth_transaction *tx,
                                  enum auth_collection kind, knurl_error *error)
{
    struct auth_document *doc = &tx->documents[kind];
    struct stat info;
    int fd;
    FILE *file;
    lonejson_status code;
    if (doc->loaded)
        return KNURL_OK;
    doc->records.elem_size = sizeof(struct auth_record);
    fd = openat(tx->store->directory, filenames[kind],
                O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        if (errno != ENOENT || kind == AUTH_ACCOUNTS)
            return io(error, "cannot open auth document");
        doc->loaded = 1;
        return KNURL_OK;
    }
    /* The descriptor still refers to a complete snapshot if another
     * process atomically replaced the file after open. */
    file = private_fd(fd, 0, 1) && fstat(fd, &info) == 0 && info.st_size > 0 &&
                   info.st_size <= (off_t)AUTH_MAX_FILE_BYTES
               ? fdopen(fd, "rb")
               : NULL;
    if (file == NULL) {
        close(fd);
        return io(error, "auth document must be a bounded private file");
    }
    code = lonejson_parse_filep(tx->runtime, &document_map, doc, file, NULL);
    fclose(file);
    if (code != LONEJSON_STATUS_OK || doc->records.count > AUTH_MAX_RECORDS)
        return io(error, "invalid or oversized auth document");
    doc->loaded = 1;
    return KNURL_OK;
}

static knurl_status save_document(struct auth_transaction *tx,
                                  enum auth_collection kind, knurl_error *error)
{
    const struct auth_document *doc = &tx->documents[kind];
    char *text;
    size_t length;
    knurl_status status = KNURL_OK;
    text = lonejson_serialize_alloc(tx->runtime, &document_map, doc, &length,
                                    NULL);
    if (text == NULL)
        return io(error, "cannot serialize auth document");
    if (length > AUTH_MAX_FILE_BYTES)
        status = knurl_fail(error, KNURL_LIMIT, "auth_json",
                            "auth document exceeds its byte bound");
    else if (!save(tx->store->directory, filenames[kind], text, length))
        status = io(error, "cannot replace and sync auth document");
    free(text);
    return status;
}

knurl_status knurl_auth_json_begin(knurl_auth_json *json, int locked,
                                   struct auth_transaction *tx,
                                   knurl_error *error)
{
    lonejson_config config = lonejson_default_config();
    lonejson_error native;
    knurl_status status;
    memset(tx, 0, sizeof(*tx));
    tx->store = json;
    tx->lock = locked ? lock_store(json, error) : -1;
    if (locked && tx->lock < 0)
        return KNURL_IO;
    /* A full record array and the parser's growth buffers can exceed the
     * default 1 MiB budget even while the document is within our file bounds.
     */
    config.max_alloc_bytes = 4UL * AUTH_MAX_FILE_BYTES;
    config.write_max_output_bytes = AUTH_MAX_FILE_BYTES;
    tx->runtime = lonejson_new(&config, &native);
    status = tx->runtime != NULL
                 ? KNURL_OK
                 : knurl_json_error(native.code, &native, error);
    if (status != KNURL_OK && tx->lock >= 0) {
        close(tx->lock);
        tx->lock = -1;
    }
    return status;
}

knurl_status knurl_auth_json_end(struct auth_transaction *tx,
                                 knurl_status status, knurl_error *error)
{
    enum auth_collection kind;
    knurl_status saved;
    for (kind = AUTH_ACCOUNTS; kind < AUTH_COLLECTIONS; ++kind) {
        if (tx->documents[kind].dirty &&
            (status == KNURL_OK || status == KNURL_EOF ||
             status == KNURL_LIMIT)) {
            saved = save_document(tx, kind, error);
            if (saved != KNURL_OK)
                status = saved;
        }
        (void)lonejson_cleanup(&document_map, &tx->documents[kind], NULL);
    }
    lonejson_free(tx->runtime);
    if (tx->lock >= 0)
        close(tx->lock);
    return status;
}

static size_t find(const struct auth_document *doc, const char *key)
{
    const struct auth_record *rows = doc->records.items;
    size_t i;
    for (i = 0; i < doc->records.count; ++i)
        if (strcmp(rows[i].key, key) == 0)
            break;
    return i;
}

knurl_status knurl_auth_json_read(struct auth_transaction *tx, const char *key,
                                  struct auth_record *record,
                                  knurl_error *error)
{
    enum auth_collection kind = collection(key);
    const struct auth_document *doc = &tx->documents[kind];
    const struct auth_record *rows;
    size_t index;
    knurl_status status = load_document(tx, kind, error);
    memset(record, 0, sizeof(*record));
    if (status != KNURL_OK)
        return status;
    index = find(doc, key);
    if (index == doc->records.count)
        return KNURL_EOF;
    rows = doc->records.items;
    *record = rows[index];
    return KNURL_OK;
}

static void expire(struct auth_document *doc, lonejson_int64 now)
{
    struct auth_record *rows = doc->records.items;
    size_t from, to = 0;
    for (from = 0; from < doc->records.count; ++from)
        if (rows[from].expires == 0 || rows[from].expires > now)
            rows[to++] = rows[from];
    if (to != doc->records.count) {
        doc->records.count = to;
        doc->dirty = 1;
    }
}

knurl_status knurl_auth_json_write(struct auth_transaction *tx, const char *key,
                                   const struct auth_record *record,
                                   knurl_error *error)
{
    enum auth_collection kind = collection(key);
    struct auth_document *doc = &tx->documents[kind];
    struct auth_record *row;
    size_t index;
    lonejson_error native;
    knurl_status status = load_document(tx, kind, error);
    if (status != KNURL_OK)
        return status;
    index = find(doc, key);
    if (index == doc->records.count) {
        if (index == AUTH_MAX_RECORDS &&
            (kind == AUTH_SESSIONS || kind == AUTH_EMAIL_CODES ||
             kind == AUTH_TOTP_STEPS || kind == AUTH_ATTEMPTS)) {
            expire(doc, (lonejson_int64)time(NULL));
            index = doc->records.count;
        }
        if (index >= AUTH_MAX_RECORDS)
            return knurl_fail(error, KNURL_LIMIT, "auth_json",
                              "auth document is full");
        row = lonejson_record_object_array_append_slot(
            tx->runtime, &document_map, doc, document_fields, &doc->records,
            &native);
        if (row == NULL)
            return knurl_json_error(native.code, &native, error);
    } else {
        row = (struct auth_record *)doc->records.items + index;
    }
    *row = *record;
    memcpy(row->key, key, sizeof(row->key));
    doc->dirty = 1;
    return KNURL_OK;
}

knurl_status knurl_auth_json_delete(struct auth_transaction *tx,
                                    const char *key, knurl_error *error)
{
    enum auth_collection kind = collection(key);
    struct auth_document *doc = &tx->documents[kind];
    struct auth_record *rows;
    size_t index;
    knurl_status status = load_document(tx, kind, error);
    if (status != KNURL_OK)
        return status;
    index = find(doc, key);
    if (index < doc->records.count) {
        rows = doc->records.items;
        --doc->records.count;
        rows[index] = rows[doc->records.count];
        doc->dirty = 1;
    }
    return KNURL_OK;
}

knurl_status knurl_auth_json_prune(knurl_auth_json *json, knurl_error *error)
{
    static const enum auth_collection expiring[] = {
        AUTH_SESSIONS, AUTH_EMAIL_CODES, AUTH_TOTP_STEPS, AUTH_ATTEMPTS};
    struct auth_transaction tx;
    lonejson_int64 now = (lonejson_int64)time(NULL);
    size_t i;
    knurl_status status;
    if (json == NULL || now < 0)
        return knurl_fail(error, KNURL_INVALID, "auth_prune",
                          "state and clock are required");
    status = knurl_auth_json_begin(json, 1, &tx, error);
    for (i = 0;
         status == KNURL_OK && i < sizeof(expiring) / sizeof(expiring[0]);
         ++i) {
        status = load_document(&tx, expiring[i], error);
        if (status == KNURL_OK)
            expire(&tx.documents[expiring[i]], now);
    }
    status = knurl_auth_json_end(&tx, status, error);
    if (status == KNURL_OK)
        knurl_error_clear(error);
    return status;
}

#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "filesystem %d: %s (%s)\n", __LINE__, #t,          \
                    error.message);                                            \
            return 1;                                                          \
        }                                                                      \
    } while (0)
#define VERIFY(f, t)                                                           \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "filesystem step %d line %d: %s\n", (f)->step,     \
                    __LINE__, #t);                                             \
            (f)->failed = 1;                                                   \
        }                                                                      \
    } while (0)
struct fixture {
    rotmos_server *server;
    rotmos_storage_provider *provider, *limited, *file_limited;
    rotmos_storage_resource *writers[2], *list;
    rotmos_asset *snapshot;
    rotmos_storage_operation *pending;
    rotmos_timer *timer;
    char root[4096], content[4096], cache[4096], original_tag[67], tag[67];
    int step, failed, done, managed, listed, current, lock, permissions, probes,
        cancelled_probes;
};
struct step {
    rotmos_storage_action action;
    const char *path, *destination, *bytes, *match, *none;
    rotmos_storage_code expected;
    int resource, depth, overwrite;
};
static const struct step steps[] = {
    {ROTMOS_STORAGE_STAT, "/missing", NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_NOT_FOUND, 0, 0, 0},
    {ROTMOS_STORAGE_MKCOL, "/d", NULL, NULL, NULL, NULL, ROTMOS_STORAGE_OK, 0,
     0, 0},
    {ROTMOS_STORAGE_PUT_BEGIN, "/d/a", NULL, NULL, NULL, "*", ROTMOS_STORAGE_OK,
     0, 0, 0},
    {ROTMOS_STORAGE_PUT_WRITE, NULL, NULL, "abcdef", NULL, NULL,
     ROTMOS_STORAGE_OK, 0, 0, 0},
    {ROTMOS_STORAGE_STAT, "/d/a", NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_NOT_FOUND, 0, 0, 0},
    {ROTMOS_STORAGE_PUT_FINISH, NULL, NULL, NULL, NULL, NULL, ROTMOS_STORAGE_OK,
     0, 0, 0},
    {ROTMOS_STORAGE_OPEN_READ, "/d/a", NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_OK, 0, 0, 0},
    {ROTMOS_STORAGE_PUT_BEGIN, "/d/a", NULL, NULL, "@", NULL, ROTMOS_STORAGE_OK,
     0, 0, 0},
    {ROTMOS_STORAGE_PUT_WRITE, NULL, NULL, "XYZ", NULL, NULL, ROTMOS_STORAGE_OK,
     0, 0, 0},
    {ROTMOS_STORAGE_PUT_FINISH, NULL, NULL, NULL, NULL, NULL, ROTMOS_STORAGE_OK,
     0, 0, 0},
    {ROTMOS_STORAGE_STAT, "/d/a", NULL, NULL, NULL, NULL, ROTMOS_STORAGE_OK, 0,
     0, 0},
    {ROTMOS_STORAGE_PUT_BEGIN, "/d/a", NULL, NULL, "old", NULL,
     ROTMOS_STORAGE_PRECONDITION, 0, 0, 0},
    {ROTMOS_STORAGE_PUT_BEGIN, "/d/a", NULL, NULL, "@", NULL, ROTMOS_STORAGE_OK,
     0, 0, 0},
    {ROTMOS_STORAGE_PUT_WRITE, NULL, NULL, "stale", NULL, NULL,
     ROTMOS_STORAGE_OK, 0, 0, 0},
    {ROTMOS_STORAGE_PUT_BEGIN, "/d/a", NULL, NULL, "@", NULL, ROTMOS_STORAGE_OK,
     1, 0, 0},
    {ROTMOS_STORAGE_PUT_WRITE, NULL, NULL, "won", NULL, NULL, ROTMOS_STORAGE_OK,
     1, 0, 0},
    {ROTMOS_STORAGE_PUT_FINISH, NULL, NULL, NULL, NULL, NULL, ROTMOS_STORAGE_OK,
     1, 0, 0},
    {ROTMOS_STORAGE_PUT_FINISH, NULL, NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_PRECONDITION, 0, 0, 0},
    {ROTMOS_STORAGE_COPY, "/d/a", "/d/b", NULL, "*", NULL, ROTMOS_STORAGE_OK, 0,
     -1, 0},
    {ROTMOS_STORAGE_COPY, "/d/a", "/d/b", NULL, NULL, NULL,
     ROTMOS_STORAGE_EXISTS, 0, -1, 0},
    {ROTMOS_STORAGE_MOVE, "/d/b", "/d/c", NULL, NULL, NULL, ROTMOS_STORAGE_OK,
     0, -1, 1},
    {ROTMOS_STORAGE_LIST_OPEN, "/d", NULL, NULL, NULL, NULL, ROTMOS_STORAGE_OK,
     0, 0, 0},
    {ROTMOS_STORAGE_LIST_NEXT, NULL, NULL, NULL, NULL, NULL, ROTMOS_STORAGE_OK,
     0, 0, 0},
    {ROTMOS_STORAGE_LIST_NEXT, NULL, NULL, NULL, NULL, NULL, ROTMOS_STORAGE_OK,
     0, 0, 0},
    {ROTMOS_STORAGE_LIST_NEXT, NULL, NULL, NULL, NULL, NULL, ROTMOS_STORAGE_END,
     0, 0, 0},
    {ROTMOS_STORAGE_COPY, "/d", "/empty", NULL, NULL, NULL, ROTMOS_STORAGE_OK,
     0, 0, 0},
    {ROTMOS_STORAGE_LIST_OPEN, "/empty", NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_OK, 0, 0, 0},
    {ROTMOS_STORAGE_LIST_NEXT, NULL, NULL, NULL, NULL, NULL, ROTMOS_STORAGE_END,
     0, 0, 0},
    {ROTMOS_STORAGE_COPY, "/d", "/deep", NULL, NULL, NULL, ROTMOS_STORAGE_OK, 0,
     -1, 0},
    {ROTMOS_STORAGE_OPEN_READ, "/deep/c", NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_OK, 0, 0, 0},
    {ROTMOS_STORAGE_DELETE, "/deep", NULL, NULL, "*", NULL, ROTMOS_STORAGE_OK,
     0, 0, 0},
    {ROTMOS_STORAGE_STAT, "/deep/c", NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_NOT_FOUND, 0, 0, 0},
    {ROTMOS_STORAGE_MKCOL, "/deep", NULL, NULL, NULL, NULL, ROTMOS_STORAGE_OK,
     0, 0, 0},
    {ROTMOS_STORAGE_PUT_BEGIN, "/d/abort", NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_OK, 0, 0, 0},
    {ROTMOS_STORAGE_PUT_WRITE, NULL, NULL, "partial", NULL, NULL,
     ROTMOS_STORAGE_OK, 0, 0, 0},
    {ROTMOS_STORAGE_PUT_ABORT, NULL, NULL, NULL, NULL, NULL, ROTMOS_STORAGE_OK,
     0, 0, 0},
    {ROTMOS_STORAGE_STAT, "/d/abort", NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_NOT_FOUND, 0, 0, 0},
    {ROTMOS_STORAGE_PUT_BEGIN, "/d/large", NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_OK, 0, 0, 0},
    {ROTMOS_STORAGE_PUT_WRITE, NULL, NULL, "123456789", NULL, NULL,
     ROTMOS_STORAGE_LIMIT, 0, 0, 0},
    {ROTMOS_STORAGE_PUT_FINISH, NULL, NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_CONFLICT, 0, 0, 0},
    {ROTMOS_STORAGE_PUT_BEGIN, "/d/quota", NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_OK, 0, 0, 0},
    {ROTMOS_STORAGE_PUT_WRITE, NULL, NULL, "12345678", NULL, NULL,
     ROTMOS_STORAGE_OK, 0, 0, 0},
    {ROTMOS_STORAGE_PUT_FINISH, NULL, NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_LIMIT, 0, 0, 0},
    {ROTMOS_STORAGE_MKCOL, "/d", NULL, NULL, NULL, NULL, ROTMOS_STORAGE_EXISTS,
     0, 0, 0},
    {ROTMOS_STORAGE_DELETE, "/d/a", NULL, NULL, "W/\"wrong\"", NULL,
     ROTMOS_STORAGE_PRECONDITION, 0, 0, 0},
    {ROTMOS_STORAGE_DELETE, "/d/a", NULL, NULL, "\"ok\", malformed", NULL,
     ROTMOS_STORAGE_INVALID, 0, 0, 0},
    {ROTMOS_STORAGE_COPY, "/d", "/d/child", NULL, NULL, NULL,
     ROTMOS_STORAGE_INVALID, 0, -1, 0},
    {ROTMOS_STORAGE_PUT_BEGIN, "/link/escaped", NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_CONFLICT, 0, 0, 0},
    {ROTMOS_STORAGE_OPEN_READ, "/alias", NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_NOT_FOUND, 0, 0, 0},
    {ROTMOS_STORAGE_PUT_BEGIN, "/orphan/file", NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_CONFLICT, 0, 0, 0},
    {ROTMOS_STORAGE_MKCOL, "/last", NULL, NULL, NULL, NULL, ROTMOS_STORAGE_OK,
     0, 0, 0},
    {ROTMOS_STORAGE_MOVE, "/d", "/empty", NULL, NULL, NULL, ROTMOS_STORAGE_OK,
     0, -1, 1},
    {ROTMOS_STORAGE_COPY, "/empty", "/deep", NULL, NULL, NULL,
     ROTMOS_STORAGE_OK, 0, -1, 1},
    {ROTMOS_STORAGE_DELETE, "/overlay/never", NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_NOT_FOUND, 0, 0, 0},
    {ROTMOS_STORAGE_STAT, "/overlay/never", NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_NOT_FOUND, 0, 0, 0},
    {ROTMOS_STORAGE_PUT_BEGIN, "/overlay/never", NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_CONFLICT, 0, 0, 0},
    {ROTMOS_STORAGE_COPY, "/empty/a", "/copy-too-big", NULL, NULL, NULL,
     ROTMOS_STORAGE_LIMIT, 0, -1, 0},
    {ROTMOS_STORAGE_MKCOL, "/over-quota", NULL, NULL, NULL, NULL,
     ROTMOS_STORAGE_LIMIT, 0, 0, 0}};
enum { STEP_TIMEOUT = sizeof(steps) / sizeof(steps[0]), PROBES = 40 };
static void advance(struct fixture *);
static void interrupted(rotmos_storage_operation *op, rotmos_status cause,
                        void *data)
{
    struct fixture *f = data;
    (void)op;
    (void)cause;
    VERIFY(f, f->step == STEP_TIMEOUT);
}
static void completed(rotmos_storage_operation *op,
                      const rotmos_storage_outcome *outcome, void *data)
{
    struct fixture *f = data;
    const struct step *s = &steps[f->step];
    const rotmos_storage_result *r = outcome->result;
    rotmos_error error;
    rotmos_storage_code expected = s->expected;
    if (f->step == 31 && f->managed)
        expected = ROTMOS_STORAGE_TOMBSTONED;
    if (f->step == 53 && f->managed)
        expected = ROTMOS_STORAGE_OK;
    if (f->step == 54 && f->managed)
        expected = ROTMOS_STORAGE_TOMBSTONED;
    if ((f->step == 51 || f->step == 52) && !f->managed && f->permissions)
        expected = ROTMOS_STORAGE_IO;
    if (outcome->cause != ROTMOS_OK || r->code != expected) {
        fprintf(stderr, "step %d cause %s expected %s received %s: %s\n",
                f->step, rotmos_status_string(outcome->cause),
                rotmos_storage_code_string(expected),
                rotmos_storage_code_string(r->code), r->error.message);
        f->failed = 1;
    }
    if ((f->step == 51 || f->step == 52) && !f->managed && f->permissions) {
        char path[4096];
        struct stat st;
        VERIFY(f, r->committed && r->partial && r->recovery_path != NULL);
        if (r->recovery_path != NULL) {
            snprintf(path, sizeof(path), "%s%s/backup", f->content,
                     r->recovery_path);
            VERIFY(f, lstat(path, &st) == 0 && S_ISDIR(st.st_mode));
        }
        snprintf(path, sizeof(path), "%s/d", f->content);
        VERIFY(f, chmod(path, 0700) == 0);
    }
    if (r->code != ROTMOS_STORAGE_OK)
        return;
    if (s->action == ROTMOS_STORAGE_PUT_BEGIN) {
        VERIFY(f, rotmos_storage_operation_take_resource(
                      op, &f->writers[s->resource], &error) == ROTMOS_OK);
    } else if (s->action == ROTMOS_STORAGE_LIST_OPEN) {
        VERIFY(f, rotmos_storage_operation_take_resource(op, &f->list,
                                                         &error) == ROTMOS_OK);
    } else if (s->action == ROTMOS_STORAGE_PUT_WRITE)
        VERIFY(f, r->accepted == strlen(s->bytes));
    else if (s->action == ROTMOS_STORAGE_PUT_FINISH) {
        VERIFY(f, r->committed && r->has_info && r->info.etag != NULL);
        if (f->step == 5) {
            VERIFY(f,
                   strcmp(r->info.etag, "\"bef57ec7f53a6d40beb640a780a639c83bc2"
                                        "9ac8a9816f1fc6c5c6dcd93c4721\"") == 0);
            snprintf(f->original_tag, sizeof(f->original_tag), "%s",
                     r->info.etag);
        }
        VERIFY(f, r->created == (f->step == 5));
        snprintf(f->tag, sizeof(f->tag), "%s", r->info.etag);
    } else if (s->action == ROTMOS_STORAGE_OPEN_READ) {
        rotmos_asset *asset;
        rotmos_u64 zero;
        char bytes[16];
        size_t n;
        zero.high = zero.low = 0;
        VERIFY(f, rotmos_storage_operation_take_asset(op, &asset, &error) ==
                      ROTMOS_OK);
        if (f->step == 6)
            f->snapshot = asset;
        else {
            VERIFY(f, rotmos_asset_read(asset, zero, bytes, sizeof(bytes), &n,
                                        &error) == ROTMOS_OK &&
                          n == 3 && memcmp(bytes, "won", 3) == 0);
            VERIFY(f, rotmos_asset_close(asset, &error) == ROTMOS_OK);
        }
    } else if (s->action == ROTMOS_STORAGE_LIST_NEXT) {
        VERIFY(f, r->has_entry && r->entry.info.size.low == 3);
        if (strcmp(r->entry.name, "a") == 0)
            f->listed |= 1;
        else if (strcmp(r->entry.name, "c") == 0)
            f->listed |= 2;
        else
            VERIFY(f, 0);
    } else if (s->action >= ROTMOS_STORAGE_DELETE)
        VERIFY(f, r->committed);
    if (f->step == 10) {
        rotmos_u64 zero;
        size_t n;
        char bytes[8];
        zero.high = zero.low = 0;
        VERIFY(f, rotmos_asset_read(f->snapshot, zero, bytes, sizeof(bytes), &n,
                                    &error) == ROTMOS_OK &&
                      n == 6 && memcmp(bytes, "abcdef", 6) == 0);
        VERIFY(f, rotmos_asset_close(f->snapshot, &error) == ROTMOS_OK);
        f->snapshot = NULL;
    }
}
static void retired(const rotmos_storage_outcome *outcome, void *data)
{
    struct fixture *f = data;
    rotmos_error error;
    const struct step *s;
    f->pending = NULL;
    if (f->step == STEP_TIMEOUT) {
        rotmos_timer_cancel(f->timer);
        f->timer = NULL;
        VERIFY(f, outcome->cause == ROTMOS_TIMEOUT && outcome->cancelled);
        VERIFY(f, flock(f->lock, LOCK_UN) == 0);
        close(f->lock);
        f->lock = -1;
        VERIFY(f, rotmos_storage_provider_close(f->file_limited, &error) ==
                      ROTMOS_OK);
        f->file_limited = NULL;
        VERIFY(f,
               rotmos_storage_provider_close(f->limited, &error) == ROTMOS_OK);
        f->limited = NULL;
        VERIFY(f,
               rotmos_storage_provider_close(f->provider, &error) == ROTMOS_OK);
        f->provider = NULL;
        f->done = 1;
        VERIFY(f, rotmos_server_request_stop(f->server, &error) == ROTMOS_OK);
        return;
    }
    s = &steps[f->step];
    if (s->action == ROTMOS_STORAGE_PUT_FINISH ||
        s->action == ROTMOS_STORAGE_PUT_ABORT) {
        VERIFY(f, rotmos_storage_resource_close(f->writers[s->resource],
                                                &error) == ROTMOS_OK);
        f->writers[s->resource] = NULL;
    }
    if ((f->step == 24 || f->step == 27) && f->list != NULL) {
        VERIFY(f, rotmos_storage_resource_close(f->list, &error) == ROTMOS_OK);
        f->list = NULL;
    }
    if (f->step == 24)
        VERIFY(f, f->listed == 3);
    ++f->step;
    advance(f);
}
static void unlock_probe(rotmos_timer *timer, void *data)
{
    struct fixture *f = data;
    (void)timer;
    /* The operation is waiting for our lock on a native thread. This owner
     * callback must run before the operation can acknowledge timeout. */
    VERIFY(f, f->pending != NULL && f->lock >= 0);
    ++f->current;
}
static void advance(struct fixture *f)
{
    rotmos_storage_arguments arguments;
    rotmos_storage_operation_config config;
    rotmos_error error;
    rotmos_status status;
    const struct step *s;
    char bytes[16], path[4096];
    if (f->failed) {
        (void)rotmos_server_request_stop(f->server, NULL);
        return;
    }
    if (f->step == 21) {
        int i, fd;
        for (i = 0; i < 66; ++i) {
            snprintf(path, sizeof(path), "%s/d/.vectis-tmp-fixture-%d",
                     f->content, i);
            fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            VERIFY(f, fd >= 0);
            if (fd >= 0)
                close(fd);
        }
    }
    if (f->step == 51 && !f->managed && f->permissions) {
        snprintf(path, sizeof(path), "%s/d", f->content);
        VERIFY(f, chmod(path, 0555) == 0);
    }
    if (f->step == 47 || f->step == 48) {
        char target[4096];
        snprintf(target, sizeof(target), "%s/%s", f->root,
                 f->step == 47 ? "outside" : "secret");
        snprintf(path, sizeof(path), "%s/%s", f->content,
                 f->step == 47 ? "link" : "alias");
        VERIFY(f, symlink(target, path) == 0);
    }
    if (f->step == 49) {
        snprintf(path, sizeof(path), "%s/link", f->content);
        VERIFY(f, unlink(path) == 0);
        snprintf(path, sizeof(path), "%s/alias", f->content);
        VERIFY(f, unlink(path) == 0);
    }
    rotmos_storage_arguments_init(&arguments);
    rotmos_storage_operation_config_init(&config);
    config.completed = completed;
    config.interrupted = interrupted;
    config.retired = retired;
    config.userdata = f;
    if (f->step == STEP_TIMEOUT) {
        snprintf(path, sizeof(path), "%s/webdav/test/.lock", f->cache);
        f->lock = open(path, O_RDWR | O_CLOEXEC);
        VERIFY(f, f->lock >= 0 && flock(f->lock, LOCK_EX | LOCK_NB) == 0);
        config.timeout_ms = 30;
        arguments.action = ROTMOS_STORAGE_MKCOL;
        arguments.path = "/blocked";
        VERIFY(f, rotmos_loop_timer(rotmos_server_loop(f->server), 1, 1,
                                    unlock_probe, NULL, f, &f->timer,
                                    &error) == ROTMOS_OK);
    } else {
        s = &steps[f->step];
        arguments.action = s->action;
        arguments.path = s->path;
        arguments.destination = s->destination;
        if (f->step == 52 && !f->managed && f->permissions) {
            int fd;
            arguments.path = "/d";
            arguments.destination = "/empty";
            snprintf(path, sizeof(path), "%s/empty/protected", f->content);
            VERIFY(f, mkdir(path, 0700) == 0);
            snprintf(path, sizeof(path), "%s/empty/protected/leaf", f->content);
            fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            VERIFY(f, fd >= 0);
            if (fd >= 0)
                close(fd);
            snprintf(path, sizeof(path), "%s/empty/protected", f->content);
            VERIFY(f, chmod(path, 0555) == 0);
        }
        arguments.depth = s->depth;
        arguments.overwrite = s->overwrite;
        arguments.if_match = s->match;
        if (s->match != NULL && strcmp(s->match, "@") == 0)
            arguments.if_match = f->tag;
        if (s->match != NULL && strcmp(s->match, "old") == 0)
            arguments.if_match = f->original_tag;
        arguments.if_none_match = s->none;
        if (s->bytes != NULL) {
            snprintf(bytes, sizeof(bytes), "%s", s->bytes);
            arguments.bytes.data = bytes;
            arguments.bytes.length = strlen(bytes);
        }
        if (s->action == ROTMOS_STORAGE_LIST_NEXT)
            arguments.resource = f->list;
        else if (s->action == ROTMOS_STORAGE_PUT_WRITE ||
                 s->action == ROTMOS_STORAGE_PUT_FINISH ||
                 s->action == ROTMOS_STORAGE_PUT_ABORT)
            arguments.resource = f->writers[s->resource];
    }
    status = rotmos_storage_operation_start(
        f->step == STEP_TIMEOUT - 1
            ? f->limited
            : (f->step == STEP_TIMEOUT - 2 ? f->file_limited : f->provider),
        rotmos_server_loop(f->server), &arguments, &config, &f->pending,
        &error);
    memset(bytes, 'x', sizeof(bytes));
    VERIFY(f, status == ROTMOS_OK);
    if (status != ROTMOS_OK) {
        fprintf(stderr, "start: %s\n", error.message);
        (void)rotmos_server_request_stop(f->server, NULL);
    }
}
static void probe_retired(const rotmos_storage_outcome *outcome, void *data)
{
    struct fixture *f = data;
    if (outcome->cause != ROTMOS_OK ||
        outcome->result->code != ROTMOS_STORAGE_NOT_FOUND)
        fprintf(stderr, "probe: %s / %s / %s\n",
                rotmos_status_string(outcome->cause),
                rotmos_storage_code_string(outcome->result->code),
                outcome->result->error.message);
    VERIFY(f, outcome->cause == ROTMOS_OK &&
                  outcome->result->code == ROTMOS_STORAGE_NOT_FOUND);
    ++f->probes;
    if (f->probes == PROBES && f->cancelled_probes == 3)
        advance(f);
}
static void admission_cancelled(const rotmos_storage_outcome *outcome,
                                void *data)
{
    struct fixture *f = data;
    VERIFY(f, outcome->cause == ROTMOS_CANCELLED && outcome->cancelled);
    ++f->cancelled_probes;
    if (f->probes == PROBES && f->cancelled_probes == 3)
        advance(f);
}
static rotmos_status worker_start(rotmos_server *server,
                                  const rotmos_process_context *process,
                                  void *data, rotmos_error *error)
{
    struct fixture *f = data;
    rotmos_storage_filesystem_config config;
    rotmos_storage_operation_config operation;
    rotmos_storage_arguments arguments;
    rotmos_storage_provider *provider;
    rotmos_storage_operation *handle;
    int i;
    (void)process;
    rotmos_storage_filesystem_config_init(&config);
    config.cache_dir = f->cache;
    config.site_id = "test";
    config.root_dir = f->managed ? NULL : f->content;
    rotmos_storage_operation_config_init(&operation);
    operation.retired = probe_retired;
    operation.userdata = f;
    rotmos_storage_arguments_init(&arguments);
    arguments.path = "/never";
    for (i = 0; i < PROBES; ++i) {
        if (rotmos_storage_filesystem_new(server, &config, &provider, error) !=
            ROTMOS_OK)
            return ROTMOS_INVALID;
        if (rotmos_storage_operation_start(provider, rotmos_server_loop(server),
                                           &arguments, &operation, &handle,
                                           error) != ROTMOS_OK)
            return ROTMOS_INVALID;
        if (rotmos_storage_provider_close(provider, error) != ROTMOS_OK)
            return ROTMOS_INVALID;
    }
    operation.retired = admission_cancelled;
    for (i = 0; i < 3; ++i) {
        if (rotmos_storage_filesystem_new(server, &config, &provider, error) !=
            ROTMOS_OK)
            return ROTMOS_INVALID;
        if (rotmos_storage_operation_start(provider, rotmos_server_loop(server),
                                           &arguments, &operation, &handle,
                                           error) != ROTMOS_OK)
            return ROTMOS_INVALID;
        if (rotmos_storage_operation_cancel(handle, error) != ROTMOS_OK)
            return ROTMOS_INVALID;
        if (rotmos_storage_provider_close(provider, error) != ROTMOS_OK)
            return ROTMOS_INVALID;
    }
    VERIFY(f, f->cancelled_probes == 0);
    return ROTMOS_OK;
}
static int worker_exit(rotmos_server *server,
                       const rotmos_process_context *process, void *data)
{
    struct fixture *f = data;
    (void)server;
    (void)process;
    return f->failed || !f->done || f->current == 0 || f->probes != PROBES ||
           f->cancelled_probes != 3;
}
static int remove_fixture(const char *path)
{
    DIR *directory;
    struct dirent *entry;
    struct stat st;
    char child[4096];
    int ok = 1;
    if (chmod(path, 0700) != 0)
        return 0;
    directory = opendir(path);
    if (directory == NULL)
        return 0;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >=
            (int)sizeof(child)) {
            ok = 0;
            break;
        }
        if (lstat(child, &st) != 0) {
            ok = 0;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!remove_fixture(child))
                ok = 0;
        } else if (unlink(child) != 0)
            ok = 0;
    }
    closedir(directory);
    return rmdir(path) == 0 && ok;
}
int main(int argc, char **argv)
{
    struct fixture f;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_storage_filesystem_config storage;
    rotmos_server *server;
    rotmos_run_result result;
    rotmos_error error;
    rotmos_status status;
    char cwd[2048], relative[] = "storage-fs-XXXXXX", path[4096], outside[4096];
    int fd;
    (void)argv;
    memset(&f, 0, sizeof(f));
    f.lock = -1;
    f.managed = argc > 1;
    f.permissions = geteuid() != 0;
    CHECK(getcwd(cwd, sizeof(cwd)) != NULL && mkdtemp(relative) != NULL);
    snprintf(f.root, sizeof(f.root), "%s/%s", cwd, relative);
    snprintf(f.cache, sizeof(f.cache), "%s/cache", f.root);
    snprintf(f.content, sizeof(f.content), "%s/%s", f.root,
             f.managed ? "cache/webdav/test/content" : "content");
    CHECK(mkdir(f.cache, 0700) == 0);
    if (f.managed) {
        snprintf(path, sizeof(path), "%s/webdav", f.cache);
        CHECK(mkdir(path, 0700) == 0);
        snprintf(path, sizeof(path), "%s/webdav/test", f.cache);
        CHECK(mkdir(path, 0700) == 0);
    }
    CHECK(mkdir(f.content, 0700) == 0);
    snprintf(outside, sizeof(outside), "%s/outside", f.root);
    CHECK(mkdir(outside, 0700) == 0);
    snprintf(outside, sizeof(outside), "%s/secret", f.root);
    fd = open(outside, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(fd >= 0 && write(fd, "secret", 6) == 6 && close(fd) == 0);
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.workers.shutdown_timeout_ms = 1000;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.callbacks.on_worker_start = worker_start;
    config.callbacks.on_worker_exit = worker_exit;
    config.callbacks.userdata = &f;
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    f.server = server;
    rotmos_storage_filesystem_config_init(&storage);
    storage.cache_dir = f.cache;
    storage.site_id = "test";
    storage.root_dir = f.managed ? NULL : f.content;
    storage.max_file_bytes = 8;
    storage.max_total_bytes = 12;
    storage.max_resources = 16;
    CHECK(rotmos_storage_filesystem_content_directory(
              &storage, path, sizeof(path), &error) == ROTMOS_OK &&
          strcmp(path, f.content) == 0);
    CHECK(rotmos_storage_filesystem_content_directory(
              &storage, path, 2, &error) == ROTMOS_OVERFLOW &&
          path[0] == 0);
    snprintf(path, sizeof(path), "%s/../cache/.", f.cache);
    storage.cache_dir = path;
    CHECK(rotmos_storage_filesystem_new(server, &storage, &f.provider,
                                        &error) == ROTMOS_OK);
    storage.cache_dir = f.cache;
    storage.max_resources = 1;
    CHECK(rotmos_storage_filesystem_new(server, &storage, &f.limited, &error) ==
          ROTMOS_OK);
    storage.max_resources = 64;
    storage.max_file_bytes = 2;
    storage.max_total_bytes = 64;
    CHECK(rotmos_storage_filesystem_new(server, &storage, &f.file_limited,
                                        &error) == ROTMOS_OK);
    CHECK(rotmos_storage_provider_capabilities(f.provider) ==
          (ROTMOS_STORAGE_CAN_WRITE | ROTMOS_STORAGE_CAN_CONDITIONAL |
           ROTMOS_STORAGE_CAN_COPY | ROTMOS_STORAGE_CAN_MOVE |
           ROTMOS_STORAGE_CAN_SCOPE_CHECK));
    snprintf(path, sizeof(path), "%s/webdav/test/.lock", f.cache);
    CHECK(access(path, F_OK) != 0);
    status = rotmos_server_run(server, &result, &error);
    if (status != ROTMOS_OK)
        fprintf(stderr, "run %s: %s\n", error.operation, error.message);
    CHECK(status == ROTMOS_OK && !result.escalated && !result.has_failure);
    CHECK(rotmos_storage_provider_close(f.file_limited, &error) == ROTMOS_OK);
    CHECK(rotmos_storage_provider_close(f.limited, &error) == ROTMOS_OK);
    CHECK(rotmos_storage_provider_close(f.provider, &error) == ROTMOS_OK);
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    CHECK(remove_fixture(f.root));
    return 0;
}

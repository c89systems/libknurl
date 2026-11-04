#define _POSIX_C_SOURCE 200809L
#include <rotmos/rotmos.h>
#include "executor.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "storage %d: %s (%s)\n", __LINE__, #t,             \
                    error.message);                                            \
            return 1;                                                          \
        }                                                                      \
    } while (0)
#define VERIFY(f, t)                                                           \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "storage callback %d: %s\n", __LINE__, #t);        \
            __atomic_store_n(&(f)->failed, 1, __ATOMIC_RELEASE);               \
        }                                                                      \
    } while (0)
enum {
    SYNC = 1,
    BAD,
    LATE,
    TIMED,
    CANCEL_ERROR,
    BEGIN,
    WRITE_FIRST,
    WRITE_LAST,
    FINISH,
    CLOSED_BEGIN,
    CLOSED_WRITE,
    RO_STAT,
    RO_READ,
    RO_LIST,
    RO_ITEM,
    RO_END,
    PARTIAL
};
struct fixture {
    rotmos_server *server;
    rotmos_executor *executor, *small;
    rotmos_storage_provider *provider, *readonly;
    rotmos_asset_provider *assets;
    rotmos_storage_operation *late;
    rotmos_storage_resource *writer;
    rotmos_source *read_source;
    rotmos_timer *tick;
    pthread_t owner;
    pid_t child;
    unsigned int released, late_started, failed;
    int outstanding, invoked, provider_retired, consumer_retired, completed,
        interruptions;
    int probe_completed, probe_retired, small_retired, pools_closed, closing;
    int late_retired, late_committed, timeout_retired, cancel_error_retired;
    int writer_closed, provider_closed, asset_opened, asset_closed, listed,
        finished;
    int abandon, escaped;
};
struct client_context {
    struct fixture *f;
    int kind;
};
struct native_context {
    struct fixture *f;
    rotmos_storage_operation *operation;
    rotmos_job *job;
};
struct writer {
    struct fixture *f;
    char bytes[32];
    size_t size;
};
struct directory {
    struct fixture *f;
    int index;
};
static int start(struct fixture *, int, rotmos_storage_arguments *);
static rotmos_status native_work(rotmos_job *job, rotmos_bytes args, void *data,
                                 rotmos_error *error)
{
    struct native_context *c = data;
    struct fixture *f = c->f;
    const rotmos_storage_arguments *a =
        rotmos_storage_operation_arguments(c->operation);
    rotmos_storage_result invalid;
    struct writer *writer;
    size_t n;
    VERIFY(f, !pthread_equal(f->owner, pthread_self()));
    rotmos_storage_result_init(&invalid);
    VERIFY(f, rotmos_storage_operation_complete(c->operation, &invalid,
                                                error) == ROTMOS_STATE);
    if (a->action == ROTMOS_STORAGE_DELETE && strcmp(a->path, "/late") == 0)
        __atomic_store_n(&f->late_started, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&f->released, __ATOMIC_ACQUIRE)) {
        if (rotmos_job_cancelled(job) && !(a->action == ROTMOS_STORAGE_DELETE &&
                                           strcmp(a->path, "/late") == 0))
            return ROTMOS_CANCELLED;
        (void)poll(NULL, 0, 1);
    }
    VERIFY(f, args.length == 6 && memcmp(args.data, "copied", 6) == 0);
    if (a->action == ROTMOS_STORAGE_PUT_WRITE) {
        writer = rotmos_storage_resource_userdata(a->resource);
        n = a->bytes.length > 3 ? 3 : a->bytes.length;
        VERIFY(f, writer->size + n <= sizeof(writer->bytes));
        memcpy(writer->bytes + writer->size, a->bytes.data, n);
        writer->size += n;
    }
    return rotmos_job_result_copy(job, args, error);
}
static void native_retired(const rotmos_job_outcome *outcome, void *data)
{
    struct native_context *c = data;
    rotmos_storage_result result;
    const rotmos_storage_arguments *a =
        rotmos_storage_operation_arguments(c->operation);
    rotmos_error error;
    rotmos_status status;
    c->job = NULL;
    rotmos_storage_result_init(&result);
    VERIFY(c->f, pthread_equal(c->f->owner, pthread_self()));
    if (outcome->work_status == ROTMOS_OK) {
        if (a->action == ROTMOS_STORAGE_PUT_WRITE)
            result.accepted = a->bytes.length > 3 ? 3 : a->bytes.length;
        else if (a->action == ROTMOS_STORAGE_DELETE)
            result.committed = 1;
        else {
            result.has_info = 1;
            result.info.size.low = 7;
        }
    } else if (!rotmos_storage_operation_cancelled(c->operation)) {
        result.code = ROTMOS_STORAGE_IO;
        result.error = outcome->error;
    }
    status = rotmos_storage_operation_complete(c->operation, &result, &error);
    VERIFY(c->f, status == ROTMOS_OK);
}
static void close_writer(void *data)
{
    struct writer *w = data;
    w->f->writer_closed++;
    free(w);
}
static void close_provider(void *data)
{
    struct fixture *f = data;
    f->provider_closed++;
}
static rotmos_status invoke(rotmos_storage_operation *op,
                            const rotmos_storage_arguments *a, void *data,
                            rotmos_error *error)
{
    struct fixture *f = data;
    rotmos_storage_result r;
    rotmos_storage_resource_config rc;
    rotmos_storage_resource *resource;
    struct writer *writer;
    struct native_context *native;
    rotmos_job_config job;
    rotmos_status status;
    char marker[7] = "copied";
    f->invoked++;
    rotmos_storage_result_init(&r);
    if (a->action == ROTMOS_STORAGE_STAT && strcmp(a->path, "/sync") == 0) {
        r.has_info = 1;
        r.info.etag = "\"sync\"";
        r.info.size.low = 4;
        status = rotmos_storage_operation_complete(op, &r, error);
        VERIFY(f, status == ROTMOS_OK);
        VERIFY(f, rotmos_storage_operation_complete(op, &r, error) ==
                      ROTMOS_STATE);
        return status;
    }
    if (a->action == ROTMOS_STORAGE_STAT && strcmp(a->path, "/bad") == 0)
        return rotmos_storage_operation_complete(op, &r, error);
    if (a->action == ROTMOS_STORAGE_COPY) {
        char recovery[64] = "/.vectis-txn/recovery";
        r.code = ROTMOS_STORAGE_IO;
        r.committed = r.partial = 1;
        r.recovery_path = recovery;
        status = rotmos_storage_operation_complete(op, &r, error);
        memset(recovery, 'x', sizeof(recovery));
        return status;
    }
    if (a->action == ROTMOS_STORAGE_PUT_BEGIN) {
        writer = calloc(1, sizeof(*writer));
        if (writer == NULL)
            return ROTMOS_NOMEM;
        writer->f = f;
        rotmos_storage_resource_config_init(&rc);
        rc.kind = ROTMOS_STORAGE_WRITE;
        rc.close = close_writer;
        rc.userdata = writer;
        status = rotmos_storage_resource_new(op, &rc, &resource, error);
        if (status != ROTMOS_OK) {
            free(writer);
            return status;
        }
        r.resource = resource;
        status = rotmos_storage_operation_complete(op, &r, error);
        if (status != ROTMOS_OK)
            rotmos_storage_resource_close(resource, NULL);
        return status;
    }
    if (a->action == ROTMOS_STORAGE_PUT_FINISH) {
        writer = rotmos_storage_resource_userdata(a->resource);
        VERIFY(f, writer->size == 6 && memcmp(writer->bytes, "abcdef", 6) == 0);
        r.committed = 1;
        return rotmos_storage_operation_complete(op, &r, error);
    }
    native = calloc(1, sizeof(*native));
    if (native == NULL)
        return ROTMOS_NOMEM;
    native->f = f;
    native->operation = op;
    status = rotmos_storage_operation_set_userdata(op, native, error);
    if (status != ROTMOS_OK) {
        free(native);
        return status;
    }
    VERIFY(f, rotmos_storage_operation_set_userdata(op, native, error) ==
                  ROTMOS_STATE);
    rotmos_job_config_init(&job);
    job.work = native_work;
    job.retired = native_retired;
    job.arguments.data = marker;
    job.arguments.length = 6;
    job.userdata = native;
    job.timeout_disabled = 1;
    status = rotmos_executor_submit(f->executor, &job, &native->job, error);
    memset(marker, 'x', 6);
    return status == ROTMOS_OK      ? ROTMOS_AGAIN
           : status == ROTMOS_AGAIN ? ROTMOS_LIMIT
                                    : status;
}
static rotmos_status cancel(rotmos_storage_operation *op,
                            const rotmos_storage_arguments *args, void *data,
                            rotmos_error *error)
{
    struct native_context *native = rotmos_storage_operation_userdata(op);
    (void)data;
    if (native != NULL && native->job != NULL) {
        rotmos_job_cancel(native->job, NULL);
        if (args->path != NULL && strcmp(args->path, "/cancel-error") == 0) {
            rotmos_error_clear(error);
            return ROTMOS_IO;
        }
        return ROTMOS_AGAIN;
    }
    return ROTMOS_OK;
}
static void provider_retired(rotmos_storage_operation *op,
                             const rotmos_storage_outcome *outcome, void *data)
{
    struct fixture *f = data;
    struct native_context *native = rotmos_storage_operation_userdata(op);
    (void)outcome;
    f->provider_retired++;
    if (native != NULL) {
        VERIFY(f, native->job == NULL);
        if (native->job == NULL)
            free(native);
    }
}
static void consumer_interrupted(rotmos_storage_operation *op,
                                 rotmos_status cause, void *data)
{
    struct client_context *c = data;
    VERIFY(c->f, pthread_equal(c->f->owner, pthread_self()) &&
                     rotmos_storage_operation_cancelled(op) &&
                     cause == ROTMOS_TIMEOUT && c->kind == TIMED);
    c->f->interruptions++;
}
static void consumer_retired(const rotmos_storage_outcome *outcome, void *data)
{
    struct client_context *c = data;
    struct fixture *f = c->f;
    VERIFY(f, pthread_equal(f->owner, pthread_self()));
    if (c->kind == LATE) {
        VERIFY(f, outcome->cause == ROTMOS_CANCELLED && outcome->cancelled);
        f->late_retired++;
        f->late_committed = outcome->result->committed;
        f->late = NULL;
    }
    if (c->kind == TIMED) {
        VERIFY(f, outcome->cause == ROTMOS_TIMEOUT && outcome->cancelled);
        f->timeout_retired++;
    }
    if (c->kind == CANCEL_ERROR) {
        VERIFY(f, outcome->cause == ROTMOS_CANCELLED && outcome->cancelled);
        f->cancel_error_retired++;
    }
    f->consumer_retired++;
    f->outstanding--;
    free(c);
}
static void consumer_completed(rotmos_storage_operation *op,
                               const rotmos_storage_outcome *outcome,
                               void *data)
{
    struct client_context *c = data;
    struct fixture *f = c->f;
    const rotmos_storage_result *r = outcome->result;
    rotmos_storage_arguments a;
    rotmos_storage_resource *resource;
    rotmos_asset *asset;
    rotmos_error error;
    rotmos_u64 offset, length;
    char bytes[8], chunk[7] = "abcdef";
    size_t n;
    rotmos_storage_operation *bad;
    VERIFY(f, pthread_equal(f->owner, pthread_self()) && !outcome->cancelled);
    f->completed++;
    rotmos_storage_arguments_init(&a);
    if (c->kind == BAD) {
        VERIFY(f, outcome->cause == ROTMOS_INVALID);
        return;
    }
    VERIFY(f, outcome->cause == ROTMOS_OK);
    switch (c->kind) {
    case SYNC:
        VERIFY(f, r->has_info && r->info.size.low == 4 &&
                      strcmp(r->info.etag, "\"sync\"") == 0);
        break;
    case PARTIAL:
        VERIFY(f, r->code == ROTMOS_STORAGE_IO && r->committed && r->partial &&
                      strcmp(r->recovery_path, "/.vectis-txn/recovery") == 0);
        break;
    case BEGIN:
    case CLOSED_BEGIN:
        VERIFY(f, rotmos_storage_operation_take_resource(op, &resource,
                                                         &error) == ROTMOS_OK);
        a.action = ROTMOS_STORAGE_PUT_WRITE;
        a.resource = resource;
        a.bytes.data = chunk;
        a.bytes.length = 6;
        if (c->kind == BEGIN) {
            f->writer = resource;
            VERIFY(f, start(f, WRITE_FIRST, &a) == 0);
        } else {
            VERIFY(f, start(f, CLOSED_WRITE, &a) == 0);
            VERIFY(f, rotmos_storage_resource_close(resource, &error) ==
                          ROTMOS_OK);
            VERIFY(f, rotmos_storage_operation_start(
                          f->provider, rotmos_storage_operation_loop(op), &a,
                          NULL, &bad, &error) == ROTMOS_INVALID &&
                          bad == NULL);
        }
        memset(chunk, 'x', 6);
        break;
    case WRITE_FIRST:
        VERIFY(f, r->accepted == 3);
        a.action = ROTMOS_STORAGE_PUT_WRITE;
        a.resource = f->writer;
        a.bytes.data = "def";
        a.bytes.length = 3;
        VERIFY(f, start(f, WRITE_LAST, &a) == 0);
        break;
    case WRITE_LAST:
        VERIFY(f, r->accepted == 3);
        a.action = ROTMOS_STORAGE_PUT_FINISH;
        a.resource = f->writer;
        VERIFY(f, start(f, FINISH, &a) == 0);
        break;
    case FINISH:
        VERIFY(f, r->committed);
        f->finished = 1;
        VERIFY(f,
               rotmos_storage_resource_close(f->writer, &error) == ROTMOS_OK);
        f->writer = NULL;
        break;
    case CLOSED_WRITE:
        VERIFY(f, r->accepted == 3);
        break;
    case RO_STAT:
        VERIFY(f, r->has_info && r->info.size.low == 7);
        break;
    case RO_READ:
        VERIFY(f, rotmos_storage_operation_take_asset(op, &asset, &error) ==
                      ROTMOS_OK);
        offset.high = length.high = 0;
        offset.low = 3;
        length.low = 2;
        VERIFY(f,
               rotmos_asset_source_take(asset, offset, length, &f->read_source,
                                        &error) == ROTMOS_OK);
        VERIFY(f, rotmos_source_read(f->read_source, bytes, sizeof(bytes), &n,
                                     &error) == ROTMOS_OK &&
                      n == 2 && memcmp(bytes, "lo", 2) == 0);
        break;
    case RO_LIST:
        VERIFY(f, rotmos_storage_operation_take_resource(op, &resource,
                                                         &error) == ROTMOS_OK);
        a.action = ROTMOS_STORAGE_LIST_NEXT;
        a.resource = resource;
        VERIFY(f, start(f, RO_ITEM, &a) == 0);
        break;
    case RO_ITEM:
        VERIFY(f, r->has_entry && strcmp(r->entry.name, "read") == 0 &&
                      r->entry.info.size.low == 7);
        f->listed++;
        a.action = ROTMOS_STORAGE_LIST_NEXT;
        a.resource = rotmos_storage_operation_arguments(op)->resource;
        VERIFY(f, start(f, RO_END, &a) == 0);
        break;
    case RO_END:
        VERIFY(f, r->code == ROTMOS_STORAGE_END);
        resource = rotmos_storage_operation_arguments(op)->resource;
        VERIFY(f, rotmos_storage_resource_close(resource, &error) == ROTMOS_OK);
        break;
    default:
        VERIFY(f, 0);
        break;
    }
}
static int start(struct fixture *f, int kind, rotmos_storage_arguments *args)
{
    struct client_context *c;
    rotmos_storage_operation_config config;
    rotmos_storage_operation *op;
    rotmos_error error;
    rotmos_status status;
    int count = f->completed;
    c = calloc(1, sizeof(*c));
    CHECK(c != NULL);
    c->f = f;
    c->kind = kind;
    rotmos_storage_operation_config_init(&config);
    config.completed = consumer_completed;
    config.interrupted = consumer_interrupted;
    config.retired = consumer_retired;
    config.userdata = c;
    if (kind == TIMED)
        config.timeout_ms = 20;
    status = rotmos_storage_operation_start(
        kind >= RO_STAT && kind <= RO_END ? f->readonly : f->provider,
        rotmos_server_loop(f->server), args, &config, &op, &error);
    if (status != ROTMOS_OK) {
        fprintf(stderr, "operation %d: %s\n", kind, error.message);
        free(c);
        return 1;
    }
    CHECK(f->completed == count);
    f->outstanding++;
    if (kind == LATE)
        f->late = op;
    if (kind == CANCEL_ERROR)
        CHECK(rotmos_storage_operation_cancel(op, &error) == ROTMOS_IO);
    return 0;
}
static void asset_closed(void *data)
{
    struct fixture *f = data;
    f->asset_closed++;
}
static void directory_closed(void *data)
{
    struct directory *d = data;
    d->f->asset_closed++;
    free(d);
}
static rotmos_status next_entry(rotmos_asset_entry *entry, void *data,
                                rotmos_error *error)
{
    struct directory *d = data;
    (void)error;
    if (d->index++ == 0)
        return ROTMOS_AGAIN;
    if (d->index > 2)
        return ROTMOS_EOF;
    memset(entry, 0, sizeof(*entry));
    entry->name = "read";
    entry->info.size.low = 7;
    return ROTMOS_OK;
}
static rotmos_status asset_open(const char *path, rotmos_asset **out,
                                void *data, rotmos_error *error)
{
    struct fixture *f = data;
    rotmos_asset_config config;
    rotmos_bytes bytes;
    struct directory *d;
    rotmos_status status;
    rotmos_asset_config_init(&config);
    if (strcmp(path, "/") == 0) {
        d = calloc(1, sizeof(*d));
        if (d == NULL)
            return ROTMOS_NOMEM;
        d->f = f;
        config.info.kind = ROTMOS_ASSET_DIRECTORY;
        config.next = next_entry;
        config.close = directory_closed;
        config.userdata = d;
        status = rotmos_asset_new(&config, out, error);
        if (status != ROTMOS_OK)
            free(d);
    } else if (strcmp(path, "/read") == 0) {
        config.info.size.low = 7;
        config.info.etag = "\"read\"";
        bytes.data = "payload";
        bytes.length = 7;
        status = rotmos_asset_memory(&config.info, bytes, asset_closed, f, out,
                                     error);
    } else
        return ROTMOS_EOF;
    if (status == ROTMOS_OK)
        f->asset_opened++;
    return status;
}
static rotmos_status probe_work(rotmos_job *job, rotmos_bytes args, void *data,
                                rotmos_error *error)
{
    struct fixture *f = data;
    rotmos_bytes oversized;
    VERIFY(f, !pthread_equal(f->owner, pthread_self()));
    if (f->abandon) {
        __atomic_store_n(&f->late_started, 1, __ATOMIC_RELEASE);
        for (;;)
            (void)poll(NULL, 0, 1000);
    }
    while (!__atomic_load_n(&f->released, __ATOMIC_ACQUIRE) &&
           !rotmos_job_cancelled(job))
        (void)poll(NULL, 0, 1);
    if (rotmos_job_cancelled(job))
        return ROTMOS_OK;
    VERIFY(f, args.length == 6 && memcmp(args.data, "copied", 6) == 0);
    oversized = args;
    oversized.length = 17;
    VERIFY(f, rotmos_job_result_copy(job, oversized, error) == ROTMOS_LIMIT);
    return rotmos_job_result_copy(job, args, error);
}
static int submit_probe(struct fixture *);
static void probe_complete(rotmos_job *job, const rotmos_job_outcome *outcome,
                           void *data)
{
    struct fixture *f = data;
    rotmos_error error;
    VERIFY(f, rotmos_job_result_copy(job, outcome->result, &error) ==
                  ROTMOS_STATE);
    VERIFY(f, outcome->cause == ROTMOS_OK && outcome->result.length == 6 &&
                  memcmp(outcome->result.data, "copied", 6) == 0);
    f->probe_completed++;
    if (f->probe_completed == 1)
        VERIFY(f, submit_probe(f) == 0);
}
static void probe_retired(const rotmos_job_outcome *outcome, void *data)
{
    struct fixture *f = data;
    VERIFY(f, outcome->cause == ROTMOS_OK);
    f->probe_retired++;
}
static void small_retired(const rotmos_job_outcome *outcome, void *data)
{
    struct fixture *f = data;
    VERIFY(f, outcome->cause == ROTMOS_CANCELLED && outcome->cancelled);
    f->small_retired++;
}
static int submit_probe(struct fixture *f)
{
    rotmos_job_config config;
    rotmos_job *job;
    rotmos_error error;
    char copied[7] = "copied";
    rotmos_job_config_init(&config);
    config.work = probe_work;
    config.completed = probe_complete;
    config.retired = probe_retired;
    config.arguments.data = copied;
    config.arguments.length = 6;
    config.userdata = f;
    config.timeout_disabled = 1;
    CHECK(rotmos_executor_submit(f->executor, &config, &job, &error) ==
          ROTMOS_OK);
    memset(copied, 'x', 6);
    return 0;
}
static void pool_closed(void *data)
{
    struct fixture *f = data;
    f->pools_closed++;
    if (f->pools_closed == 2)
        rotmos_server_request_stop(f->server, NULL);
}
static void advance(rotmos_timer *timer, void *data)
{
    struct fixture *f = data;
    rotmos_error error;
    if (f->abandon) {
        if (__atomic_load_n(&f->late_started, __ATOMIC_ACQUIRE)) {
            rotmos_timer_cancel(timer);
            rotmos_server_request_stop(f->server, NULL);
        }
        return;
    }
    if (__atomic_load_n(&f->failed, __ATOMIC_ACQUIRE)) {
        rotmos_timer_cancel(timer);
        rotmos_server_request_stop(f->server, NULL);
        return;
    }
    if (f->closing || !__atomic_load_n(&f->released, __ATOMIC_ACQUIRE) ||
        f->outstanding || f->probe_retired != 2 || f->small_retired != 1)
        return;
    VERIFY(f, f->late_retired == 1 && f->late_committed == 1 &&
                  f->timeout_retired == 1 && f->cancel_error_retired == 1 &&
                  f->finished && f->listed == 1 && f->writer_closed == 2);
    f->closing = 1;
    rotmos_timer_cancel(timer);
    VERIFY(f, rotmos_storage_provider_close(f->provider, &error) == ROTMOS_OK);
    VERIFY(f, rotmos_storage_provider_close(f->readonly, &error) == ROTMOS_OK);
    VERIFY(f, rotmos_source_destroy(f->read_source, &error) == ROTMOS_OK);
    VERIFY(f, rotmos_asset_provider_close(f->assets, &error) == ROTMOS_OK);
    VERIFY(f, f->asset_opened == f->asset_closed);
    VERIFY(f, rotmos_executor_close(f->executor, &error) == ROTMOS_OK &&
                  rotmos_executor_close(f->small, &error) == ROTMOS_OK);
}
static rotmos_status worker_start(rotmos_server *server,
                                  const rotmos_process_context *process,
                                  void *data, rotmos_error *error)
{
    struct fixture *f = data;
    rotmos_executor_config executor;
    rotmos_storage_provider_config provider;
    rotmos_asset_provider_config assets;
    rotmos_storage_arguments args;
    rotmos_storage_operation *op;
    rotmos_job_config job;
    rotmos_job *accepted, *extra;
    rotmos_storage_provider *limited;
    rotmos_status status;
    char path[32] = "/late";
    int invoked;
    (void)process;
    f->server = server;
    f->owner = pthread_self();
    rotmos_executor_config_init(&executor);
    executor.max_argument_bytes = 8;
    executor.max_result_bytes = 16;
    executor.retired = pool_closed;
    executor.userdata = f;
    status = rotmos_executor_new(server, &executor, &f->executor, error);
    if (status != ROTMOS_OK)
        return status;
    if (f->abandon) {
        if (submit_probe(f) != 0)
            return ROTMOS_INVALID;
        return rotmos_loop_timer(rotmos_server_loop(server), 1, 1, advance,
                                 NULL, f, &f->tick, error);
    }
    VERIFY(f, submit_probe(f) == 0);
    executor.threads = 1;
    executor.max_jobs = 1;
    status = rotmos_executor_new(server, &executor, &f->small, error);
    if (status != ROTMOS_OK)
        return status;
    rotmos_job_config_init(&job);
    job.work = probe_work;
    job.retired = small_retired;
    job.userdata = f;
    job.timeout_disabled = 1;
    status = rotmos_executor_submit(f->small, &job, &accepted, error);
    if (status != ROTMOS_OK)
        return status;
    VERIFY(f, rotmos_executor_submit(f->small, &job, &extra, error) ==
                      ROTMOS_AGAIN &&
                  extra == NULL);
    VERIFY(f, rotmos_job_cancel(accepted, error) == ROTMOS_OK &&
                  f->small_retired == 0);
    job.arguments.data = "copied";
    job.arguments.length = 9;
    VERIFY(f, rotmos_executor_submit(f->executor, &job, &extra, error) ==
                      ROTMOS_LIMIT &&
                  extra == NULL);
    rotmos_storage_provider_config_init(&provider);
    provider.invoke = invoke;
    provider.cancel = cancel;
    provider.retired = provider_retired;
    provider.close = close_provider;
    provider.userdata = f;
    provider.capabilities = ROTMOS_STORAGE_CAN_WRITE |
                            ROTMOS_STORAGE_CAN_CONDITIONAL |
                            ROTMOS_STORAGE_CAN_COPY | ROTMOS_STORAGE_CAN_MOVE;
    status = rotmos_storage_provider_new(&provider, &f->provider, error);
    if (status != ROTMOS_OK)
        return status;
    provider.capabilities = ROTMOS_STORAGE_CAN_WRITE;
    status = rotmos_storage_provider_new(&provider, &limited, error);
    if (status != ROTMOS_OK)
        return status;
    rotmos_storage_arguments_init(&args);
    args.action = ROTMOS_STORAGE_DELETE;
    args.path = "/condition";
    args.if_match = "*";
    invoked = f->invoked;
    VERIFY(f, rotmos_storage_operation_start(
                  limited, rotmos_server_loop(server), &args, NULL, &op,
                  error) == ROTMOS_NOT_SUPPORTED &&
                  op == NULL && f->invoked == invoked);
    VERIFY(f, rotmos_storage_provider_close(limited, error) == ROTMOS_OK);
    rotmos_asset_provider_config_init(&assets);
    assets.open = asset_open;
    assets.userdata = f;
    status = rotmos_asset_provider_new(&assets, &f->assets, error);
    if (status != ROTMOS_OK)
        return status;
    status = rotmos_storage_provider_assets(f->assets, &f->readonly, error);
    if (status != ROTMOS_OK)
        return status;
    VERIFY(f, rotmos_storage_operation_start(
                  f->readonly, rotmos_server_loop(server), &args, NULL, &op,
                  error) == ROTMOS_NOT_SUPPORTED &&
                  op == NULL);
    rotmos_storage_arguments_init(&args);
    args.action = ROTMOS_STORAGE_DELETE;
    args.path = path;
    VERIFY(f, start(f, LATE, &args) == 0);
    memset(path, 'x', 5);
    args.path = "/cancel-error";
    VERIFY(f, start(f, CANCEL_ERROR, &args) == 0);
    args.action = ROTMOS_STORAGE_STAT;
    args.path = "/timeout";
    VERIFY(f, start(f, TIMED, &args) == 0);
    args.path = "/sync";
    VERIFY(f, start(f, SYNC, &args) == 0);
    args.path = "/bad";
    VERIFY(f, start(f, BAD, &args) == 0);
    args.action = ROTMOS_STORAGE_COPY;
    args.path = "/partial";
    args.destination = "/destination";
    VERIFY(f, start(f, PARTIAL, &args) == 0);
    rotmos_storage_arguments_init(&args);
    args.action = ROTMOS_STORAGE_PUT_BEGIN;
    args.path = "/write";
    VERIFY(f, start(f, BEGIN, &args) == 0);
    args.path = "/closed";
    VERIFY(f, start(f, CLOSED_BEGIN, &args) == 0);
    args.action = ROTMOS_STORAGE_STAT;
    args.path = "/read";
    VERIFY(f, start(f, RO_STAT, &args) == 0);
    args.action = ROTMOS_STORAGE_OPEN_READ;
    VERIFY(f, start(f, RO_READ, &args) == 0);
    args.action = ROTMOS_STORAGE_LIST_OPEN;
    args.path = "/";
    VERIFY(f, start(f, RO_LIST, &args) == 0);
    return rotmos_loop_timer(rotmos_server_loop(server), 1, 1, advance, NULL, f,
                             &f->tick, error);
}
static rotmos_status handler(rotmos_request *request, rotmos_reply *reply,
                             void *data, rotmos_error *error)
{
    struct fixture *f = data;
    const char *path = rotmos_request_path(request).data;
    if (strcmp(path, "/status") == 0)
        return rotmos_reply_text(
            reply, 200, "text/plain",
            __atomic_load_n(&f->late_started, __ATOMIC_ACQUIRE) ? "ready"
                                                                : "waiting",
            error);
    if (strcmp(path, "/cancel-late") == 0) {
        if (rotmos_storage_operation_cancel(f->late, error) != ROTMOS_OK)
            return ROTMOS_STATE;
        VERIFY(f, f->late_retired == 0);
        return rotmos_reply_text(reply, 200, "text/plain", "cancelled", error);
    }
    if (strcmp(path, "/release") == 0) {
        __atomic_store_n(&f->released, 1, __ATOMIC_RELEASE);
        return rotmos_reply_text(reply, 200, "text/plain", "released", error);
    }
    return rotmos_reply_text(reply, 200, "text/plain", "progress", error);
}
static int request(unsigned short port, const char *path, char *body)
{
    struct sockaddr_in address;
    struct timeval timeout;
    char bytes[2048], message[256], *separator;
    size_t used = 0;
    ssize_t n;
    int fd;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 1;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
        return 1;
    snprintf(message, sizeof(message),
             "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
             path);
    if (write(fd, message, strlen(message)) != (ssize_t)strlen(message))
        return 1;
    while ((n = read(fd, bytes + used, sizeof(bytes) - used - 1)) > 0) {
        used += (size_t)n;
        if (used + 1 == sizeof(bytes))
            return 1;
    }
    close(fd);
    if (n < 0)
        return 1;
    bytes[used] = 0;
    separator = strstr(bytes, "\r\n\r\n");
    if (strstr(bytes, " 200 ") == NULL || separator == NULL)
        return 1;
    snprintf(body, 64, "%s", separator + 4);
    return 0;
}
static int client(unsigned short port)
{
    char body[64];
    int i;
    for (i = 0; i < 1000; ++i) {
        if (request(port, "/status", body))
            return 1;
        if (strcmp(body, "ready") == 0)
            break;
        (void)poll(NULL, 0, 1);
    }
    if (i == 1000)
        return 1;
    if (request(port, "/ping", body) || strcmp(body, "progress") != 0)
        return 1;
    if (request(port, "/cancel-late", body) || strcmp(body, "cancelled") != 0)
        return 1;
    (void)poll(NULL, 0, 50);
    if (request(port, "/ping", body) || strcmp(body, "progress") != 0)
        return 1;
    return request(port, "/release", body) || strcmp(body, "released") != 0;
}
static rotmos_status process_start(rotmos_server *server,
                                   const rotmos_process_context *process,
                                   void *data, rotmos_error *error)
{
    rotmos_executor *executor;
    (void)process;
    (void)data;
    if (rotmos_executor_new(server, NULL, &executor, error) != ROTMOS_STATE ||
        executor != NULL)
        return ROTMOS_INVALID;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
static rotmos_status ready(rotmos_server *server,
                           const rotmos_process_context *process, void *data,
                           rotmos_error *error)
{
    struct fixture *f = data;
    rotmos_bound_listener listener;
    int result;
    (void)process;
    {
        rotmos_storage_provider_config config;
        rotmos_storage_provider *provider;
        rotmos_storage_operation *operation;
        rotmos_storage_arguments arguments;
        rotmos_storage_provider_config_init(&config);
        config.invoke = invoke;
        config.cancel = cancel;
        config.userdata = f;
        if (rotmos_storage_provider_new(&config, &provider, error) != ROTMOS_OK)
            return ROTMOS_INVALID;
        rotmos_storage_arguments_init(&arguments);
        arguments.path = "/sync";
        if (rotmos_storage_operation_start(provider, rotmos_server_loop(server),
                                           &arguments, NULL, &operation,
                                           error) != ROTMOS_STATE ||
            operation != NULL)
            return ROTMOS_INVALID;
        if (rotmos_storage_provider_close(provider, error) != ROTMOS_OK)
            return ROTMOS_INVALID;
    }
    if (f->abandon)
        return ROTMOS_OK;
    if (rotmos_server_listener_at(server, 0, &listener, error) != ROTMOS_OK)
        return ROTMOS_STATE;
    f->child = fork();
    if (f->child == 0) {
        signal(SIGTERM, SIG_DFL);
        result = client(listener.port);
        if (result)
            kill(getppid(), SIGTERM);
        _exit(result);
    }
    return f->child < 0 ? ROTMOS_IO : ROTMOS_OK;
}
static int exited(rotmos_server *server, const rotmos_process_context *process,
                  void *data)
{
    struct fixture *f = data;
    (void)server;
    (void)process;
    return __atomic_load_n(&f->failed, __ATOMIC_ACQUIRE) || !f->closing ||
           f->pools_closed != 2 || f->outstanding || f->provider_closed != 2;
}
int main(int argc, char **argv)
{
    struct fixture fixture;
    rotmos_server_config config;
    rotmos_listener_config listener;
    rotmos_route_config route;
    rotmos_server *server;
    rotmos_executor *invalid;
    rotmos_run_result result;
    rotmos_status status;
    rotmos_error error;
    int child_status;
    (void)argv;
    memset(&fixture, 0, sizeof(fixture));
    fixture.child = -1;
    fixture.abandon = argc > 1;
    rotmos_server_config_init(&config);
    rotmos_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.workers.affinity_disabled = config.http.backend_quiet = 1;
    config.workers.shutdown_timeout_ms = fixture.abandon ? 100 : 1000;
    config.signals.mode = ROTMOS_SIGNALS_LIBRARY;
    config.callbacks.on_process_start = process_start;
    config.callbacks.on_worker_start = worker_start;
    config.callbacks.on_parent_ready = ready;
    config.callbacks.on_worker_exit = exited;
    config.callbacks.userdata = &fixture;
    CHECK(rotmos_server_new(&config, &server, &error) == ROTMOS_OK);
    CHECK(rotmos_executor_new(server, NULL, &invalid, &error) == ROTMOS_STATE &&
          invalid == NULL);
    rotmos_route_config_init(&route);
    route.path = "^/.*$";
    route.path_kind = ROTMOS_PATH_REGEX;
    route.method = ROTMOS_HTTP_GET;
    route.handler = handler;
    route.userdata = &fixture;
    CHECK(rotmos_router_route(rotmos_server_router(server), &route, NULL,
                              &error) == ROTMOS_OK);
    status = rotmos_server_run(server, &result, &error);
    if (fixture.abandon) {
        CHECK(status == ROTMOS_OK && result.escalated && result.has_failure &&
              result.last_failure.abandoned &&
              result.last_failure.error.status == ROTMOS_TIMEOUT);
    } else {
        if (status != ROTMOS_OK) {
            fprintf(stderr, "run: %s: %s\n", error.operation, error.message);
            if (fixture.child > 0) {
                kill(fixture.child, SIGTERM);
                waitpid(fixture.child, NULL, 0);
            }
            rotmos_server_destroy(server, NULL);
            return 1;
        }
        CHECK(!result.escalated && result.recovered_failures.low == 0 &&
              result.recovered_failures.high == 0);
        CHECK(waitpid(fixture.child, &child_status, 0) == fixture.child &&
              WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    }
    CHECK(rotmos_server_destroy(server, &error) == ROTMOS_OK);
    return 0;
}

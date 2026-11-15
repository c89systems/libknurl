#define _POSIX_C_SOURCE 200809L
#include "executor_internal.h"
#include "server_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct knurl_job {
    knurl_executor *executor;
    knurl_job_config config;
    knurl_job *next, *queue_next;
    unsigned char *arguments, *result;
    size_t result_size;
    pthread_t thread;
    knurl_u64 deadline, finished;
    knurl_status cause, work_status;
    knurl_error error;
    unsigned int cancelled, done, started;
    int deadline_enabled, interrupted, detached, delivered;
};
struct knurl_executor {
    knurl_loop *loop;
    knurl_executor *next;
    knurl_executor_config config;
    pthread_mutex_t queue_lock;
    knurl_job *queue_head, *queue_tail, *jobs, *jobs_tail;
    knurl_watch *watch;
    knurl_timer *timer;
    int work[2], notify[2];
    size_t count, registrations;
    unsigned int live, closing;
    int exposed, finishing, linked;
};
static knurl_status fail(knurl_error *error, knurl_status status,
                         const char *message)
{
    return knurl_fail(error, status, "executor", message);
}
static knurl_status owner(knurl_executor *e, knurl_error *error)
{
    if (e == NULL)
        return fail(error, KNURL_INVALID, "missing executor");
    return knurl_loop_owner(e->loop, error);
}
static int pipe_new(int pair[2])
{
    int i, flags;
    if (pipe(pair) != 0)
        return 0;
    for (i = 0; i < 2; ++i) {
        flags = fcntl(pair[i], F_GETFL, 0);
        if (flags < 0 || fcntl(pair[i], F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(pair[i], F_SETFD, FD_CLOEXEC) < 0) {
            int code = errno;
            close(pair[0]);
            close(pair[1]);
            pair[0] = pair[1] = -1;
            errno = code;
            return 0;
        }
    }
    return 1;
}
static int signal_pipe(int fd, size_t count)
{
    unsigned char bytes[KNURL_EXECUTOR_MAX_THREADS];
    ssize_t n;
    int saved = errno;
    memset(bytes, 1, sizeof(bytes));
    if (count > sizeof(bytes))
        count = sizeof(bytes);
    do {
        n = write(fd, bytes, count);
    } while (n < 0 && errno == EINTR);
    if (n == (ssize_t)count) {
        errno = saved;
        return 1;
    }
    return 0;
}
int knurl_job_cancelled(const knurl_job *job)
{
    return job == NULL ||
           __atomic_load_n(&job->cancelled, __ATOMIC_ACQUIRE) != 0;
}
knurl_status knurl_job_result_copy(knurl_job *job, knurl_bytes result,
                                   knurl_error *error)
{
    unsigned char *copy = NULL;
    if (job == NULL ||
        job->executor->loop->impl->process_id != (long)getpid() ||
        !__atomic_load_n(&job->started, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&job->done, __ATOMIC_ACQUIRE) ||
        !pthread_equal(job->thread, pthread_self()))
        return fail(error, KNURL_STATE,
                    "results must be published by the running native callback");
    if ((result.data == NULL && result.length != 0) ||
        result.length > job->executor->config.max_result_bytes)
        return fail(error, KNURL_LIMIT,
                    "native result exceeds its declared bound");
    if (result.length != 0) {
        copy = malloc(result.length);
        if (copy == NULL)
            return fail(error, KNURL_NOMEM, "copying native result");
        memcpy(copy, result.data, result.length);
    }
    free(job->result);
    job->result = copy;
    job->result_size = result.length;
    knurl_error_clear(error);
    return KNURL_OK;
}
static void *worker_main(void *data)
{
    knurl_executor *e = data;
    knurl_job *job;
    struct pollfd descriptor;
    unsigned char byte;
    int n;
    descriptor.fd = e->work[0];
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    for (;;) {
        if (__atomic_load_n(&e->closing, __ATOMIC_ACQUIRE))
            break;
        do {
            n = poll(&descriptor, 1, -1);
        } while (n < 0 && errno == EINTR);
        if (n < 0 || (descriptor.revents & (POLLERR | POLLNVAL)))
            break;
        if (read(e->work[0], &byte, 1) != 1)
            continue;
        (void)pthread_mutex_lock(&e->queue_lock);
        job = e->queue_head;
        if (job != NULL) {
            e->queue_head = job->queue_next;
            if (e->queue_head == NULL)
                e->queue_tail = NULL;
            job->queue_next = NULL;
        }
        (void)pthread_mutex_unlock(&e->queue_lock);
        if (job == NULL)
            continue;
        job->thread = pthread_self();
        if (knurl_job_cancelled(job))
            job->work_status = KNURL_CANCELLED;
        else {
            __atomic_store_n(&job->started, 1, __ATOMIC_RELEASE);
            job->work_status = job->config.work(
                job, job->config.arguments, job->config.userdata, &job->error);
            if (job->work_status == KNURL_AGAIN ||
                job->work_status == KNURL_EOF || job->work_status < KNURL_OK ||
                job->work_status > KNURL_BACKEND)
                job->work_status =
                    fail(&job->error, KNURL_INVALID,
                         "native callback must finish with a terminal status");
            if (job->work_status != KNURL_OK &&
                job->error.status != job->work_status)
                fail(&job->error, job->work_status, "native callback failed");
        }
        (void)knurl_loop_monotonic(&job->finished, NULL);
        __atomic_store_n(&job->done, 1, __ATOMIC_RELEASE);
        (void)signal_pipe(e->notify[1], 1);
    }
    /* After this final release store, the worker never accesses e, jobs or any
     * loop object. The closing timer covers a notification observed before the
     * release store; detached thread epilogues do not own library contexts. */
    (void)signal_pipe(e->notify[1], 1);
    (void)__atomic_sub_fetch(&e->live, 1, __ATOMIC_RELEASE);
    return NULL;
}
void knurl_executor_config_init(knurl_executor_config *config)
{
    if (config != NULL) {
        memset(config, 0, sizeof(*config));
        config->threads = 2;
        config->max_jobs = 32;
        config->max_argument_bytes = config->max_result_bytes = 65536;
    }
}
void knurl_job_config_init(knurl_job_config *config)
{
    if (config != NULL) {
        memset(config, 0, sizeof(*config));
        config->timeout_ms = 30000;
    }
}
static void unlink_executor(knurl_executor *e)
{
    knurl_executor **p = &e->loop->impl->executors;
    if (!e->linked)
        return;
    while (*p != NULL && *p != e)
        p = &(*p)->next;
    if (*p == e)
        *p = e->next;
    e->linked = 0;
    --e->loop->impl->executor_count;
}
static void release_executor(knurl_executor *e)
{
    size_t i;
    unlink_executor(e);
    for (i = 0; i < 2; ++i) {
        if (e->work[i] >= 0)
            close(e->work[i]);
        if (e->notify[i] >= 0)
            close(e->notify[i]);
    }
    (void)pthread_mutex_destroy(&e->queue_lock);
    if (e->exposed && e->config.retired != NULL)
        e->config.retired(e->config.userdata);
    free(e);
}
static void registration_retired(void *data)
{
    knurl_executor *e = data;
    if (--e->registrations == 0)
        release_executor(e);
}
static void cancel_job(knurl_job *job, knurl_status cause, int detach)
{
    if (job->delivered)
        return;
    if (detach)
        job->detached = 1;
    if (job->cause == KNURL_OK)
        job->cause = cause;
    __atomic_store_n(&job->cancelled, 1, __ATOMIC_RELEASE);
}
static void stop_queued(knurl_executor *e)
{
    knurl_job *job;
    if (pthread_mutex_trylock(&e->queue_lock) != 0)
        return;
    while ((job = e->queue_head) != NULL) {
        e->queue_head = job->queue_next;
        job->queue_next = NULL;
        job->work_status = KNURL_CANCELLED;
        (void)knurl_loop_monotonic(&job->finished, NULL);
        __atomic_store_n(&job->done, 1, __ATOMIC_RELEASE);
    }
    e->queue_tail = NULL;
    (void)pthread_mutex_unlock(&e->queue_lock);
}
static void acknowledge_queued(knurl_job *job)
{
    knurl_executor *e = job->executor;
    knurl_job **cursor, *previous = NULL;
    unsigned char byte;
    if (pthread_mutex_trylock(&e->queue_lock) != 0)
        return;
    cursor = &e->queue_head;
    while (*cursor != NULL && *cursor != job) {
        previous = *cursor;
        cursor = &(*cursor)->queue_next;
    }
    if (*cursor == job) {
        *cursor = job->queue_next;
        if (e->queue_tail == job)
            e->queue_tail = previous;
        job->queue_next = NULL;
        /* Balance a queued notification when available. A worker that already
         * reserved the token simply observes an empty queue. */
        (void)read(e->work[0], &byte, 1);
        job->work_status = KNURL_CANCELLED;
        (void)knurl_loop_monotonic(&job->finished, NULL);
        __atomic_store_n(&job->done, 1, __ATOMIC_RELEASE);
    }
    (void)pthread_mutex_unlock(&e->queue_lock);
}
static void drain(knurl_executor *e)
{
    knurl_job *job, *previous = NULL, **cursor;
    knurl_job_outcome outcome;
    knurl_u64 now;
    int order, done;
    size_t visited = 0, bound = e->count;
    if (e->finishing)
        return;
    (void)knurl_loop_now(e->loop, &now, NULL);
    if (__atomic_load_n(&e->closing, __ATOMIC_ACQUIRE))
        stop_queued(e);
    cursor = &e->jobs;
    while (visited++ < bound && (job = *cursor) != NULL) {
        done = __atomic_load_n(&job->done, __ATOMIC_ACQUIRE) != 0;
        if (job->deadline_enabled && job->cause == KNURL_OK) {
            (void)knurl_u64_compare(done ? job->finished : now, job->deadline,
                                    &order, NULL);
            if (order >= 0)
                cancel_job(job, KNURL_TIMEOUT, 0);
        }
        if (!done && knurl_job_cancelled(job)) {
            acknowledge_queued(job);
            done = __atomic_load_n(&job->done, __ATOMIC_ACQUIRE) != 0;
        }
        if (done) {
            *cursor = job->next;
            if (e->jobs_tail == job)
                e->jobs_tail = previous;
            --e->count;
        }
        if (job->cause != KNURL_OK && !job->detached && !job->interrupted) {
            job->interrupted = 1;
            if (job->config.interrupted != NULL)
                job->config.interrupted(job, job->cause, job->config.userdata);
        }
        if (!done) {
            previous = job;
            cursor = &job->next;
            continue;
        }
        memset(&outcome, 0, sizeof(outcome));
        outcome.started = __atomic_load_n(&job->started, __ATOMIC_ACQUIRE) != 0;
        outcome.cancelled = knurl_job_cancelled(job);
        outcome.cause = job->cause != KNURL_OK ? job->cause : job->work_status;
        outcome.work_status = job->work_status;
        outcome.error = job->error;
        outcome.result.data = job->result;
        outcome.result.length = job->result_size;
        if (outcome.cause != KNURL_OK && outcome.error.status != outcome.cause)
            fail(&outcome.error, outcome.cause,
                 "native job interrupted or failed");
        job->delivered = 1;
        if (!outcome.cancelled && job->config.completed != NULL)
            job->config.completed(job, &outcome, job->config.userdata);
        if (job->config.retired != NULL)
            job->config.retired(&outcome, job->config.userdata);
        free(job->arguments);
        free(job->result);
        free(job);
    }
    if (__atomic_load_n(&e->closing, __ATOMIC_ACQUIRE) && e->count == 0 &&
        __atomic_load_n(&e->live, __ATOMIC_ACQUIRE) == 0) {
        e->finishing = 1;
        unlink_executor(e);
        knurl_watch_cancel(e->watch);
        knurl_timer_cancel(e->timer);
    }
}
static void notify_ready(knurl_watch *watch, unsigned int flags, void *data)
{
    knurl_executor *e = data;
    unsigned char bytes[128];
    ssize_t n;
    (void)watch;
    (void)flags;
    do {
        n = read(e->notify[0], bytes, sizeof(bytes));
    } while (n > 0 || (n < 0 && errno == EINTR));
    drain(e);
}
static void tick(knurl_timer *timer, void *data)
{
    (void)timer;
    drain(data);
}
knurl_status knurl_executor_close(knurl_executor *e, knurl_error *error)
{
    knurl_status status;
    knurl_job *job;
    if (e == NULL) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    status = owner(e, error);
    if (status != KNURL_OK)
        return status;
    if (!__atomic_exchange_n(&e->closing, 1, __ATOMIC_ACQ_REL)) {
        for (job = e->jobs; job != NULL; job = job->next)
            cancel_job(job, KNURL_CANCELLED, 0);
        (void)signal_pipe(e->work[1], e->config.threads);
    }
    (void)signal_pipe(e->notify[1], 1);
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_executor_new(knurl_server *server,
                                const knurl_executor_config *config,
                                knurl_executor **out, knurl_error *error)
{
    knurl_executor_config defaults;
    knurl_executor *e;
    knurl_loop *loop;
    knurl_status status;
    pthread_attr_t attributes;
    pthread_t thread;
    size_t i;
    int code;
    if (out != NULL)
        *out = NULL;
    loop = server != NULL ? knurl_server_loop(server) : NULL;
    if (out == NULL || server == NULL || loop == NULL ||
        server->impl->process.role != KNURL_PROCESS_HTTP ||
        !loop->impl->native_ready || !loop->impl->http_worker)
        return fail(error, KNURL_STATE,
                    "create executors only in an active post-fork HTTP worker");
    status = knurl_loop_owner(loop, error);
    if (status != KNURL_OK)
        return status;
    if (!loop->impl->accepting || loop->impl->executor_stopping)
        return fail(error, KNURL_STATE, "worker is stopping");
    if (loop->impl->executor_count >= KNURL_EXECUTOR_MAX_POOLS)
        return fail(error, KNURL_LIMIT, "worker executor pool budget is full");
    knurl_executor_config_init(&defaults);
    if (config != NULL)
        defaults = *config;
    if (defaults.threads == 0)
        defaults.threads = 2;
    if (defaults.max_jobs == 0)
        defaults.max_jobs = 32;
    if (defaults.max_argument_bytes == 0)
        defaults.max_argument_bytes = 65536;
    if (defaults.max_result_bytes == 0)
        defaults.max_result_bytes = 65536;
    if (defaults.threads > KNURL_EXECUTOR_MAX_THREADS ||
        defaults.max_jobs > KNURL_EXECUTOR_MAX_JOBS ||
        defaults.max_argument_bytes > 1048576 ||
        defaults.max_result_bytes > 1048576)
        return fail(error, KNURL_INVALID,
                    "executor bounds exceed supported finite limits");
    e = calloc(1, sizeof(*e));
    if (e == NULL)
        return fail(error, KNURL_NOMEM, "allocating native executor");
    e->loop = loop;
    e->config = defaults;
    e->work[0] = e->work[1] = e->notify[0] = e->notify[1] = -1;
    code = pthread_mutex_init(&e->queue_lock, NULL);
    if (code != 0) {
        free(e);
        return fail(error, KNURL_IO, "initializing native queue lock");
    }
    if (!pipe_new(e->work) || !pipe_new(e->notify)) {
        release_executor(e);
        return fail(error, KNURL_IO, "creating bounded native wake pipes");
    }
    status = knurl_loop_watch(loop, e->notify[0], KNURL_READABLE, notify_ready,
                              registration_retired, e, &e->watch, error);
    if (status != KNURL_OK) {
        release_executor(e);
        return status;
    }
    e->registrations = 1;
    status = knurl_loop_timer(loop, 10, 10, tick, registration_retired, e,
                              &e->timer, error);
    if (status != KNURL_OK) {
        e->finishing = 1;
        knurl_watch_cancel(e->watch);
        return status;
    }
    ++e->registrations;
    e->next = loop->impl->executors;
    loop->impl->executors = e;
    e->linked = 1;
    ++loop->impl->executor_count;
    code = pthread_attr_init(&attributes);
    if (code == 0) {
        code =
            pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        if (code == 0) {
            for (i = 0; i < defaults.threads; ++i) {
                (void)__atomic_add_fetch(&e->live, 1, __ATOMIC_RELAXED);
                code = pthread_create(&thread, &attributes, worker_main, e);
                if (code != 0) {
                    (void)__atomic_sub_fetch(&e->live, 1, __ATOMIC_RELEASE);
                    break;
                }
            }
        }
        (void)pthread_attr_destroy(&attributes);
    }
    if (code != 0) {
        knurl_executor_close(e, NULL);
        return fail(error, KNURL_IO, "starting post-fork native threads");
    }
    e->exposed = 1;
    *out = e;
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_executor_submit(knurl_executor *e,
                                   const knurl_job_config *config,
                                   knurl_job **out, knurl_error *error)
{
    knurl_job *job;
    knurl_job_config effective;
    knurl_status status;
    knurl_u64 now, delay;
    int code;
    if (out != NULL)
        *out = NULL;
    status = owner(e, error);
    if (status != KNURL_OK)
        return status;
    if (config == NULL || out == NULL || config->work == NULL ||
        (config->arguments.data == NULL && config->arguments.length != 0) ||
        config->timeout_ms < 0)
        return fail(
            error, KNURL_INVALID,
            "provide native work, copied arguments and nonnegative timeout");
    if (__atomic_load_n(&e->closing, __ATOMIC_ACQUIRE))
        return fail(error, KNURL_STATE, "native executor is closing");
    if (config->arguments.length > e->config.max_argument_bytes)
        return fail(error, KNURL_LIMIT,
                    "native arguments exceed their declared bound");
    if (e->count >= e->config.max_jobs) {
        knurl_error_clear(error);
        return KNURL_AGAIN;
    }
    effective = *config;
    if (effective.timeout_ms == 0)
        effective.timeout_ms = 30000;
    job = calloc(1, sizeof(*job));
    if (job == NULL)
        return fail(error, KNURL_NOMEM, "allocating native job");
    job->executor = e;
    job->config = effective;
    if (effective.arguments.length != 0) {
        job->arguments = malloc(effective.arguments.length);
        if (job->arguments == NULL) {
            free(job);
            return fail(error, KNURL_NOMEM, "copying native arguments");
        }
        memcpy(job->arguments, effective.arguments.data,
               effective.arguments.length);
    }
    job->config.arguments.data = job->arguments;
    if (!effective.timeout_disabled) {
        status = knurl_loop_now(e->loop, &now, error);
        if (status == KNURL_OK)
            status = knurl_u64_from_size((size_t)effective.timeout_ms, &delay,
                                         error);
        if (status == KNURL_OK)
            status = knurl_u64_add(now, delay, &job->deadline, error);
        if (status != KNURL_OK) {
            free(job->arguments);
            free(job);
            return status;
        }
        job->deadline_enabled = 1;
    }
    code = pthread_mutex_trylock(&e->queue_lock);
    if (code != 0) {
        free(job->arguments);
        free(job);
        if (code == EBUSY) {
            knurl_error_clear(error);
            return KNURL_AGAIN;
        }
        return fail(error, KNURL_IO, "accessing native queue");
    }
    if (!signal_pipe(e->work[1], 1)) {
        (void)pthread_mutex_unlock(&e->queue_lock);
        free(job->arguments);
        free(job);
        return fail(error, KNURL_IO, "waking native executor");
    }
    if (e->queue_tail != NULL)
        e->queue_tail->queue_next = job;
    else
        e->queue_head = job;
    e->queue_tail = job;
    if (e->jobs_tail != NULL)
        e->jobs_tail->next = job;
    else
        e->jobs = job;
    e->jobs_tail = job;
    ++e->count;
    *out = job;
    (void)pthread_mutex_unlock(&e->queue_lock);
    knurl_error_clear(error);
    return KNURL_OK;
}
knurl_status knurl_job_cancel(knurl_job *job, knurl_error *error)
{
    knurl_status status;
    if (job == NULL) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    status = owner(job->executor, error);
    if (status != KNURL_OK)
        return status;
    cancel_job(job, KNURL_CANCELLED, 1);
    (void)signal_pipe(job->executor->notify[1], 1);
    knurl_error_clear(error);
    return KNURL_OK;
}
void knurl_executors_stopping(knurl_loop *loop)
{
    knurl_executor *e;
    if (loop == NULL)
        return;
    loop->impl->executor_stopping = 1;
    for (e = loop->impl->executors; e != NULL; e = e->next)
        (void)knurl_executor_close(e, NULL);
}

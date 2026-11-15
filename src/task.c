#define _POSIX_C_SOURCE 200809L
#include <knurl/task.h>
#include "executor_internal.h"
#include <stdlib.h>
#include <string.h>

struct knurl_task_pool_impl {
    knurl_task_pool shell;
    knurl_executor *executor;
    knurl_loop *loop;
    knurl_task_pool_config config;
    size_t refs;
};
struct knurl_task_impl {
    knurl_task shell;
    knurl_job *job;
    struct knurl_task_pool_impl *pool;
    knurl_loop *loop;
    knurl_waker *waker;
    knurl_task_config config;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_t thread;
    knurl_task_call_fn callback;
    void *data;
    knurl_error call_error, error;
    knurl_status call_status, status;
    unsigned int cancelled, started;
    int pending, executing, admitted;
};
static knurl_status fail(knurl_error *e, knurl_status s, const char *m)
{
    return knurl_fail(e, s, "task", m);
}
static void pool_release(struct knurl_task_pool_impl *p)
{
    if (--p->refs != 0)
        return;
    if (p->config.retired != NULL)
        p->config.retired(p->config.userdata);
    free(p);
}
void knurl_task_pool_config_init(knurl_task_pool_config *c)
{
    if (c != NULL)
        memset(c, 0, sizeof(*c));
}
void knurl_task_config_init(knurl_task_config *c)
{
    if (c != NULL)
        memset(c, 0, sizeof(*c));
}
int knurl_task_cancelled(const knurl_task *task)
{
    return task == NULL ||
           __atomic_load_n(&task->impl->cancelled, __ATOMIC_ACQUIRE) != 0;
}
static void interrupt_task(struct knurl_task_impl *t)
{
    pthread_mutex_lock(&t->lock);
    __atomic_store_n(&t->cancelled, 1, __ATOMIC_RELEASE);
    if (!t->executing)
        t->pending = 0;
    pthread_cond_signal(&t->ready);
    pthread_mutex_unlock(&t->lock);
}
knurl_status knurl_task_cancel(knurl_task *task, knurl_error *error)
{
    knurl_status status;
    if (task == NULL)
        return fail(error, KNURL_INVALID, "missing task");
    status = knurl_loop_owner(task->impl->loop, error);
    if (status != KNURL_OK)
        return status;
    interrupt_task(task->impl);
    return knurl_job_cancel(task->impl->job, error);
}
static void interrupted(knurl_job *job, knurl_status status, void *data)
{
    (void)job;
    (void)status;
    interrupt_task(data);
}
static void call_ready(knurl_loop *loop, void *data)
{
    struct knurl_task_impl *t = data;
    knurl_task_call_fn fn;
    void *value;
    (void)loop;
    pthread_mutex_lock(&t->lock);
    if (!t->pending || knurl_task_cancelled(&t->shell)) {
        pthread_mutex_unlock(&t->lock);
        return;
    }
    t->executing = 1;
    fn = t->callback;
    value = t->data;
    pthread_mutex_unlock(&t->lock);
    t->call_status = fn(value, &t->call_error);
    pthread_mutex_lock(&t->lock);
    t->executing = t->pending = 0;
    pthread_cond_signal(&t->ready);
    pthread_mutex_unlock(&t->lock);
}
knurl_status knurl_task_call(knurl_task *task, knurl_task_call_fn fn,
                             void *data, knurl_error *error)
{
    struct knurl_task_impl *t;
    knurl_status status;
    if (task == NULL || fn == NULL)
        return fail(error, KNURL_INVALID, "task and callback required");
    t = task->impl;
    if (!__atomic_load_n(&t->started, __ATOMIC_ACQUIRE) ||
        !pthread_equal(t->thread, pthread_self()))
        return fail(error, KNURL_STATE, "call requires the native task thread");
    pthread_mutex_lock(&t->lock);
    if (knurl_task_cancelled(task)) {
        pthread_mutex_unlock(&t->lock);
        return fail(error, KNURL_CANCELLED, "task cancelled");
    }
    t->callback = fn;
    t->data = data;
    t->pending = 1;
    knurl_error_clear(&t->call_error);
    status = knurl_waker_signal(t->waker, error);
    if (status == KNURL_OK) {
        while (t->pending)
            pthread_cond_wait(&t->ready, &t->lock);
        status = t->call_status;
        if (error != NULL)
            *error = t->call_error;
        if (knurl_task_cancelled(task))
            status = fail(error, KNURL_CANCELLED, "task cancelled");
    } else
        t->pending = 0;
    t->callback = NULL;
    t->data = NULL;
    pthread_mutex_unlock(&t->lock);
    return status;
}
static knurl_status work(knurl_job *job, knurl_bytes args, void *data,
                         knurl_error *error)
{
    struct knurl_task_impl *t = data;
    (void)job;
    (void)args;
    t->thread = pthread_self();
    __atomic_store_n(&t->started, 1, __ATOMIC_RELEASE);
    return t->config.work(&t->shell, t->config.userdata, error);
}
static void task_retired(void *data)
{
    struct knurl_task_impl *t = data;
    if (t->admitted && t->config.retired != NULL)
        t->config.retired(&t->shell, t->status, &t->error, t->config.userdata);
    pthread_cond_destroy(&t->ready);
    pthread_mutex_destroy(&t->lock);
    if (t->pool != NULL)
        pool_release(t->pool);
    free(t);
}
static void acknowledged(const knurl_job_outcome *outcome, void *data)
{
    struct knurl_task_impl *t = data;
    t->job = NULL;
    t->status = outcome->cause;
    t->error = outcome->error;
    (void)knurl_waker_destroy(t->waker, NULL);
}
knurl_status knurl_task_pool_submit(knurl_task_pool *pool,
                                    const knurl_task_config *config,
                                    knurl_task **out, knurl_error *error)
{
    static const knurl_task methods = {knurl_task_cancelled, knurl_task_call,
                                       knurl_task_cancel, NULL};
    struct knurl_task_impl *t;
    knurl_job_config c;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (pool == NULL || config == NULL || config->work == NULL || out == NULL)
        return fail(error, KNURL_INVALID, "pool, work and output required");
    status = knurl_loop_owner(pool->impl->loop, error);
    if (status != KNURL_OK)
        return status;
    if (pool->impl->executor == NULL)
        return fail(error, KNURL_STATE, "task pool is closed");
    t = calloc(1, sizeof(*t));
    if (t == NULL)
        return fail(error, KNURL_NOMEM, "allocating task");
    t->shell = methods;
    t->shell.impl = t;
    t->config = *config;
    t->loop = pool->impl->loop;
    if (pthread_mutex_init(&t->lock, NULL) != 0) {
        free(t);
        return fail(error, KNURL_IO, "creating task lock");
    }
    if (pthread_cond_init(&t->ready, NULL) != 0) {
        pthread_mutex_destroy(&t->lock);
        free(t);
        return fail(error, KNURL_IO, "creating task condition");
    }
    status =
        knurl_waker_new(t->loop, call_ready, task_retired, t, &t->waker, error);
    if (status != KNURL_OK) {
        task_retired(t);
        return status;
    }
    knurl_job_config_init(&c);
    c.work = work;
    c.interrupted = interrupted;
    c.retired = acknowledged;
    c.userdata = t;
    c.timeout_ms = config->timeout_ms;
    c.timeout_disabled = config->timeout_disabled;
    status = knurl_executor_submit(pool->impl->executor, &c, &t->job, error);
    if (status != KNURL_OK) {
        (void)knurl_waker_destroy(t->waker, NULL);
        return status;
    }
    t->admitted = 1;
    t->pool = pool->impl;
    ++t->pool->refs;
    *out = &t->shell;
    return KNURL_OK;
}
knurl_status knurl_task_pool_close(knurl_task_pool *pool, knurl_error *error)
{
    if (pool == NULL) {
        knurl_error_clear(error);
        return KNURL_OK;
    }
    return knurl_executor_close(pool->impl->executor, error);
}
static void pool_retired(void *data)
{
    struct knurl_task_pool_impl *p = data;
    p->executor = NULL;
    pool_release(p);
}
knurl_status knurl_task_pool_new(knurl_server *server,
                                 const knurl_task_pool_config *config,
                                 knurl_task_pool **out, knurl_error *error)
{
    static const knurl_task_pool methods = {knurl_task_pool_submit,
                                            knurl_task_pool_close, NULL};
    struct knurl_task_pool_impl *p;
    knurl_executor_config c;
    knurl_status status;
    if (out == NULL)
        return fail(error, KNURL_INVALID, "output required");
    *out = NULL;
    p = calloc(1, sizeof(*p));
    if (p == NULL)
        return fail(error, KNURL_NOMEM, "allocating task pool");
    p->shell = methods;
    p->shell.impl = p;
    p->refs = 1;
    if (config != NULL)
        p->config = *config;
    p->loop = knurl_server_loop(server);
    knurl_executor_config_init(&c);
    c.threads = p->config.threads;
    c.max_jobs = p->config.max_tasks ? p->config.max_tasks : 16;
    c.retired = pool_retired;
    c.userdata = p;
    status = knurl_executor_new(server, &c, &p->executor, error);
    if (status != KNURL_OK) {
        free(p);
        return status;
    }
    *out = &p->shell;
    return KNURL_OK;
}

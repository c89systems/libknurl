#include "binding.h"

static struct rl_cleanup stopped;

void rl_cleanup_later(struct rl_server *server, struct rl_cleanup *item,
                      void (*dispose)(lua_State *, struct rl_cleanup *))
{
    struct rl_cleanup *head =
        __atomic_load_n(&server->cleanup, __ATOMIC_ACQUIRE);
    item->dispose = dispose;
    do {
        if (head == &stopped) {
            dispose(NULL, item);
            return;
        }
        item->next = head;
    } while (!__atomic_compare_exchange_n(&server->cleanup, &head, item, 0,
                                          __ATOMIC_RELEASE, __ATOMIC_ACQUIRE));
}

void rl_cleanup_drain(struct rl_server *server, int closing)
{
    struct rl_cleanup *item, *next;
    item = __atomic_exchange_n(&server->cleanup, closing ? &stopped : NULL,
                               __ATOMIC_ACQ_REL);
    while (item != NULL) {
        next = item->next;
        item->dispose(server->worker, item);
        item = next;
    }
}

static void retired(knurl_task *task, knurl_status status,
                    const knurl_error *error, void *data)
{
    struct rl_work *work = data;
    (void)task;
    work->task = NULL;
    if (work->retry == NULL)
        work->complete(work, status, error);
    else {
        work->retry_status = status;
        work->retry_error = *error;
    }
}

static knurl_status run(knurl_task *task, void *data, knurl_error *error)
{
    struct rl_work *work = data;
    knurl_status status;
    rl_cleanup_drain(work->server, 0);
    status = work->run(task, work, error);
    rl_cleanup_drain(work->server, 0);
    return status;
}

static knurl_status submit(struct rl_work *work, long timeout,
                           knurl_error *error)
{
    knurl_task_config config;
    knurl_task_config_init(&config);
    config.work = run;
    config.retired = retired;
    config.userdata = work;
    config.timeout_ms = timeout;
    return knurl_server_submit_application_task(work->server->native, &config,
                                                &work->task, error);
}

static void retry_retired(void *data)
{
    struct rl_work *work = data;
    work->retry = NULL;
    if (work->task == NULL)
        work->complete(work, work->retry_status, &work->retry_error);
}

static void retry(knurl_timer *timer, void *data)
{
    struct rl_work *work = data;
    knurl_error error;
    knurl_u64 now, pending;
    int order = 0;
    size_t remaining;
    knurl_status status =
        knurl_loop_now(knurl_server_loop(work->server->native), &now, &error);
    if (status == KNURL_OK)
        status = knurl_u64_compare(work->deadline, now, &order, &error);
    if (status == KNURL_OK && order <= 0)
        status = rl_fail(&error, KNURL_TIMEOUT, "Lua task admission timed out");
    if (status == KNURL_OK) {
        pending.high =
            work->deadline.high - now.high - (work->deadline.low < now.low);
        pending.low = (work->deadline.low - now.low) & 0xffffffffUL;
        status = knurl_u64_to_size(pending, &remaining, &error);
        if (status == KNURL_OK)
            status = submit(work, (long)remaining, &error);
    }
    if (status != KNURL_AGAIN) {
        work->retry_status = status;
        work->retry_error = error;
        knurl_timer_cancel(timer);
    }
}

knurl_status rl_work_start(struct rl_work *work, knurl_error *error)
{
    knurl_loop *loop = knurl_server_loop(work->server->native);
    long timeout = work->server->pool.timeout_ms;
    knurl_u64 start, delay;
    knurl_status status;
    if (timeout == 0)
        timeout = 30000;
    status = knurl_loop_now(loop, &start, error);
    if (status != KNURL_OK)
        return status;
    status = submit(work, timeout, error);
    if (status != KNURL_AGAIN)
        return status;
    status = knurl_u64_from_size((size_t)timeout, &delay, error);
    if (status == KNURL_OK)
        status = knurl_u64_add(start, delay, &work->deadline, error);
    if (status != KNURL_OK)
        return status;
    work->retry_status = rl_fail(&work->retry_error, KNURL_CANCELLED,
                                 "worker stopped before Lua task admission");
    return knurl_loop_timer(loop, timeout < 10 ? timeout : 10, 10, retry,
                            retry_retired, work, &work->retry, error);
}

void rl_work_cancel(struct rl_work *work)
{
    if (work->task != NULL)
        (void)knurl_task_cancel(work->task, NULL);
    else if (work->retry != NULL)
        knurl_timer_cancel(work->retry);
}

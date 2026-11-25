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
    work->complete(work, status, error);
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
    config.timeout_disabled = work->terminal;
    return knurl_server_submit_application_task(work->server->native, &config,
                                                &work->task, error);
}

static void unlink_work(struct rl_server *server, struct rl_work *work,
                        struct rl_work *previous)
{
    if (previous != NULL)
        previous->pending_next = work->pending_next;
    else
        server->pending_head = work->pending_next;
    if (server->pending_tail == work)
        server->pending_tail = previous;
    if (!work->terminal)
        --server->pending_count;
    work->pending_next = NULL;
    work->pending = 0;
}

static knurl_status submit_pending(struct rl_work *work, knurl_error *error)
{
    knurl_u64 now, pending;
    int order = 0;
    size_t remaining;
    knurl_status status;
    if (work->terminal)
        return submit(work, 0, error);
    status =
        knurl_loop_now(knurl_server_loop(work->server->native), &now, error);
    if (status == KNURL_OK)
        status = knurl_u64_compare(work->deadline, now, &order, error);
    if (status == KNURL_OK && order <= 0)
        return rl_fail(error, KNURL_TIMEOUT, "Lua task admission timed out");
    if (status == KNURL_OK) {
        pending.high =
            work->deadline.high - now.high - (work->deadline.low < now.low);
        pending.low = (work->deadline.low - now.low) & 0xffffffffUL;
        status = knurl_u64_to_size(pending, &remaining, error);
        if (status == KNURL_OK)
            status = submit(work, (long)remaining, error);
    }
    return status;
}

static knurl_status arm_retry(struct rl_server *, knurl_error *);

static void retry_retired(void *data)
{
    struct rl_server *server = data;
    struct rl_work *work;
    int ready = server->retry_ready;
    knurl_error error;
    knurl_status status;
    server->retry_timer = NULL;
    server->retry_ready = 0;
    if (ready) {
        while ((work = server->pending_head) != NULL) {
            status = submit_pending(work, &error);
            if (status == KNURL_AGAIN)
                break;
            unlink_work(server, work, NULL);
            if (status != KNURL_OK)
                work->complete(work, status, &error);
        }
        if (server->pending_head == NULL)
            return;
        status = arm_retry(server, &error);
        if (status == KNURL_OK)
            return;
    } else {
        status = rl_fail(&error, KNURL_CANCELLED,
                         "worker stopped before Lua task admission");
    }
    while ((work = server->pending_head) != NULL) {
        unlink_work(server, work, NULL);
        work->complete(work, status, &error);
    }
}

static void retry(knurl_timer *timer, void *data)
{
    struct rl_server *server = data;
    (void)timer;
    server->retry_ready = 1;
}

static knurl_status arm_retry(struct rl_server *server, knurl_error *error)
{
    long delay = server->pool.timeout_ms;
    if (server->retry_timer != NULL)
        return KNURL_OK;
    if (delay == 0 || delay > 10)
        delay = 10;
    return knurl_loop_timer(knurl_server_loop(server->native), delay, 0, retry,
                            retry_retired, server, &server->retry_timer, error);
}

knurl_status rl_work_start(struct rl_work *work, knurl_error *error)
{
    struct rl_server *server = work->server;
    knurl_loop *loop = knurl_server_loop(server->native);
    long timeout = server->pool.timeout_ms;
    size_t limit = server->pool.max_tasks ? server->pool.max_tasks : 16;
    knurl_u64 start, delay;
    knurl_status status;
    if (timeout == 0)
        timeout = 30000;
    if (!work->terminal) {
        status = knurl_loop_now(loop, &start, error);
        if (status != KNURL_OK)
            return status;
    }
    if (server->pending_head == NULL) {
        status = submit(work, timeout, error);
        if (status != KNURL_AGAIN)
            return status;
    }
    if (!work->terminal) {
        if (server->pending_count >= limit)
            return rl_fail(error, KNURL_LIMIT,
                           "Lua callback admission queue is full");
        status = knurl_u64_from_size((size_t)timeout, &delay, error);
        if (status == KNURL_OK)
            status = knurl_u64_add(start, delay, &work->deadline, error);
        if (status != KNURL_OK)
            return status;
    }
    status = arm_retry(server, error);
    if (status != KNURL_OK)
        return status;
    work->pending = 1;
    if (server->pending_tail != NULL)
        server->pending_tail->pending_next = work;
    else
        server->pending_head = work;
    server->pending_tail = work;
    if (!work->terminal)
        ++server->pending_count;
    knurl_error_clear(error);
    return KNURL_OK;
}

void rl_work_cancel(struct rl_work *work)
{
    if (work->task != NULL)
        (void)knurl_task_cancel(work->task, NULL);
    else if (work->pending) {
        struct rl_work *previous = NULL;
        struct rl_work *item = work->server->pending_head;
        knurl_error error;
        while (item != work) {
            previous = item;
            item = item->pending_next;
        }
        unlink_work(work->server, work, previous);
        rl_fail(&error, KNURL_CANCELLED, "Lua callback cancelled");
        work->complete(work, KNURL_CANCELLED, &error);
    }
}

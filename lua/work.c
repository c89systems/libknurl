#include "binding.h"

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
    return work->run(task, work, error);
}

static knurl_status submit(struct rl_work *work, knurl_error *error)
{
    knurl_task_config config;
    knurl_task_config_init(&config);
    config.work = run;
    config.retired = retired;
    config.userdata = work;
    config.timeout_disabled = work->cleanup;
    return knurl_server_submit_application_task(work->server, &config,
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
    knurl_status status = submit(work, &error);
    if (status != KNURL_AGAIN) {
        work->retry_status = status;
        work->retry_error = error;
        knurl_timer_cancel(timer);
    }
}

knurl_status rl_work_start(struct rl_work *work, knurl_error *error)
{
    knurl_status status = submit(work, error);
    if (status != KNURL_AGAIN)
        return status;
    work->retry_status = rl_fail(&work->retry_error, KNURL_CANCELLED,
                                 "worker stopped before Lua task admission");
    return knurl_loop_timer(knurl_server_loop(work->server), 10, 10, retry,
                            retry_retired, work, &work->retry, error);
}

void rl_work_cancel(struct rl_work *work)
{
    if (work->task != NULL)
        (void)knurl_task_cancel(work->task, NULL);
    else if (work->retry != NULL)
        knurl_timer_cancel(work->retry);
}

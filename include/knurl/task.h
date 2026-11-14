#ifndef KNURL_TASK_H
#define KNURL_TASK_H
#include <knurl/loop.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct knurl_task knurl_task;
typedef struct knurl_task_pool knurl_task_pool;
/** Native work; no Lua or live request access. Poll task->cancelled(). */
typedef knurl_status (*knurl_task_fn)(knurl_task *, void *, knurl_error *);
/** Owner-thread call requested by native work. Must return promptly. */
typedef knurl_status (*knurl_task_call_fn)(void *, knurl_error *);
/** Owner-loop acknowledgment, exactly once after successful admission, even
 * after cancellation. Context may be released here; task expires on return. */
typedef void (*knurl_task_retire_fn)(knurl_task *, knurl_status,
                                     const knurl_error *, void *);
typedef struct knurl_task_pool_config {
    /** Zero selects 2 threads / 16 admitted tasks; maximum 8 / 256. */
    size_t threads, max_tasks;
    knurl_retire_fn retired;
    void *userdata;
} knurl_task_pool_config;
typedef struct knurl_task_config {
    knurl_task_fn work;
    knurl_task_retire_fn retired;
    void *userdata;
    /** Zero selects 30 seconds, including queue time. */
    long timeout_ms;
    int timeout_disabled;
} knurl_task_config;
KNURL_API void knurl_task_pool_config_init(knurl_task_pool_config *);
KNURL_API void knurl_task_config_init(knurl_task_config *);
/** Post-fork active HTTP worker only. Uses the bounded Core executor. */
KNURL_API knurl_status knurl_task_pool_new(knurl_server *,
                                           const knurl_task_pool_config *,
                                           knurl_task_pool **, knurl_error *);
/** Owner loop only. Copies config, borrows userdata through retirement.
 * AGAIN means full or briefly contended; failure takes no ownership. */
KNURL_API knurl_status knurl_task_pool_submit(knurl_task_pool *,
                                              const knurl_task_config *,
                                              knurl_task **, knurl_error *);
/** Asynchronously cancel all work. Pool expires at config.retired, after all
 * task retirement callbacks have returned. Shutdown closes pools automatically.
 */
KNURL_API knurl_status knurl_task_pool_close(knurl_task_pool *, knurl_error *);
/** Thread-safe cooperative check while the task is retained. */
KNURL_API int knurl_task_cancelled(const knurl_task *);
/** Native work thread only. Calls fn on the owner loop and waits on this
 * native thread. Borrowed data stays valid until return. Cancellation prevents
 * new calls and waits for an already executing callback to return. No callback
 * is delivered after task retirement. */
KNURL_API knurl_status knurl_task_call(knurl_task *, knurl_task_call_fn, void *,
                                       knurl_error *);
/** Owner loop only; retirement waits for native acknowledgment. */
KNURL_API knurl_status knurl_task_cancel(knurl_task *, knurl_error *);
/** Library-owned receiver; do not copy or initialize the shell or impl. */
struct knurl_task_pool {
    knurl_status (*submit)(knurl_task_pool *, const knurl_task_config *,
                           knurl_task **, knurl_error *);
    knurl_status (*close)(knurl_task_pool *, knurl_error *);
    struct knurl_task_pool_impl *impl;
};
/** Borrowed receiver valid through its retirement callback; do not copy. */
struct knurl_task {
    int (*cancelled)(const knurl_task *);
    knurl_status (*call)(knurl_task *, knurl_task_call_fn, void *,
                         knurl_error *);
    knurl_status (*cancel)(knurl_task *, knurl_error *);
    struct knurl_task_impl *impl;
};
#ifdef __cplusplus
}
#endif
#endif

#ifndef KNURL_TASK_H
#define KNURL_TASK_H

/** @file task.h Bounded native tasks for active HTTP workers. Work executes on
 * native threads; configuration, submission, cancellation and retirement use
 * the owning server loop. Accepted userdata stays borrowed until retirement.
 */

#include <knurl/loop.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Borrowed task handle, valid through its retirement callback. */
typedef struct knurl_task knurl_task;
/** Owned native pool, valid until its optional pool retirement notification. */
typedef struct knurl_task_pool knurl_task_pool;
/** Native work callback. Return a terminal status; AGAIN/EOF are INVALID.
 * Use independently retained native state, never live request/transport handles
 * or a foreign runtime. Poll task->cancelled() for cooperative cancellation.
 */
typedef knurl_status (*knurl_task_fn)(knurl_task *task, void *userdata,
                                      knurl_error *error);
/** Owner-loop call requested by native work. Must return promptly without
 * blocking, yielding or unwinding a foreign exception. data is borrowed until
 * knurl_task_call returns on the native thread.
 */
typedef knurl_status (*knurl_task_call_fn)(void *data, knurl_error *error);
/** Owner-loop acknowledgment, exactly once after successful admission, even
 * after cancellation. status includes timeout/cancellation; error is borrowed
 * for this call. Context may be released here; task expires on return.
 */
typedef void (*knurl_task_retire_fn)(knurl_task *task, knurl_status status,
                                     const knurl_error *error, void *userdata);
/** Pool bounds and owner-loop retirement notification. Initialize before use.
 */
typedef struct knurl_task_pool_config {
    /** Native threads in this pool. Zero selects 2; maximum 8. */
    size_t threads;
    /** Admitted tasks, including queued work. Zero selects 16; maximum 256. */
    size_t max_tasks;
    /** Optional owner-loop notification after close and every task retirement.
     * The pool expires on return. Not invoked when construction fails.
     */
    knurl_retire_fn retired;
    /** Borrowed context for retired; retain until that notification. */
    void *userdata;
} knurl_task_pool_config;
/** Copied task declaration; userdata remains borrowed until retirement. */
typedef struct knurl_task_config {
    /** Required native callback; queued cancellation may skip execution. */
    knurl_task_fn work;
    /** Optional owner-loop acknowledgment; release userdata here. */
    knurl_task_retire_fn retired;
    /** Borrowed by work and retired; cancellation does not release it early. */
    void *userdata;
    /** Nonnegative milliseconds including queue time. Zero selects 30000. */
    long timeout_ms;
    /** Nonzero disables the cooperative deadline; shutdown still cancels work.
     */
    int timeout_disabled;
} knurl_task_config;
/** Initialize default pool bounds and NULL callbacks; NULL config is harmless.
 */
KNURL_API void knurl_task_pool_config_init(knurl_task_pool_config *config);
/** Initialize default task timeout and NULL callbacks; NULL config is harmless.
 */
KNURL_API void knurl_task_config_init(knurl_task_config *config);
/** Create a pool on the owner loop in an active post-fork HTTP worker.
 * Starts this pool's native threads; NULL config selects defaults. Each pool
 * uses one of four worker executor slots, also used by internal native I/O,
 * filesystem and dynamic DNS pools. Exhaustion returns LIMIT; invalid bounds
 * return INVALID. Clears *out on failure. No callback runs during construction.
 */
KNURL_API knurl_status knurl_task_pool_new(knurl_server *server,
                                           const knurl_task_pool_config *config,
                                           knurl_task_pool **out,
                                           knurl_error *error);
/** Owner loop only. Copies config, borrows userdata through retirement.
 * AGAIN means full or briefly contended; failure takes no ownership and clears
 * *out. Success returns a borrowed task and owns one retirement notification
 * when configured. Submitting to a closing pool returns STATE.
 */
KNURL_API knurl_status knurl_task_pool_submit(knurl_task_pool *pool,
                                              const knurl_task_config *config,
                                              knurl_task **out,
                                              knurl_error *error);
/** Owner loop only. Stop intake and cooperatively cancel queued/running work.
 * Pool expires at config.retired, after all task retirement callbacks return.
 * Does not wait for threads. Shutdown closes pools automatically; NULL is OK.
 */
KNURL_API knurl_status knurl_task_pool_close(knurl_task_pool *pool,
                                             knurl_error *error);
/** Thread-safe cooperative check while the task is retained; NULL returns 1. */
KNURL_API int knurl_task_cancelled(const knurl_task *task);
/** Native work thread only. Calls fn on the owner loop and waits on this
 * native thread. Borrowed data stays valid until return. Cancellation prevents
 * new calls and waits for an already executing callback to return. No callback
 * is delivered after task retirement. */
KNURL_API knurl_status knurl_task_call(knurl_task *task, knurl_task_call_fn fn,
                                       void *data, knurl_error *error);
/** Owner loop only. Request cooperative cancellation without waiting; task and
 * userdata remain retained until native acknowledgment and retirement.
 */
KNURL_API knurl_status knurl_task_cancel(knurl_task *task, knurl_error *error);
/** Library-owned receiver; do not copy or initialize the shell or impl. */
struct knurl_task_pool {
    /** Owner loop only; copy config and borrow userdata through retirement.
     * AGAIN takes no ownership. Equivalent to knurl_task_pool_submit.
     */
    knurl_status (*submit)(knurl_task_pool *pool,
                           const knurl_task_config *config, knurl_task **out,
                           knurl_error *error);
    /** Owner loop only; cancel asynchronously. See knurl_task_pool_close. */
    knurl_status (*close)(knurl_task_pool *pool, knurl_error *error);
    /** Private library state; applications must not access it. */
    struct knurl_task_pool_impl *impl;
};
/** Borrowed receiver valid through its retirement callback; do not copy. */
struct knurl_task {
    /** Thread-safe cancellation check; see knurl_task_cancelled. */
    int (*cancelled)(const knurl_task *task);
    /** Native work thread only; await an owner-loop call. See knurl_task_call.
     */
    knurl_status (*call)(knurl_task *task, knurl_task_call_fn fn, void *data,
                         knurl_error *error);
    /** Owner loop only; cancel cooperatively. See knurl_task_cancel. */
    knurl_status (*cancel)(knurl_task *task, knurl_error *error);
    /** Private library state; applications must not access it. */
    struct knurl_task_impl *impl;
};
#ifdef __cplusplus
}
#endif
#endif

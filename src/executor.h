#ifndef KNURL_PRIVATE_EXECUTOR_H
#define KNURL_PRIVATE_EXECUTOR_H
#include <knurl/loop.h>
#ifdef __cplusplus
extern "C" {
#endif
#define KNURL_EXECUTOR_MAX_THREADS 8u
#define KNURL_EXECUTOR_MAX_JOBS 256u
#define KNURL_EXECUTOR_MAX_POOLS 4u
typedef struct knurl_executor knurl_executor;
typedef struct knurl_job knurl_job;
typedef struct knurl_job_outcome {
    knurl_status cause, work_status;
    knurl_error error;
    knurl_bytes result;
    int started, cancelled;
} knurl_job_outcome;
typedef knurl_status (*knurl_job_fn)(knurl_job *, knurl_bytes, void *,
                                     knurl_error *);
typedef void (*knurl_job_complete_fn)(knurl_job *, const knurl_job_outcome *,
                                      void *);
typedef void (*knurl_job_interrupt_fn)(knurl_job *, knurl_status, void *);
typedef void (*knurl_job_retire_fn)(const knurl_job_outcome *, void *);
typedef struct knurl_executor_config {
    size_t threads, max_jobs, max_argument_bytes, max_result_bytes;
    knurl_retire_fn retired;
    void *userdata;
} knurl_executor_config;
typedef struct knurl_job_config {
    knurl_job_fn work;
    knurl_job_complete_fn completed;
    knurl_job_interrupt_fn interrupted;
    knurl_job_retire_fn retired;
    knurl_bytes arguments;
    void *userdata;
    long timeout_ms;
    int timeout_disabled;
} knurl_job_config;
void knurl_executor_config_init(knurl_executor_config *);
void knurl_job_config_init(knurl_job_config *);
/* Explicitly starts threads, only in an active HTTP worker after its final
 * fork. Zero bounds select 2 threads, 32 jobs and 64 KiB arguments/results;
 * individual byte bounds may not exceed 1 MiB. Constructor failure takes no
 * retirement notification. The loop retains accepted work through shutdown.
 */
knurl_status knurl_executor_new(knurl_server *, const knurl_executor_config *,
                                knurl_executor **, knurl_error *);
/* Owner-loop only. Success copies arguments and retains userdata through its
 * one retirement callback. AGAIN/full capacity takes nothing. Work runs once
 * on a default-stack native thread and must not access Lua or live transport
 * handles. Completed/interrupted/retired run on the owner loop, never inline.
 * Handles are borrowed until retirement. No implicit whole-value buffering.
 */
knurl_status knurl_executor_submit(knurl_executor *, const knurl_job_config *,
                                   knurl_job **, knurl_error *);
/* Native work callback only. Replaces a bounded copied result. */
knurl_status knurl_job_result_copy(knurl_job *, knurl_bytes, knurl_error *);
/* Thread-safe cooperative check, valid only while the job is retained. */
int knurl_job_cancelled(const knurl_job *);
/* Owner-loop only. Cancel revokes completion immediately; retirement waits
 * for acknowledgement. Timeout invokes interrupted once, suppresses completed,
 * and also waits for acknowledgement. Retired retains the actual work status
 * and result even after cancellation (for example, a committed mutation).
 */
knurl_status knurl_job_cancel(knurl_job *, knurl_error *);
/* Asynchronous close cancels queued/running jobs and stops intake. The handle
 * remains retained until config.retired; no join waits on an HTTP callback.
 * Worker stopping closes all its pools automatically. An uncooperative job is
 * abandoned by the existing parent shutdown deadline, not freed while running.
 */
knurl_status knurl_executor_close(knurl_executor *, knurl_error *);
#ifdef __cplusplus
}
#endif
#endif

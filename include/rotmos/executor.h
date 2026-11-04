#ifndef ROTMOS_EXECUTOR_H
#define ROTMOS_EXECUTOR_H
#include <rotmos/loop.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ROTMOS_EXECUTOR_MAX_THREADS 8u
#define ROTMOS_EXECUTOR_MAX_JOBS 256u
#define ROTMOS_EXECUTOR_MAX_POOLS 4u
typedef struct rotmos_executor rotmos_executor;
typedef struct rotmos_job rotmos_job;
typedef struct rotmos_job_outcome {
    rotmos_status cause, work_status;
    rotmos_error error;
    rotmos_bytes result;
    int started, cancelled;
} rotmos_job_outcome;
typedef rotmos_status (*rotmos_job_fn)(rotmos_job *, rotmos_bytes, void *,
                                       rotmos_error *);
typedef void (*rotmos_job_complete_fn)(rotmos_job *, const rotmos_job_outcome *,
                                       void *);
typedef void (*rotmos_job_interrupt_fn)(rotmos_job *, rotmos_status, void *);
typedef void (*rotmos_job_retire_fn)(const rotmos_job_outcome *, void *);
typedef struct rotmos_executor_config {
    size_t threads, max_jobs, max_argument_bytes, max_result_bytes;
    rotmos_retire_fn retired;
    void *userdata;
} rotmos_executor_config;
typedef struct rotmos_job_config {
    rotmos_job_fn work;
    rotmos_job_complete_fn completed;
    rotmos_job_interrupt_fn interrupted;
    rotmos_job_retire_fn retired;
    rotmos_bytes arguments;
    void *userdata;
    long timeout_ms;
    int timeout_disabled;
} rotmos_job_config;
ROTMOS_API void rotmos_executor_config_init(rotmos_executor_config *);
ROTMOS_API void rotmos_job_config_init(rotmos_job_config *);
/* Explicitly starts threads, only in an active HTTP worker after its final
 * fork. Zero bounds select 2 threads, 32 jobs and 64 KiB arguments/results;
 * individual byte bounds may not exceed 1 MiB. Constructor failure takes no
 * retirement notification. The loop retains accepted work through shutdown.
 */
ROTMOS_API rotmos_status rotmos_executor_new(rotmos_server *,
                                             const rotmos_executor_config *,
                                             rotmos_executor **,
                                             rotmos_error *);
/* Owner-loop only. Success copies arguments and retains userdata through its
 * one retirement callback. AGAIN/full capacity takes nothing. Work runs once
 * on a default-stack native thread and must not access Lua or live transport
 * handles. Completed/interrupted/retired run on the owner loop, never inline.
 * Handles are borrowed until retirement. No implicit whole-value buffering.
 */
ROTMOS_API rotmos_status rotmos_executor_submit(rotmos_executor *,
                                                const rotmos_job_config *,
                                                rotmos_job **, rotmos_error *);
/* Native work callback only. Replaces a bounded copied result. */
ROTMOS_API rotmos_status rotmos_job_result_copy(rotmos_job *, rotmos_bytes,
                                                rotmos_error *);
/* Thread-safe cooperative check, valid only while the job is retained. */
ROTMOS_API int rotmos_job_cancelled(const rotmos_job *);
/* Owner-loop only. Cancel revokes completion immediately; retirement waits
 * for acknowledgement. Timeout invokes interrupted once, suppresses completed,
 * and also waits for acknowledgement. Retired retains the actual work status
 * and result even after cancellation (for example, a committed mutation).
 */
ROTMOS_API rotmos_status rotmos_job_cancel(rotmos_job *, rotmos_error *);
/* Asynchronous close cancels queued/running jobs and stops intake. The handle
 * remains retained until config.retired; no join waits on an HTTP callback.
 * Worker stopping closes all its pools automatically. An uncooperative job is
 * abandoned by the existing parent shutdown deadline, not freed while running.
 */
ROTMOS_API rotmos_status rotmos_executor_close(rotmos_executor *,
                                               rotmos_error *);
#ifdef __cplusplus
}
#endif
#endif

#include "kore_internal.h"

#include <sys/wait.h>
#include <string.h>

static size_t slot(const struct kore_worker *child)
{
    return child->id == KORE_WORKER_KEYMGR
               ? 0
               : (child->id == KORE_WORKER_ACME ? 1 : child->id + 1U);
}

void knurl_kore_child_failure(struct knurl_runtime *runtime,
                              const knurl_error *error)
{
    size_t index;
    if (worker == NULL || error->status == KNURL_OK)
        return;
    index = slot(worker);
    if (!__atomic_load_n(&runtime->shared->failures[index].valid,
                         __ATOMIC_ACQUIRE)) {
        runtime->shared->failures[index].error = *error;
        __atomic_store_n(&runtime->shared->failures[index].valid, 1,
                         __ATOMIC_RELEASE);
    }
}

void knurl_kore_worker_event(void *userdata, const struct kore_worker *child,
                             int kind, int status, int expected, int abandoned)
{
    struct knurl_runtime *runtime;
    knurl_worker_event event;
    knurl_u64 one;
    size_t index;
    runtime = userdata;
    index = slot(child);
    if (kind == KORE_LIBRARY_WORKER_SPAWNING) {
        __atomic_store_n(&runtime->shared->failures[index].valid, 0,
                         __ATOMIC_RELEASE);
        if (child->id < KORE_WORKER_KEYMGR)
            knurl_kore_tls_worker(runtime, child->id - 1U, 0);
        if (child->id == KORE_WORKER_KEYMGR)
            __atomic_store_n(&runtime->shared->acme_identity_valid, 0,
                             __ATOMIC_RELEASE);
        return;
    }
    memset(&event, 0, sizeof(event));
    event.kind = kind == KORE_LIBRARY_WORKER_STARTED
                     ? KNURL_WORKER_STARTED
                     : (kind == KORE_LIBRARY_WORKER_RESTARTED
                            ? KNURL_WORKER_RESTARTED
                            : (kind == KORE_LIBRARY_WORKER_ESCALATED
                                   ? KNURL_WORKER_ESCALATED
                                   : KNURL_WORKER_EXITED));
    event.process.role =
        child->id == KORE_WORKER_KEYMGR
            ? KNURL_PROCESS_KEY_MANAGER
            : (child->id == KORE_WORKER_ACME ? KNURL_PROCESS_ACME
                                             : KNURL_PROCESS_HTTP);
    event.process.process_id = (long)child->pid;
    event.process.worker_index =
        event.process.role == KNURL_PROCESS_HTTP ? child->id - 1U : 0;
    event.process.restarted = child->restarted;
    event.expected = expected;
    event.abandoned = abandoned;
    index = slot(child);
    knurl_error_clear(&event.error);
    if (event.kind == KNURL_WORKER_EXITED) {
        if (WIFEXITED(status))
            event.exit_status = WEXITSTATUS(status);
        if (WIFSIGNALED(status))
            event.signal_number = WTERMSIG(status);
    }
    if (event.kind == KNURL_WORKER_ESCALATED ||
        (event.kind == KNURL_WORKER_EXITED && child->library_timed_out)) {
        (void)knurl_fail(&event.error, KNURL_TIMEOUT, "worker_deadline",
                         child->library_timed_out == 2
                             ? "owned child exceeded the shutdown deadline"
                             : "owned child exceeded its operation deadline");
        event.error.source = KNURL_ERROR_BACKEND;
    } else if (event.kind == KNURL_WORKER_EXITED &&
               __atomic_load_n(&runtime->shared->failures[index].valid,
                               __ATOMIC_ACQUIRE)) {
        event.error = runtime->shared->failures[index].error;
    } else if (event.kind == KNURL_WORKER_EXITED &&
               (!expected || event.exit_status || event.signal_number ||
                abandoned)) {
        (void)knurl_fail(&event.error, KNURL_BACKEND, "worker_exit",
                         "owned child exited before orderly completion");
        event.error.source = KNURL_ERROR_BACKEND;
    }
    if (event.error.status != KNURL_OK) {
        runtime->last_failure = event;
        runtime->has_failure = 1;
        if (event.kind == KNURL_WORKER_EXITED &&
            event.process.role == KNURL_PROCESS_KEY_MANAGER)
            knurl_kore_tls_observe(runtime, &event.error);
        if (event.kind == KNURL_WORKER_EXITED && expected &&
            event.exit_status != 0 && !abandoned) {
            runtime->status = event.error.status;
            runtime->error = event.error;
        }
        if (event.kind == KNURL_WORKER_EXITED && !expected) {
            runtime->recovery_pending[index] = 1;
            if (!runtime->ready ||
                runtime->server->config.workers.death_policy ==
                    KNURL_WORKER_TERMINATE) {
                runtime->status = event.error.status;
                runtime->error = event.error;
            }
        }
    }
    if (event.kind == KNURL_WORKER_EXITED &&
        event.process.role == KNURL_PROCESS_HTTP)
        knurl_kore_tls_worker(runtime, event.process.worker_index, 0);
    if (event.kind == KNURL_WORKER_RESTARTED &&
        runtime->recovery_pending[index]) {
        one.high = 0;
        one.low = 1;
        if (knurl_u64_add(runtime->recovered_failures, one,
                          &runtime->recovered_failures,
                          &runtime->error) != KNURL_OK) {
            runtime->status = KNURL_OVERFLOW;
            kore_quit = KORE_QUIT_FATAL;
        }
        runtime->recovery_pending[index] = 0;
    }
    if (runtime->server->config.callbacks.on_worker_event != NULL)
        runtime->server->config.callbacks.on_worker_event(
            runtime->server, &event,
            runtime->server->config.callbacks.userdata);
}

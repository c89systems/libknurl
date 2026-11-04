#include "source_internal.h"
#include "response_internal.h"
#include "native_io.h"
#include <stdlib.h>
#include <string.h>
enum { PULL_IDLE, PULL_RUNNING, PULL_READY, PULL_PAUSED };
struct native_source {
    rotmos_source *source;
    rotmos_executor *pool;
    rotmos_job *job;
    rotmos_timer *admission;
    unsigned char *bytes;
    size_t capacity, offered, count, offset, references;
    rotmos_source_read_fn read;
    void *userdata;
    rotmos_status status, outcome;
    rotmos_error error;
    int phase, wake, revoked, closing;
};
static void unref(struct native_source *b)
{
    if (--b->references != 0)
        return;
    free(b->bytes);
    free(b);
}
static void finalize(struct native_source *b)
{
    rotmos_source *source;
    if (!b->closing || b->job != NULL || b->source == NULL)
        return;
    source = b->source;
    b->source = NULL;
    source->native = NULL;
    rotmos_source_release_now(source, b->outcome);
    unref(b);
}
static rotmos_status work(rotmos_job *job, rotmos_bytes arguments, void *data,
                          rotmos_error *error)
{
    struct native_source *b = data;
    (void)arguments;
    (void)error;
    /* Only the copied read declaration and owned chunk enter application code.
     * Response fields are accessed exclusively by owner-loop callbacks. */
    b->source->native_job = job;
    b->count = 0;
    rotmos_error_clear(&b->error);
    b->status =
        b->read(b->bytes, b->offered, &b->count, b->userdata, &b->error);
    b->source->native_job = NULL;
    return ROTMOS_OK;
}
static void retired(const rotmos_job_outcome *outcome, void *data)
{
    struct native_source *b = data;
    b->job = NULL;
    if (!b->revoked) {
        if (!outcome->started || outcome->cancelled ||
            outcome->work_status != ROTMOS_OK) {
            b->status =
                outcome->cause != ROTMOS_OK ? outcome->cause : ROTMOS_IO;
            b->source->failure = b->status;
            b->error = outcome->error;
        } else
            b->status = rotmos_source_validate(b->source, b->offered, b->count,
                                               b->status, &b->error);
        b->offset = 0;
        if (b->status == ROTMOS_AGAIN)
            b->phase = b->wake ? PULL_IDLE : PULL_PAUSED;
        else
            b->phase = PULL_READY;
        if (b->phase != PULL_PAUSED && b->source->resume != NULL)
            b->source->resume(b->source->resume_data);
    }
    finalize(b);
    unref(b);
}
static rotmos_status submit(struct native_source *b, rotmos_error *error)
{
    rotmos_job_config config;
    rotmos_status status;
    rotmos_job_config_init(&config);
    config.work = work;
    config.retired = retired;
    config.userdata = b;
    config.timeout_disabled = 1;
    status = rotmos_executor_submit(b->pool, &config, &b->job, error);
    if (status == ROTMOS_OK) {
        ++b->references;
        b->phase = PULL_RUNNING;
    }
    return status;
}
static void admission_retired(void *data)
{
    struct native_source *b = data;
    b->admission = NULL;
    if (!b->revoked && b->phase == PULL_IDLE && b->source->resume != NULL)
        b->source->resume(b->source->resume_data);
    unref(b);
}
static void admission(rotmos_timer *timer, void *data)
{
    struct native_source *b = data;
    rotmos_status status;
    rotmos_error error;
    if (b->revoked) {
        rotmos_timer_cancel(timer);
        return;
    }
    status = submit(b, &error);
    if (status == ROTMOS_AGAIN)
        return;
    rotmos_timer_cancel(timer);
    if (status != ROTMOS_OK) {
        b->error = error;
        b->status = status;
        b->phase = PULL_READY;
        b->source->failure = status;
        if (b->source->resume != NULL)
            b->source->resume(b->source->resume_data);
    }
}
rotmos_status rotmos_source_native_pull(rotmos_source *source,
                                        rotmos_response *response, void *buffer,
                                        size_t capacity, size_t *produced,
                                        rotmos_error *error)
{
    struct native_source *b = source->native;
    rotmos_status status;
    size_t n;
    *produced = 0;
    if (source->eof && b == NULL)
        return ROTMOS_EOF;
    if (b == NULL) {
        b = calloc(1, sizeof(*b));
        if (b == NULL)
            return rotmos_fail(error, ROTMOS_NOMEM, "source_native",
                               "allocating native source");
        b->capacity = response->chunk;
        b->bytes = malloc(b->capacity);
        if (b->bytes == NULL) {
            free(b);
            return rotmos_fail(error, ROTMOS_NOMEM, "source_native",
                               "allocating native chunk");
        }
        status = rotmos_native_io_pool(rotmos_request_server(response->request),
                                       &b->pool, error);
        if (status != ROTMOS_OK) {
            free(b->bytes);
            free(b);
            return status;
        }
        b->source = source;
        b->references = 1;
        b->read = source->config.read;
        b->userdata = source->config.userdata;
        source->native = b;
    }
    if (b->revoked)
        return ROTMOS_CANCELLED;
    if (b->phase == PULL_READY) {
        if (b->status != ROTMOS_OK) {
            if (error != NULL)
                *error = b->error;
            return b->status;
        }
        n = b->count - b->offset;
        if (n > capacity)
            n = capacity;
        memcpy(buffer, b->bytes + b->offset, n);
        b->offset += n;
        *produced = n;
        if (b->offset == b->count)
            b->phase = PULL_IDLE;
        return ROTMOS_OK;
    }
    if (b->phase != PULL_IDLE || b->admission != NULL)
        return ROTMOS_AGAIN;
    b->offered = capacity < b->capacity ? capacity : b->capacity;
    b->wake = 0;
    source->resume_requested = 0;
    status = submit(b, error);
    if (status == ROTMOS_OK)
        return ROTMOS_AGAIN;
    if (status != ROTMOS_AGAIN)
        return status;
    status = rotmos_loop_timer(source->loop, 1, 1, admission, admission_retired,
                               b, &b->admission, error);
    if (status != ROTMOS_OK)
        return status;
    ++b->references;
    return ROTMOS_AGAIN;
}
void rotmos_source_native_resume(rotmos_source *source)
{
    struct native_source *b = source->native;
    b->wake = 1;
    if (b->phase == PULL_PAUSED)
        b->phase = PULL_IDLE;
}
void rotmos_source_native_revoke(rotmos_source *source)
{
    struct native_source *b = source->native;
    b->revoked = 1;
    if (b->admission != NULL)
        rotmos_timer_cancel(b->admission);
    if (b->job != NULL)
        (void)rotmos_job_cancel(b->job, NULL);
}
void rotmos_source_native_retire(rotmos_source *source, rotmos_status outcome)
{
    struct native_source *b = source->native;
    b->closing = 1;
    b->outcome = outcome;
    rotmos_source_native_revoke(source);
    finalize(b);
}

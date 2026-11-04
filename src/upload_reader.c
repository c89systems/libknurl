#define _POSIX_C_SOURCE 200809L
#include "upload_internal.h"
#include "native_io.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct reader_route {
    rotmos_upload_reader_config config;
    void (*release_userdata)(void *);
};
struct reader_upload {
    rotmos_upload_reader_config config;
    /* Owner loop only; revoked before request close. */
    rotmos_request *request;
    rotmos_upload_metadata metadata;
    void *text;
    rotmos_header *pairs;
    rotmos_source *reader;
    rotmos_reply *result;
    rotmos_executor *pool;
    rotmos_job *job;
    rotmos_waker *waker;
    rotmos_timer *admission;
    size_t references, queued, capacity;
    int pipe[2], started, body_end, done, failed;
};
static void unref(void *data)
{
    struct reader_upload *s = data;
    if (--s->references != 0)
        return;
    if (s->pipe[0] != -1)
        close(s->pipe[0]);
    if (s->pipe[1] != -1)
        close(s->pipe[1]);
    if (s->reader != NULL)
        rotmos_source_release(s->reader, ROTMOS_CANCELLED);
    rotmos_reply_destroy(s->result, NULL);
    free(s->text);
    free(s->pairs);
    free(s);
}
static void close_writer(struct reader_upload *s)
{
    if (s->pipe[1] != -1) {
        close(s->pipe[1]);
        s->pipe[1] = -1;
    }
}
static rotmos_status io_error(rotmos_error *error, const char *message)
{
    int code = errno;
    rotmos_status status =
        rotmos_fail(error, ROTMOS_IO, "upload_reader", message);
    if (error != NULL) {
        error->source = ROTMOS_ERROR_SYSTEM;
        error->native_code = code;
    }
    return status;
}
static void fail(struct reader_upload *s, rotmos_status status)
{
    if (s->request == NULL || s->failed)
        return;
    s->failed = 1;
    s->request->outcome.cause = status;
    rotmos_response_fail(s->request->response, status);
}
static rotmos_status submit_reply(rotmos_request *request, rotmos_error *error)
{
    rotmos_status status =
        rotmos_response_submit(request->response, request->reply, error);
    if (status == ROTMOS_OK && request->in_headers) {
        request->response->complete_reply = 1;
        request->response->phase = ROTMOS_RESPONSE_FINISHING;
    }
    return status;
}
static void publish(struct reader_upload *s)
{
    rotmos_reply *old;
    rotmos_status status;
    if (!s->done || !s->body_end || s->failed || s->request == NULL)
        return;
    old = s->request->reply;
    s->request->reply = s->result;
    s->result = NULL;
    s->request->reply->owner = s->request;
    old->owner = NULL;
    rotmos_reply_destroy(old, NULL);
    status = rotmos_router_hooks_after(s->request, NULL);
    if (status == ROTMOS_OK)
        status = submit_reply(s->request, NULL);
    if (status != ROTMOS_OK)
        fail(s, status);
}
static void waker_retired(void *data)
{
    struct reader_upload *s = data;
    s->waker = NULL;
    unref(s);
}
static void wake(rotmos_loop *loop, void *data)
{
    struct reader_upload *s = data;
    (void)loop;
    if (s->request != NULL && !s->body_end && !s->failed)
        (void)rotmos_request_body_resume(s->request, NULL);
}
static void stop_waker(struct reader_upload *s)
{
    if (s->waker != NULL)
        (void)rotmos_waker_destroy(s->waker, NULL);
}
static rotmos_status read_pipe(void *buffer, size_t capacity, size_t *produced,
                               void *data, rotmos_error *error)
{
    struct reader_upload *s = data;
    struct pollfd descriptor;
    ssize_t n;
    int code;
    *produced = 0;
    descriptor.fd = s->pipe[0];
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    for (;;) {
        if (rotmos_source_cancelled(s->reader))
            return ROTMOS_CANCELLED;
        do {
            n = read(s->pipe[0], buffer, capacity);
        } while (n < 0 && errno == EINTR);
        if (n > 0) {
            __atomic_sub_fetch(&s->queued, (size_t)n, __ATOMIC_ACQ_REL);
            *produced = (size_t)n;
            (void)rotmos_waker_signal(s->waker, NULL);
            return ROTMOS_OK;
        }
        if (n == 0)
            return ROTMOS_EOF;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return io_error(error, "reading upload queue");
        /* Only native work waits. Periodic cancellation observes executor stop
         * even if no request-facing callback can write another byte. */
        do {
            code = poll(&descriptor, 1, 10);
        } while (code < 0 && errno == EINTR);
        if (code < 0 || (descriptor.revents & POLLNVAL))
            return io_error(error, "waiting for upload input");
    }
}
static rotmos_status work(rotmos_job *job, rotmos_bytes arguments, void *data,
                          rotmos_error *error)
{
    struct reader_upload *s = data;
    rotmos_status status;
    int complete;
    (void)arguments;
    s->reader->native_job = job;
    status = rotmos_source_cancelled(s->reader)
                 ? ROTMOS_CANCELLED
                 : s->config.handler(&s->metadata, s->reader, s->result,
                                     s->config.userdata, error);
    complete = s->reader->eof ||
               (s->metadata.has_content_length &&
                s->reader->position.high == s->metadata.content_length.high &&
                s->reader->position.low == s->metadata.content_length.low) ||
               (!s->metadata.has_content_length &&
                s->reader->position.high == 0 && s->reader->position.low == 0);
    if (status == ROTMOS_OK && s->reader->failure != ROTMOS_OK)
        status = s->reader->failure;
    if (status == ROTMOS_OK && (!complete || !s->result->selected))
        status = rotmos_fail(
            error, ROTMOS_STATE, "upload_reader",
            "handler must consume admitted input and select a reply");
    s->reader->native_job = NULL;
    return status;
}
static void retired(const rotmos_job_outcome *outcome, void *data)
{
    struct reader_upload *s = data;
    rotmos_status status =
        outcome->cancelled ? outcome->cause : outcome->work_status;
    s->job = NULL;
    s->done = 1;
    stop_waker(s);
    if (s->request != NULL) {
        if (status != ROTMOS_OK)
            fail(s, status);
        else
            publish(s);
    }
    unref(s);
}
static rotmos_status submit(struct reader_upload *s, rotmos_error *error)
{
    rotmos_job_config job;
    rotmos_status status;
    rotmos_job_config_init(&job);
    job.work = work;
    job.retired = retired;
    job.userdata = s;
    job.timeout_disabled = 1;
    status = rotmos_executor_submit(s->pool, &job, &s->job, error);
    if (status == ROTMOS_OK)
        ++s->references;
    return status;
}
static void admission_retired(void *data)
{
    struct reader_upload *s = data;
    s->admission = NULL;
    unref(s);
}
static void admission(rotmos_timer *timer, void *data)
{
    struct reader_upload *s = data;
    rotmos_status status;
    if (s->request == NULL) {
        rotmos_timer_cancel(timer);
        return;
    }
    status = submit(s, NULL);
    if (status == ROTMOS_AGAIN)
        return;
    rotmos_timer_cancel(timer);
    if (status != ROTMOS_OK) {
        fail(s, status);
        stop_waker(s);
    }
}
static rotmos_status start(struct reader_upload *s, rotmos_error *error)
{
    rotmos_source_config config;
    rotmos_status status;
    size_t i;
    int flags;
    if (s->started)
        return ROTMOS_OK;
    s->started = 1;
    status = rotmos_native_io_pool(rotmos_request_server(s->request), &s->pool,
                                   error);
    if (status != ROTMOS_OK)
        return status;
    if (pipe(s->pipe) != 0) {
        s->pipe[0] = s->pipe[1] = -1;
        return io_error(error, "creating upload queue");
    }
    for (i = 0; i < 2; ++i) {
        flags = fcntl(s->pipe[i], F_GETFL, 0);
        if (flags < 0 || fcntl(s->pipe[i], F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(s->pipe[i], F_SETFD, FD_CLOEXEC) < 0)
            return io_error(error, "configuring upload queue");
    }
    status = rotmos_upload_metadata_copy(s->request, &s->metadata, &s->text,
                                         &s->pairs, error);
    if (status != ROTMOS_OK)
        return status;
    rotmos_source_config_init(&config);
    config.read = read_pipe;
    config.userdata = s;
    config.has_length = s->metadata.has_content_length;
    config.length = s->metadata.content_length;
    status = rotmos_source_new(&config, &s->reader, error);
    if (status != ROTMOS_OK)
        return status;
    s->reader->borrowed = 1;
    status = rotmos_reply_new(&s->result, error);
    if (status != ROTMOS_OK)
        return status;
    for (i = 0; i < s->request->reply->header_count; ++i) {
        rotmos_header *h = &s->request->reply->headers[i];
        status = rotmos_reply_header_add(s->result, h->name.data, h->value.data,
                                         error);
        if (status != ROTMOS_OK)
            return status;
    }
    status = rotmos_waker_new(rotmos_request_loop(s->request), wake,
                              waker_retired, s, &s->waker, error);
    if (status != ROTMOS_OK)
        return status;
    ++s->references;
    status = submit(s, error);
    if (status != ROTMOS_AGAIN)
        return status;
    status = rotmos_loop_timer(rotmos_request_loop(s->request), 1, 1, admission,
                               admission_retired, s, &s->admission, error);
    if (status == ROTMOS_OK)
        ++s->references;
    return status;
}
static rotmos_status body(rotmos_request *request, const void *bytes,
                          size_t length, size_t *consumed, void *data,
                          rotmos_error *error)
{
    struct reader_upload *s = request->codec_data;
    size_t used, n;
    ssize_t written;
    rotmos_status status;
    (void)data;
    *consumed = 0;
    status = start(s, error);
    if (status != ROTMOS_OK)
        return status;
    if (s->failed || s->done)
        return ROTMOS_AGAIN;
    used = __atomic_load_n(&s->queued, __ATOMIC_ACQUIRE);
    n = s->capacity - used;
    if (n > length)
        n = length;
    if (n == 0)
        return ROTMOS_AGAIN;
    /* Reserve before publishing bytes, so a simultaneous native read can
     * never subtract from an unpublished count. Roll back an unwritten suffix.
     */
    __atomic_add_fetch(&s->queued, n, __ATOMIC_ACQ_REL);
    do {
        written = write(s->pipe[1], bytes, n);
    } while (written < 0 && errno == EINTR);
    __atomic_sub_fetch(&s->queued, n - (written > 0 ? (size_t)written : 0),
                       __ATOMIC_ACQ_REL);
    if (written > 0) {
        *consumed = (size_t)written;
        return ROTMOS_OK;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return ROTMOS_AGAIN;
    return io_error(error, "writing upload queue");
}
static rotmos_status body_end(rotmos_request *request, void *data,
                              rotmos_error *error)
{
    struct reader_upload *s = request->codec_data;
    rotmos_status status;
    (void)data;
    s->body_end = 1;
    status = start(s, error);
    close_writer(s);
    if (status == ROTMOS_OK)
        publish(s);
    return status;
}
static void closed(rotmos_request *request,
                   const rotmos_request_outcome *outcome, void *data)
{
    struct reader_upload *s = request->codec_data;
    (void)outcome;
    (void)data;
    s->request = NULL;
    if (s->reader != NULL)
        __atomic_store_n(&s->reader->cancelled, 1u, __ATOMIC_RELEASE);
    close_writer(s);
    if (s->admission != NULL)
        rotmos_timer_cancel(s->admission);
    if (s->job != NULL)
        (void)rotmos_job_cancel(s->job, NULL);
    else
        stop_waker(s);
}
static rotmos_status headers(rotmos_request *request, rotmos_request_plan *plan,
                             void *data, rotmos_error *error)
{
    const struct reader_route *route = data;
    struct reader_upload *s;
    int stopped;
    rotmos_status status = rotmos_reply_new(&request->reply, error);
    if (status != ROTMOS_OK)
        return status;
    request->reply->owner = request;
    status = rotmos_router_hooks_before(request, &stopped, error);
    if (status != ROTMOS_OK)
        return status;
    if (stopped) {
        plan->disposition = ROTMOS_REQUEST_REPLY;
        return submit_reply(request, error);
    }
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "upload_reader",
                           "allocating upload context");
    s->request = request;
    s->config = route->config;
    s->references = 1;
    s->capacity = s->config.buffer_bytes != 0 ? s->config.buffer_bytes : 262144;
    s->pipe[0] = s->pipe[1] = -1;
    request->codec_data = s;
    request->codec_cleanup = unref;
    plan->body = s->config.body;
    plan->body.mode = ROTMOS_BODY_STREAM;
    plan->on_body = body;
    plan->on_body_end = body_end;
    plan->on_close = closed;
    plan->userdata = rotmos_request_userdata(request);
    return ROTMOS_OK;
}
void rotmos_upload_reader_config_init(rotmos_upload_reader_config *config)
{
    if (config != NULL) {
        memset(config, 0, sizeof(*config));
        rotmos_body_policy_init_upload(&config->body);
    }
}
static void release_route(void *data)
{
    struct reader_route *r = data;
    if (r->release_userdata != NULL)
        r->release_userdata(r->config.userdata);
    free(r);
}
rotmos_status
rotmos_upload_reader_add(rotmos_router *router, rotmos_route_group *group,
                         const rotmos_upload_reader_config *config,
                         void (*release_userdata)(void *), rotmos_route **out,
                         rotmos_error *error)
{
    struct reader_route *copy;
    rotmos_route_config route;
    rotmos_route *registered;
    rotmos_status status;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || config->handler == NULL ||
        (config->body.mode != ROTMOS_BODY_STREAMING_UPLOAD &&
         config->body.mode != ROTMOS_BODY_STREAM))
        return rotmos_fail(error, ROTMOS_INVALID, "upload_reader",
                           "provide stream policy and native handler");
    copy = malloc(sizeof(*copy));
    if (copy == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "upload_reader",
                           "copying upload declaration");
    copy->config = *config;
    copy->release_userdata = release_userdata;
    rotmos_route_config_init(&route);
    route.method = config->method;
    route.methods = config->methods;
    route.path = config->path;
    route.path_kind = config->path_kind;
    route.body = config->body;
    route.hooks = config->hooks;
    route.on_headers = headers;
    route.userdata = copy;
    status =
        rotmos_router_facade_route(router, group, &route, &registered, error);
    if (status != ROTMOS_OK)
        free(copy);
    else {
        registered->release_userdata = release_route;
        if (out != NULL)
            *out = registered;
    }
    return status;
}
rotmos_status
rotmos_router_upload_reader(rotmos_router *router,
                            const rotmos_upload_reader_config *config,
                            rotmos_route **out, rotmos_error *error)
{
    return rotmos_upload_reader_add(router, NULL, config, NULL, out, error);
}
rotmos_status
rotmos_route_group_upload_reader(rotmos_route_group *group,
                                 const rotmos_upload_reader_config *config,
                                 rotmos_route **out, rotmos_error *error)
{
    if (group == NULL) {
        if (out != NULL)
            *out = NULL;
        return rotmos_fail(error, ROTMOS_INVALID, "upload_reader",
                           "group is required");
    }
    return rotmos_upload_reader_add(NULL, group, config, NULL, out, error);
}

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
    knurl_upload_reader_config config;
    struct knurl_upload_reader_codec codec;
    void (*release_userdata)(void *);
};
struct reader_upload {
    knurl_upload_reader_config config;
    struct knurl_upload_reader_codec codec;
    void *codec_state;
    void *host_state;
    int host_acquired;
    /* Owner loop only; revoked before request close. */
    knurl_request *request;
    knurl_upload_metadata metadata;
    void *text;
    knurl_header *pairs;
    knurl_source *reader;
    knurl_reply *result;
    knurl_executor *pool;
    knurl_job *job;
    knurl_waker *waker;
    knurl_timer *admission;
    size_t references, queued, capacity;
    int pipe[2], started, body_end, done, failed, published;
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
        knurl_source_release(s->reader, KNURL_CANCELLED);
    knurl_reply_destroy(s->result, NULL);
    if (s->codec.release != NULL && s->codec_state != NULL)
        s->codec.release(s->codec_state);
    if (s->host_acquired)
        s->config.release(s->host_state);
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
static knurl_status io_error(knurl_error *error, const char *message)
{
    int code = errno;
    knurl_status status = knurl_fail(error, KNURL_IO, "upload_reader", message);
    if (error != NULL) {
        error->source = KNURL_ERROR_SYSTEM;
        error->native_code = code;
    }
    return status;
}
static void fail(struct reader_upload *s, knurl_status status)
{
    if (s->request == NULL || s->failed)
        return;
    s->failed = 1;
    s->request->impl->outcome.cause = status;
    if (s->codec.finish != NULL) {
        /* Parsed-route errors retain ordinary facade status mapping, including
         * early rejection before the sender has finished the upload. */
        if (!s->body_end)
            s->request->impl->response->impl->close_connection = 1;
        if (knurl_reply_status(s->request->impl->reply,
                               status == KNURL_INVALID         ? 400
                               : status == KNURL_NOT_SUPPORTED ? 501
                                                               : 500,
                               NULL) == KNURL_OK &&
            knurl_response_submit(s->request->impl->response,
                                  s->request->impl->reply, NULL) == KNURL_OK)
            return;
    }
    knurl_response_fail(s->request->impl->response, status);
}
static knurl_status submit_reply(knurl_request *request, knurl_error *error)
{
    knurl_status status = knurl_response_submit(request->impl->response,
                                                request->impl->reply, error);
    if (status == KNURL_OK && request->impl->in_headers) {
        request->impl->response->impl->complete_reply = 1;
        request->impl->response->impl->phase = KNURL_RESPONSE_FINISHING;
    }
    return status;
}
static void publish(struct reader_upload *s)
{
    knurl_reply *old;
    knurl_status status;
    knurl_error error;
    if (!s->done || !s->body_end || s->failed || s->published ||
        s->request == NULL)
        return;
    s->published = 1;
    old = s->request->impl->reply;
    s->request->impl->reply = s->result;
    s->result = NULL;
    s->request->impl->reply->impl->owner = s->request;
    old->impl->owner = NULL;
    knurl_reply_destroy(old, NULL);
    knurl_error_clear(&error);
    status = s->codec.finish != NULL
                 ? s->codec.finish(s->request, s->request->impl->reply,
                                   s->codec_state, &error)
                 : KNURL_OK;
    if (status == KNURL_OK)
        status = knurl_router_hooks_after(s->request, &error);
    if (status == KNURL_OK)
        status = submit_reply(s->request, NULL);
    if (status != KNURL_OK)
        fail(s, status);
}
static void waker_retired(void *data)
{
    struct reader_upload *s = data;
    s->waker = NULL;
    unref(s);
}
static void wake(knurl_loop *loop, void *data)
{
    struct reader_upload *s = data;
    (void)loop;
    if (s->request != NULL && !s->body_end && !s->failed)
        (void)knurl_request_body_resume(s->request, NULL);
}
static void stop_waker(struct reader_upload *s)
{
    if (s->waker != NULL)
        (void)knurl_waker_destroy(s->waker, NULL);
}
static knurl_status read_pipe(void *buffer, size_t capacity, size_t *produced,
                              void *data, knurl_error *error)
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
        if (knurl_source_cancelled(s->reader))
            return KNURL_CANCELLED;
        do {
            n = read(s->pipe[0], buffer, capacity);
        } while (n < 0 && errno == EINTR);
        if (n > 0) {
            __atomic_sub_fetch(&s->queued, (size_t)n, __ATOMIC_ACQ_REL);
            *produced = (size_t)n;
            (void)knurl_waker_signal(s->waker, NULL);
            return KNURL_OK;
        }
        if (n == 0)
            return KNURL_EOF;
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
static knurl_status work(knurl_job *job, knurl_bytes arguments, void *data,
                         knurl_error *error)
{
    struct reader_upload *s = data;
    knurl_status status;
    int complete;
    (void)arguments;
    s->reader->impl->native_job = job;
    status = knurl_source_cancelled(s->reader)
                 ? KNURL_CANCELLED
                 : s->config.handler(&s->metadata, s->reader, s->result,
                                     s->codec.open != NULL ? s->codec_state
                                     : s->host_acquired    ? s->host_state
                                                           : s->config.userdata,
                                     error);
    complete =
        s->reader->impl->eof ||
        (s->metadata.has_content_length &&
         s->reader->impl->position.high == s->metadata.content_length.high &&
         s->reader->impl->position.low == s->metadata.content_length.low) ||
        (!s->metadata.has_content_length &&
         s->reader->impl->position.high == 0 &&
         s->reader->impl->position.low == 0);
    if (status == KNURL_OK && s->reader->impl->failure != KNURL_OK)
        status = s->reader->impl->failure;
    if (status == KNURL_OK && !complete)
        status = knurl_fail(error, KNURL_STATE, "upload_reader",
                            "handler must consume admitted input");
    s->reader->impl->native_job = NULL;
    return status;
}
static void retired(const knurl_job_outcome *outcome, void *data)
{
    struct reader_upload *s = data;
    knurl_status status =
        outcome->cancelled ? outcome->cause : outcome->work_status;
    s->job = NULL;
    s->done = 1;
    stop_waker(s);
    if (s->request != NULL) {
        if (status != KNURL_OK)
            fail(s, status);
        else
            publish(s);
    }
    unref(s);
}
static knurl_status submit(struct reader_upload *s, knurl_error *error)
{
    knurl_job_config job;
    knurl_status status;
    knurl_job_config_init(&job);
    job.work = work;
    job.retired = retired;
    job.userdata = s;
    job.timeout_disabled = 1;
    status = knurl_executor_submit(s->pool, &job, &s->job, error);
    if (status == KNURL_OK)
        ++s->references;
    return status;
}
static void admission_retired(void *data)
{
    struct reader_upload *s = data;
    s->admission = NULL;
    unref(s);
}
static void admission(knurl_timer *timer, void *data)
{
    struct reader_upload *s = data;
    knurl_status status;
    if (s->request == NULL) {
        knurl_timer_cancel(timer);
        return;
    }
    status = submit(s, NULL);
    if (status == KNURL_AGAIN)
        return;
    knurl_timer_cancel(timer);
    if (status != KNURL_OK) {
        fail(s, status);
        stop_waker(s);
    }
}
static knurl_status start(struct reader_upload *s, knurl_error *error)
{
    knurl_source_config config;
    knurl_status status;
    size_t i;
    int flags;
    if (s->started)
        return KNURL_OK;
    s->started = 1;
    if (s->codec.open != NULL) {
        status = s->codec.open(s->request, s->config.userdata, &s->codec_state,
                               error);
        if (status != KNURL_OK)
            return status;
    }
    status =
        knurl_native_io_pool(knurl_request_server(s->request), &s->pool, error);
    if (status != KNURL_OK)
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
    status = knurl_upload_metadata_copy(s->request, &s->metadata, &s->text,
                                        &s->pairs, error);
    if (status != KNURL_OK)
        return status;
    if (s->config.acquire != NULL) {
        status = s->config.acquire(s->request, &s->metadata, s->config.userdata,
                                   &s->host_state, error);
        if (status == KNURL_AGAIN || status == KNURL_EOF)
            return knurl_fail(error, KNURL_INVALID, "upload_reader",
                              "acquire must return OK or a terminal failure");
        if (status != KNURL_OK)
            return status;
        s->host_acquired = 1;
    }
    knurl_source_config_init(&config);
    config.read = read_pipe;
    config.userdata = s;
    config.has_length = s->metadata.has_content_length;
    config.length = s->metadata.content_length;
    status = knurl_source_new(&config, &s->reader, error);
    if (status != KNURL_OK)
        return status;
    s->reader->impl->borrowed = 1;
    status = knurl_reply_new(&s->result, error);
    if (status != KNURL_OK)
        return status;
    for (i = 0; i < s->request->impl->reply->impl->header_count; ++i) {
        knurl_header *h = &s->request->impl->reply->impl->headers[i];
        status = knurl_reply_header_add(s->result, h->name.data, h->value.data,
                                        error);
        if (status != KNURL_OK)
            return status;
    }
    status = knurl_waker_new(knurl_request_loop(s->request), wake,
                             waker_retired, s, &s->waker, error);
    if (status != KNURL_OK)
        return status;
    ++s->references;
    status = submit(s, error);
    if (status != KNURL_AGAIN)
        return status;
    status = knurl_loop_timer(knurl_request_loop(s->request), 1, 1, admission,
                              admission_retired, s, &s->admission, error);
    if (status == KNURL_OK)
        ++s->references;
    return status;
}
static knurl_status body(knurl_request *request, const void *bytes,
                         size_t length, size_t *consumed, void *data,
                         knurl_error *error)
{
    struct reader_upload *s = request->impl->codec_data;
    size_t used, n;
    ssize_t written;
    knurl_status status;
    (void)data;
    *consumed = 0;
    status = start(s, error);
    if (status != KNURL_OK)
        return status;
    if (s->failed || s->done)
        return KNURL_AGAIN;
    used = __atomic_load_n(&s->queued, __ATOMIC_ACQUIRE);
    n = s->capacity - used;
    if (n > length)
        n = length;
    if (n == 0)
        return KNURL_AGAIN;
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
        return KNURL_OK;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return KNURL_AGAIN;
    return io_error(error, "writing upload queue");
}
static knurl_status body_end(knurl_request *request, void *data,
                             knurl_error *error)
{
    struct reader_upload *s = request->impl->codec_data;
    knurl_status status;
    (void)data;
    s->body_end = 1;
    status = start(s, error);
    close_writer(s);
    if (status == KNURL_OK)
        publish(s);
    return status;
}
static void closed(knurl_request *request, const knurl_request_outcome *outcome,
                   void *data)
{
    struct reader_upload *s = request->impl->codec_data;
    (void)outcome;
    (void)data;
    s->request = NULL;
    if (s->reader != NULL)
        __atomic_store_n(&s->reader->impl->cancelled, 1u, __ATOMIC_RELEASE);
    close_writer(s);
    if (s->admission != NULL)
        knurl_timer_cancel(s->admission);
    if (s->job != NULL)
        (void)knurl_job_cancel(s->job, NULL);
    else
        stop_waker(s);
}
static knurl_status headers(knurl_request *request, knurl_request_plan *plan,
                            void *data, knurl_error *error)
{
    const struct reader_route *route = data;
    struct reader_upload *s;
    int stopped;
    knurl_status status = knurl_reply_new(&request->impl->reply, error);
    if (status != KNURL_OK)
        return status;
    request->impl->reply->impl->owner = request;
    status = knurl_router_hooks_before(request, &stopped, error);
    if (status != KNURL_OK)
        return status;
    if (stopped) {
        plan->disposition = KNURL_REQUEST_REPLY;
        return submit_reply(request, error);
    }
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return knurl_fail(error, KNURL_NOMEM, "upload_reader",
                          "allocating upload context");
    s->request = request;
    s->config = route->config;
    s->codec = route->codec;
    s->references = 1;
    s->capacity = s->config.buffer_bytes != 0 ? s->config.buffer_bytes : 262144;
    s->pipe[0] = s->pipe[1] = -1;
    request->impl->codec_data = s;
    request->impl->codec_cleanup = unref;
    plan->body = s->config.body;
    plan->body.mode = KNURL_BODY_STREAM;
    plan->on_body = body;
    plan->on_body_end = body_end;
    plan->on_close = closed;
    plan->userdata = knurl_request_userdata(request);
    return KNURL_OK;
}
void knurl_upload_reader_config_init(knurl_upload_reader_config *config)
{
    if (config != NULL) {
        memset(config, 0, sizeof(*config));
        knurl_body_policy_init_upload(&config->body);
    }
}
static void release_route(void *data)
{
    struct reader_route *r = data;
    if (r->release_userdata != NULL)
        r->release_userdata(r->config.userdata);
    free(r);
}
knurl_status
knurl_upload_reader_add(knurl_router *router, knurl_route_group *group,
                        const knurl_upload_reader_config *config,
                        const struct knurl_upload_reader_codec *codec,
                        void (*release_userdata)(void *), knurl_route **out,
                        knurl_error *error)
{
    struct reader_route *copy;
    knurl_route_config route;
    knurl_route *registered;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (config != NULL &&
        ((config->acquire == NULL) != (config->release == NULL)))
        return knurl_fail(error, KNURL_INVALID, "upload_reader",
                          "provide both acquire and release callbacks");
    if (config == NULL || config->handler == NULL ||
        (config->body.mode != KNURL_BODY_STREAMING_UPLOAD &&
         config->body.mode != KNURL_BODY_STREAM))
        return knurl_fail(error, KNURL_INVALID, "upload_reader",
                          "provide stream policy and native handler");
    copy = malloc(sizeof(*copy));
    if (copy == NULL)
        return knurl_fail(error, KNURL_NOMEM, "upload_reader",
                          "copying upload declaration");
    copy->config = *config;
    memset(&copy->codec, 0, sizeof(copy->codec));
    if (codec != NULL)
        copy->codec = *codec;
    copy->release_userdata = release_userdata;
    knurl_route_config_init(&route);
    route.method = config->method;
    route.methods = config->methods;
    route.path = config->path;
    route.path_kind = config->path_kind;
    route.body = config->body;
    route.hooks = config->hooks;
    route.on_headers = headers;
    route.userdata = copy;
    status =
        knurl_router_facade_route(router, group, &route, &registered, error);
    if (status != KNURL_OK)
        free(copy);
    else {
        registered->impl->release_userdata = release_route;
        if (out != NULL)
            *out = registered;
    }
    return status;
}
knurl_status
knurl_router_upload_reader(knurl_router *router,
                           const knurl_upload_reader_config *config,
                           knurl_route **out, knurl_error *error)
{
    return knurl_upload_reader_add(router, NULL, config, NULL, NULL, out,
                                   error);
}
knurl_status
knurl_route_group_upload_reader(knurl_route_group *group,
                                const knurl_upload_reader_config *config,
                                knurl_route **out, knurl_error *error)
{
    if (group == NULL) {
        if (out != NULL)
            *out = NULL;
        return knurl_fail(error, KNURL_INVALID, "upload_reader",
                          "group is required");
    }
    return knurl_upload_reader_add(NULL, group, config, NULL, NULL, out, error);
}

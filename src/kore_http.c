#define _POSIX_C_SOURCE 200809L
#include "kore_internal.h"
#include "reply_internal.h"
#include "response_internal.h"
#include "body_internal.h"
#include "channel_internal.h"
#include "policy_internal.h"
#include <rotmos/kore.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char *rotmos_kore_revision(const rotmos_server *server)
{
    return server != NULL ? ROTMOS_KORE_REVISION : NULL;
}

struct http_request *rotmos_kore_request(rotmos_request *request)
{
    return request != NULL ? request->native : NULL;
}

static rotmos_http_method method(int native)
{
    switch (native) {
    case HTTP_METHOD_GET:
        return ROTMOS_HTTP_GET;
    case HTTP_METHOD_POST:
        return ROTMOS_HTTP_POST;
    case HTTP_METHOD_PUT:
        return ROTMOS_HTTP_PUT;
    case HTTP_METHOD_PATCH:
        return ROTMOS_HTTP_PATCH;
    case HTTP_METHOD_DELETE:
        return ROTMOS_HTTP_DELETE;
    case HTTP_METHOD_HEAD:
        return ROTMOS_HTTP_HEAD;
    case HTTP_METHOD_OPTIONS:
        return ROTMOS_HTTP_OPTIONS;
    case HTTP_METHOD_PROPFIND:
        return ROTMOS_HTTP_PROPFIND;
    case HTTP_METHOD_MKCOL:
        return ROTMOS_HTTP_MKCOL;
    case HTTP_METHOD_COPY:
        return ROTMOS_HTTP_COPY;
    case HTTP_METHOD_MOVE:
        return ROTMOS_HTTP_MOVE;
    }
    return ROTMOS_HTTP_ANY;
}

void rotmos_kore_endpoint(int fd, int peer, rotmos_endpoint *out, char *buffer)
{
    struct sockaddr_storage storage;
    struct sockaddr_in *ipv4;
    struct sockaddr_in6 *ipv6;
    socklen_t length;
    const void *address;
    length = sizeof(storage);
    if ((peer ? getpeername(fd, (struct sockaddr *)&storage, &length)
              : getsockname(fd, (struct sockaddr *)&storage, &length)) != 0)
        return;
    if (storage.ss_family == AF_INET) {
        ipv4 = (struct sockaddr_in *)&storage;
        address = &ipv4->sin_addr;
        out->port = ntohs(ipv4->sin_port);
    } else if (storage.ss_family == AF_INET6) {
        ipv6 = (struct sockaddr_in6 *)&storage;
        address = &ipv6->sin6_addr;
        out->port = ntohs(ipv6->sin6_port);
    } else {
        return;
    }
    if (inet_ntop(storage.ss_family, address, buffer, 46) != NULL)
        out->address = rotmos_view(buffer);
}

static int reject(struct http_request *native, int status)
{
    native->owner->flags |= CONN_CLOSE_EMPTY;
    native->flags |= HTTP_REQUEST_DELETE;
    http_response_close(native, status, NULL, 0);
    return KORE_RESULT_ERROR;
}

static rotmos_status metadata(struct rotmos_runtime *runtime,
                              struct http_request *native,
                              rotmos_request *request, rotmos_error *error)
{
    struct http_header *header;
    size_t i;
    request->native = native;
    request->server = runtime->server;
    request->method = method(native->method);
    request->method_token = rotmos_view(native->raw_method);
    request->version = rotmos_view(
        (native->flags & HTTP_VERSION_1_0) ? "HTTP/1.0" : "HTTP/1.1");
    request->target = rotmos_view(native->raw_target);
    request->raw_path = rotmos_view(native->path);
    request->query = rotmos_view(native->query_string);
    request->authority = rotmos_view(native->host);
    request->tls = native->owner->tls != 0;
    for (i = 0; i < runtime->bound_count; ++i) {
        if (native->owner->owner->server == runtime->native_servers[i]) {
            request->listener = i;
            break;
        }
    }
    rotmos_kore_endpoint(native->owner->fd, 1, &request->peer,
                         request->peer_address);
    rotmos_kore_endpoint(native->owner->fd, 0, &request->local,
                         request->local_address);
    TAILQ_FOREACH(header, &native->req_headers, list)++ request->header_count;
    if (request->header_count != 0) {
        request->headers =
            malloc(request->header_count * sizeof(*request->headers));
        if (request->headers == NULL)
            return rotmos_fail(error, ROTMOS_NOMEM, "request_headers",
                               "cannot allocate header views");
    }
    i = 0;
    TAILQ_FOREACH(header, &native->req_headers, list)
    {
        request->headers[i].name = rotmos_view(header->header);
        request->headers[i++].value = rotmos_view(header->value);
    }
    /* Redirects preserve the raw target, including encoded path separators;
     * routing decoders must not reject or rewrite that URI. */
    if (runtime->server->config.listeners[request->listener].kind ==
        ROTMOS_LISTENER_REDIRECT)
        return ROTMOS_OK;
    return rotmos_request_decode(request, error);
}

static rotmos_status admit(rotmos_request *request, rotmos_error *error)
{
    rotmos_body_policy *policy;
    rotmos_u64 hard;
    int order;
    policy = &request->plan.body;
    if (request->plan.disposition == ROTMOS_REQUEST_WEBSOCKET &&
        request->websocket != NULL && request->channel != NULL)
        return ROTMOS_OK;
    if (request->plan.disposition == ROTMOS_REQUEST_TAKEOVER &&
        request->channel != NULL && request->websocket == NULL)
        return ROTMOS_OK;
    if (request->channel != NULL)
        return rotmos_fail(error, ROTMOS_INVALID, "request_plan",
                           "installed ownership requires its WEBSOCKET or "
                           "TAKEOVER disposition");
    if (request->plan.disposition != ROTMOS_REQUEST_MANAGED) {
        if (request->plan.disposition != ROTMOS_REQUEST_REPLY)
            return rotmos_fail(
                error, ROTMOS_NOT_SUPPORTED, "request_plan",
                "upgrade/takeover requires an installed connection capability");
        if (request->response->phase != ROTMOS_RESPONSE_FINISHING)
            return rotmos_fail(
                error, ROTMOS_INVALID, "request_plan",
                "REPLY requires a successfully ended final response");
        if (request->has_coding ||
            (request->has_length &&
             (request->content_length.high || request->content_length.low))) {
            rotmos_kore_body_cancel(request, ROTMOS_CANCELLED);
            request->response->close_connection = 1;
        } else {
            return rotmos_kore_body_empty_reply(request, error);
        }
        return ROTMOS_OK;
    }
    if (request->has_coding)
        return rotmos_fail(error, ROTMOS_NOT_SUPPORTED, "request_plan",
                           "chunked bodies require connection takeover");
    if (!request->has_length && (request->method == ROTMOS_HTTP_POST ||
                                 request->method == ROTMOS_HTTP_PUT ||
                                 request->method == ROTMOS_HTTP_PATCH)) {
        request->rejection_status = 411;
        return rotmos_fail(
            error, ROTMOS_INVALID, "request_plan",
            "managed POST, PUT and PATCH require Content-Length");
    }
    if (request->plan.on_body_end == NULL && !request->response->producer &&
        request->response->phase == ROTMOS_RESPONSE_BUILDING)
        return rotmos_fail(
            error, ROTMOS_INVALID, "request_plan",
            "MANAGED requires body completion or an installed response owner");
    if ((policy->mode == ROTMOS_BODY_STREAM && request->plan.on_body == NULL) ||
        (policy->mode != ROTMOS_BODY_STREAM && request->plan.on_body != NULL))
        return rotmos_fail(error, ROTMOS_INVALID, "request_plan",
                           "on_body belongs exclusively to STREAM delivery");
    if (policy->mode == ROTMOS_BODY_JSON ||
        policy->mode == ROTMOS_BODY_STREAMING_UPLOAD)
        return rotmos_fail(
            error, ROTMOS_NOT_SUPPORTED, "request_plan",
            "typed JSON and reader-upload facades are not connected yet");
    if (policy->max_bytes.high == 0 && policy->max_bytes.low == 0)
        policy->max_bytes = request->server->config.limits.default_body_bytes;
    hard = request->server->config.limits.hard_body_bytes;
    if (!rotmos_u64_valid(policy->max_bytes))
        return rotmos_fail(error, ROTMOS_INVALID, "request_plan",
                           "invalid body limit words");
    if (hard.high || hard.low) {
        (void)rotmos_u64_compare(policy->max_bytes, hard, &order, NULL);
        if (order > 0)
            return rotmos_fail(
                error, ROTMOS_INVALID, "request_plan",
                "plan body limit exceeds the configured hard ceiling");
    }
    if (request->has_length) {
        (void)rotmos_u64_compare(request->content_length, policy->max_bytes,
                                 &order, NULL);
        if (order > 0 ||
            (policy->mode == ROTMOS_BODY_NONE &&
             (request->content_length.high || request->content_length.low))) {
            request->rejection_status = 413;
            return rotmos_fail(error, ROTMOS_LIMIT, "request_plan",
                               "declared body exceeds the admitted limit");
        }
        if ((request->content_length.high || request->content_length.low) &&
            request->method != ROTMOS_HTTP_POST &&
            request->method != ROTMOS_HTTP_PUT &&
            request->method != ROTMOS_HTTP_PATCH &&
            request->method != ROTMOS_HTTP_DELETE &&
            request->method != ROTMOS_HTTP_PROPFIND &&
            request->method != ROTMOS_HTTP_MKCOL)
            return rotmos_fail(
                error, ROTMOS_NOT_SUPPORTED, "request_plan",
                "this managed method does not accept a positive body length");
    }
    return rotmos_body_create(
        policy, request->server->config.limits.body_spool_directory,
        &request->body_store, error);
}

int rotmos_kore_prebody(struct http_request *native, const void *data,
                        size_t length)
{
    struct rotmos_runtime *runtime;
    rotmos_request *request;
    rotmos_request_plan plan;
    rotmos_error error;
    rotmos_bytes value;
    rotmos_status status;
    size_t i;
    int hosts;
    int lengths;
    int coding;
    int expect, handled;
    runtime = kore_library_hooks.arg;
    if (!__atomic_load_n(&runtime->shared->serving, __ATOMIC_ACQUIRE))
        return reject(native, 503);
    runtime->server->state = ROTMOS_SERVER_SERVING;
    request = calloc(1, sizeof(*request));
    if (request == NULL)
        return reject(native, 500);
    native->hdlr_extra = request;
    status = metadata(runtime, native, request, &error);
    if (status != ROTMOS_OK)
        return reject(native, status == ROTMOS_INVALID ? 400 : 500);
    hosts = lengths = coding = expect = 0;
    for (i = 0; i < request->header_count; ++i) {
        if (rotmos_text_equal_case(request->headers[i].name.data, "host"))
            ++hosts;
        if (rotmos_text_equal_case(request->headers[i].name.data,
                                   "content-length"))
            ++lengths;
        if (rotmos_text_equal_case(request->headers[i].name.data,
                                   "transfer-encoding"))
            ++coding;
        if (rotmos_text_equal_case(request->headers[i].name.data, "expect"))
            ++expect;
    }
    if (hosts != 1 || lengths > 1 || coding > 1 || (coding && lengths))
        return reject(native, 400);
    if (coding) {
        (void)rotmos_request_header_first(request, "transfer-encoding", &value,
                                          NULL);
        if (!rotmos_text_equal_case(value.data, "chunked"))
            return reject(native, 501);
        request->has_coding = 1;
    }
    if (lengths) {
        (void)rotmos_request_header_first(request, "content-length", &value,
                                          NULL);
        if (rotmos_u64_from_decimal(value.data, value.length,
                                    &request->content_length,
                                    &error) != ROTMOS_OK)
            return reject(native, 400);
        request->has_length = 1;
    }
    if (expect) {
        (void)rotmos_request_header_first(request, "expect", &value, NULL);
        if (expect > 1 || !rotmos_text_equal_case(value.data, "100-continue") ||
            memcmp(request->version.data, "HTTP/1.1", 8) != 0)
            return reject(native, 417);
    }
    status = rotmos_kore_response_create(runtime, request, &error);
    if (status != ROTMOS_OK)
        return reject(native, 500);
    status = rotmos_policy_start(request, &error);
    if (status == ROTMOS_OK)
        status = rotmos_policy_admit(request, &handled, &error);
    if (status != ROTMOS_OK) {
        rotmos_kore_request_fail(request, status, 500);
        return KORE_RESULT_RETRY;
    }
    if (handled)
        return KORE_RESULT_RETRY;
    if (runtime->server->config.listeners[request->listener].kind ==
        ROTMOS_LISTENER_REDIRECT)
        return rotmos_kore_redirect(request);
    status = rotmos_kore_body_create(request, data, length, &error);
    if (status != ROTMOS_OK) {
        rotmos_kore_request_fail(request, status, 500);
        return KORE_RESULT_RETRY;
    }
    rotmos_request_plan_init(&plan);
    plan.userdata = request->userdata;
    request->dispatch_started = 1;
    request->in_headers = 1;
    if (runtime->server->config.dispatch.mode == ROTMOS_DISPATCH_CALLBACK)
        status = runtime->server->config.dispatch.on_headers(
            request, &plan, runtime->server->config.dispatch.userdata, &error);
    else
        status = rotmos_router_dispatch_headers(runtime->server->router,
                                                request, &plan, &error);
    request->plan = plan;
    request->in_headers = 0;
    if (status == ROTMOS_OK)
        status = admit(request, &error);
    if (status != ROTMOS_OK) {
        rotmos_channel_finish(request->channel,
                              status > ROTMOS_EOF && status <= ROTMOS_BACKEND
                                  ? status
                                  : ROTMOS_INVALID);
        rotmos_kore_request_fail(
            request, status,
            request->rejection_status
                ? request->rejection_status
                : (status == ROTMOS_NOT_SUPPORTED ? 501 : 500));
        return KORE_RESULT_RETRY;
    }
    if (expect && plan.disposition == ROTMOS_REQUEST_MANAGED &&
        request->has_length &&
        (request->content_length.high || request->content_length.low) &&
        request->response->phase == ROTMOS_RESPONSE_BUILDING) {
        status =
            rotmos_response_interim(request->response, 100, NULL, 0, &error);
        if (status != ROTMOS_OK)
            rotmos_kore_request_fail(request, status, 500);
        else
            rotmos_kore_http_status(runtime, native, 100);
    }
    request->response->schedule(request->response);
    return KORE_RESULT_RETRY;
}

int rotmos_kore_dispatch(struct http_request *native)
{
    /* Public managed requests are scheduled from the prebody handoff. */
    (void)native;
    return KORE_RESULT_RETRY;
}

void rotmos_kore_request_free(struct http_request *native)
{
    rotmos_request *request;
    request = native->hdlr_extra;
    native->hdlr_extra = NULL;
    if (request != NULL) {
        rotmos_request_cleanup(request);
        request->native = NULL;
        free(request);
    }
}

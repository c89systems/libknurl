#define _POSIX_C_SOURCE 200809L
#include "kore_internal.h"
#include "reply_internal.h"
#include "response_internal.h"
#include "body_internal.h"
#include "channel_internal.h"
#include "policy_internal.h"
#include <knurl/kore.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char *knurl_kore_revision(const knurl_server *server)
{
    return server != NULL ? KNURL_KORE_REVISION : NULL;
}

struct http_request *knurl_kore_request(knurl_request *request)
{
    return request != NULL ? request->impl->native : NULL;
}

static knurl_http_method method(int native)
{
    switch (native) {
    case HTTP_METHOD_GET:
        return KNURL_HTTP_GET;
    case HTTP_METHOD_POST:
        return KNURL_HTTP_POST;
    case HTTP_METHOD_PUT:
        return KNURL_HTTP_PUT;
    case HTTP_METHOD_PATCH:
        return KNURL_HTTP_PATCH;
    case HTTP_METHOD_DELETE:
        return KNURL_HTTP_DELETE;
    case HTTP_METHOD_HEAD:
        return KNURL_HTTP_HEAD;
    case HTTP_METHOD_OPTIONS:
        return KNURL_HTTP_OPTIONS;
    case HTTP_METHOD_PROPFIND:
        return KNURL_HTTP_PROPFIND;
    case HTTP_METHOD_MKCOL:
        return KNURL_HTTP_MKCOL;
    case HTTP_METHOD_COPY:
        return KNURL_HTTP_COPY;
    case HTTP_METHOD_MOVE:
        return KNURL_HTTP_MOVE;
    }
    return KNURL_HTTP_ANY;
}

void knurl_kore_endpoint(int fd, int peer, knurl_endpoint *out, char *buffer)
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
        out->address = knurl_view(buffer);
}

static int reject(struct http_request *native, int status)
{
    native->owner->flags |= CONN_CLOSE_EMPTY;
    native->flags |= HTTP_REQUEST_DELETE;
    http_response_close(native, status, NULL, 0);
    return KORE_RESULT_ERROR;
}

static knurl_status metadata(struct knurl_runtime *runtime,
                             struct http_request *native,
                             knurl_request *request, knurl_error *error)
{
    struct http_header *header;
    size_t i;
    request->impl->native = native;
    request->impl->server = runtime->server;
    request->impl->method = method(native->method);
    request->impl->method_token = knurl_view(native->raw_method);
    request->impl->version = knurl_view(
        (native->flags & HTTP_VERSION_1_0) ? "HTTP/1.0" : "HTTP/1.1");
    request->impl->target = knurl_view(native->raw_target);
    request->impl->raw_path = knurl_view(native->path);
    request->impl->query = knurl_view(native->query_string);
    request->impl->authority = knurl_view(native->host);
    request->impl->tls = native->owner->tls != 0;
    for (i = 0; i < runtime->bound_count; ++i) {
        if (native->owner->owner->server == runtime->native_servers[i]) {
            request->impl->listener = i;
            break;
        }
    }
    knurl_kore_endpoint(native->owner->fd, 1, &request->impl->peer,
                        request->impl->peer_address);
    knurl_kore_endpoint(native->owner->fd, 0, &request->impl->local,
                        request->impl->local_address);
    TAILQ_FOREACH(header, &native->req_headers, list)
    ++request->impl->header_count;
    if (request->impl->header_count != 0) {
        request->impl->headers = malloc(request->impl->header_count *
                                        sizeof(*request->impl->headers));
        if (request->impl->headers == NULL)
            return knurl_fail(error, KNURL_NOMEM, "request_headers",
                              "cannot allocate header views");
    }
    i = 0;
    TAILQ_FOREACH(header, &native->req_headers, list)
    {
        request->impl->headers[i].name = knurl_view(header->header);
        request->impl->headers[i++].value = knurl_view(header->value);
    }
    /* Redirects preserve the raw target, including encoded path separators;
     * routing decoders must not reject or rewrite that URI. */
    if (runtime->server->impl->config.listeners[request->impl->listener].kind ==
        KNURL_LISTENER_REDIRECT)
        return KNURL_OK;
    return knurl_request_decode(request, error);
}

static knurl_status admit(knurl_request *request, knurl_error *error)
{
    knurl_body_policy *policy;
    knurl_u64 hard;
    int order;
    policy = &request->impl->plan.body;
    if (request->impl->plan.disposition == KNURL_REQUEST_WEBSOCKET &&
        request->impl->websocket != NULL && request->impl->channel != NULL)
        return KNURL_OK;
    if (request->impl->plan.disposition == KNURL_REQUEST_TAKEOVER &&
        request->impl->channel != NULL && request->impl->websocket == NULL)
        return KNURL_OK;
    if (request->impl->channel != NULL)
        return knurl_fail(error, KNURL_INVALID, "request_plan",
                          "installed ownership requires its WEBSOCKET or "
                          "TAKEOVER disposition");
    if (request->impl->plan.disposition != KNURL_REQUEST_MANAGED) {
        if (request->impl->plan.disposition != KNURL_REQUEST_REPLY)
            return knurl_fail(
                error, KNURL_NOT_SUPPORTED, "request_plan",
                "upgrade/takeover requires an installed connection capability");
        if (request->impl->response->impl->phase != KNURL_RESPONSE_FINISHING)
            return knurl_fail(
                error, KNURL_INVALID, "request_plan",
                "REPLY requires a successfully ended final response");
        if (request->impl->has_coding ||
            (request->impl->has_length &&
             (request->impl->content_length.high ||
              request->impl->content_length.low))) {
            knurl_kore_body_cancel(request, KNURL_CANCELLED);
            request->impl->response->impl->close_connection = 1;
        } else {
            return knurl_kore_body_empty_reply(request, error);
        }
        return KNURL_OK;
    }
    if (!request->impl->has_length && !request->impl->has_coding &&
        (request->impl->method == KNURL_HTTP_POST ||
         request->impl->method == KNURL_HTTP_PUT ||
         request->impl->method == KNURL_HTTP_PATCH)) {
        request->impl->rejection_status = 411;
        return knurl_fail(error, KNURL_INVALID, "request_plan",
                          "managed POST, PUT and PATCH require Content-Length "
                          "or chunked framing");
    }
    if (request->impl->plan.on_body_end == NULL &&
        !request->impl->response->impl->producer &&
        request->impl->response->impl->phase == KNURL_RESPONSE_BUILDING)
        return knurl_fail(
            error, KNURL_INVALID, "request_plan",
            "MANAGED requires body completion or an installed response owner");
    if ((policy->mode == KNURL_BODY_STREAM &&
         request->impl->plan.on_body == NULL) ||
        (policy->mode != KNURL_BODY_STREAM &&
         request->impl->plan.on_body != NULL))
        return knurl_fail(error, KNURL_INVALID, "request_plan",
                          "on_body belongs exclusively to STREAM delivery");
    policy->max_bytes = knurl_body_policy_limit(
        policy, request->impl->server->impl->config.limits.default_body_bytes);
    hard = request->impl->server->impl->config.limits.hard_body_bytes;
    if (!knurl_u64_valid(policy->max_bytes))
        return knurl_fail(error, KNURL_INVALID, "request_plan",
                          "invalid body limit words");
    /* NONE never admits payload bytes; its inherited budget is unused. */
    if (policy->mode != KNURL_BODY_NONE && (hard.high || hard.low)) {
        (void)knurl_u64_compare(policy->max_bytes, hard, &order, NULL);
        if (order > 0)
            return knurl_fail(
                error, KNURL_INVALID, "request_plan",
                "plan body limit exceeds the configured hard ceiling");
    }
    if (request->impl->has_coding && policy->mode == KNURL_BODY_NONE) {
        request->impl->rejection_status = 413;
        return knurl_fail(error, KNURL_LIMIT, "request_plan",
                          "request body is not allowed for this route");
    }
    if (request->impl->has_coding &&
        !(KNURL_KORE_BODY_METHODS &
          KNURL_HTTP_METHOD_MASK(request->impl->method)))
        return knurl_fail(error, KNURL_NOT_SUPPORTED, "request_plan",
                          "this managed method does not accept chunked input");
    if (request->impl->has_length) {
        (void)knurl_u64_compare(request->impl->content_length,
                                policy->max_bytes, &order, NULL);
        if (policy->mode == KNURL_BODY_NONE &&
            (request->impl->content_length.high ||
             request->impl->content_length.low)) {
            request->impl->rejection_status = 413;
            return knurl_fail(error, KNURL_LIMIT, "request_plan",
                              "request body is not allowed for this route");
        }
        if (order > 0) {
            request->impl->rejection_status = 413;
            return knurl_fail(error, KNURL_LIMIT, "request_plan",
                              "request body exceeds route limit");
        }
        if ((request->impl->content_length.high ||
             request->impl->content_length.low) &&
            !(KNURL_KORE_BODY_METHODS &
              KNURL_HTTP_METHOD_MASK(request->impl->method)))
            return knurl_fail(
                error, KNURL_NOT_SUPPORTED, "request_plan",
                "this managed method does not accept a positive body length");
    }
    return knurl_body_create(
        policy, request->impl->server->impl->config.limits.body_spool_directory,
        &request->impl->body_store, error);
}

int knurl_kore_prebody(struct http_request *native, const void *data,
                       size_t length)
{
    struct knurl_runtime *runtime;
    knurl_request *request;
    knurl_request_plan plan;
    knurl_error error;
    knurl_bytes value;
    knurl_status status;
    size_t i;
    int hosts;
    int lengths;
    int coding;
    int expect, handled;
    runtime = kore_library_hooks.arg;
    if (!__atomic_load_n(&runtime->shared->serving, __ATOMIC_ACQUIRE))
        return reject(native, 503);
    runtime->server->impl->state = KNURL_SERVER_SERVING;
    request = calloc(1, sizeof(struct knurl_request_impl));
    if (request == NULL)
        return reject(native, 500);
    knurl_request_receiver_init(request);
    native->hdlr_extra = request;
    status = metadata(runtime, native, request, &error);
    if (status != KNURL_OK)
        return reject(native, status == KNURL_INVALID ? 400 : 500);
    hosts = lengths = coding = expect = 0;
    for (i = 0; i < request->impl->header_count; ++i) {
        if (knurl_text_equal_case(request->impl->headers[i].name.data, "host"))
            ++hosts;
        if (knurl_text_equal_case(request->impl->headers[i].name.data,
                                  "content-length"))
            ++lengths;
        if (knurl_text_equal_case(request->impl->headers[i].name.data,
                                  "transfer-encoding"))
            ++coding;
        if (knurl_text_equal_case(request->impl->headers[i].name.data,
                                  "expect"))
            ++expect;
    }
    if (hosts != 1 || lengths > 1 || coding > 1 || (coding && lengths))
        return reject(native, 400);
    if (coding) {
        if (memcmp(request->impl->version.data, "HTTP/1.1", 8) != 0)
            return reject(native, 400);
        (void)knurl_request_header_first(request, "transfer-encoding", &value,
                                         NULL);
        if (!knurl_text_equal_case(value.data, "chunked"))
            return reject(native, 501);
        request->impl->has_coding = 1;
    }
    if (lengths) {
        (void)knurl_request_header_first(request, "content-length", &value,
                                         NULL);
        if (knurl_u64_from_decimal(value.data, value.length,
                                   &request->impl->content_length,
                                   &error) != KNURL_OK)
            return reject(native, 400);
        request->impl->has_length = 1;
    }
    if (expect) {
        (void)knurl_request_header_first(request, "expect", &value, NULL);
        if (expect > 1 || !knurl_text_equal_case(value.data, "100-continue") ||
            memcmp(request->impl->version.data, "HTTP/1.1", 8) != 0)
            return reject(native, 417);
    }
    status = knurl_kore_response_create(runtime, request, &error);
    if (status != KNURL_OK)
        return reject(native, 500);
    status = knurl_policy_start(request, &error);
    if (status == KNURL_OK)
        status = knurl_policy_admit(request, &handled, &error);
    if (status != KNURL_OK) {
        knurl_kore_request_fail(request, status, 500);
        return KORE_RESULT_RETRY;
    }
    if (handled)
        return KORE_RESULT_RETRY;
    if (runtime->server->impl->config.listeners[request->impl->listener].kind ==
        KNURL_LISTENER_REDIRECT)
        return knurl_kore_redirect(request);
    status = knurl_kore_body_create(request, data, length, &error);
    if (status != KNURL_OK) {
        knurl_kore_request_fail(request, status, 500);
        return KORE_RESULT_RETRY;
    }
    knurl_request_plan_init(&plan);
    plan.userdata = request->impl->userdata;
    request->impl->dispatch_started = 1;
    request->impl->in_headers = 1;
    if (runtime->server->impl->config.dispatch.mode == KNURL_DISPATCH_CALLBACK)
        status = runtime->server->impl->config.dispatch.on_headers(
            request, &plan, runtime->server->impl->config.dispatch.userdata,
            &error);
    else
        status = knurl_router_dispatch_headers(runtime->server->impl->router,
                                               request, &plan, &error);
    request->impl->plan = plan;
    request->impl->in_headers = 0;
    if (status == KNURL_OK)
        status = admit(request, &error);
    if (status != KNURL_OK) {
        knurl_channel_finish(request->impl->channel,
                             status > KNURL_EOF && status <= KNURL_BACKEND
                                 ? status
                                 : KNURL_INVALID);
        knurl_kore_request_fail_message(
            request, status,
            request->impl->rejection_status
                ? request->impl->rejection_status
                : (status == KNURL_NOT_SUPPORTED ? 501 : 500),
            request->impl->rejection_status == 413 ? error.message : NULL);
        return KORE_RESULT_RETRY;
    }
    if (expect && plan.disposition == KNURL_REQUEST_MANAGED &&
        (request->impl->has_coding ||
         (request->impl->has_length && (request->impl->content_length.high ||
                                        request->impl->content_length.low))) &&
        request->impl->response->impl->phase == KNURL_RESPONSE_BUILDING) {
        status = knurl_response_interim(request->impl->response, 100, NULL, 0,
                                        &error);
        if (status != KNURL_OK)
            knurl_kore_request_fail(request, status, 500);
        else
            knurl_kore_http_status(runtime, native, 100);
    }
    request->impl->response->impl->schedule(request->impl->response);
    return KORE_RESULT_RETRY;
}

int knurl_kore_dispatch(struct http_request *native)
{
    /* Public managed requests are scheduled from the prebody handoff. */
    (void)native;
    return KORE_RESULT_RETRY;
}

void knurl_kore_request_free(struct http_request *native)
{
    knurl_request *request;
    request = native->hdlr_extra;
    native->hdlr_extra = NULL;
    if (request != NULL) {
        knurl_request_cleanup(request);
        request->impl->native = NULL;
        free(request);
    }
}

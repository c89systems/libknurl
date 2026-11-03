#define _POSIX_C_SOURCE 200809L
#include "kore_internal.h"
#include "reply_internal.h"
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

static void endpoint(int fd, int peer, rotmos_endpoint *out, char *buffer)
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
    endpoint(native->owner->fd, 1, &request->peer, request->peer_address);
    endpoint(native->owner->fd, 0, &request->local, request->local_address);
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
    return rotmos_request_decode(request, error);
}

int rotmos_kore_prebody(struct http_request *native, const void *data,
                        size_t length)
{
    struct rotmos_runtime *runtime;
    rotmos_request *request;
    rotmos_error error;
    rotmos_bytes value;
    rotmos_status status;
    rotmos_u64 limit;
    size_t maximum;
    size_t declared;
    size_t i;
    int hosts;
    int lengths;
    int coding;
    int order;
    (void)data;
    (void)length;
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
    hosts = lengths = coding = 0;
    for (i = 0; i < request->header_count; ++i) {
        if (rotmos_text_equal_case(request->headers[i].name.data, "host"))
            ++hosts;
        if (rotmos_text_equal_case(request->headers[i].name.data,
                                   "content-length"))
            ++lengths;
        if (rotmos_text_equal_case(request->headers[i].name.data,
                                   "transfer-encoding"))
            ++coding;
    }
    if (hosts != 1 || lengths > 1 || coding > 1 || (coding && lengths))
        return reject(native, 400);
    if (coding)
        return reject(native, 501);
    if (lengths) {
        (void)rotmos_request_header_first(request, "content-length", &value,
                                          NULL);
        status = rotmos_u64_from_decimal(value.data, value.length,
                                         &request->content_length, &error);
        if (status != ROTMOS_OK)
            return reject(native, 400);
        request->has_length = 1;
    }
    status = rotmos_router_select(runtime->server->router, request, &error);
    if (status != ROTMOS_OK)
        return reject(native, status == ROTMOS_EOF ? 404 : 500);
    limit = request->route->config.body.max_bytes;
    if (limit.high == 0 && limit.low == 0)
        limit = runtime->server->config.limits.default_body_bytes;
    if (request->has_length) {
        if (rotmos_u64_to_size(request->content_length, &declared, &error) !=
            ROTMOS_OK)
            return reject(native, 413);
        if (request->route->config.body.mode == ROTMOS_BODY_NONE &&
            declared != 0)
            return reject(native, 413);
        (void)rotmos_u64_compare(request->content_length, limit, &order, NULL);
        if (order > 0)
            return reject(native, 413);
        maximum = request->route->config.body.memory_buffer_limit_bytes;
        if (maximum == 0 &&
            rotmos_u64_to_size(limit, &maximum, &error) != ROTMOS_OK)
            return reject(native, 413);
        if (declared > maximum)
            return reject(native, 413);
        if (declared != 0 && !(native->flags & HTTP_REQUEST_EXPECT_BODY))
            return reject(native, 501);
    }
    status = rotmos_request_header_first(request, "expect", &value, NULL);
    if (status == ROTMOS_OK) {
        if (!rotmos_text_equal_case(value.data, "100-continue"))
            return reject(native, 417);
        if (request->has_length &&
            (request->content_length.high || request->content_length.low))
            http_response(native, 100, NULL, 0);
    }
    return KORE_RESULT_OK;
}

static int release_bytes(struct netbuf *buffer)
{
    free(buffer->extra);
    return KORE_RESULT_OK;
}

static int dispatch_error(struct http_request *native, int status)
{
    (void)reject(native, status);
    return KORE_RESULT_OK;
}

static void header_add(struct http_request *native, const rotmos_header *value)
{
    struct http_header *header;
    header = kore_pool_get(&http_header_pool);
    header->header = kore_strdup(value->name.data);
    header->value = kore_strdup(value->value.data);
    TAILQ_INSERT_TAIL(&native->resp_headers, header, list);
}

int rotmos_kore_dispatch(struct http_request *native)
{
    rotmos_request *request;
    rotmos_reply *reply;
    rotmos_status status;
    rotmos_error error;
    unsigned char *body;
    size_t length;
    size_t i;
    int code;
    int pretty;
    request = native->hdlr_extra;
    if (request == NULL)
        return dispatch_error(native, 500);
    request->body_ready = native->http_body_fd == -1;
    if (native->http_body != NULL) {
        request->body.data = native->http_body->data;
        request->body.length = (size_t)native->content_length;
    }
    status = rotmos_reply_new(&request->reply, &error);
    if (status != ROTMOS_OK)
        return dispatch_error(native, 500);
    reply = request->reply;
    reply->owner = request;
    status = rotmos_router_invoke(request, &error);
    if (status == ROTMOS_OK && (reply->kind == ROTMOS_REPLY_FILE ||
                                reply->kind == ROTMOS_REPLY_SOURCE))
        status = ROTMOS_NOT_SUPPORTED;
    if (status != ROTMOS_OK) {
        code = status == ROTMOS_INVALID
                   ? 400
                   : (status == ROTMOS_NOT_SUPPORTED ? 501 : 500);
        reply->owner = NULL;
        (void)rotmos_reply_destroy(reply, NULL);
        request->reply = NULL;
        return dispatch_error(native, code);
    }
    if (!reply->selected)
        (void)rotmos_reply_status(reply, 204, NULL);
    code = reply->status;
    for (i = 0; i < reply->header_count; ++i) {
        if (rotmos_text_equal_case(reply->headers[i].name.data,
                                   "transfer-encoding") ||
            rotmos_text_equal_case(reply->headers[i].name.data, "connection") ||
            rotmos_text_equal_case(reply->headers[i].name.data, "keep-alive"))
            continue;
        if (rotmos_text_equal_case(reply->headers[i].name.data,
                                   "content-length")) {
            if (native->method != HTTP_METHOD_HEAD && code != 304)
                continue;
            if (rotmos_u64_from_decimal(reply->headers[i].value.data,
                                        reply->headers[i].value.length,
                                        &request->content_length,
                                        NULL) != ROTMOS_OK)
                return dispatch_error(native, 500);
            native->flags |= HTTP_REQUEST_NO_CONTENT_LENGTH;
        }
        header_add(native, &reply->headers[i]);
    }
    if (reply->content_type != NULL)
        http_response_header(native, "content-type", reply->content_type);
    body = reply->body;
    length = reply->body_length;
    if (code == 204 || code == 205 || code == 304)
        length = 0;
    /* The staged declaration is authoritative, including an empty error body.
     */
    pretty = http_pretty_error;
    http_pretty_error = 0;
    if (length != 0) {
        reply->body = NULL;
        reply->body_length = 0;
        http_response_stream(native, code, body, length, release_bytes, body);
    } else {
        http_response(native, code, NULL, 0);
    }
    http_pretty_error = pretty;
    return KORE_RESULT_OK;
}

void rotmos_kore_request_free(struct http_request *native)
{
    rotmos_request *request;
    request = native->hdlr_extra;
    native->hdlr_extra = NULL;
    if (request != NULL) {
        request->native = NULL;
        rotmos_request_cleanup(request);
        free(request);
    }
}

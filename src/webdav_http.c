#include "webdav_internal.h"
#include "server_internal.h"
#include "static_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
static const char *header(knurl_request *request, const char *name)
{
    knurl_bytes bytes;
    return knurl_request_header_first(request, name, &bytes, NULL) == KNURL_OK
               ? bytes.data
               : NULL;
}
void knurl_dav_ref(struct dav_request *s) { ++s->references; }
void knurl_dav_unref(struct dav_request *s)
{
    if (--s->references != 0)
        return;
    if (s->writer != NULL)
        knurl_storage_resource_close(s->writer, NULL);
    if (s->list != NULL)
        knurl_storage_resource_close(s->list, NULL);
    if (s->asset != NULL)
        knurl_asset_close(s->asset, NULL);
    knurl_asset_info_clear(&s->item_info);
    free(s->path);
    free(s->destination);
    free(s->item_path);
    free(s);
}
static void request_release(void *data) { knurl_dav_unref(data); }
knurl_status knurl_dav_finish(struct dav_request *s, knurl_error *error)
{
    knurl_status status;
    if (s->request == NULL || s->finalized)
        return KNURL_STATE;
    s->finalized = 1;
    status = knurl_router_hooks_after(s->request, error);
    if (status == KNURL_OK)
        status = knurl_response_submit(knurl_request_response(s->request),
                                       s->request->impl->reply, error);
    if (status == KNURL_OK && s->request->impl->in_headers) {
        s->request->impl->response->impl->complete_reply = 1;
        s->request->impl->response->impl->phase = KNURL_RESPONSE_FINISHING;
    }
    if (status != KNURL_OK)
        knurl_dav_error(s, 500, status);
    return status;
}
void knurl_dav_error(struct dav_request *s, int code, knurl_status cause)
{
    knurl_response *response;
    knurl_bytes empty = {0};
    if (s->request == NULL)
        return;
    s->stopped = 1;
    response = knurl_request_response(s->request);
    if (!s->finalized) {
        if (knurl_reply_status(s->request->impl->reply, code, NULL) ==
            KNURL_OK) {
            (void)knurl_dav_finish(s, NULL);
            return;
        }
    }
    if (knurl_response_replace_uncommitted(response, NULL) == KNURL_OK &&
        knurl_response_bytes(response, code, "text/plain", empty, NULL) ==
            KNURL_OK)
        return;
    (void)knurl_response_abort(response, cause, NULL);
}
static int bounded(const char *text, size_t maximum)
{
    size_t n;
    if (text == NULL)
        return 1;
    for (n = 0; n <= maximum; ++n)
        if (text[n] == 0)
            return 1;
    return 0;
}
knurl_status knurl_dav_authorize(struct dav_request *s, const char *path,
                                 int kind, int *allowed, knurl_error *error)
{
    knurl_webdav_auth_request input;
    knurl_webdav_auth_response output;
    knurl_status status;
    int code;
    *allowed = 1;
    if (s->route->config.auth_disabled)
        return KNURL_OK;
    input.request = s->request;
    input.method = kind == 1 ? KNURL_HTTP_PROPFIND : s->method;
    input.mount_path_prefix = s->route->prefix;
    input.resource_path = path;
    knurl_webdav_auth_response_init(&output);
    status = s->route->config.auth(&input, &output,
                                   s->route->config.auth_userdata, error);
    *allowed = 0;
    if (status != KNURL_OK)
        return status;
    if (output.action < KNURL_WEBDAV_DENY ||
        output.action > KNURL_WEBDAV_REDIRECT ||
        !bounded(output.principal, KNURL_WEBDAV_PRINCIPAL_MAX) ||
        !bounded(output.location, 4096) ||
        !bounded(output.www_authenticate, 4096) ||
        !bounded(output.content_type, 256) ||
        output.body.length > KNURL_WEBDAV_AUTH_BODY_MAX ||
        (output.body.data == NULL && output.body.length != 0))
        return knurl_fail(error, KNURL_INVALID, "webdav_auth",
                          "auth reply exceeds bounded contract");
    if (output.action == KNURL_WEBDAV_ALLOW) {
        *allowed = 1;
        if (kind == 0 && output.principal != NULL)
            strcpy(s->principal, output.principal);
        return KNURL_OK;
    }
    if (kind == 1)
        return KNURL_OK;
    s->denied = 1;
    code = output.status;
    if (code == 0)
        code = output.action == KNURL_WEBDAV_CHALLENGE
                   ? 401
                   : (output.action == KNURL_WEBDAV_REDIRECT
                          ? 302
                          : (s->route->config.conceal_disabled ? 403 : 404));
    status = KNURL_OK;
    if (output.location != NULL)
        status = knurl_reply_header_set(s->request->impl->reply, "location",
                                        output.location, error);
    if (status == KNURL_OK && output.www_authenticate != NULL)
        status =
            knurl_reply_header_set(s->request->impl->reply, "www-authenticate",
                                   output.www_authenticate, error);
    if (status == KNURL_OK) {
        if (output.body.length != 0)
            status = knurl_reply_bytes(s->request->impl->reply, code,
                                       output.content_type != NULL
                                           ? output.content_type
                                           : "text/plain",
                                       output.body, error);
        else
            status = knurl_reply_status(s->request->impl->reply, code, error);
    }
    return status;
}
static knurl_status scope_check(knurl_storage_operation *operation,
                                const char *path, const char *destination,
                                int *allowed, void *data, knurl_error *error)
{
    struct dav_request *s = data;
    knurl_status status;
    (void)operation;
    *allowed = 0;
    if (s->request == NULL || s->stopped)
        return KNURL_CANCELLED;
    status = knurl_dav_authorize(s, path, 2, allowed, error);
    if (status == KNURL_OK && *allowed && destination != NULL)
        status = knurl_dav_authorize(s, destination, 2, allowed, error);
    return status;
}
static int http_code(knurl_storage_code code)
{
    switch (code) {
    case KNURL_STORAGE_NOT_FOUND:
        return 404;
    case KNURL_STORAGE_EXISTS:
    case KNURL_STORAGE_PRECONDITION:
        return 412;
    case KNURL_STORAGE_INVALID:
        return 400;
    case KNURL_STORAGE_LIMIT:
        return 413;
    case KNURL_STORAGE_CONFLICT:
    case KNURL_STORAGE_TOMBSTONED:
        return 409;
    case KNURL_STORAGE_NOT_SUPPORTED:
        return 501;
    case KNURL_STORAGE_DENIED:
        return 403;
    default:
        return 500;
    }
}
static void completed(knurl_storage_operation *operation,
                      const knurl_storage_outcome *outcome, void *data)
{
    struct dav_request *s = data;
    if (s->request == NULL || s->stopped || outcome->cause != KNURL_OK ||
        outcome->result->code != KNURL_STORAGE_OK)
        return;
    if (s->action == KNURL_STORAGE_OPEN_READ) {
        if (knurl_storage_operation_take_asset(operation, &s->asset, NULL) !=
            KNURL_OK)
            knurl_dav_error(s, 500, KNURL_STATE);
    } else if (s->action == KNURL_STORAGE_PUT_BEGIN) {
        if (knurl_storage_operation_take_resource(operation, &s->writer,
                                                  NULL) != KNURL_OK)
            knurl_dav_error(s, 500, KNURL_STATE);
    } else if (s->action == KNURL_STORAGE_LIST_OPEN) {
        if (knurl_storage_operation_take_resource(operation, &s->list, NULL) !=
            KNURL_OK)
            knurl_dav_error(s, 500, KNURL_STATE);
    }
}
static void resume_body(struct dav_request *s)
{
    if (s->body_end) {
        knurl_bytes empty = {0};
        if (knurl_dav_start(s, KNURL_STORAGE_PUT_FINISH, empty, NULL) !=
            KNURL_OK)
            knurl_dav_error(s, 500, KNURL_IO);
    } else
        (void)knurl_request_body_resume(s->request, NULL);
}
static void retired(const knurl_storage_outcome *outcome, void *data)
{
    struct dav_request *s = data;
    knurl_error error;
    const knurl_storage_result *r = outcome->result;
    knurl_bytes empty = {0};
    s->pending = NULL;
    if (s->request == NULL || s->stopped) {
        knurl_dav_unref(s);
        return;
    }
    if (outcome->cause != KNURL_OK) {
        knurl_dav_error(s, outcome->cause == KNURL_TIMEOUT ? 504 : 500,
                        outcome->cause);
        goto done;
    }
    if (r->code == KNURL_STORAGE_DENIED && s->denied) {
        (void)knurl_dav_finish(s, NULL);
        goto done;
    }
    if (s->action == KNURL_STORAGE_LIST_NEXT && r->code == KNURL_STORAGE_END) {
        s->prop_phase = 3;
        knurl_storage_resource_close(s->list, NULL);
        s->list = NULL;
        (void)knurl_response_producer_resume(knurl_request_response(s->request),
                                             NULL);
        goto done;
    }
    if (r->code != KNURL_STORAGE_OK) {
        int code = http_code(r->code);
        if ((s->method == KNURL_HTTP_GET || s->method == KNURL_HTTP_HEAD ||
             s->method == KNURL_HTTP_PROPFIND) &&
            (r->code == KNURL_STORAGE_TOMBSTONED ||
             r->code == KNURL_STORAGE_CONFLICT))
            code = 404;
        knurl_dav_error(s, code, KNURL_IO);
        goto done;
    }
    if (s->action == KNURL_STORAGE_PUT_BEGIN) {
        resume_body(s);
    } else if (s->action == KNURL_STORAGE_PUT_WRITE) {
        s->accepted = r->accepted;
        resume_body(s);
    } else if (s->action == KNURL_STORAGE_OPEN_READ) {
        knurl_static_config config;
        knurl_static_config_init(&config);
        config.content_type = s->route->config.content_type != NULL
                                  ? s->route->config.content_type
                                  : "application/octet-stream";
        config.cache_control = s->route->config.cache_control != NULL
                                   ? s->route->config.cache_control
                                   : "no-cache";
        if (knurl_static_reply_asset(&config, 1, s->request,
                                     s->request->impl->reply, s->asset, s->path,
                                     &error) == KNURL_OK) {
            s->asset = NULL;
            (void)knurl_dav_finish(s, NULL);
        } else {
            s->asset = NULL;
            knurl_dav_error(s, 500, error.status);
        }
    } else if (s->action == KNURL_STORAGE_STAT) {
        knurl_asset_info_clear(&s->item_info);
        if (knurl_asset_info_copy(&r->info, &s->item_info, &error) != KNURL_OK)
            knurl_dav_error(s, 500, error.status);
        else if (s->item_info.kind == KNURL_ASSET_DIRECTORY && s->depth == 1) {
            if (knurl_dav_start(s, KNURL_STORAGE_LIST_OPEN, empty, &error) !=
                KNURL_OK)
                knurl_dav_error(s, 500, error.status);
        } else if (knurl_dav_propfind(s, &error) != KNURL_OK)
            knurl_dav_error(s, 500, error.status);
    } else if (s->action == KNURL_STORAGE_LIST_OPEN) {
        if (knurl_dav_propfind(s, &error) != KNURL_OK)
            knurl_dav_error(s, 500, error.status);
    } else if (s->action == KNURL_STORAGE_LIST_NEXT)
        knurl_dav_prop_entry(s, r);
    else {
        int code = s->action == KNURL_STORAGE_MKCOL
                       ? 201
                       : ((s->action == KNURL_STORAGE_PUT_FINISH ||
                           s->action == KNURL_STORAGE_COPY ||
                           s->action == KNURL_STORAGE_MOVE) &&
                                  r->created
                              ? 201
                              : 204);
        if (knurl_reply_status(s->request->impl->reply, code, &error) ==
            KNURL_OK)
            (void)knurl_dav_finish(s, NULL);
        else
            knurl_dav_error(s, 500, error.status);
    }
done:
    knurl_dav_unref(s);
}
static void interrupted(knurl_storage_operation *operation, knurl_status cause,
                        void *data)
{
    (void)operation;
    knurl_dav_error(data, cause == KNURL_TIMEOUT ? 504 : 500, cause);
}
knurl_status knurl_dav_start(struct dav_request *s, knurl_storage_action action,
                             knurl_bytes bytes, knurl_error *error)
{
    knurl_storage_arguments arguments;
    knurl_storage_operation_config config;
    knurl_status status;
    if (s->pending != NULL || s->request == NULL)
        return KNURL_STATE;
    knurl_storage_arguments_init(&arguments);
    arguments.action = action;
    arguments.path = s->path;
    arguments.destination = s->destination;
    arguments.overwrite = s->overwrite;
    arguments.depth = s->depth;
    arguments.bytes = bytes;
    if (action == KNURL_STORAGE_PUT_WRITE ||
        action == KNURL_STORAGE_PUT_FINISH || action == KNURL_STORAGE_PUT_ABORT)
        arguments.resource = s->writer;
    else if (action == KNURL_STORAGE_LIST_NEXT)
        arguments.resource = s->list;
    if (action == KNURL_STORAGE_PUT_BEGIN || action == KNURL_STORAGE_DELETE ||
        action == KNURL_STORAGE_MKCOL || action == KNURL_STORAGE_COPY ||
        action == KNURL_STORAGE_MOVE) {
        arguments.if_match = header(s->request, "if-match");
        arguments.if_none_match = header(s->request, "if-none-match");
    }
    knurl_storage_operation_config_init(&config);
    config.completed = completed;
    config.interrupted = interrupted;
    config.retired = retired;
    config.userdata = s;
    config.timeout_ms = s->route->config.operation_timeout_ms;
    config.timeout_disabled = s->route->config.operation_timeout_disabled;
    if (!s->route->config.auth_disabled &&
        (action == KNURL_STORAGE_DELETE || action == KNURL_STORAGE_COPY ||
         action == KNURL_STORAGE_MOVE))
        config.check_scope = scope_check;
    s->action = action;
    status = knurl_storage_operation_start(
        s->route->provider, knurl_request_loop(s->request), &arguments, &config,
        &s->pending, error);
    if (status == KNURL_OK)
        knurl_dav_ref(s);
    return status;
}
static knurl_status body(knurl_request *request, const void *data,
                         size_t length, size_t *consumed, void *userdata,
                         knurl_error *error)
{
    struct dav_request *s = request->impl->codec_data;
    knurl_bytes bytes;
    (void)userdata;
    *consumed = 0;
    if (s->stopped)
        return KNURL_AGAIN;
    if (s->method != KNURL_HTTP_PUT) {
        *consumed = length;
        if (s->method == KNURL_HTTP_MKCOL && length != 0)
            knurl_dav_error(s, 415, KNURL_INVALID);
        return KNURL_OK;
    }
    if (s->accepted != 0) {
        if (s->accepted > length)
            return KNURL_INVALID;
        *consumed = s->accepted;
        s->accepted = 0;
        return KNURL_OK;
    }
    if (s->pending != NULL || s->writer == NULL)
        return KNURL_AGAIN;
    bytes.data = data;
    bytes.length =
        length > KNURL_STORAGE_CHUNK_MAX ? KNURL_STORAGE_CHUNK_MAX : length;
    if (knurl_dav_start(s, KNURL_STORAGE_PUT_WRITE, bytes, error) != KNURL_OK)
        return KNURL_IO;
    return KNURL_AGAIN;
}
static knurl_status body_end(knurl_request *request, void *userdata,
                             knurl_error *error)
{
    struct dav_request *s = request->impl->codec_data;
    knurl_bytes empty = {0};
    (void)userdata;
    s->body_end = 1;
    if (s->stopped || s->pending != NULL)
        return KNURL_OK;
    if (s->method == KNURL_HTTP_PUT)
        return knurl_dav_start(s, KNURL_STORAGE_PUT_FINISH, empty, error);
    if (s->method == KNURL_HTTP_GET || s->method == KNURL_HTTP_HEAD)
        return knurl_dav_start(s, KNURL_STORAGE_OPEN_READ, empty, error);
    if (s->method == KNURL_HTTP_PROPFIND)
        return knurl_dav_start(s, KNURL_STORAGE_STAT, empty, error);
    return knurl_dav_start(
        s,
        s->method == KNURL_HTTP_DELETE
            ? KNURL_STORAGE_DELETE
            : (s->method == KNURL_HTTP_MKCOL
                   ? KNURL_STORAGE_MKCOL
                   : (s->method == KNURL_HTTP_COPY ? KNURL_STORAGE_COPY
                                                   : KNURL_STORAGE_MOVE)),
        empty, error);
}
static void request_close(knurl_request *request,
                          const knurl_request_outcome *outcome, void *userdata)
{
    struct dav_request *s = request->impl->codec_data;
    (void)outcome;
    (void)userdata;
    s->request = NULL;
    s->stopped = 1;
    if (s->pending != NULL)
        (void)knurl_storage_operation_cancel(s->pending, NULL);
    if (s->writer != NULL) {
        knurl_storage_resource_close(s->writer, NULL);
        s->writer = NULL;
    }
    if (s->list != NULL) {
        knurl_storage_resource_close(s->list, NULL);
        s->list = NULL;
    }
}
static char *resource(struct dav_route *r, const char *path)
{
    const char *relative = path;
    char *copy;
    size_t n, prefix = strlen(r->prefix);
    if (strcmp(r->prefix, "/") != 0) {
        if (strncmp(path, r->prefix, prefix) != 0 ||
            (path[prefix] != 0 && path[prefix] != '/'))
            return NULL;
        relative = path + prefix;
    }
    if (*relative == 0)
        relative = "/";
    n = strlen(relative);
    if (n > KNURL_ASSET_PATH_MAX)
        return NULL;
    copy = knurl_copy_text(relative);
    if (copy == NULL)
        return NULL;
    if (n > 1 && copy[n - 1] == '/')
        copy[n - 1] = 0;
    if (!knurl_asset_path_valid(copy)) {
        free(copy);
        return NULL;
    }
    return copy;
}
static char *destination(struct dav_route *r, knurl_request *request,
                         const char *value)
{
    const char *path, *scheme, *host, *end, *port;
    size_t authority, h, p, length;
    char *raw, *selected;
    knurl_bytes decoded;
    if (value == NULL)
        return NULL;
    path = value;
    scheme = strstr(value, "://");
    if (scheme != NULL) {
        if (scheme - value == 4 && strncasecmp(value, "http", 4) == 0)
            port = ":80";
        else if (scheme - value == 5 && strncasecmp(value, "https", 5) == 0)
            port = ":443";
        else
            return NULL;
        path = strchr(scheme + 3, '/');
        if (path == NULL)
            return NULL;
        host = header(request, "host");
        if (host == NULL)
            return NULL;
        authority = (size_t)(path - scheme - 3);
        h = strlen(host);
        p = strlen(port);
        if (strcspn(scheme + 3, "@\\?# \t\r\n") < authority ||
            strcspn(host, "@\\/?# \t\r\n") < h)
            return NULL;
        if (authority > p && memcmp(path - p, port, p) == 0)
            authority -= p;
        if (h > p && memcmp(host + h - p, port, p) == 0)
            h -= p;
        if (authority == 0 || authority != h ||
            strncasecmp(scheme + 3, host, h) != 0)
            return NULL;
    } else if (path[0] != '/' || path[1] == '/')
        return NULL;
    end = strpbrk(path, "?#");
    length = end != NULL ? (size_t)(end - path) : strlen(path);
    if (length == 0 || length > KNURL_ASSET_PATH_MAX * 2u)
        return NULL;
    raw = malloc(length + 1);
    if (raw == NULL)
        return NULL;
    memcpy(raw, path, length);
    raw[length] = 0;
    if (knurl_request_path_decode(raw, &decoded, NULL) != KNURL_OK) {
        free(raw);
        return NULL;
    }
    free(raw);
    selected = resource(r, decoded.data);
    free((void *)decoded.data);
    return selected;
}
static const char *allow(struct dav_route *r)
{
    if (!(r->capabilities & KNURL_STORAGE_CAN_WRITE))
        return "OPTIONS, PROPFIND, GET, HEAD";
    if ((r->capabilities & (KNURL_STORAGE_CAN_COPY | KNURL_STORAGE_CAN_MOVE)) ==
        (KNURL_STORAGE_CAN_COPY | KNURL_STORAGE_CAN_MOVE))
        return "OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE, MKCOL, COPY, MOVE";
    if (r->capabilities & KNURL_STORAGE_CAN_COPY)
        return "OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE, MKCOL, COPY";
    if (r->capabilities & KNURL_STORAGE_CAN_MOVE)
        return "OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE, MKCOL, MOVE";
    return "OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE, MKCOL";
}
knurl_status knurl_dav_headers(knurl_request *request, knurl_request_plan *plan,
                               void *data, knurl_error *error)
{
    struct dav_route *r = data;
    struct dav_request *s;
    knurl_status status;
    const char *value;
    knurl_bytes empty = {0};
    int finish, authorized, supported;
    status = knurl_reply_new(&request->impl->reply, error);
    if (status != KNURL_OK)
        return status;
    request->impl->reply->impl->owner = request;
    status = knurl_router_hooks_before(request, &finish, error);
    if (status != KNURL_OK)
        return status;
    if (finish) {
        plan->disposition = KNURL_REQUEST_REPLY;
        status = knurl_response_submit(request->impl->response,
                                       request->impl->reply, error);
        if (status == KNURL_OK) {
            request->impl->response->impl->complete_reply = 1;
            request->impl->response->impl->phase = KNURL_RESPONSE_FINISHING;
        }
        return status;
    }
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return KNURL_NOMEM;
    s->route = r;
    s->request = request;
    s->references = 1;
    s->method = knurl_request_method(request);
    s->overwrite = 1;
    s->depth = -1;
    request->impl->codec_data = s;
    request->impl->codec_cleanup = request_release;
    plan->on_body = body;
    plan->on_body_end = body_end;
    plan->on_close = request_close;
    knurl_body_policy_init_stream(&plan->body);
    plan->body.max_bytes = r->config.max_body_bytes;
    {
        knurl_u64 hard =
            request->impl->server->impl->config.limits.hard_body_bytes;
        int order = 0;
        if (!plan->body.max_bytes.high && !plan->body.max_bytes.low)
            plan->body.max_bytes =
                request->impl->server->impl->config.limits.default_body_bytes;
        if (hard.high || hard.low) {
            (void)knurl_u64_compare(plan->body.max_bytes, hard, &order, NULL);
            if (order > 0)
                plan->body.max_bytes = hard;
        }
    }
    s->path = resource(r, knurl_request_path(request).data);
    if (s->path == NULL) {
        knurl_dav_error(s, 404, KNURL_INVALID);
        goto replied;
    }
    status = knurl_dav_authorize(s, s->path, 0, &authorized, error);
    if (status != KNURL_OK)
        return status;
    if (!authorized) {
        status = knurl_dav_finish(s, error);
        goto replied;
    }
    supported = s->method == KNURL_HTTP_OPTIONS ||
                s->method == KNURL_HTTP_PROPFIND ||
                s->method == KNURL_HTTP_GET || s->method == KNURL_HTTP_HEAD;
    if (r->capabilities & KNURL_STORAGE_CAN_WRITE)
        supported = supported || s->method == KNURL_HTTP_PUT ||
                    s->method == KNURL_HTTP_DELETE ||
                    s->method == KNURL_HTTP_MKCOL ||
                    (s->method == KNURL_HTTP_COPY &&
                     (r->capabilities & KNURL_STORAGE_CAN_COPY)) ||
                    (s->method == KNURL_HTTP_MOVE &&
                     (r->capabilities & KNURL_STORAGE_CAN_MOVE));
    if (!supported || s->method == KNURL_HTTP_OPTIONS) {
        status = knurl_reply_header_set(request->impl->reply, "allow", allow(r),
                                        error);
        if (status == KNURL_OK && s->method == KNURL_HTTP_OPTIONS)
            status =
                knurl_reply_header_set(request->impl->reply, "dav", "1", error);
        if (status == KNURL_OK)
            status = knurl_reply_status(request->impl->reply,
                                        supported ? 204 : 405, error);
        if (status == KNURL_OK)
            status = knurl_dav_finish(s, error);
        goto replied;
    }
    if (s->method == KNURL_HTTP_PROPFIND) {
        value = header(request, "depth");
        if (value == NULL || strcmp(value, "infinity") == 0) {
            status = knurl_reply_text(
                request->impl->reply, 403, "application/xml; charset=utf-8",
                "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:error "
                "xmlns:D=\"DAV:\"><D:propfind-finite-depth/></D:error>",
                error);
            if (status == KNURL_OK)
                status = knurl_dav_finish(s, error);
            goto replied;
        }
        if (strcmp(value, "0") != 0 && strcmp(value, "1") != 0) {
            knurl_dav_error(s, 400, KNURL_INVALID);
            goto replied;
        }
        s->depth = value[0] - '0';
    }
    if (s->method == KNURL_HTTP_COPY || s->method == KNURL_HTTP_MOVE) {
        s->destination =
            destination(r, request, header(request, "destination"));
        if (s->destination == NULL) {
            knurl_dav_error(s, 400, KNURL_INVALID);
            goto replied;
        }
        status = knurl_dav_authorize(s, s->destination, 2, &authorized, error);
        if (status != KNURL_OK)
            return status;
        if (!authorized) {
            status = knurl_dav_finish(s, error);
            goto replied;
        }
        value = header(request, "overwrite");
        if (value != NULL && strcmp(value, "T") != 0 &&
            strcmp(value, "F") != 0) {
            knurl_dav_error(s, 400, KNURL_INVALID);
            goto replied;
        }
        s->overwrite = value == NULL || strcmp(value, "T") == 0;
        value = header(request, "depth");
        if (value != NULL && strcmp(value, "infinity") != 0 &&
            (s->method == KNURL_HTTP_MOVE || strcmp(value, "0") != 0)) {
            knurl_dav_error(s, 400, KNURL_INVALID);
            goto replied;
        }
        if (value != NULL && strcmp(value, "0") == 0)
            s->depth = 0;
    }
    plan->userdata = knurl_request_userdata(request);
    if (s->method == KNURL_HTTP_PUT)
        return knurl_dav_start(s, KNURL_STORAGE_PUT_BEGIN, empty, error);
    return KNURL_OK;
replied:
    plan->disposition = KNURL_REQUEST_REPLY;
    plan->userdata = knurl_request_userdata(request);
    return status;
}

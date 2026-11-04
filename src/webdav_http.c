#include "webdav_internal.h"
#include "static_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
static const char *header(rotmos_request *request, const char *name)
{
    rotmos_bytes bytes;
    return rotmos_request_header_first(request, name, &bytes, NULL) == ROTMOS_OK
               ? bytes.data
               : NULL;
}
void rotmos_dav_ref(struct dav_request *s) { ++s->references; }
void rotmos_dav_unref(struct dav_request *s)
{
    if (--s->references != 0)
        return;
    if (s->writer != NULL)
        rotmos_storage_resource_close(s->writer, NULL);
    if (s->list != NULL)
        rotmos_storage_resource_close(s->list, NULL);
    if (s->asset != NULL)
        rotmos_asset_close(s->asset, NULL);
    rotmos_asset_info_clear(&s->item_info);
    free(s->path);
    free(s->destination);
    free(s->item_path);
    free(s);
}
static void request_release(void *data) { rotmos_dav_unref(data); }
rotmos_status rotmos_dav_finish(struct dav_request *s, rotmos_error *error)
{
    rotmos_status status;
    if (s->request == NULL || s->finalized)
        return ROTMOS_STATE;
    s->finalized = 1;
    status = rotmos_router_hooks_after(s->request, error);
    if (status == ROTMOS_OK)
        status = rotmos_response_submit(rotmos_request_response(s->request),
                                        s->request->reply, error);
    if (status == ROTMOS_OK && s->request->in_headers) {
        s->request->response->complete_reply = 1;
        s->request->response->phase = ROTMOS_RESPONSE_FINISHING;
    }
    if (status != ROTMOS_OK)
        rotmos_dav_error(s, 500, status);
    return status;
}
void rotmos_dav_error(struct dav_request *s, int code, rotmos_status cause)
{
    rotmos_response *response;
    rotmos_bytes empty = {0};
    if (s->request == NULL)
        return;
    s->stopped = 1;
    response = rotmos_request_response(s->request);
    if (!s->finalized) {
        if (rotmos_reply_status(s->request->reply, code, NULL) == ROTMOS_OK) {
            (void)rotmos_dav_finish(s, NULL);
            return;
        }
    }
    if (rotmos_response_replace_uncommitted(response, NULL) == ROTMOS_OK &&
        rotmos_response_bytes(response, code, NULL, empty, NULL) == ROTMOS_OK)
        return;
    (void)rotmos_response_abort(response, cause, NULL);
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
rotmos_status rotmos_dav_authorize(struct dav_request *s, const char *path,
                                   int kind, int *allowed, rotmos_error *error)
{
    rotmos_webdav_auth_request input;
    rotmos_webdav_auth_response output;
    rotmos_status status;
    int code;
    *allowed = 1;
    if (s->route->config.auth_disabled)
        return ROTMOS_OK;
    input.request = s->request;
    input.method = kind == 1 ? ROTMOS_HTTP_PROPFIND : s->method;
    input.mount_path_prefix = s->route->prefix;
    input.resource_path = path;
    rotmos_webdav_auth_response_init(&output);
    status = s->route->config.auth(&input, &output,
                                   s->route->config.auth_userdata, error);
    *allowed = 0;
    if (status != ROTMOS_OK)
        return status;
    if (output.action < ROTMOS_WEBDAV_DENY ||
        output.action > ROTMOS_WEBDAV_REDIRECT ||
        !bounded(output.principal, ROTMOS_WEBDAV_PRINCIPAL_MAX) ||
        !bounded(output.location, 4096) ||
        !bounded(output.www_authenticate, 4096) ||
        !bounded(output.content_type, 256) ||
        output.body.length > ROTMOS_WEBDAV_AUTH_BODY_MAX ||
        (output.body.data == NULL && output.body.length != 0))
        return rotmos_fail(error, ROTMOS_INVALID, "webdav_auth",
                           "auth reply exceeds bounded contract");
    if (output.action == ROTMOS_WEBDAV_ALLOW) {
        *allowed = 1;
        if (kind == 0 && output.principal != NULL)
            strcpy(s->principal, output.principal);
        return ROTMOS_OK;
    }
    if (kind == 1)
        return ROTMOS_OK;
    s->denied = 1;
    code = output.status;
    if (code == 0)
        code = output.action == ROTMOS_WEBDAV_CHALLENGE
                   ? 401
                   : (output.action == ROTMOS_WEBDAV_REDIRECT
                          ? 302
                          : (s->route->config.conceal_disabled ? 403 : 404));
    status = ROTMOS_OK;
    if (output.location != NULL)
        status = rotmos_reply_header_set(s->request->reply, "location",
                                         output.location, error);
    if (status == ROTMOS_OK && output.www_authenticate != NULL)
        status = rotmos_reply_header_set(s->request->reply, "www-authenticate",
                                         output.www_authenticate, error);
    if (status == ROTMOS_OK) {
        if (output.body.length != 0)
            status = rotmos_reply_bytes(s->request->reply, code,
                                        output.content_type != NULL
                                            ? output.content_type
                                            : "text/plain",
                                        output.body, error);
        else
            status = rotmos_reply_status(s->request->reply, code, error);
    }
    return status;
}
static rotmos_status scope_check(rotmos_storage_operation *operation,
                                 const char *path, const char *destination,
                                 int *allowed, void *data, rotmos_error *error)
{
    struct dav_request *s = data;
    rotmos_status status;
    (void)operation;
    *allowed = 0;
    if (s->request == NULL || s->stopped)
        return ROTMOS_CANCELLED;
    status = rotmos_dav_authorize(s, path, 2, allowed, error);
    if (status == ROTMOS_OK && *allowed && destination != NULL)
        status = rotmos_dav_authorize(s, destination, 2, allowed, error);
    return status;
}
static int http_code(rotmos_storage_code code)
{
    switch (code) {
    case ROTMOS_STORAGE_NOT_FOUND:
        return 404;
    case ROTMOS_STORAGE_EXISTS:
    case ROTMOS_STORAGE_PRECONDITION:
        return 412;
    case ROTMOS_STORAGE_INVALID:
        return 400;
    case ROTMOS_STORAGE_LIMIT:
        return 413;
    case ROTMOS_STORAGE_CONFLICT:
    case ROTMOS_STORAGE_TOMBSTONED:
        return 409;
    case ROTMOS_STORAGE_NOT_SUPPORTED:
        return 501;
    case ROTMOS_STORAGE_DENIED:
        return 403;
    default:
        return 500;
    }
}
static void completed(rotmos_storage_operation *operation,
                      const rotmos_storage_outcome *outcome, void *data)
{
    struct dav_request *s = data;
    if (s->request == NULL || s->stopped || outcome->cause != ROTMOS_OK ||
        outcome->result->code != ROTMOS_STORAGE_OK)
        return;
    if (s->action == ROTMOS_STORAGE_OPEN_READ) {
        if (rotmos_storage_operation_take_asset(operation, &s->asset, NULL) !=
            ROTMOS_OK)
            rotmos_dav_error(s, 500, ROTMOS_STATE);
    } else if (s->action == ROTMOS_STORAGE_PUT_BEGIN) {
        if (rotmos_storage_operation_take_resource(operation, &s->writer,
                                                   NULL) != ROTMOS_OK)
            rotmos_dav_error(s, 500, ROTMOS_STATE);
    } else if (s->action == ROTMOS_STORAGE_LIST_OPEN) {
        if (rotmos_storage_operation_take_resource(operation, &s->list, NULL) !=
            ROTMOS_OK)
            rotmos_dav_error(s, 500, ROTMOS_STATE);
    }
}
static void resume_body(struct dav_request *s)
{
    if (s->body_end) {
        rotmos_bytes empty = {0};
        if (rotmos_dav_start(s, ROTMOS_STORAGE_PUT_FINISH, empty, NULL) !=
            ROTMOS_OK)
            rotmos_dav_error(s, 500, ROTMOS_IO);
    } else
        (void)rotmos_request_body_resume(s->request, NULL);
}
static void retired(const rotmos_storage_outcome *outcome, void *data)
{
    struct dav_request *s = data;
    rotmos_error error;
    const rotmos_storage_result *r = outcome->result;
    rotmos_bytes empty = {0};
    s->pending = NULL;
    if (s->request == NULL || s->stopped) {
        rotmos_dav_unref(s);
        return;
    }
    if (outcome->cause != ROTMOS_OK) {
        rotmos_dav_error(s, outcome->cause == ROTMOS_TIMEOUT ? 504 : 500,
                         outcome->cause);
        goto done;
    }
    if (r->code == ROTMOS_STORAGE_DENIED && s->denied) {
        (void)rotmos_dav_finish(s, NULL);
        goto done;
    }
    if (s->action == ROTMOS_STORAGE_LIST_NEXT &&
        r->code == ROTMOS_STORAGE_END) {
        s->prop_phase = 3;
        rotmos_storage_resource_close(s->list, NULL);
        s->list = NULL;
        (void)rotmos_response_producer_resume(
            rotmos_request_response(s->request), NULL);
        goto done;
    }
    if (r->code != ROTMOS_STORAGE_OK) {
        int code = http_code(r->code);
        if ((s->method == ROTMOS_HTTP_GET || s->method == ROTMOS_HTTP_HEAD ||
             s->method == ROTMOS_HTTP_PROPFIND) &&
            (r->code == ROTMOS_STORAGE_TOMBSTONED ||
             r->code == ROTMOS_STORAGE_CONFLICT))
            code = 404;
        rotmos_dav_error(s, code, ROTMOS_IO);
        goto done;
    }
    if (s->action == ROTMOS_STORAGE_PUT_BEGIN) {
        resume_body(s);
    } else if (s->action == ROTMOS_STORAGE_PUT_WRITE) {
        s->accepted = r->accepted;
        resume_body(s);
    } else if (s->action == ROTMOS_STORAGE_OPEN_READ) {
        rotmos_static_config config;
        rotmos_static_config_init(&config);
        config.content_type = s->route->config.content_type != NULL
                                  ? s->route->config.content_type
                                  : "application/octet-stream";
        config.cache_control = s->route->config.cache_control != NULL
                                   ? s->route->config.cache_control
                                   : "no-cache";
        if (rotmos_static_reply_asset(&config, 1, s->request, s->request->reply,
                                      s->asset, s->path, &error) == ROTMOS_OK) {
            s->asset = NULL;
            (void)rotmos_dav_finish(s, NULL);
        } else {
            s->asset = NULL;
            rotmos_dav_error(s, 500, error.status);
        }
    } else if (s->action == ROTMOS_STORAGE_STAT) {
        rotmos_asset_info_clear(&s->item_info);
        if (rotmos_asset_info_copy(&r->info, &s->item_info, &error) !=
            ROTMOS_OK)
            rotmos_dav_error(s, 500, error.status);
        else if (s->item_info.kind == ROTMOS_ASSET_DIRECTORY && s->depth == 1) {
            if (rotmos_dav_start(s, ROTMOS_STORAGE_LIST_OPEN, empty, &error) !=
                ROTMOS_OK)
                rotmos_dav_error(s, 500, error.status);
        } else if (rotmos_dav_propfind(s, &error) != ROTMOS_OK)
            rotmos_dav_error(s, 500, error.status);
    } else if (s->action == ROTMOS_STORAGE_LIST_OPEN) {
        if (rotmos_dav_propfind(s, &error) != ROTMOS_OK)
            rotmos_dav_error(s, 500, error.status);
    } else if (s->action == ROTMOS_STORAGE_LIST_NEXT)
        rotmos_dav_prop_entry(s, r);
    else {
        int code = s->action == ROTMOS_STORAGE_MKCOL
                       ? 201
                       : ((s->action == ROTMOS_STORAGE_PUT_FINISH ||
                           s->action == ROTMOS_STORAGE_COPY ||
                           s->action == ROTMOS_STORAGE_MOVE) &&
                                  r->created
                              ? 201
                              : 204);
        if (rotmos_reply_status(s->request->reply, code, &error) == ROTMOS_OK)
            (void)rotmos_dav_finish(s, NULL);
        else
            rotmos_dav_error(s, 500, error.status);
    }
done:
    rotmos_dav_unref(s);
}
static void interrupted(rotmos_storage_operation *operation,
                        rotmos_status cause, void *data)
{
    (void)operation;
    rotmos_dav_error(data, cause == ROTMOS_TIMEOUT ? 504 : 500, cause);
}
rotmos_status rotmos_dav_start(struct dav_request *s,
                               rotmos_storage_action action, rotmos_bytes bytes,
                               rotmos_error *error)
{
    rotmos_storage_arguments arguments;
    rotmos_storage_operation_config config;
    rotmos_status status;
    if (s->pending != NULL || s->request == NULL)
        return ROTMOS_STATE;
    rotmos_storage_arguments_init(&arguments);
    arguments.action = action;
    arguments.path = s->path;
    arguments.destination = s->destination;
    arguments.overwrite = s->overwrite;
    arguments.depth = s->depth;
    arguments.bytes = bytes;
    if (action == ROTMOS_STORAGE_PUT_WRITE ||
        action == ROTMOS_STORAGE_PUT_FINISH ||
        action == ROTMOS_STORAGE_PUT_ABORT)
        arguments.resource = s->writer;
    else if (action == ROTMOS_STORAGE_LIST_NEXT)
        arguments.resource = s->list;
    if (action == ROTMOS_STORAGE_PUT_BEGIN || action == ROTMOS_STORAGE_DELETE ||
        action == ROTMOS_STORAGE_MKCOL || action == ROTMOS_STORAGE_COPY ||
        action == ROTMOS_STORAGE_MOVE) {
        arguments.if_match = header(s->request, "if-match");
        arguments.if_none_match = header(s->request, "if-none-match");
    }
    rotmos_storage_operation_config_init(&config);
    config.completed = completed;
    config.interrupted = interrupted;
    config.retired = retired;
    config.userdata = s;
    config.timeout_ms = s->route->config.operation_timeout_ms;
    config.timeout_disabled = s->route->config.operation_timeout_disabled;
    if (!s->route->config.auth_disabled &&
        (action == ROTMOS_STORAGE_DELETE || action == ROTMOS_STORAGE_COPY ||
         action == ROTMOS_STORAGE_MOVE))
        config.check_scope = scope_check;
    s->action = action;
    status = rotmos_storage_operation_start(
        s->route->provider, rotmos_request_loop(s->request), &arguments,
        &config, &s->pending, error);
    if (status == ROTMOS_OK)
        rotmos_dav_ref(s);
    return status;
}
static rotmos_status body(rotmos_request *request, const void *data,
                          size_t length, size_t *consumed, void *userdata,
                          rotmos_error *error)
{
    struct dav_request *s = request->codec_data;
    rotmos_bytes bytes;
    (void)userdata;
    *consumed = 0;
    if (s->stopped)
        return ROTMOS_AGAIN;
    if (s->method != ROTMOS_HTTP_PUT) {
        *consumed = length;
        if (s->method == ROTMOS_HTTP_MKCOL && length != 0)
            rotmos_dav_error(s, 415, ROTMOS_INVALID);
        return ROTMOS_OK;
    }
    if (s->accepted != 0) {
        if (s->accepted > length)
            return ROTMOS_INVALID;
        *consumed = s->accepted;
        s->accepted = 0;
        return ROTMOS_OK;
    }
    if (s->pending != NULL || s->writer == NULL)
        return ROTMOS_AGAIN;
    bytes.data = data;
    bytes.length =
        length > ROTMOS_STORAGE_CHUNK_MAX ? ROTMOS_STORAGE_CHUNK_MAX : length;
    if (rotmos_dav_start(s, ROTMOS_STORAGE_PUT_WRITE, bytes, error) !=
        ROTMOS_OK)
        return ROTMOS_IO;
    return ROTMOS_AGAIN;
}
static rotmos_status body_end(rotmos_request *request, void *userdata,
                              rotmos_error *error)
{
    struct dav_request *s = request->codec_data;
    rotmos_bytes empty = {0};
    (void)userdata;
    s->body_end = 1;
    if (s->stopped || s->pending != NULL)
        return ROTMOS_OK;
    if (s->method == ROTMOS_HTTP_PUT)
        return rotmos_dav_start(s, ROTMOS_STORAGE_PUT_FINISH, empty, error);
    if (s->method == ROTMOS_HTTP_GET || s->method == ROTMOS_HTTP_HEAD)
        return rotmos_dav_start(s, ROTMOS_STORAGE_OPEN_READ, empty, error);
    if (s->method == ROTMOS_HTTP_PROPFIND)
        return rotmos_dav_start(s, ROTMOS_STORAGE_STAT, empty, error);
    return rotmos_dav_start(
        s,
        s->method == ROTMOS_HTTP_DELETE
            ? ROTMOS_STORAGE_DELETE
            : (s->method == ROTMOS_HTTP_MKCOL
                   ? ROTMOS_STORAGE_MKCOL
                   : (s->method == ROTMOS_HTTP_COPY ? ROTMOS_STORAGE_COPY
                                                    : ROTMOS_STORAGE_MOVE)),
        empty, error);
}
static void request_close(rotmos_request *request,
                          const rotmos_request_outcome *outcome, void *userdata)
{
    struct dav_request *s = request->codec_data;
    (void)outcome;
    (void)userdata;
    s->request = NULL;
    s->stopped = 1;
    if (s->pending != NULL)
        (void)rotmos_storage_operation_cancel(s->pending, NULL);
    if (s->writer != NULL) {
        rotmos_storage_resource_close(s->writer, NULL);
        s->writer = NULL;
    }
    if (s->list != NULL) {
        rotmos_storage_resource_close(s->list, NULL);
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
    if (n > ROTMOS_ASSET_PATH_MAX)
        return NULL;
    copy = rotmos_copy_text(relative);
    if (copy == NULL)
        return NULL;
    if (n > 1 && copy[n - 1] == '/')
        copy[n - 1] = 0;
    if (!rotmos_asset_path_valid(copy)) {
        free(copy);
        return NULL;
    }
    return copy;
}
static char *destination(struct dav_route *r, rotmos_request *request,
                         const char *value)
{
    const char *path, *scheme, *host, *end, *port;
    size_t authority, h, p, length;
    char *raw, *selected;
    rotmos_bytes decoded;
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
    if (length == 0 || length > ROTMOS_ASSET_PATH_MAX * 2u)
        return NULL;
    raw = malloc(length + 1);
    if (raw == NULL)
        return NULL;
    memcpy(raw, path, length);
    raw[length] = 0;
    if (rotmos_request_path_decode(raw, &decoded, NULL) != ROTMOS_OK) {
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
    if (!(r->capabilities & ROTMOS_STORAGE_CAN_WRITE))
        return "OPTIONS, PROPFIND, GET, HEAD";
    if ((r->capabilities &
         (ROTMOS_STORAGE_CAN_COPY | ROTMOS_STORAGE_CAN_MOVE)) ==
        (ROTMOS_STORAGE_CAN_COPY | ROTMOS_STORAGE_CAN_MOVE))
        return "OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE, MKCOL, COPY, MOVE";
    if (r->capabilities & ROTMOS_STORAGE_CAN_COPY)
        return "OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE, MKCOL, COPY";
    if (r->capabilities & ROTMOS_STORAGE_CAN_MOVE)
        return "OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE, MKCOL, MOVE";
    return "OPTIONS, PROPFIND, GET, HEAD, PUT, DELETE, MKCOL";
}
rotmos_status rotmos_dav_headers(rotmos_request *request,
                                 rotmos_request_plan *plan, void *data,
                                 rotmos_error *error)
{
    struct dav_route *r = data;
    struct dav_request *s;
    rotmos_status status;
    const char *value;
    rotmos_bytes empty = {0};
    int finish, authorized, supported;
    status = rotmos_reply_new(&request->reply, error);
    if (status != ROTMOS_OK)
        return status;
    request->reply->owner = request;
    status = rotmos_router_hooks_before(request, &finish, error);
    if (status != ROTMOS_OK)
        return status;
    if (finish) {
        plan->disposition = ROTMOS_REQUEST_REPLY;
        status =
            rotmos_response_submit(request->response, request->reply, error);
        if (status == ROTMOS_OK) {
            request->response->complete_reply = 1;
            request->response->phase = ROTMOS_RESPONSE_FINISHING;
        }
        return status;
    }
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return ROTMOS_NOMEM;
    s->route = r;
    s->request = request;
    s->references = 1;
    s->method = rotmos_request_method(request);
    s->overwrite = 1;
    s->depth = -1;
    request->codec_data = s;
    request->codec_cleanup = request_release;
    plan->on_body = body;
    plan->on_body_end = body_end;
    plan->on_close = request_close;
    rotmos_body_policy_init_stream(&plan->body);
    plan->body.max_bytes = r->config.max_body_bytes;
    s->path = resource(r, rotmos_request_path(request).data);
    if (s->path == NULL) {
        rotmos_dav_error(s, 404, ROTMOS_INVALID);
        goto replied;
    }
    status = rotmos_dav_authorize(s, s->path, 0, &authorized, error);
    if (status != ROTMOS_OK)
        return status;
    if (!authorized) {
        status = rotmos_dav_finish(s, error);
        goto replied;
    }
    supported = s->method == ROTMOS_HTTP_OPTIONS ||
                s->method == ROTMOS_HTTP_PROPFIND ||
                s->method == ROTMOS_HTTP_GET || s->method == ROTMOS_HTTP_HEAD;
    if (r->capabilities & ROTMOS_STORAGE_CAN_WRITE)
        supported = supported || s->method == ROTMOS_HTTP_PUT ||
                    s->method == ROTMOS_HTTP_DELETE ||
                    s->method == ROTMOS_HTTP_MKCOL ||
                    (s->method == ROTMOS_HTTP_COPY &&
                     (r->capabilities & ROTMOS_STORAGE_CAN_COPY)) ||
                    (s->method == ROTMOS_HTTP_MOVE &&
                     (r->capabilities & ROTMOS_STORAGE_CAN_MOVE));
    if (!supported || s->method == ROTMOS_HTTP_OPTIONS) {
        status =
            rotmos_reply_header_set(request->reply, "allow", allow(r), error);
        if (status == ROTMOS_OK && s->method == ROTMOS_HTTP_OPTIONS)
            status = rotmos_reply_header_set(request->reply, "dav", "1", error);
        if (status == ROTMOS_OK)
            status = rotmos_reply_status(request->reply, supported ? 204 : 405,
                                         error);
        if (status == ROTMOS_OK)
            status = rotmos_dav_finish(s, error);
        goto replied;
    }
    if (s->method == ROTMOS_HTTP_PROPFIND) {
        value = header(request, "depth");
        if (value == NULL || strcmp(value, "infinity") == 0) {
            status = rotmos_reply_text(
                request->reply, 403, "application/xml; charset=utf-8",
                "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:error "
                "xmlns:D=\"DAV:\"><D:propfind-finite-depth/></D:error>",
                error);
            if (status == ROTMOS_OK)
                status = rotmos_dav_finish(s, error);
            goto replied;
        }
        if (strcmp(value, "0") != 0 && strcmp(value, "1") != 0) {
            rotmos_dav_error(s, 400, ROTMOS_INVALID);
            goto replied;
        }
        s->depth = value[0] - '0';
    }
    if (s->method == ROTMOS_HTTP_COPY || s->method == ROTMOS_HTTP_MOVE) {
        s->destination =
            destination(r, request, header(request, "destination"));
        if (s->destination == NULL) {
            rotmos_dav_error(s, 400, ROTMOS_INVALID);
            goto replied;
        }
        status = rotmos_dav_authorize(s, s->destination, 2, &authorized, error);
        if (status != ROTMOS_OK)
            return status;
        if (!authorized) {
            status = rotmos_dav_finish(s, error);
            goto replied;
        }
        value = header(request, "overwrite");
        if (value != NULL && strcmp(value, "T") != 0 &&
            strcmp(value, "F") != 0) {
            rotmos_dav_error(s, 400, ROTMOS_INVALID);
            goto replied;
        }
        s->overwrite = value == NULL || strcmp(value, "T") == 0;
        value = header(request, "depth");
        if (value != NULL && strcmp(value, "infinity") != 0 &&
            (s->method == ROTMOS_HTTP_MOVE || strcmp(value, "0") != 0)) {
            rotmos_dav_error(s, 400, ROTMOS_INVALID);
            goto replied;
        }
        if (value != NULL && strcmp(value, "0") == 0)
            s->depth = 0;
    }
    plan->userdata = rotmos_request_userdata(request);
    if (s->method == ROTMOS_HTTP_PUT)
        return rotmos_dav_start(s, ROTMOS_STORAGE_PUT_BEGIN, empty, error);
    return ROTMOS_OK;
replied:
    plan->disposition = ROTMOS_REQUEST_REPLY;
    plan->userdata = rotmos_request_userdata(request);
    return status;
}

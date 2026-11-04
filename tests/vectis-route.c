#include "vectis-route.h"
#include <stdlib.h>
#include <string.h>
struct string_copy {
    struct string_copy *next;
    rotmos_bytes view;
    char text[1];
};
struct reference_request {
    rotmos_request *request;
    struct string_copy *strings;
    struct reference_source body_reader;
    rotmos_error reader_error;
};
struct forwarding {
    struct forwarding *next;
    struct reference_app *app;
    reference_route_handler handler;
    void *userdata;
};
static reference_status status_from(rotmos_status status)
{
    switch (status) {
    case ROTMOS_OK:
        return REFERENCE_OK;
    case ROTMOS_INVALID:
    case ROTMOS_LIMIT:
    case ROTMOS_OVERFLOW:
        return REFERENCE_INVALID;
    case ROTMOS_NOMEM:
        return REFERENCE_NOMEM;
    case ROTMOS_CONFLICT:
        return REFERENCE_CONFLICT;
    case ROTMOS_NOT_SUPPORTED:
        return REFERENCE_NOT_IMPLEMENTED;
    case ROTMOS_TIMEOUT:
        return REFERENCE_TIMEOUT;
    default:
        return REFERENCE_STATE;
    }
}
static rotmos_status status_to(reference_status status)
{
    static const rotmos_status values[] = {
        ROTMOS_OK,       ROTMOS_INVALID,       ROTMOS_NOMEM,  ROTMOS_STATE,
        ROTMOS_CONFLICT, ROTMOS_NOT_SUPPORTED, ROTMOS_TIMEOUT};
    return status >= REFERENCE_OK && status <= REFERENCE_TIMEOUT
               ? values[status]
               : ROTMOS_STATE;
}
void reference_error_clear(reference_error *error)
{
    if (error != NULL)
        memset(error, 0, sizeof(*error));
}
reference_status reference_error_from_rotmos(rotmos_status status,
                                             const rotmos_error *input,
                                             reference_error *error)
{
    static const int sources[] = {0, 1, 1, 2, 4, 5, 1};
    reference_error_clear(error);
    if (error != NULL && status != ROTMOS_OK) {
        error->code = status_from(status);
        if (input != NULL) {
            error->source = input->source >= ROTMOS_ERROR_NONE &&
                                    input->source <= ROTMOS_ERROR_XML
                                ? sources[input->source]
                                : 1;
            error->dependency_code = input->native_code;
            memcpy(error->message, input->message, sizeof(error->message));
            memcpy(error->detail, input->detail, sizeof(error->detail));
            error->message[255] = error->detail[255] = 0;
        }
    }
    return status_from(status);
}
static rotmos_status error_to(reference_status status,
                              const reference_error *in, rotmos_error *out)
{
    rotmos_status result = status_to(status);
    rotmos_error_clear(out);
    if (out != NULL && result != ROTMOS_OK) {
        out->status = result;
        out->source = in->source == 4   ? ROTMOS_ERROR_LONEJSON
                      : in->source == 5 ? ROTMOS_ERROR_PSLOG
                                        : ROTMOS_ERROR_CORE;
        out->native_code = in->dependency_code;
        memcpy(out->message, in->message, sizeof(out->message));
        memcpy(out->detail, in->detail, sizeof(out->detail));
        out->message[255] = out->detail[255] = 0;
    }
    return result;
}
static rotmos_status request_start(rotmos_server *server,
                                   rotmos_request *native, void *data,
                                   rotmos_error *error)
{
    struct reference_app *app = data;
    struct reference_request *request = calloc(1, sizeof(*request));
    (void)server;
    if (request == NULL)
        return ROTMOS_NOMEM;
    request->request = native;
    rotmos_request_set_userdata(native, request);
    ++app->requests;
    rotmos_error_clear(error);
    return ROTMOS_OK;
}
static void request_close(rotmos_server *server, rotmos_request *native,
                          const rotmos_request_outcome *outcome, void *data)
{
    struct reference_app *app = data;
    struct reference_request *request = rotmos_request_userdata(native);
    struct string_copy *next, *s;
    (void)server;
    (void)outcome;
    if (request == NULL)
        return;
    for (s = request->strings; s != NULL; s = next) {
        next = s->next;
        free(s);
    }
    free(request);
    rotmos_request_set_userdata(native, NULL);
    ++app->closed;
}
void reference_route_callbacks(struct reference_app *app,
                               rotmos_server_callbacks *callbacks)
{
    callbacks->on_request_start = request_start;
    callbacks->on_request_close = request_close;
    callbacks->userdata = app;
}
void reference_route_cleanup(struct reference_app *app)
{
    struct forwarding *f = app->routes, *next;
    while (f != NULL) {
        next = f->next;
        free(f);
        f = next;
    }
    app->routes = NULL;
}
static rotmos_status forward(rotmos_request *request, rotmos_reply *reply,
                             void *data, rotmos_error *error)
{
    struct forwarding *f = data;
    struct reference_response response;
    reference_error translated;
    reference_status status;
    reference_error_clear(&translated);
    response.reply = reply;
    status = f->handler(f->app, rotmos_request_userdata(request), &response,
                        f->userdata, &translated);
    return error_to(status, &translated, error);
}
reference_status
reference_route_register(struct reference_app *app,
                         const struct reference_route_config *in,
                         reference_error *error)
{
    rotmos_route_config config;
    rotmos_error native;
    rotmos_status status;
    struct forwarding *f;
    if (app == NULL || in == NULL)
        return reference_error_from_rotmos(ROTMOS_INVALID, NULL, error);
    f = malloc(sizeof(*f));
    if (f == NULL)
        return reference_error_from_rotmos(ROTMOS_NOMEM, NULL, error);
    rotmos_route_config_init(&config);
    config.method = (rotmos_http_method)in->method;
    config.methods = in->methods;
    config.path = in->path;
    config.path_kind = (rotmos_route_path_kind)in->path_kind;
    config.body.mode = (rotmos_body_mode)in->body.mode;
    config.body.memory_buffer_limit_bytes = in->body.memory_buffer_limit_bytes;
    config.body.disk_spool_disabled = in->body.disk_spool_disabled;
    status = rotmos_u64_from_size(in->body.max_bytes, &config.body.max_bytes,
                                  &native);
    f->app = app;
    f->handler = in->handler;
    f->userdata = in->userdata;
    config.handler = in->handler != NULL ? forward : NULL;
    config.userdata = f;
    if (status == ROTMOS_OK)
        status = rotmos_router_route(rotmos_server_router(app->server), &config,
                                     NULL, &native);
    if (status == ROTMOS_OK) {
        f->next = app->routes;
        app->routes = f;
    } else
        free(f);
    return reference_error_from_rotmos(status, &native, error);
}
static const char *string_view(struct reference_request *request,
                               rotmos_bytes view)
{
    struct string_copy *s;
    if (request == NULL || view.data == NULL)
        return NULL;
    for (s = request->strings; s != NULL; s = s->next)
        if (s->view.data == view.data && s->view.length == view.length)
            return s->text;
    if (view.length > (size_t)-1 - sizeof(*s))
        return NULL;
    s = malloc(sizeof(*s) + view.length);
    if (s == NULL)
        return NULL;
    s->next = request->strings;
    s->view = view;
    memcpy(s->text, view.data, view.length);
    s->text[view.length] = 0;
    request->strings = s;
    return s->text;
}
int reference_request_method(struct reference_request *r)
{
    return r != NULL ? (int)rotmos_request_method(r->request) : 0;
}
const char *reference_request_path(struct reference_request *r)
{
    return r != NULL ? string_view(r, rotmos_request_path(r->request)) : NULL;
}
const char *reference_request_ip(struct reference_request *r)
{
    return r != NULL
               ? string_view(r, rotmos_request_effective_address(r->request))
               : NULL;
}
#define LOOKUP(name, native)                                                   \
    const char *reference_request_##name(struct reference_request *r,          \
                                         const char *key)                      \
    {                                                                          \
        rotmos_bytes value;                                                    \
        return r != NULL && native(r->request, key, &value, NULL) == ROTMOS_OK \
                   ? string_view(r, value)                                     \
                   : NULL;                                                     \
    }
LOOKUP(param, rotmos_request_path_param)
LOOKUP(query, rotmos_request_query_first)
LOOKUP(header, rotmos_request_header_first)
#undef LOOKUP
size_t reference_request_query_count(struct reference_request *r)
{
    return r != NULL ? rotmos_request_query_count(r->request) : 0;
}
int reference_request_query_at(struct reference_request *r, size_t index,
                               const char **name, const char **value)
{
    rotmos_header pair;
    if (r == NULL ||
        rotmos_request_query_at(r->request, index, &pair, NULL) != ROTMOS_OK)
        return 0;
    if (name != NULL)
        *name = string_view(r, pair.name);
    if (value != NULL)
        *value = string_view(r, pair.value);
    return 1;
}
reference_status reference_request_bytes(struct reference_request *r,
                                         rotmos_bytes *out,
                                         reference_error *error)
{
    rotmos_error e;
    rotmos_status s =
        rotmos_request_body_bytes(r != NULL ? r->request : NULL, out, &e);
    return reference_error_from_rotmos(s, &e, error);
}
static size_t body_read(struct reference_source *source, void *buffer,
                        size_t capacity, struct reference_source_error *error)
{
    struct reference_request *r = source->impl;
    size_t count = 0;
    rotmos_status status =
        rotmos_body_read(rotmos_request_body(r->request), buffer, capacity,
                         &count, &r->reader_error);
    if (status != ROTMOS_OK && status != ROTMOS_EOF) {
        error->code = status == ROTMOS_NOMEM ? 2 : 3;
        error->message = r->reader_error.message;
    }
    return count;
}
static int body_reset(struct reference_source *source,
                      struct reference_source_error *error)
{
    struct reference_request *r = source->impl;
    rotmos_status status =
        rotmos_body_rewind(rotmos_request_body(r->request), &r->reader_error);
    if (status == ROTMOS_OK)
        return 0;
    error->code = status == ROTMOS_NOMEM ? 2 : 3;
    error->message = r->reader_error.message;
    return error->code;
}
struct reference_source *
reference_request_body_reader(struct reference_request *r)
{
    if (r == NULL || rotmos_request_body(r->request) == NULL)
        return NULL;
    r->body_reader.read = body_read;
    r->body_reader.reset = body_reset;
    r->body_reader.impl = r;
    return &r->body_reader;
}
const char *reference_request_body_path(struct reference_request *r)
{
    const char *path;
    int fd;
    return r != NULL && rotmos_body_file(rotmos_request_body(r->request), &fd,
                                         &path, NULL) == ROTMOS_OK
               ? path
               : NULL;
}
int reference_request_body_is_spooled(struct reference_request *r)
{
    return r != NULL && rotmos_body_kind(rotmos_request_body(r->request)) ==
                            ROTMOS_BODY_FILE;
}
reference_status reference_response_status(struct reference_response *r,
                                           int code, reference_error *error)
{
    rotmos_error e;
    rotmos_status s =
        rotmos_reply_status(r != NULL ? r->reply : NULL, code, &e);
    return reference_error_from_rotmos(s, &e, error);
}
reference_status reference_response_header(struct reference_response *r,
                                           const char *name, const char *value,
                                           reference_error *error)
{
    rotmos_error e;
    rotmos_status s =
        rotmos_reply_header_add(r != NULL ? r->reply : NULL, name, value, &e);
    return reference_error_from_rotmos(s, &e, error);
}
reference_status reference_response_text(struct reference_response *r, int code,
                                         const char *type, const char *text,
                                         reference_error *error)
{
    rotmos_error e;
    rotmos_status s =
        rotmos_reply_text(r != NULL ? r->reply : NULL, code, type, text, &e);
    return reference_error_from_rotmos(s, &e, error);
}
struct source_forward {
    struct reference_source *host;
    int taken;
};
reference_status reference_response_file(struct reference_response *r, int code,
                                         const char *type, const char *path,
                                         reference_error *error)
{
    rotmos_error e;
    rotmos_status s =
        rotmos_reply_file(r != NULL ? r->reply : NULL, code, type, path, &e);
    return reference_error_from_rotmos(s, &e, error);
}
static rotmos_status source_error(const struct reference_source_error *in,
                                  rotmos_error *out, const char *prefix)
{
    rotmos_status status = in->code == 2 ? ROTMOS_NOMEM : ROTMOS_STATE;
    rotmos_error_clear(out);
    if (out != NULL) {
        out->status = status;
        out->source = ROTMOS_ERROR_BACKEND;
        out->native_code = in->code;
        strcpy(out->message, prefix);
        strcat(out->message, ": ");
        strncat(out->message,
                in->message != NULL ? in->message : "unknown source error",
                sizeof(out->message) - strlen(out->message) - 1);
    }
    return status;
}
static rotmos_status source_read(void *buffer, size_t capacity, size_t *count,
                                 void *data, rotmos_error *error)
{
    struct source_forward *f = data;
    struct reference_source_error host = {0, NULL};
    *count = f->host->read(f->host, buffer, capacity, &host);
    return *count != 0 ? ROTMOS_OK
           : host.code != 0
               ? source_error(&host, error, "failed to read response source")
               : ROTMOS_EOF;
}
static rotmos_status source_reset(void *data, rotmos_error *error)
{
    struct source_forward *f = data;
    struct reference_source_error host = {0, NULL};
    int status = f->host->reset(f->host, &host);
    if (status == 0)
        return ROTMOS_OK;
    host.code = status;
    return source_error(&host, error, "failed to reset response source");
}
static void source_close(void *data, rotmos_status cause)
{
    struct source_forward *f = data;
    (void)cause;
    if (f->taken && f->host->close != NULL)
        f->host->close(f->host);
    free(f);
}
static reference_status response_source(struct reference_response *r, int code,
                                        const char *type,
                                        struct reference_source *host, int live,
                                        reference_error *error)
{
    rotmos_source_config config;
    rotmos_source *source;
    rotmos_error e;
    rotmos_status status;
    struct source_forward *f;
    if (r == NULL || host == NULL || host->read == NULL)
        return reference_error_from_rotmos(ROTMOS_INVALID, NULL, error);
    f = calloc(1, sizeof(*f));
    if (f == NULL)
        return reference_error_from_rotmos(ROTMOS_NOMEM, NULL, error);
    f->host = host;
    rotmos_source_config_init(&config);
    config.read = source_read;
    config.reset = host->reset != NULL ? source_reset : NULL;
    config.reset_before_reply = 1;
    config.close = source_close;
    config.userdata = f;
    status = rotmos_source_new(&config, &source, &e);
    if (status != ROTMOS_OK)
        free(f);
    else {
        status =
            live ? rotmos_reply_source_take(r->reply, code, type, source, &e)
                 : rotmos_reply_source_spooled(r->reply, code, type, source,
                                               NULL, &e);
        if (live && status == ROTMOS_OK)
            f->taken = 1;
        else
            (void)rotmos_source_destroy(source, NULL);
    }
    reference_error_from_rotmos(status, &e, error);
    if (status != ROTMOS_OK && error != NULL &&
        e.source == ROTMOS_ERROR_BACKEND)
        error->source =
            3; /* Host source errors remain LOCKDC at this boundary. */
    return status_from(status);
}
reference_status reference_response_source(struct reference_response *r,
                                           int code, const char *type,
                                           struct reference_source *source,
                                           reference_error *error)
{
    return response_source(r, code, type, source, 0, error);
}
reference_status reference_response_stream_source(
    struct reference_response *r, int code, const char *type,
    struct reference_source *source, reference_error *error)
{
    return response_source(r, code, type, source, 1, error);
}

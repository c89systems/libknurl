#define _POSIX_C_SOURCE 200809L
#include "vectis-route.h"
#include "vectis-forward.h"
#include <knurl/kore.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
struct string_copy {
    struct string_copy *next;
    knurl_bytes view;
    char text[1];
};
struct reference_request {
    knurl_request *request;
    struct string_copy *strings;
    struct reference_source body_reader;
    knurl_error reader_error;
    const knurl_upload_metadata *metadata;
    size_t references;
    int closed;
};
struct forwarding {
    struct forwarding *next;
    struct reference_app *app;
    reference_route_handler handler;
    void *userdata;
};
static reference_status status_from(knurl_status status)
{
    switch (status) {
    case KNURL_OK:
        return REFERENCE_OK;
    case KNURL_INVALID:
    case KNURL_LIMIT:
    case KNURL_OVERFLOW:
        return REFERENCE_INVALID;
    case KNURL_NOMEM:
        return REFERENCE_NOMEM;
    case KNURL_CONFLICT:
        return REFERENCE_CONFLICT;
    case KNURL_NOT_SUPPORTED:
        return REFERENCE_NOT_IMPLEMENTED;
    case KNURL_TIMEOUT:
        return REFERENCE_TIMEOUT;
    default:
        return REFERENCE_STATE;
    }
}
static knurl_status status_to(reference_status status)
{
    static const knurl_status values[] = {
        KNURL_OK,       KNURL_INVALID,       KNURL_NOMEM,  KNURL_STATE,
        KNURL_CONFLICT, KNURL_NOT_SUPPORTED, KNURL_TIMEOUT};
    return status >= REFERENCE_OK && status <= REFERENCE_TIMEOUT
               ? values[status]
               : KNURL_STATE;
}
void reference_error_clear(reference_error *error)
{
    if (error != NULL)
        memset(error, 0, sizeof(*error));
}
reference_status reference_error_from_knurl(knurl_status status,
                                            const knurl_error *input,
                                            reference_error *error)
{
    static const int sources[] = {0, 1, 1, 2, 4, 5, 1};
    reference_error_clear(error);
    if (error != NULL && status != KNURL_OK) {
        error->code = status_from(status);
        if (input != NULL) {
            error->source = input->source >= KNURL_ERROR_NONE &&
                                    input->source <= KNURL_ERROR_XML
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
knurl_status reference_error_to_knurl(reference_status status,
                                      const reference_error *in,
                                      knurl_error *out)
{
    knurl_status result = status_to(status);
    knurl_error_clear(out);
    if (out != NULL && result != KNURL_OK) {
        out->status = result;
        out->source = in->source == 4   ? KNURL_ERROR_LONEJSON
                      : in->source == 5 ? KNURL_ERROR_PSLOG
                                        : KNURL_ERROR_CORE;
        out->native_code = in->dependency_code;
        memcpy(out->message, in->message, sizeof(out->message));
        memcpy(out->detail, in->detail, sizeof(out->detail));
        out->message[255] = out->detail[255] = 0;
    }
    return result;
}
static knurl_status request_start(knurl_server *server, knurl_request *native,
                                  void *data, knurl_error *error)
{
    struct reference_app *app = data;
    struct reference_request *request = calloc(1, sizeof(*request));
    (void)server;
    if (request == NULL)
        return KNURL_NOMEM;
    request->request = native;
    request->references = 1;
    knurl_request_set_userdata(native, request);
    ++app->requests;
    knurl_error_clear(error);
    return KNURL_OK;
}
void reference_request_release(void *data)
{
    struct reference_request *request = data;
    struct string_copy *next, *s;
    if (request == NULL || --request->references != 0)
        return;
    for (s = request->strings; s != NULL; s = next) {
        next = s->next;
        free(s);
    }
    free(request);
}
knurl_status reference_request_acquire(knurl_request *native,
                                       const knurl_upload_metadata *metadata,
                                       void **out, knurl_error *error)
{
    struct reference_request *request = knurl_request_userdata(native);
    if (request == NULL || metadata == NULL || out == NULL)
        return KNURL_STATE;
    request->metadata = metadata;
    ++request->references;
    *out = request;
    knurl_error_clear(error);
    return KNURL_OK;
}
static void request_close(knurl_server *server, knurl_request *native,
                          const knurl_request_outcome *outcome, void *data)
{
    struct reference_app *app = data;
    struct reference_request *request = knurl_request_userdata(native);
    (void)server;
    (void)outcome;
    if (request == NULL)
        return;
    __atomic_store_n(&request->closed, 1, __ATOMIC_RELEASE);
    request->request = NULL;
    reference_request_release(request);
    knurl_request_set_userdata(native, NULL);
    ++app->closed;
}
int reference_request_is_closed(const struct reference_request *request)
{
    return request == NULL ||
           __atomic_load_n(&request->closed, __ATOMIC_ACQUIRE);
}
struct http_request *reference_request_kore(struct reference_request *request)
{
    return request != NULL && request->metadata == NULL
               ? knurl_kore_request(request->request)
               : NULL;
}
void reference_route_callbacks(struct reference_app *app,
                               knurl_server_callbacks *callbacks)
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
static knurl_status forward(knurl_request *request, knurl_reply *reply,
                            void *data, knurl_error *error)
{
    struct forwarding *f = data;
    struct reference_response response;
    reference_error translated;
    reference_status status;
    reference_error_clear(&translated);
    response.reply = reply;
    status = f->handler(f->app, knurl_request_userdata(request), &response,
                        f->userdata, &translated);
    return reference_error_to_knurl(status, &translated, error);
}
reference_status
reference_route_register(struct reference_app *app,
                         const struct reference_route_config *in,
                         reference_error *error)
{
    knurl_route_config config;
    knurl_error native;
    knurl_status status;
    struct forwarding *f;
    if (app == NULL || in == NULL)
        return reference_error_from_knurl(KNURL_INVALID, NULL, error);
    f = malloc(sizeof(*f));
    if (f == NULL)
        return reference_error_from_knurl(KNURL_NOMEM, NULL, error);
    knurl_route_config_init(&config);
    config.method = (knurl_http_method)in->method;
    config.methods = in->methods;
    config.path = in->path;
    config.path_kind = (knurl_route_path_kind)in->path_kind;
    config.body.mode = (knurl_body_mode)in->body.mode;
    config.body.memory_buffer_limit_bytes = in->body.memory_buffer_limit_bytes;
    config.body.disk_spool_disabled = in->body.disk_spool_disabled;
    status = knurl_u64_from_size(in->body.max_bytes, &config.body.max_bytes,
                                 &native);
    f->app = app;
    f->handler = in->handler;
    f->userdata = in->userdata;
    config.handler = in->handler != NULL ? forward : NULL;
    config.userdata = f;
    if (status == KNURL_OK)
        status = knurl_router_route(knurl_server_router(app->server), &config,
                                    NULL, &native);
    if (status == KNURL_OK) {
        f->next = app->routes;
        app->routes = f;
    } else
        free(f);
    return reference_error_from_knurl(status, &native, error);
}
static const char *string_view(struct reference_request *request,
                               knurl_bytes view)
{
    struct string_copy *s;
    if (request == NULL || view.data == NULL)
        return NULL;
    if (request->metadata != NULL)
        return view.data;
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
    return r == NULL             ? 0
           : r->metadata != NULL ? (int)r->metadata->method
                                 : (int)knurl_request_method(r->request);
}
const char *reference_request_path(struct reference_request *r)
{
    return r == NULL ? NULL
                     : string_view(r, r->metadata != NULL
                                          ? r->metadata->path
                                          : knurl_request_path(r->request));
}
const char *reference_request_ip(struct reference_request *r)
{
    return r != NULL ? string_view(
                           r, r->metadata != NULL
                                  ? r->metadata->effective_address
                                  : knurl_request_effective_address(r->request))
                     : NULL;
}
static const char *metadata_lookup(struct reference_request *r, const char *key,
                                   int kind)
{
    const knurl_header *pairs = kind == 0   ? r->metadata->params
                                : kind == 1 ? r->metadata->queries
                                            : r->metadata->headers;
    size_t count = kind == 0   ? r->metadata->param_count
                   : kind == 1 ? r->metadata->query_count
                               : r->metadata->header_count;
    size_t i;
    if (key == NULL)
        return NULL;
    for (i = 0; i < count; ++i)
        if (kind == 2 ? !strcasecmp(pairs[i].name.data, key)
                      : !strcmp(pairs[i].name.data, key))
            return pairs[i].value.data;
    return NULL;
}
#define LOOKUP(name, native, kind)                                             \
    const char *reference_request_##name(struct reference_request *r,          \
                                         const char *key)                      \
    {                                                                          \
        knurl_bytes value;                                                     \
        if (r != NULL && r->metadata != NULL)                                  \
            return metadata_lookup(r, key, kind);                              \
        return r != NULL && native(r->request, key, &value, NULL) == KNURL_OK  \
                   ? string_view(r, value)                                     \
                   : NULL;                                                     \
    }
LOOKUP(param, knurl_request_path_param, 0)
LOOKUP(query, knurl_request_query_first, 1)
LOOKUP(header, knurl_request_header_first, 2)
#undef LOOKUP
size_t reference_request_query_count(struct reference_request *r)
{
    return r == NULL             ? 0
           : r->metadata != NULL ? r->metadata->query_count
                                 : knurl_request_query_count(r->request);
}
int reference_request_query_at(struct reference_request *r, size_t index,
                               const char **name, const char **value)
{
    knurl_header pair;
    if (r != NULL && r->metadata != NULL) {
        if (index >= r->metadata->query_count)
            return 0;
        pair = r->metadata->queries[index];
        if (name != NULL)
            *name = pair.name.data;
        if (value != NULL)
            *value = pair.value.data;
        return 1;
    }
    if (r == NULL ||
        knurl_request_query_at(r->request, index, &pair, NULL) != KNURL_OK)
        return 0;
    if (name != NULL)
        *name = string_view(r, pair.name);
    if (value != NULL)
        *value = string_view(r, pair.value);
    return 1;
}
reference_status reference_request_bytes(struct reference_request *r,
                                         knurl_bytes *out,
                                         reference_error *error)
{
    knurl_error e;
    knurl_status s;
    if (r != NULL && r->metadata != NULL)
        return reference_error_from_knurl(KNURL_INVALID, NULL, error);
    s = knurl_request_body_bytes(r != NULL ? r->request : NULL, out, &e);
    return reference_error_from_knurl(s, &e, error);
}
static size_t body_read(struct reference_source *source, void *buffer,
                        size_t capacity, struct reference_source_error *error)
{
    struct reference_request *r = source->impl;
    size_t count = 0;
    knurl_status status =
        knurl_body_read(knurl_request_body(r->request), buffer, capacity,
                        &count, &r->reader_error);
    if (status != KNURL_OK && status != KNURL_EOF) {
        error->code = status == KNURL_NOMEM ? 2 : 3;
        error->message = r->reader_error.message;
    }
    return count;
}
static int body_reset(struct reference_source *source,
                      struct reference_source_error *error)
{
    struct reference_request *r = source->impl;
    knurl_status status =
        knurl_body_rewind(knurl_request_body(r->request), &r->reader_error);
    if (status == KNURL_OK)
        return 0;
    error->code = status == KNURL_NOMEM ? 2 : 3;
    error->message = r->reader_error.message;
    return error->code;
}
struct reference_source *
reference_request_body_reader(struct reference_request *r)
{
    if (r == NULL || r->metadata != NULL ||
        knurl_request_body(r->request) == NULL)
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
    return r != NULL && r->metadata == NULL &&
                   knurl_body_file(knurl_request_body(r->request), &fd, &path,
                                   NULL) == KNURL_OK
               ? path
               : NULL;
}
int reference_request_body_is_spooled(struct reference_request *r)
{
    return r != NULL && r->metadata == NULL &&
           knurl_body_kind(knurl_request_body(r->request)) == KNURL_BODY_FILE;
}
reference_status reference_response_status(struct reference_response *r,
                                           int code, reference_error *error)
{
    knurl_error e;
    knurl_status s = knurl_reply_status(r != NULL ? r->reply : NULL, code, &e);
    return reference_error_from_knurl(s, &e, error);
}
reference_status reference_response_header(struct reference_response *r,
                                           const char *name, const char *value,
                                           reference_error *error)
{
    knurl_error e;
    knurl_status s =
        knurl_reply_header_add(r != NULL ? r->reply : NULL, name, value, &e);
    return reference_error_from_knurl(s, &e, error);
}
reference_status reference_response_text(struct reference_response *r, int code,
                                         const char *type, const char *text,
                                         reference_error *error)
{
    knurl_error e;
    knurl_status s =
        knurl_reply_text(r != NULL ? r->reply : NULL, code, type, text, &e);
    return reference_error_from_knurl(s, &e, error);
}
struct source_forward {
    struct reference_source *host;
    int taken, material;
};
void reference_body_config_init(struct reference_body_config *config)
{
    if (config != NULL) {
        memset(config, 0, sizeof(*config));
        config->memory_limit_bytes = 262144UL;
    }
}
struct result_reader {
    struct reference_source source;
    knurl_source *native;
    knurl_error error;
};
static size_t result_read(struct reference_source *source, void *buffer,
                          size_t capacity, struct reference_source_error *error)
{
    struct result_reader *reader = source->impl;
    size_t produced = 0;
    knurl_status status = knurl_source_read(reader->native, buffer, capacity,
                                            &produced, &reader->error);
    error->code = status == KNURL_OK || status == KNURL_EOF ? 0 : 3;
    error->message = error->code ? reader->error.message : NULL;
    return produced;
}
static int result_reset(struct reference_source *source,
                        struct reference_source_error *error)
{
    struct result_reader *reader = source->impl;
    knurl_status status = knurl_source_reset(reader->native, &reader->error);
    error->code = status == KNURL_OK ? 0 : 3;
    error->message = error->code ? reader->error.message : NULL;
    return error->code;
}
static void result_close(struct reference_source *source)
{
    struct result_reader *reader = source->impl;
    (void)knurl_source_destroy(reader->native, NULL);
    free(reader);
}
reference_status
reference_body_open_reader(const struct reference_body_result *in,
                           struct reference_source **out,
                           reference_error *error)
{
    struct result_reader *reader;
    knurl_body_result view;
    knurl_error native;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (in == NULL || out == NULL)
        return reference_error_from_knurl(KNURL_INVALID, NULL, error);
    reader = calloc(1, sizeof(*reader));
    if (reader == NULL)
        return reference_error_from_knurl(KNURL_NOMEM, NULL, error);
    knurl_body_result_init(&view);
    view.kind = (knurl_body_storage)in->kind;
    (void)knurl_u64_from_size(in->size, &view.length, NULL);
    view.memory.data = in->memory.data;
    view.memory.length = in->memory.size;
    view.path = in->path;
    status = knurl_body_result_open_reader(&view, &reader->native, &native);
    if (status != KNURL_OK) {
        free(reader);
        return reference_error_from_knurl(status, &native, error);
    }
    reader->source.read = result_read;
    reader->source.reset = result_reset;
    reader->source.close = result_close;
    reader->source.impl = reader;
    *out = &reader->source;
    return reference_error_from_knurl(KNURL_OK, NULL, error);
}
void reference_body_result_cleanup(struct reference_body_result *result)
{
    if (result != NULL) {
        if (result->owns_memory)
            free(result->memory.data);
        free(result->path);
        memset(result, 0, sizeof(*result));
    }
}
void reference_mutable_bytes_cleanup(struct reference_mutable_bytes *bytes)
{
    if (bytes != NULL) {
        free(bytes->data);
        bytes->data = NULL;
        bytes->size = 0;
    }
}
void reference_body_spill_cleanup(struct reference_body_spill_result *result)
{
    if (result != NULL) {
        reference_mutable_bytes_cleanup(&result->memory);
        free(result->path);
        memset(result, 0, sizeof(*result));
    }
}
reference_status reference_request_materialize(
    struct reference_request *r, const struct reference_body_config *in,
    struct reference_body_result *out, reference_error *error)
{
    knurl_body_materialize_config config;
    knurl_body_result result;
    knurl_error native;
    knurl_status status;
    if (out == NULL)
        return reference_error_from_knurl(KNURL_INVALID, NULL, error);
    memset(out, 0, sizeof(*out));
    if (r != NULL && r->metadata != NULL)
        return reference_error_from_knurl(KNURL_INVALID, NULL, error);
    knurl_body_materialize_config_init(&config);
    if (in != NULL) {
        config.buffer = in->buffer;
        config.capacity = in->buffer_size;
        config.memory_limit_bytes = in->memory_limit_bytes;
        config.directory = in->directory;
        config.prefix = in->prefix;
    }
    status = knurl_request_body_materialize(r != NULL ? r->request : NULL,
                                            in != NULL ? &config : NULL,
                                            &result, &native);
    if (status == KNURL_OK)
        status = knurl_u64_to_size(result.length, &out->size, &native);
    if (status != KNURL_OK) {
        knurl_body_result_cleanup(&result);
        return reference_error_from_knurl(status, &native, error);
    }
    out->kind = result.kind;
    out->memory.data = (void *)result.memory.data;
    out->memory.size = result.memory.length;
    out->path = result.path;
    out->owns_memory = result.owns_memory;
    return reference_error_from_knurl(KNURL_OK, NULL, error);
}
reference_status
reference_request_body_copy(struct reference_request *r,
                            struct reference_mutable_bytes *out,
                            reference_error *error)
{
    struct reference_body_config config;
    struct reference_body_result result;
    reference_status status;
    if (out == NULL)
        return reference_error_from_knurl(KNURL_INVALID, NULL, error);
    out->data = NULL;
    out->size = 0;
    reference_body_config_init(&config);
    config.memory_limit_bytes = (size_t)-1;
    status = reference_request_materialize(r, &config, &result, error);
    if (status == REFERENCE_OK) {
        *out = result.memory;
        result.memory.data = NULL;
        result.owns_memory = 0;
        reference_body_result_cleanup(&result);
    }
    return status;
}
reference_status reference_request_body_spill(
    struct reference_request *r, const struct reference_body_spill_config *in,
    struct reference_body_spill_result *out, reference_error *error)
{
    struct reference_body_config config;
    struct reference_body_result result;
    reference_status status;
    if (out == NULL)
        return reference_error_from_knurl(KNURL_INVALID, NULL, error);
    memset(out, 0, sizeof(*out));
    reference_body_config_init(&config);
    if (in != NULL) {
        config.memory_limit_bytes = in->memory_limit_bytes;
        config.directory = in->directory;
        config.prefix = in->prefix;
    }
    status = reference_request_materialize(r, in != NULL ? &config : NULL,
                                           &result, error);
    if (status == REFERENCE_OK) {
        out->memory = result.memory;
        out->path = result.path;
        out->size = result.size;
        out->spooled_to_disk = result.kind == 2;
    }
    return status;
}
reference_status reference_response_bytes(struct reference_response *r,
                                          int code, const char *type,
                                          knurl_bytes bytes,
                                          reference_error *error)
{
    knurl_error native;
    knurl_status status = knurl_reply_bytes(r != NULL ? r->reply : NULL, code,
                                            type, bytes, &native);
    return reference_error_from_knurl(status, &native, error);
}
reference_status reference_response_file(struct reference_response *r, int code,
                                         const char *type, const char *path,
                                         reference_error *error)
{
    knurl_error e;
    knurl_status s =
        knurl_reply_file(r != NULL ? r->reply : NULL, code, type, path, &e);
    return reference_error_from_knurl(s, &e, error);
}
static knurl_status source_error(const struct reference_source_error *in,
                                 knurl_error *out, const char *prefix)
{
    knurl_status status = in->code == 2 ? KNURL_NOMEM : KNURL_STATE;
    knurl_error_clear(out);
    if (out != NULL) {
        out->status = status;
        out->source = KNURL_ERROR_BACKEND;
        out->native_code = in->code;
        strcpy(out->message, prefix);
        strcat(out->message, ": ");
        strncat(out->message,
                in->message != NULL ? in->message : "unknown source error",
                sizeof(out->message) - strlen(out->message) - 1);
    }
    return status;
}
static knurl_status source_read(void *buffer, size_t capacity, size_t *count,
                                void *data, knurl_error *error)
{
    struct source_forward *f = data;
    struct reference_source_error host = {0, NULL};
    knurl_status status;
    *count = f->host->read(f->host, buffer, capacity, &host);
    if (*count != 0)
        return KNURL_OK;
    if (host.code == 0)
        return KNURL_EOF;
    status =
        source_error(&host, error,
                     f->material == 1   ? "failed to read TLS source"
                     : f->material == 2 ? "failed to read input source"
                                        : "failed to read response source");
    if (f->material == 2) {
        status = KNURL_IO;
        if (error != NULL)
            error->status = status;
    }
    return status;
}
static knurl_status source_reset(void *data, knurl_error *error)
{
    struct source_forward *f = data;
    struct reference_source_error host = {0, NULL};
    int status = f->host->reset(f->host, &host);
    if (status == 0)
        return KNURL_OK;
    host.code = status;
    return source_error(&host, error,
                        f->material ? "failed to reset TLS source"
                                    : "failed to reset response source");
}
static void source_close(void *data, knurl_status cause)
{
    struct source_forward *f = data;
    (void)cause;
    if (f->taken && f->host->close != NULL)
        f->host->close(f->host);
    free(f);
}
static knurl_status source_fail(knurl_error *error, knurl_status status,
                                const char *message)
{
    knurl_error_clear(error);
    if (error != NULL) {
        error->status = status;
        error->source = KNURL_ERROR_CORE;
        strcpy(error->operation, "reference_source");
        strncpy(error->message, message, sizeof(error->message) - 1);
    }
    return status;
}
static knurl_status source_wrap(struct reference_source *host, int reset,
                                int context, knurl_source **out,
                                knurl_error *error)
{
    struct source_forward *f;
    knurl_source_config config;
    knurl_status status;
    if (out == NULL)
        return source_fail(error, KNURL_INVALID, "source output is required");
    *out = NULL;
    if (host == NULL || host->read == NULL)
        return source_fail(error, KNURL_INVALID, "source reader is required");
    f = calloc(1, sizeof(*f));
    if (f == NULL)
        return source_fail(error, KNURL_NOMEM,
                           "cannot allocate source association");
    f->host = host;
    f->material = context;
    knurl_source_config_init(&config);
    config.read = source_read;
    config.reset = host->reset != NULL ? source_reset : NULL;
    config.close = source_close;
    config.userdata = f;
    status = knurl_source_new(&config, out, error);
    if (status != KNURL_OK)
        free(f);
    else if (reset && host->reset != NULL) {
        status = knurl_source_reset(*out, error);
        if (status != KNURL_OK) {
            (void)knurl_source_destroy(*out, NULL);
            *out = NULL;
        }
    }
    return status;
}
knurl_status reference_source_borrow(struct reference_source *host,
                                     knurl_source **out, knurl_error *error)
{
    return source_wrap(host, 1, 1, out, error);
}
knurl_status reference_input_open(const struct reference_input_source *in,
                                  knurl_source **out, knurl_error *error)
{
    knurl_bytes bytes;
    if (out == NULL)
        return source_fail(error, KNURL_INVALID, "source output is required");
    *out = NULL;
    if (in != NULL) {
        if (in->source != NULL)
            return source_wrap(in->source, 0, 2, out, error);
        if (in->path != NULL)
            return knurl_source_file(in->path, out, error);
        if (in->memory != NULL || in->memory_size != 0) {
            bytes.data = in->memory;
            bytes.length = in->memory_size;
            return knurl_source_bytes(bytes, out, error);
        }
    }
    return source_fail(error, KNURL_INVALID, "input source is empty");
}
struct array_forward {
    reference_array_item callback;
    void *userdata;
};
static reference_status reader_error(reference_error *error,
                                     knurl_status status, const char *message)
{
    knurl_error native;
    source_fail(&native, status, message);
    return reference_error_from_knurl(status, &native, error);
}
static reference_status request_reader(struct reference_request *request,
                                       reference_error *error)
{
    if (request == NULL)
        return reader_error(error, KNURL_INVALID, "request is required");
    if (request->metadata != NULL)
        return reader_error(error, KNURL_INVALID,
                            "streaming upload body requires its reader");
    if (request->request == NULL)
        return reader_error(error, KNURL_STATE, "request has closed");
    if (knurl_body_kind(knurl_request_body(request->request)) ==
        KNURL_BODY_STORAGE_NONE)
        return reader_error(error, KNURL_INVALID,
                            "request body reader is not available");
    return REFERENCE_OK;
}
static knurl_status forward_item(size_t index, const lonejson_record_view *view,
                                 void *data, knurl_error *error)
{
    struct array_forward *forward = data;
    reference_error translated;
    reference_status status;
    reference_error_clear(&translated);
    status =
        forward->callback(forward->userdata, index, view->record, &translated);
    return reference_error_to_knurl(status, &translated, error);
}
static reference_status array_result(knurl_status status, knurl_error *native,
                                     reference_error *error)
{
    /* Vectis labels stream-open/next failures INVALID_JSON. Application item
     * callback errors pass through unchanged; runtime creation keeps NOMEM. */
    if (status == KNURL_IO ||
        (status != KNURL_OK && !strcmp(native->operation, "lonejson"))) {
        status = native->status = KNURL_INVALID;
        native->source = KNURL_ERROR_LONEJSON;
        native->native_code = (long)LONEJSON_STATUS_INVALID_JSON;
    }
    return reference_error_from_knurl(status, native, error);
}
reference_status
reference_request_array_each(struct reference_request *request,
                             const char *selector, const lonejson_map *map,
                             void *item, reference_array_item callback,
                             void *userdata, reference_error *error)
{
    struct array_forward forward;
    lonejson_schema_view schema;
    knurl_error native;
    knurl_status status;
    reference_status translated;
    if (callback == NULL || item == NULL)
        return reader_error(error, KNURL_INVALID,
                            "provide array callback and item storage");
    translated = request_reader(request, error);
    if (translated != REFERENCE_OK)
        return translated;
    forward.callback = callback;
    forward.userdata = userdata;
    status = knurl_json_schema_from_map(NULL, map, &schema, &native);
    if (status == KNURL_OK)
        status = knurl_request_json_array_each(request->request, selector,
                                               &schema, item, forward_item,
                                               &forward, &native);
    return array_result(status, &native, error);
}
reference_status reference_request_json_into(struct reference_request *request,
                                             const lonejson_map *map,
                                             void *record,
                                             reference_error *error)
{
    lonejson_schema_view schema;
    knurl_error native;
    knurl_status status;
    reference_status translated = request_reader(request, error);
    if (translated != REFERENCE_OK)
        return translated;
    status = knurl_json_schema_from_map(NULL, map, &schema, &native);
    if (status == KNURL_OK)
        status =
            knurl_request_json_into(request->request, &schema, record, &native);
    return reference_error_from_knurl(status, &native, error);
}
reference_status
reference_array_each_source(const struct reference_input_source *in,
                            const char *selector, const lonejson_map *map,
                            void *item, reference_array_item callback,
                            void *userdata, reference_error *error)
{
    struct array_forward forward;
    lonejson_schema_view schema;
    knurl_source *source = NULL;
    knurl_error native;
    knurl_status status;
    if (callback == NULL || item == NULL)
        return reader_error(error, KNURL_INVALID,
                            "provide array callback and item storage");
    forward.callback = callback;
    forward.userdata = userdata;
    status = knurl_json_schema_from_map(NULL, map, &schema, &native);
    if (status == KNURL_OK)
        status = reference_input_open(in, &source, &native);
    if (status != KNURL_OK)
        return reference_error_from_knurl(status, &native, error);
    if (status == KNURL_OK)
        status = knurl_json_array_each_source(source, selector, &schema, item,
                                              forward_item, &forward, &native);
    knurl_source_destroy(source, NULL);
    return array_result(status, &native, error);
}
static reference_status response_source(struct reference_response *r, int code,
                                        const char *type,
                                        struct reference_source *host, int live,
                                        reference_error *error)
{
    knurl_source_config config;
    knurl_source *source;
    knurl_error e;
    knurl_status status;
    struct source_forward *f;
    if (r == NULL || host == NULL || host->read == NULL)
        return reference_error_from_knurl(KNURL_INVALID, NULL, error);
    f = calloc(1, sizeof(*f));
    if (f == NULL)
        return reference_error_from_knurl(KNURL_NOMEM, NULL, error);
    f->host = host;
    knurl_source_config_init(&config);
    config.read = source_read;
    config.reset = host->reset != NULL ? source_reset : NULL;
    config.reset_before_reply = 1;
    config.close = source_close;
    config.userdata = f;
    status = knurl_source_new(&config, &source, &e);
    if (status != KNURL_OK)
        free(f);
    else {
        status = live
                     ? knurl_reply_source_take(r->reply, code, type, source, &e)
                     : knurl_reply_source_spooled(r->reply, code, type, source,
                                                  NULL, &e);
        if (live && status == KNURL_OK)
            f->taken = 1;
        else
            (void)knurl_source_destroy(source, NULL);
    }
    reference_error_from_knurl(status, &e, error);
    if (status != KNURL_OK && error != NULL && e.source == KNURL_ERROR_BACKEND)
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

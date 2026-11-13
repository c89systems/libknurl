#include "vectis-services.h"
#include <stdlib.h>
#include <string.h>

reference_status
reference_static_file(struct reference_app *app,
                      const struct reference_static_file_config *in,
                      reference_error *error)
{
    knurl_static_config c;
    knurl_error e;
    knurl_status s;
    if (app == NULL || in == NULL)
        return reference_error_from_knurl(KNURL_INVALID, NULL, error);
    knurl_static_config_init(&c);
    c.path = in->path;
    c.file_path = in->file_path;
    c.content_type = in->content_type;
    c.methods = in->methods;
    s = knurl_router_static_file(knurl_server_router(app->server), &c, NULL,
                                 &e);
    return reference_error_from_knurl(s, &e, error);
}
reference_status
reference_static_directory(struct reference_app *app,
                           const struct reference_static_directory_config *in,
                           reference_error *error)
{
    knurl_static_config c;
    knurl_error e;
    knurl_status s;
    if (app == NULL || in == NULL)
        return reference_error_from_knurl(KNURL_INVALID, NULL, error);
    knurl_static_config_init(&c);
    c.path = in->path_prefix;
    c.root_dir = in->root_dir;
    c.content_type = in->content_type;
    c.index_file = in->index_file;
    c.index_enabled = in->index_enabled;
    c.methods = in->methods;
    s = knurl_router_static_directory(knurl_server_router(app->server), &c,
                                      NULL, &e);
    return reference_error_from_knurl(s, &e, error);
}
reference_status
reference_static_embedded(struct reference_app *app,
                          const struct reference_static_embedded_config *in,
                          reference_error *error)
{
    knurl_static_config c;
    knurl_error e;
    knurl_status s;
    if (app == NULL || in == NULL || in->fs == NULL)
        return reference_error_from_knurl(KNURL_INVALID, NULL, error);
    knurl_static_config_init(&c);
    c.path = in->path_prefix;
    c.provider = in->fs->provider;
    c.index_enabled = 1;
    c.content_type = in->content_type;
    c.cache_control = in->cache_control;
    c.not_found_body = in->not_found_body;
    c.not_found_content_type = in->not_found_content_type;
    c.methods = in->methods;
    s = knurl_router_static_assets(knurl_server_router(app->server), &c, NULL,
                                   &e);
    return reference_error_from_knurl(s, &e, error);
}
void reference_openapi_document_init(struct reference_openapi_document *doc)
{
    if (doc != NULL) {
        doc->title = "Vectis API";
        doc->version = "0.0.0";
    }
}
reference_status
reference_openapi_request_json(reference_openapi_route_doc *doc,
                               reference_openapi_schema schema,
                               reference_error *error)
{
    knurl_error e;
    knurl_status s = knurl_openapi_request_json(doc, schema, &e);
    return reference_error_from_knurl(s, &e, error);
}
reference_status reference_openapi_response_json(
    reference_openapi_route_doc *doc, int code, const char *description,
    reference_openapi_schema schema, reference_error *error)
{
    knurl_error e;
    knurl_status s =
        knurl_openapi_response_json(doc, code, description, schema, &e);
    return reference_error_from_knurl(s, &e, error);
}
reference_status reference_openapi_attach(
    struct reference_app *app, unsigned int methods, const char *path,
    const reference_openapi_route_doc *doc, reference_error *error)
{
    knurl_error e;
    knurl_status s = knurl_router_openapi_doc(
        app != NULL ? knurl_server_router(app->server) : NULL, methods, path,
        doc, &e);
    return reference_error_from_knurl(s, &e, error);
}
reference_status reference_openapi_generate(
    struct reference_app *app, const struct reference_openapi_document *in,
    int format, struct reference_mutable_bytes *out, reference_error *error)
{
    knurl_openapi_document doc;
    knurl_owned_bytes bytes;
    knurl_error e;
    knurl_status s;
    if (out == NULL)
        return reference_error_from_knurl(KNURL_INVALID, NULL, error);
    out->data = NULL;
    out->size = 0;
    knurl_openapi_document_init(&doc);
    doc.title = in != NULL && in->title != NULL ? in->title : "Vectis API";
    doc.version = in != NULL && in->version != NULL ? in->version : "0.0.0";
    s = knurl_router_openapi(app != NULL ? knurl_server_router(app->server)
                                         : NULL,
                             &doc, (knurl_openapi_format)format, &bytes, &e);
    if (s == KNURL_OK) {
        out->data = bytes.data;
        out->size = bytes.length;
    }
    return reference_error_from_knurl(s, &e, error);
}

struct proxy_string {
    struct proxy_string *next;
    knurl_bytes view;
    char text[1];
};
static void inbound_clear(reference_proxy_inbound *in)
{
    struct proxy_string *s = in->strings, *next;
    while (s != NULL) {
        next = s->next;
        free(s);
        s = next;
    }
}
knurl_http_method
reference_proxy_inbound_method(const reference_proxy_inbound *in)
{
    return knurl_proxy_inbound_method(in->native);
}
const char *reference_proxy_inbound_path(const reference_proxy_inbound *in)
{
    return knurl_proxy_inbound_path(in->native);
}
const char *reference_proxy_inbound_query(const reference_proxy_inbound *in)
{
    return knurl_proxy_inbound_query(in->native);
}
const char *reference_proxy_inbound_host(const reference_proxy_inbound *in)
{
    return knurl_proxy_inbound_host(in->native);
}
int reference_proxy_inbound_websocket(const reference_proxy_inbound *in)
{
    return knurl_proxy_inbound_websocket(in->native);
}
size_t reference_proxy_inbound_header_count(const reference_proxy_inbound *in)
{
    return knurl_proxy_inbound_header_count(in->native);
}
const char *
reference_proxy_inbound_path_param(const reference_proxy_inbound *in,
                                   const char *key)
{
    knurl_bytes value = knurl_proxy_inbound_path_param(in->native, key);
    reference_proxy_inbound *owner = (reference_proxy_inbound *)in;
    struct proxy_string *s;
    if (value.data == NULL)
        return NULL;
    for (s = owner->strings; s != NULL; s = s->next)
        if (s->view.data == value.data && s->view.length == value.length)
            return s->text;
    if (value.length > (size_t)-1 - sizeof(*s))
        return NULL;
    s = malloc(sizeof(*s) + value.length);
    if (!s)
        return NULL;
    s->view = value;
    memcpy(s->text, value.data, value.length);
    s->text[value.length] = 0;
    s->next = owner->strings;
    owner->strings = s;
    return s->text;
}

static knurl_status proxy_rewrite(const knurl_proxy_inbound *in,
                                  knurl_proxy_outbound *out, void *data,
                                  knurl_error *error)
{
    struct reference_proxy_binding *b = data;
    reference_proxy_inbound input;
    reference_error e;
    reference_status status;
    reference_error_clear(&e);
    input.native = in;
    input.strings = NULL;
    status =
        b->callbacks.rewrite(&input, out, b->callbacks.rewrite_userdata, &e);
    inbound_clear(&input);
    return reference_error_to_knurl(status, &e, error);
}
static knurl_status proxy_preflight(const knurl_proxy_inbound *in,
                                    knurl_proxy_local_response *out, void *data,
                                    knurl_error *error)
{
    struct reference_proxy_binding *b = data;
    reference_proxy_inbound input;
    reference_error e;
    reference_status status;
    reference_error_clear(&e);
    input.native = in;
    input.strings = NULL;
    status = b->callbacks.preflight(&input, out,
                                    b->callbacks.preflight_userdata, &e);
    inbound_clear(&input);
    return reference_error_to_knurl(status, &e, error);
}
static knurl_status proxy_response(knurl_proxy_response *out, void *data,
                                   knurl_error *error)
{
    struct reference_proxy_binding *b = data;
    reference_error e;
    reference_status status;
    reference_error_clear(&e);
    status = b->callbacks.modify_response(
        out, b->callbacks.modify_response_userdata, &e);
    return reference_error_to_knurl(status, &e, error);
}
static knurl_status proxy_error(const knurl_error *cause, int code,
                                knurl_proxy_local_response *out, void *data,
                                knurl_error *error)
{
    struct reference_proxy_binding *b = data;
    reference_error input, e;
    reference_status status;
    (void)reference_error_from_knurl(cause->status, cause, &input);
    reference_error_clear(&e);
    status = b->callbacks.on_error(&input, code, out,
                                   b->callbacks.on_error_userdata, &e);
    return reference_error_to_knurl(status, &e, error);
}
static void proxy_exchange(knurl_request *request,
                           const knurl_proxy_exchange *event, void *data)
{
    struct reference_proxy_binding *b = data;
    b->callbacks.on_exchange(knurl_request_userdata(request), event,
                             b->callbacks.on_exchange_userdata);
}
reference_status reference_proxy_register(
    struct reference_app *app, const struct reference_proxy_config *in,
    struct reference_proxy_binding *b, reference_error *error)
{
    knurl_proxy_config c;
    knurl_error e;
    knurl_status status;
    if (app == NULL || in == NULL || b == NULL)
        return reference_error_from_knurl(KNURL_INVALID, NULL, error);
    b->callbacks = *in;
    knurl_proxy_config_init(&c);
#define COPY(field) c.field = in->field
    COPY(path);
    COPY(methods);
    c.path_kind = (knurl_route_path_kind)in->path_kind;
    COPY(target);
    COPY(alternate_targets);
    COPY(alternate_target_count);
    c.upstream_http_version =
        (knurl_proxy_http_version)in->upstream_http_version;
    COPY(connect_timeout_ms);
    COPY(idle_timeout_ms);
    COPY(total_timeout_ms);
    COPY(buffer_limit_bytes);
    COPY(tls_ca_pem);
    COPY(tls_client_cert_pem);
    COPY(tls_client_key_pem);
#undef COPY
    c.rewrite = in->rewrite ? proxy_rewrite : NULL;
    c.rewrite_userdata = b;
    c.preflight = in->preflight ? proxy_preflight : NULL;
    c.preflight_userdata = b;
    c.modify_response = in->modify_response ? proxy_response : NULL;
    c.modify_response_userdata = b;
    c.on_error = in->on_error ? proxy_error : NULL;
    c.on_error_userdata = b;
    c.on_exchange = in->on_exchange ? proxy_exchange : NULL;
    c.on_exchange_userdata = b;
    status = knurl_router_proxy(knurl_server_router(app->server), &c, NULL, &e);
    return reference_error_from_knurl(status, &e, error);
}
#define PROXY_EDIT(name, type, argtype)                                        \
    reference_status reference_proxy_##name(type *view, argtype value,         \
                                            reference_error *error)            \
    {                                                                          \
        knurl_error e;                                                         \
        knurl_status s = knurl_proxy_##name(view, value, &e);                  \
        return reference_error_from_knurl(s, &e, error);                       \
    }
PROXY_EDIT(outbound_select_target, reference_proxy_outbound, size_t)
PROXY_EDIT(outbound_set_method, reference_proxy_outbound, knurl_http_method)
PROXY_EDIT(outbound_set_path, reference_proxy_outbound, const char *)
PROXY_EDIT(outbound_set_query, reference_proxy_outbound, const char *)
PROXY_EDIT(outbound_set_host, reference_proxy_outbound, const char *)
PROXY_EDIT(outbound_remove_header, reference_proxy_outbound, const char *)
PROXY_EDIT(response_set_status, reference_proxy_response, int)
PROXY_EDIT(response_remove_header, reference_proxy_response, const char *)
#undef PROXY_EDIT
#define PROXY_HEADER(name, type)                                               \
    reference_status reference_proxy_##name(type *view, const char *name,      \
                                            const char *value,                 \
                                            reference_error *error)            \
    {                                                                          \
        knurl_error e;                                                         \
        knurl_status s = knurl_proxy_##name(view, name, value, &e);            \
        return reference_error_from_knurl(s, &e, error);                       \
    }
PROXY_HEADER(outbound_add_header, reference_proxy_outbound)
PROXY_HEADER(outbound_set_header, reference_proxy_outbound)
PROXY_HEADER(response_add_header, reference_proxy_response)
PROXY_HEADER(response_set_header, reference_proxy_response)
PROXY_HEADER(local_add_header, reference_proxy_local_response)
#undef PROXY_HEADER
reference_status
reference_proxy_local_respond(reference_proxy_local_response *out, int code,
                              const void *body, size_t size,
                              reference_error *error)
{
    knurl_error e;
    knurl_status s = knurl_proxy_local_respond(out, code, body, size, &e);
    return reference_error_from_knurl(s, &e, error);
}
reference_status
reference_proxy_inbound_header_at(const reference_proxy_inbound *in, size_t i,
                                  const char **name, const char **value)
{
    return reference_error_from_knurl(
        knurl_proxy_inbound_header_at(in->native, i, name, value), NULL, NULL);
}
reference_status
reference_proxy_response_header_at(const reference_proxy_response *in, size_t i,
                                   const char **name, const char **value)
{
    return reference_error_from_knurl(
        knurl_proxy_response_header_at(in, i, name, value), NULL, NULL);
}

static void body_policy(const struct reference_route_config *in,
                        knurl_body_policy *out)
{
    out->mode = (knurl_body_mode)in->body.mode;
    (void)knurl_u64_from_size(in->body.max_bytes, &out->max_bytes, NULL);
    out->memory_buffer_limit_bytes = in->body.memory_buffer_limit_bytes;
    out->disk_spool_disabled = in->body.disk_spool_disabled;
}
static knurl_status upload_open(knurl_request *request, void *data,
                                void **state, knurl_error *error)
{
    struct reference_upload_binding *b = data;
    reference_error e;
    reference_status s;
    reference_error_clear(&e);
    s = b->config.open(b->app, knurl_request_userdata(request),
                       b->config.userdata, state, &e);
    return reference_error_to_knurl(s, &e, error);
}
static knurl_status upload_write(knurl_request *request, const void *bytes,
                                 size_t size, size_t *consumed, void *state,
                                 void *data, knurl_error *error)
{
    struct reference_upload_binding *b = data;
    reference_error e;
    reference_status s;
    reference_error_clear(&e);
    s = b->config.write(b->app, knurl_request_userdata(request), bytes, size,
                        state, b->config.userdata, &e);
    *consumed = s == REFERENCE_OK ? size : 0;
    return reference_error_to_knurl(s, &e, error);
}
static knurl_status upload_finish(knurl_request *request, knurl_reply *reply,
                                  void *state, void *data, knurl_error *error)
{
    struct reference_upload_binding *b = data;
    struct reference_response response;
    reference_error e;
    reference_status s;
    reference_error_clear(&e);
    response.reply = reply;
    s = b->config.finish(b->app, knurl_request_userdata(request), &response,
                         state, b->config.userdata, &e);
    return reference_error_to_knurl(s, &e, error);
}
static void upload_close(knurl_request *request, knurl_status cause,
                         void *state, void *data)
{
    struct reference_upload_binding *b = data;
    (void)cause;
    b->config.close(b->app, knurl_request_userdata(request), state,
                    b->config.userdata);
}
struct reader_owner {
    struct reference_upload_binding *binding;
    struct reference_request *request;
};
static knurl_status reader_acquire(knurl_request *request,
                                   const knurl_upload_metadata *metadata,
                                   void *data, void **out, knurl_error *error)
{
    struct reader_owner *owner = malloc(sizeof(*owner));
    knurl_status status;
    void *retained = NULL;
    if (owner == NULL)
        return KNURL_NOMEM;
    status = reference_request_acquire(request, metadata, &retained, error);
    if (status != KNURL_OK) {
        free(owner);
        return status;
    }
    owner->binding = data;
    owner->request = retained;
    *out = owner;
    return KNURL_OK;
}
static void reader_release(void *data)
{
    struct reader_owner *owner = data;
    reference_request_release(owner->request);
    free(owner);
}
struct reader_view {
    struct reference_source source;
    knurl_source *native;
    knurl_error error;
};
static size_t reader_read(struct reference_source *source, void *bytes,
                          size_t capacity, struct reference_source_error *error)
{
    struct reader_view *v = source->impl;
    size_t count = 0;
    knurl_status s =
        knurl_source_read(v->native, bytes, capacity, &count, &v->error);
    error->code = s == KNURL_OK || s == KNURL_EOF ? 0
                  : s == KNURL_NOMEM              ? 2
                                                  : 3;
    error->message = error->code ? v->error.message : NULL;
    return count;
}
static knurl_status upload_reader(const knurl_upload_metadata *metadata,
                                  knurl_source *reader, knurl_reply *reply,
                                  void *data, knurl_error *error)
{
    struct reader_owner *owner = data;
    struct reference_upload_binding *b = owner->binding;
    struct reference_response response;
    struct reader_view view;
    reference_error e;
    reference_status s;
    (void)metadata;
    memset(&view, 0, sizeof(view));
    view.native = reader;
    view.source.read = reader_read;
    view.source.impl = &view;
    response.reply = reply;
    reference_error_clear(&e);
    s = b->config.reader(b->app, owner->request, &view.source, &response,
                         b->config.userdata, &e);
    return reference_error_to_knurl(s, &e, error);
}
reference_status reference_upload_register(
    struct reference_app *app, const struct reference_upload_config *in,
    struct reference_upload_binding *b, reference_error *error)
{
    knurl_error e;
    knurl_status status;
    if (app == NULL || in == NULL || b == NULL)
        return reference_error_from_knurl(KNURL_INVALID, NULL, error);
    b->app = app;
    b->config = *in;
    if (in->reader != NULL) {
        knurl_upload_reader_config c;
        knurl_upload_reader_config_init(&c);
        c.method = (knurl_http_method)in->route.method;
        c.methods = in->route.methods;
        c.path = in->route.path;
        c.path_kind = (knurl_route_path_kind)in->route.path_kind;
        body_policy(&in->route, &c.body);
        c.buffer_bytes = in->buffer_bytes;
        c.handler = upload_reader;
        c.userdata = b;
        c.acquire = reader_acquire;
        c.release = reader_release;
        status = knurl_router_upload_reader(knurl_server_router(app->server),
                                            &c, NULL, &e);
    } else {
        knurl_upload_stream_config c;
        knurl_upload_stream_config_init(&c);
        c.method = (knurl_http_method)in->route.method;
        c.methods = in->route.methods;
        c.path = in->route.path;
        c.path_kind = (knurl_route_path_kind)in->route.path_kind;
        body_policy(&in->route, &c.body);
        c.open = in->open ? upload_open : NULL;
        c.write = in->write ? upload_write : NULL;
        c.finish = in->finish ? upload_finish : NULL;
        c.close = in->close ? upload_close : NULL;
        c.userdata = b;
        status = knurl_router_upload_stream(knurl_server_router(app->server),
                                            &c, NULL, &e);
    }
    return reference_error_from_knurl(status, &e, error);
}
reference_status
reference_upload_file_register(struct reference_app *app,
                               const struct reference_route_config *in,
                               const char *path, reference_error *error)
{
    knurl_upload_file_config c;
    knurl_error e;
    knurl_status s;
    if (app == NULL || in == NULL)
        return reference_error_from_knurl(KNURL_INVALID, NULL, error);
    knurl_upload_file_config_init(&c);
    c.method = (knurl_http_method)in->method;
    c.methods = in->methods;
    c.path = in->path;
    c.path_kind = (knurl_route_path_kind)in->path_kind;
    body_policy(in, &c.body);
    c.file_path = path;
    s = knurl_router_upload_file(knurl_server_router(app->server), &c, NULL,
                                 &e);
    return reference_error_from_knurl(s, &e, error);
}
static knurl_status ws_open(knurl_websocket *ws, void *data, knurl_error *error)
{
    struct reference_websocket_binding *b = data;
    (void)error;
    if (b->config.connect)
        b->config.connect(b->app, ws, b->config.userdata);
    return KNURL_OK;
}
static knurl_status ws_message(knurl_websocket *ws, knurl_websocket_opcode op,
                               int final, knurl_bytes bytes, void *data,
                               knurl_error *error)
{
    struct reference_websocket_binding *b = data;
    (void)error;
    if (!final || op == KNURL_WEBSOCKET_CONTINUATION)
        return KNURL_NOT_SUPPORTED;
    b->config.message(b->app, ws, (int)op, bytes.data, bytes.length,
                      b->config.userdata);
    return KNURL_OK;
}
static void ws_close(knurl_websocket *ws, const knurl_websocket_outcome *out,
                     void *data)
{
    struct reference_websocket_binding *b = data;
    (void)out;
    if (b->config.disconnect)
        b->config.disconnect(b->app, ws, b->config.userdata);
}
reference_status reference_websocket_register(
    struct reference_app *app, const struct reference_websocket_config *in,
    struct reference_websocket_binding *b, reference_error *error)
{
    knurl_websocket_route_config c;
    knurl_error e;
    knurl_status s;
    knurl_server_effective effective;
    if (!app || !in || !b || !in->message)
        return reference_error_from_knurl(KNURL_INVALID, NULL, error);
    b->app = app;
    b->config = *in;
    knurl_websocket_route_config_init(&c);
    c.path = in->path;
    c.path_kind = (knurl_route_path_kind)in->path_kind;
    s = knurl_server_effective_config(app->server, &effective, &e);
    if (s != KNURL_OK)
        return reference_error_from_knurl(s, &e, error);
    c.websocket.max_frame_bytes =
        effective.config.limits.websocket_max_frame_bytes;
    c.websocket.on_open = ws_open;
    c.websocket.on_frame = ws_message;
    c.websocket.on_close = ws_close;
    c.websocket.userdata = b;
    s = knurl_router_websocket(knurl_server_router(app->server), &c, NULL, &e);
    return reference_error_from_knurl(s, &e, error);
}
reference_status reference_websocket_send(reference_websocket *ws, int opcode,
                                          const void *data, size_t size,
                                          reference_error *error)
{
    knurl_error e;
    knurl_status s = knurl_websocket_send(ws, (knurl_websocket_opcode)opcode,
                                          data, size, &e);
    return reference_error_from_knurl(s, &e, error);
}
reference_status reference_websocket_close(reference_websocket *ws,
                                           reference_error *error)
{
    return reference_websocket_send(ws, KNURL_WEBSOCKET_CLOSE, NULL, 0, error);
}

static knurl_status dav_auth(const knurl_webdav_auth_request *in,
                             knurl_webdav_auth_response *out, void *data,
                             knurl_error *error)
{
    struct reference_webdav_binding *b = data;
    struct reference_webdav_auth_request request;
    struct reference_webdav_auth_response *response = &b->response;
    reference_error e;
    reference_status s;
    memset(response, 0, sizeof(*response));
    reference_error_clear(&e);
    request.request = knurl_request_userdata(in->request);
    request.method = in->method;
    request.mount_path_prefix = in->mount_path_prefix;
    request.resource_path = in->resource_path;
    s = b->config.auth(&request, response, b->config.auth_userdata, &e);
    if (s != REFERENCE_OK)
        return reference_error_to_knurl(s, &e, error);
    out->action = (knurl_webdav_auth_action)response->action;
    out->status = response->status_code;
    out->principal = response->principal;
    out->location = response->location;
    out->www_authenticate = response->www_authenticate;
    out->content_type = response->content_type;
    out->body.data = response->body;
    out->body.length = response->body_size;
    return KNURL_OK;
}
reference_status reference_webdav_register(
    struct reference_app *app, const struct reference_webdav_config *in,
    struct reference_webdav_binding *b, knurl_storage_provider *provider,
    reference_error *error)
{
    knurl_webdav_config c;
    knurl_error e;
    knurl_status s;
    if (!app || !in || !b)
        return reference_error_from_knurl(KNURL_INVALID, NULL, error);
    b->config = *in;
    knurl_webdav_config_init(&c);
    c.path_prefix = in->path_prefix;
    c.storage = in->storage;
    (void)knurl_u64_from_size(in->storage.max_file_bytes, &c.max_body_bytes,
                              NULL);
    c.operation_timeout_disabled = 1;
    c.provider = provider;
    c.auth_disabled = !in->auth_required;
    c.conceal_disabled = !in->conceal_unauthorized;
    c.auth = in->auth ? dav_auth : NULL;
    c.auth_userdata = b;
    s = knurl_router_webdav(knurl_server_router(app->server), &c, NULL, &e);
    return reference_error_from_knurl(s, &e, error);
}
static knurl_status tls_open(knurl_source **out, void *data, knurl_error *error)
{
    return reference_source_borrow(data, out, error);
}
static void tls_material(const struct reference_input_source *in,
                         knurl_tls_material *out)
{
    knurl_tls_material_init(out);
    if (in->path != NULL) {
        out->kind = KNURL_MATERIAL_FILE;
        out->path = in->path;
    } else if (in->memory != NULL && in->memory_size != 0) {
        out->kind = KNURL_MATERIAL_MEMORY;
        out->bytes.data = in->memory;
        out->bytes.length = in->memory_size;
    } else if (in->source != NULL) {
        out->kind = KNURL_MATERIAL_CALLBACK;
        out->open = tls_open;
        out->userdata = in->source;
    }
}
static knurl_status tls_state(knurl_server *server,
                              const knurl_process_context *process,
                              knurl_acme_event event, knurl_bytes domain,
                              knurl_bytes directory, void *data,
                              knurl_error *error)
{
    struct reference_tls_binding *b = data;
    reference_error e;
    reference_status status;
    reference_acme_state_fn call;
    (void)server;
    (void)process;
    (void)domain;
    reference_error_clear(&e);
    if (event == KNURL_ACME_ERROR) {
        if (b->state.error != NULL) {
            (void)reference_error_from_knurl(error->status, error, &e);
            b->state.error(&e, b->state.userdata);
        }
        return KNURL_OK;
    }
    call = event == KNURL_ACME_RESTORE ? b->state.hydrate : b->state.persist;
    if (call == NULL) {
        strcpy(e.message, "provide host hydration and persistence callbacks");
        return reference_error_to_knurl(REFERENCE_INVALID, &e, error);
    }
    status = call(directory.data, b->domains, b->domain_count,
                  b->state.userdata, &e);
    return reference_error_to_knurl(status, &e, error);
}
reference_status reference_tls_translate(const struct reference_tls_config *in,
                                         knurl_server_config *out,
                                         struct reference_tls_binding *b,
                                         reference_error *error)
{
    if (!in || !out || !b)
        return reference_error_from_knurl(KNURL_INVALID, NULL, error);
    memset(b, 0, sizeof(*b));
    knurl_tls_identity_init(&b->identity);
    b->identity.mode = (knurl_tls_mode)in->mode;
    b->manual_domain = in->domain ? in->domain : "*";
    b->identity.domains =
        in->mode == KNURL_TLS_ACME ? in->domains : &b->manual_domain;
    b->identity.domain_count =
        in->mode == KNURL_TLS_ACME ? in->domain_count : 1;
    tls_material(&in->bundle, &b->identity.bundle);
    tls_material(&in->certificate, &b->identity.certificate);
    tls_material(&in->private_key, &b->identity.private_key);
    tls_material(&in->ca_bundle, &b->identity.extra_chain);
    tls_material(&in->client_ca_bundle, &b->identity.client_ca);
    b->identity.require_client_certificate = in->require_client_certificate;
    knurl_listener_config_init(&b->listeners[0]);
    b->listeners[0].bind = in->bind;
    b->listeners[0].port = in->port;
    b->listeners[0].kind = in->mode == KNURL_TLS_DISABLED
                               ? KNURL_LISTENER_HTTP
                               : KNURL_LISTENER_HTTPS;
    out->listeners = b->listeners;
    out->listener_count = 1;
    out->tls.version = (knurl_tls_version)in->version;
    out->tls.cipher_list = in->cipher_list;
    out->tls.identities = in->mode == KNURL_TLS_DISABLED ? NULL : &b->identity;
    out->tls.identity_count = in->mode == KNURL_TLS_DISABLED ? 0 : 1;
    out->tls.acme.email = in->acme_email;
    out->tls.acme.directory_url = in->acme_directory_url;
    out->tls.acme.runtime_directory = in->acme_state_dir;
    b->state = in->state;
    b->domains = in->domains;
    b->domain_count = in->domain_count;
    out->tls.acme.state = in->mode == KNURL_TLS_ACME ? tls_state : NULL;
    out->tls.acme.userdata = b;
    out->tls.acme.persist_timeout_ms = in->persist_timeout_ms;
    if (in->http_redirect_enabled && in->mode != KNURL_TLS_DISABLED) {
        knurl_listener_config_init(&b->listeners[1]);
        b->listeners[1].kind = KNURL_LISTENER_REDIRECT;
        b->listeners[1].bind =
            in->http_redirect_bind ? in->http_redirect_bind : in->bind;
        b->listeners[1].port =
            in->http_redirect_port ? in->http_redirect_port : 8080;
        b->listeners[1].redirect_https_port =
            in->presented_https_port ? in->presented_https_port : in->port;
        out->listener_count = 2;
    }
    return reference_error_from_knurl(KNURL_OK, NULL, error);
}

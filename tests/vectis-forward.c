#define _POSIX_C_SOURCE 200809L
#include "vectis-forward.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

reference_status reference_json_validate_cstr(const char *json,
                                              reference_error *error)
{
    knurl_error native;
    knurl_status status = knurl_json_validate_cstr(json, &native);
    return reference_error_from_knurl(status, &native, error);
}
static reference_status invalid(reference_error *error, const char *message)
{
    reference_error_clear(error);
    if (error != NULL) {
        error->code = REFERENCE_INVALID;
        error->source = 1;
        (void)snprintf(error->message, sizeof(error->message), "%s", message);
    }
    return REFERENCE_INVALID;
}
static knurl_status xml_sink(const void *bytes, size_t length, void *data,
                             knurl_error *error)
{
    struct reference_sink *sink = data;
    struct reference_source_error cause = {0, NULL};
    if (sink->write(sink, bytes, length, &cause))
        return KNURL_OK;
    knurl_error_clear(error);
    error->status = KNURL_IO;
    error->source = KNURL_ERROR_SYSTEM;
    error->native_code = cause.code;
    (void)snprintf(error->message, sizeof(error->message), "%s",
                   cause.message != NULL ? cause.message
                                         : "failed to write XML output");
    return KNURL_IO;
}
reference_status reference_xml_parse_source(
    const struct reference_input_source *in, const lonejson_map *map,
    const reference_xml_options *options, void *record, reference_error *error)
{
    lonejson_schema_view schema;
    knurl_source *source = NULL;
    knurl_error native;
    knurl_status status;
    if (record == NULL || map == NULL)
        return invalid(error, "XML map and output struct are required");
    status = knurl_json_schema_from_map(NULL, map, &schema, &native);
    if (status == KNURL_OK)
        status = reference_input_open(in, &source, &native);
    if (status == KNURL_OK) {
        /* Baseline parse accepts fresh, uninitialized mapped storage. */
        if (lonejson_init(NULL, map, record, NULL) != LONEJSON_STATUS_OK)
            abort();
        status =
            knurl_xml_parse_source(source, &schema, options, record, &native);
    }
    knurl_source_destroy(source, NULL);
    return reference_error_from_knurl(status, &native, error);
}
reference_status reference_xml_parse(struct reference_source *source,
                                     const lonejson_map *map,
                                     const reference_xml_options *options,
                                     void *record, reference_error *error)
{
    struct reference_input_source in;
    memset(&in, 0, sizeof(in));
    in.source = source;
    return reference_xml_parse_source(&in, map, options, record, error);
}
static knurl_status xml_view(const lonejson_map *map, const void *record,
                             lonejson_record_view *view, knurl_error *error)
{
    memset(view, 0, sizeof(*view));
    view->size = sizeof(*view);
    view->abi_version = LONEJSON_VIEW_ABI_VERSION;
    view->record = (void *)record;
    return knurl_json_schema_from_map(NULL, map, &view->schema, error);
}
reference_status reference_xml_write(struct reference_sink *sink,
                                     const lonejson_map *map,
                                     const reference_xml_options *options,
                                     const void *record, reference_error *error)
{
    lonejson_record_view view;
    knurl_error native;
    knurl_status status;
    if (sink == NULL || sink->write == NULL || record == NULL)
        return invalid(error, "XML sink and value are required");
    status = xml_view(map, record, &view, &native);
    if (status == KNURL_OK)
        status = knurl_xml_write_sink(&view, options, xml_sink, sink, &native);
    return reference_error_from_knurl(status, &native, error);
}
reference_status reference_xml_bytes(const lonejson_map *map,
                                     const reference_xml_options *options,
                                     const void *record,
                                     struct reference_mutable_bytes *out,
                                     reference_error *error)
{
    lonejson_record_view view;
    knurl_owned_bytes bytes = {NULL, 0};
    knurl_error native;
    knurl_status status;
    void *terminated;
    if (out == NULL || record == NULL)
        return invalid(error, "XML value and byte output are required");
    out->data = NULL;
    out->size = 0;
    status = xml_view(map, record, &view, &native);
    if (status == KNURL_OK)
        status = knurl_xml_serialize_materialized(&view, options, 0, &bytes,
                                                  &native);
    if (status != KNURL_OK)
        return reference_error_from_knurl(status, &native, error);
    /* Vectis's byte value adds a terminator outside its public size. */
    terminated = bytes.length < (size_t)-1
                     ? realloc(bytes.data, bytes.length + 1)
                     : NULL;
    if (terminated == NULL) {
        knurl_owned_bytes_cleanup(&bytes);
        reference_error_clear(error);
        if (error != NULL) {
            error->code = REFERENCE_NOMEM;
            error->source = 1;
            strcpy(error->message, "failed to allocate XML bytes");
        }
        return REFERENCE_NOMEM;
    }
    ((char *)terminated)[bytes.length] = 0;
    out->data = terminated;
    out->size = bytes.length;
    reference_error_clear(error);
    return REFERENCE_OK;
}
static knurl_status rewrite_sink(const void *bytes, size_t length, void *data,
                                 knurl_error *error)
{
    struct reference_sink *sink = data;
    struct reference_source_error cause = {0, NULL};
    if (sink->write(sink, bytes, length, &cause))
        return KNURL_OK;
    knurl_error_clear(error);
    error->status = KNURL_INVALID;
    error->source = KNURL_ERROR_LONEJSON;
    error->native_code = (long)LONEJSON_STATUS_IO_ERROR;
    (void)snprintf(error->message, sizeof(error->message), "%s",
                   cause.message != NULL ? cause.message
                                         : "rewrite sink failed");
    return KNURL_INVALID;
}
reference_status reference_array_rewrite_source(
    const struct reference_input_source *in, const char *selector,
    struct reference_sink *sink, const lonejson_array_rewrite_options *options,
    reference_error *error)
{
    knurl_source *source = NULL;
    knurl_error native;
    knurl_status status;
    if (sink == NULL || sink->write == NULL || options == NULL)
        return invalid(error, "provide JSON rewrite sink and options");
    status = reference_input_open(in, &source, &native);
    if (status != KNURL_OK)
        return reference_error_from_knurl(status, &native, error);
    if (status == KNURL_OK)
        status = knurl_json_array_rewrite_source(
            source, selector, NULL, options, rewrite_sink, sink, &native);
    knurl_source_destroy(source, NULL);
    if (status == KNURL_IO) {
        status = native.status = KNURL_INVALID;
        native.source = KNURL_ERROR_LONEJSON;
        native.native_code = (long)LONEJSON_STATUS_IO_ERROR;
    }
    if (status != KNURL_OK && native.source == KNURL_ERROR_LONEJSON &&
        strcmp(native.operation, "json_runtime"))
        status = native.status = KNURL_INVALID;
    return reference_error_from_knurl(status, &native, error);
}
/* Vectis's declared record-size validation; this is not an encoded-output cap.
 * JSON permits absent maps. XML requires a map. Core schema views remain the
 * dependency-owned public representation; no codec or record allocation here.
 */
static reference_status schema(struct reference_typed_route *host,
                               const lonejson_map *map, size_t size,
                               const char *label, int required,
                               lonejson_schema_view *out,
                               reference_error *error)
{
    knurl_error e;
    knurl_status s;
    char message[128];
    memset(out, 0, sizeof(*out));
    if (map == NULL && !required)
        return REFERENCE_OK;
    if (map == NULL)
        (void)snprintf(message, sizeof(message), "%s map is required", label);
    else if (size == 0 || size > 10485760UL)
        (void)snprintf(message, sizeof(message), "%s%s is invalid", label,
                       required ? " size" : "_size");
    else if (size != map->struct_size)
        (void)snprintf(message, sizeof(message),
                       "%s%s does not match lonejson map", label,
                       required ? " size" : "_size");
    else {
        s = knurl_json_schema_from_map(host->runtime, map, out, &e);
        return reference_error_from_knurl(s, &e, error);
    }
    return invalid(error, message);
}
static knurl_status body(const struct reference_body_policy *in,
                         knurl_body_policy *out, knurl_error *error)
{
    out->mode = (knurl_body_mode)in->mode;
    out->memory_buffer_limit_bytes = in->memory_buffer_limit_bytes;
    out->disk_spool_disabled = in->disk_spool_disabled;
    return knurl_u64_from_size(in->max_bytes, &out->max_bytes, error);
}
static knurl_status forward(knurl_request *request,
                            const lonejson_record_view *input,
                            const lonejson_record_view *output, void *data,
                            knurl_error *error)
{
    struct reference_typed_route *route = data;
    reference_error e;
    reference_status s;
    reference_error_clear(&e);
    s = route->json(route->app, knurl_request_userdata(request),
                    input != NULL ? input->record : NULL,
                    output != NULL ? output->record : NULL, route->userdata,
                    &e);
    return reference_error_to_knurl(s, &e, error);
}
static knurl_status forward_typed(knurl_request *request,
                                  const lonejson_record_view *input,
                                  knurl_reply *reply, void *data,
                                  knurl_error *error)
{
    struct reference_typed_route *route = data;
    struct reference_response response;
    reference_error e;
    reference_status s;
    reference_error_clear(&e);
    response.reply = reply;
    s = route->typed(route->app, knurl_request_userdata(request),
                     input != NULL ? input->record : NULL, &response,
                     route->userdata, &e);
    return reference_error_to_knurl(s, &e, error);
}
reference_status reference_json_register(struct reference_typed_route *host,
                                         const struct reference_json_config *in,
                                         reference_error *error)
{
    knurl_json_route_config c;
    knurl_error e;
    knurl_status s;
    reference_status translated;
    if (host == NULL || host->app == NULL || in == NULL || in->handler == NULL)
        return invalid(error, "provide app, json route and handler");
    knurl_json_route_config_init(&c);
    translated = schema(host, in->input_map, in->input_size, "json route input",
                        0, &c.input, error);
    if (translated == REFERENCE_OK)
        translated = schema(host, in->output_map, in->output_size,
                            "json route output", 0, &c.output, error);
    if (translated != REFERENCE_OK)
        return translated;
    c.method = (knurl_http_method)in->method;
    c.methods = in->methods;
    c.path = in->path;
    c.path_kind = (knurl_route_path_kind)in->path_kind;
    s = body(&in->body, &c.body, &e);
    host->json = in->handler;
    host->userdata = in->userdata;
    c.handler = forward;
    c.userdata = host;
    c.hooks = host->hooks;
    c.records = host->records;
    if (s == KNURL_OK)
        s = knurl_router_json_route(knurl_server_router(host->app->server), &c,
                                    NULL, &e);
    return reference_error_from_knurl(s, &e, error);
}
reference_status
reference_json_typed_register(struct reference_typed_route *host,
                              const struct reference_json_typed_config *in,
                              reference_error *error)
{
    knurl_json_typed_route_config c;
    knurl_error e;
    knurl_status s;
    reference_status translated;
    if (host == NULL || host->app == NULL || in == NULL || in->handler == NULL)
        return invalid(error, "provide app, typed json route and handler");
    knurl_json_typed_route_config_init(&c);
    translated = schema(host, in->input_map, in->input_size,
                        "typed json route input", 0, &c.input, error);
    if (translated != REFERENCE_OK)
        return translated;
    c.method = (knurl_http_method)in->method;
    c.methods = in->methods;
    c.path = in->path;
    c.path_kind = (knurl_route_path_kind)in->path_kind;
    s = body(&in->body, &c.body, &e);
    host->typed = in->handler;
    host->userdata = in->userdata;
    c.handler = forward_typed;
    c.userdata = host;
    c.hooks = host->hooks;
    c.records = host->records;
    if (s == KNURL_OK)
        s = knurl_router_json_typed_route(
            knurl_server_router(host->app->server), &c, NULL, &e);
    return reference_error_from_knurl(s, &e, error);
}
reference_status reference_xml_register(struct reference_typed_route *host,
                                        const struct reference_xml_config *in,
                                        reference_error *error)
{
    knurl_xml_route_config c;
    lonejson_schema_view input;
    knurl_error e;
    knurl_status s;
    reference_status translated;
    if (host == NULL || host->app == NULL || in == NULL || in->handler == NULL)
        return invalid(error, "provide app, XML route and handler");
    knurl_xml_route_config_init(&c);
    translated = schema(host, in->input_map, in->input_size, "XML route input",
                        1, &input, error);
    if (translated != REFERENCE_OK)
        return translated;
    c.method = (knurl_http_method)in->method;
    c.methods = in->methods;
    c.path = in->path;
    c.path_kind = (knurl_route_path_kind)in->path_kind;
    c.buffer_bytes = in->buffer_bytes;
    c.input = &input;
    c.xml = in->config;
    s = body(&in->body, &c.body, &e);
    host->typed = in->handler;
    host->userdata = in->userdata;
    c.handler = forward_typed;
    c.userdata = host;
    c.hooks = host->hooks;
    c.records = host->records;
    if (s == KNURL_OK)
        s = knurl_router_xml_route(knurl_server_router(host->app->server), &c,
                                   NULL, &e);
    return reference_error_from_knurl(s, &e, error);
}
static reference_status json_response(struct reference_response *response,
                                      int status, const lonejson_map *map,
                                      const void *record, int generated,
                                      reference_error *error)
{
    lonejson_schema_view view;
    knurl_error e;
    knurl_status s;
    if (response == NULL)
        return invalid(error, "response is required");
    s = knurl_json_schema_from_map(NULL, map, &view, &e);
    if (s == KNURL_OK) {
        if (generated)
            s = knurl_reply_json_spooled(response->reply, status, &view, record,
                                         NULL, &e);
        else
            s = knurl_reply_json_materialized(response->reply, status, &view,
                                              record, 0, &e);
    }
    return reference_error_from_knurl(s, &e, error);
}
reference_status reference_json_reply(struct reference_response *response,
                                      int status, const lonejson_map *map,
                                      const void *record,
                                      reference_error *error)
{
    return json_response(response, status, map, record, 0, error);
}
reference_status
reference_response_json_generated(struct reference_response *response,
                                  int status, const lonejson_map *map,
                                  const void *record, reference_error *error)
{
    return json_response(response, status, map, record, 1, error);
}
reference_status
reference_json_reply_status(struct reference_response *response, int status,
                            reference_error *error)
{
    return reference_response_status(response, status, error);
}

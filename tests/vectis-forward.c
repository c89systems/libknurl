#define _POSIX_C_SOURCE 200809L
#include "vectis-forward.h"
#include <stdio.h>
#include <string.h>

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
static rotmos_status rewrite_sink(const void *bytes, size_t length, void *data,
                                  rotmos_error *error)
{
    struct reference_sink *sink = data;
    struct reference_source_error cause = {0, NULL};
    if (sink->write(sink, bytes, length, &cause))
        return ROTMOS_OK;
    rotmos_error_clear(error);
    error->status = ROTMOS_INVALID;
    error->source = ROTMOS_ERROR_LONEJSON;
    error->native_code = (long)LONEJSON_STATUS_IO_ERROR;
    (void)snprintf(error->message, sizeof(error->message), "%s",
                   cause.message != NULL ? cause.message
                                         : "rewrite sink failed");
    return ROTMOS_INVALID;
}
reference_status reference_array_rewrite_source(
    const struct reference_input_source *in, const char *selector,
    struct reference_sink *sink, const lonejson_array_rewrite_options *options,
    reference_error *error)
{
    rotmos_source *source = NULL;
    rotmos_error native;
    rotmos_status status;
    if (sink == NULL || sink->write == NULL || options == NULL)
        return invalid(error, "provide JSON rewrite sink and options");
    status = reference_input_open(in, &source, &native);
    if (status != ROTMOS_OK)
        return reference_error_from_rotmos(status, &native, error);
    if (status == ROTMOS_OK)
        status = rotmos_json_array_rewrite_source(
            source, selector, NULL, options, rewrite_sink, sink, &native);
    rotmos_source_destroy(source, NULL);
    if (status == ROTMOS_IO) {
        status = native.status = ROTMOS_INVALID;
        native.source = ROTMOS_ERROR_LONEJSON;
        native.native_code = (long)LONEJSON_STATUS_IO_ERROR;
    }
    if (status != ROTMOS_OK && native.source == ROTMOS_ERROR_LONEJSON &&
        strcmp(native.operation, "json_runtime"))
        status = native.status = ROTMOS_INVALID;
    return reference_error_from_rotmos(status, &native, error);
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
    rotmos_error e;
    rotmos_status s;
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
        s = rotmos_json_schema_from_map(host->runtime, map, out, &e);
        return reference_error_from_rotmos(s, &e, error);
    }
    return invalid(error, message);
}
static rotmos_status body(const struct reference_body_policy *in,
                          rotmos_body_policy *out, rotmos_error *error)
{
    out->mode = (rotmos_body_mode)in->mode;
    out->memory_buffer_limit_bytes = in->memory_buffer_limit_bytes;
    out->disk_spool_disabled = in->disk_spool_disabled;
    return rotmos_u64_from_size(in->max_bytes, &out->max_bytes, error);
}
static rotmos_status forward(rotmos_request *request,
                             const lonejson_record_view *input,
                             const lonejson_record_view *output, void *data,
                             rotmos_error *error)
{
    struct reference_typed_route *route = data;
    reference_error e;
    reference_status s;
    reference_error_clear(&e);
    s = route->json(route->app, rotmos_request_userdata(request),
                    input != NULL ? input->record : NULL,
                    output != NULL ? output->record : NULL, route->userdata,
                    &e);
    return reference_error_to_rotmos(s, &e, error);
}
static rotmos_status forward_typed(rotmos_request *request,
                                   const lonejson_record_view *input,
                                   rotmos_reply *reply, void *data,
                                   rotmos_error *error)
{
    struct reference_typed_route *route = data;
    struct reference_response response;
    reference_error e;
    reference_status s;
    reference_error_clear(&e);
    response.reply = reply;
    s = route->typed(route->app, rotmos_request_userdata(request),
                     input != NULL ? input->record : NULL, &response,
                     route->userdata, &e);
    return reference_error_to_rotmos(s, &e, error);
}
reference_status reference_json_register(struct reference_typed_route *host,
                                         const struct reference_json_config *in,
                                         reference_error *error)
{
    rotmos_json_route_config c;
    rotmos_error e;
    rotmos_status s;
    reference_status translated;
    if (host == NULL || host->app == NULL || in == NULL || in->handler == NULL)
        return invalid(error, "provide app, json route and handler");
    rotmos_json_route_config_init(&c);
    translated = schema(host, in->input_map, in->input_size, "json route input",
                        0, &c.input, error);
    if (translated == REFERENCE_OK)
        translated = schema(host, in->output_map, in->output_size,
                            "json route output", 0, &c.output, error);
    if (translated != REFERENCE_OK)
        return translated;
    c.method = (rotmos_http_method)in->method;
    c.methods = in->methods;
    c.path = in->path;
    c.path_kind = (rotmos_route_path_kind)in->path_kind;
    s = body(&in->body, &c.body, &e);
    host->json = in->handler;
    host->userdata = in->userdata;
    c.handler = forward;
    c.userdata = host;
    c.hooks = host->hooks;
    c.records = host->records;
    if (s == ROTMOS_OK)
        s = rotmos_router_json_route(rotmos_server_router(host->app->server),
                                     &c, NULL, &e);
    return reference_error_from_rotmos(s, &e, error);
}
reference_status
reference_json_typed_register(struct reference_typed_route *host,
                              const struct reference_json_typed_config *in,
                              reference_error *error)
{
    rotmos_json_typed_route_config c;
    rotmos_error e;
    rotmos_status s;
    reference_status translated;
    if (host == NULL || host->app == NULL || in == NULL || in->handler == NULL)
        return invalid(error, "provide app, typed json route and handler");
    rotmos_json_typed_route_config_init(&c);
    translated = schema(host, in->input_map, in->input_size,
                        "typed json route input", 0, &c.input, error);
    if (translated != REFERENCE_OK)
        return translated;
    c.method = (rotmos_http_method)in->method;
    c.methods = in->methods;
    c.path = in->path;
    c.path_kind = (rotmos_route_path_kind)in->path_kind;
    s = body(&in->body, &c.body, &e);
    host->typed = in->handler;
    host->userdata = in->userdata;
    c.handler = forward_typed;
    c.userdata = host;
    c.hooks = host->hooks;
    c.records = host->records;
    if (s == ROTMOS_OK)
        s = rotmos_router_json_typed_route(
            rotmos_server_router(host->app->server), &c, NULL, &e);
    return reference_error_from_rotmos(s, &e, error);
}
reference_status reference_xml_register(struct reference_typed_route *host,
                                        const struct reference_xml_config *in,
                                        reference_error *error)
{
    rotmos_xml_route_config c;
    lonejson_schema_view input;
    rotmos_error e;
    rotmos_status s;
    reference_status translated;
    if (host == NULL || host->app == NULL || in == NULL || in->handler == NULL)
        return invalid(error, "provide app, XML route and handler");
    rotmos_xml_route_config_init(&c);
    translated = schema(host, in->input_map, in->input_size, "XML route input",
                        1, &input, error);
    if (translated != REFERENCE_OK)
        return translated;
    c.method = (rotmos_http_method)in->method;
    c.methods = in->methods;
    c.path = in->path;
    c.path_kind = (rotmos_route_path_kind)in->path_kind;
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
    if (s == ROTMOS_OK)
        s = rotmos_router_xml_route(rotmos_server_router(host->app->server), &c,
                                    NULL, &e);
    return reference_error_from_rotmos(s, &e, error);
}
static reference_status json_response(struct reference_response *response,
                                      int status, const lonejson_map *map,
                                      const void *record, int generated,
                                      reference_error *error)
{
    lonejson_schema_view view;
    rotmos_error e;
    rotmos_status s;
    if (response == NULL)
        return invalid(error, "response is required");
    s = rotmos_json_schema_from_map(NULL, map, &view, &e);
    if (s == ROTMOS_OK) {
        if (generated)
            s = rotmos_reply_json_spooled(response->reply, status, &view,
                                          record, NULL, &e);
        else
            s = rotmos_reply_json_materialized(response->reply, status, &view,
                                               record, 0, &e);
    }
    return reference_error_from_rotmos(s, &e, error);
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

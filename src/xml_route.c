#include "xml_mapping_internal.h"
#include "router_internal.h"
#include "request_internal.h"
#include "upload_internal.h"
#include <stdlib.h>
#include <string.h>

struct xml_route {
    lonejson_schema_view input;
    rotmos_xml_route_fn handler;
    rotmos_xml_config xml;
    void *userdata;
};
struct xml_records {
    lonejson_record_view input;
    int input_runtime;
    struct xml_route *route;
};
static void release_records(void *data)
{
    struct xml_records *r;
    r = data;
    if (r->input.record != NULL) {
        lonejson_cleanup(r->input.schema.map, r->input.record);
        free(r->input.record);
    }
    if (r->input_runtime)
        lonejson_free(r->input.schema.runtime);
    free(r);
}
static rotmos_status allocate_record(const lonejson_schema_view *schema,
                                     const rotmos_xml_config *xml,
                                     lonejson_record_view *view, int *owned,
                                     rotmos_error *error)
{
    memset(view, 0, sizeof(*view));
    view->size = sizeof(*view);
    view->abi_version = LONEJSON_VIEW_ABI_VERSION;
    view->schema = *schema;
    if (schema->map == NULL)
        return ROTMOS_OK;
    *owned = schema->runtime == NULL;
    if (*owned)
        view->schema.runtime = rotmos_xml_lonejson_new(xml, error);
    if (view->schema.runtime == NULL)
        return error != NULL ? error->status : ROTMOS_NOMEM;
    view->record = calloc(1, schema->record_size);
    if (view->record == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "xml_route",
                           "cannot allocate typed record");
    lonejson_init(view->schema.runtime, schema->map, view->record);
    return ROTMOS_OK;
}
static rotmos_status open_records(rotmos_request *request, void *data,
                                  void **out, rotmos_error *error)
{
    struct xml_route *route;
    struct xml_records *records;
    rotmos_status status;
    (void)request;
    route = data;
    records = calloc(1, sizeof(*records));
    if (records == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "xml_route",
                           "cannot allocate route records");
    *out = records;
    records->route = route;
    status = allocate_record(&route->input, &route->xml, &records->input,
                             &records->input_runtime, error);
    return status;
}
static rotmos_status parse(const rotmos_upload_metadata *metadata,
                           rotmos_source *reader, rotmos_reply *reply,
                           void *data, rotmos_error *error)
{
    struct xml_records *records = data;
    (void)metadata;
    (void)reply;
    return rotmos_xml_parse_source(reader, &records->input.schema,
                                   &records->route->xml, records->input.record,
                                   error);
}
static rotmos_status complete(rotmos_request *request, rotmos_reply *reply,
                              void *data, rotmos_error *error)
{
    struct xml_records *records = data;
    struct xml_route *route = records->route;
    rotmos_status status =
        route->handler(request, &records->input, reply, route->userdata, error);
    if (status == ROTMOS_OK && !rotmos_reply_is_selected(reply))
        status = rotmos_reply_status(reply, 204, error);
    return status;
}
static void release_route(void *data)
{
    struct xml_route *r = data;
    free((void *)r->xml.root_element);
    free((void *)r->xml.text_key);
    free((void *)r->xml.attribute_prefix);
    free(r);
}
void rotmos_xml_route_config_init(rotmos_xml_route_config *config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    rotmos_body_policy_init_upload(&config->body);
    rotmos_xml_config_init(&config->xml);
}
static rotmos_status add_route(rotmos_router *router, rotmos_route_group *group,
                               const rotmos_xml_route_config *config,
                               rotmos_route **out, rotmos_error *error)
{
    struct xml_route *data;
    rotmos_xml_config xml;
    rotmos_upload_reader_config reader;
    struct rotmos_upload_reader_codec codec;
    rotmos_route *registered;
    rotmos_status status;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || config->handler == NULL || config->input == NULL ||
        (router == NULL && group == NULL))
        return rotmos_fail(error, ROTMOS_INVALID, "xml_route",
                           "provide router, declaration and handler");
    if (rotmos_json_schema_check(config->input, error) != ROTMOS_OK)
        return ROTMOS_INVALID;
    if (config->body.mode != ROTMOS_BODY_STREAMING_UPLOAD &&
        config->body.mode != ROTMOS_BODY_STREAM)
        return rotmos_fail(error, ROTMOS_INVALID, "xml_route",
                           "mapped input needs a live upload body policy");
    status = rotmos_xml_effective_config(&config->xml, &xml, error);
    if (status != ROTMOS_OK)
        return status;
    data = calloc(1, sizeof(*data));
    if (data == NULL)
        return rotmos_fail(error, ROTMOS_NOMEM, "xml_route",
                           "copying XML route");
    data->input = *config->input;
    data->handler = config->handler;
    data->userdata = config->userdata;
    data->xml = xml;
    data->xml.root_element = rotmos_copy_text(xml.root_element);
    data->xml.text_key = rotmos_copy_text(xml.text_key);
    data->xml.attribute_prefix = rotmos_copy_text(xml.attribute_prefix);
    if ((xml.root_element != NULL && data->xml.root_element == NULL) ||
        data->xml.text_key == NULL || data->xml.attribute_prefix == NULL) {
        release_route(data);
        return rotmos_fail(error, ROTMOS_NOMEM, "xml_route",
                           "copying XML mapping options");
    }
    rotmos_upload_reader_config_init(&reader);
    reader.method = config->method;
    reader.methods = config->methods;
    reader.path = config->path;
    reader.path_kind = config->path_kind;
    reader.body = config->body;
    reader.buffer_bytes = config->buffer_bytes;
    reader.handler = parse;
    reader.userdata = data;
    reader.hooks = config->hooks;
    codec.open = open_records;
    codec.finish = complete;
    codec.release = release_records;
    status = rotmos_upload_reader_add(router, group, &reader, &codec,
                                      release_route, &registered, error);
    if (status != ROTMOS_OK)
        release_route(data);
    else {
        if (out != NULL)
            *out = registered;
    }
    return status;
}
rotmos_status rotmos_router_xml_route(rotmos_router *router,
                                      const rotmos_xml_route_config *config,
                                      rotmos_route **out, rotmos_error *error)
{
    return add_route(router, NULL, config, out, error);
}
rotmos_status
rotmos_route_group_xml_route(rotmos_route_group *group,
                             const rotmos_xml_route_config *config,
                             rotmos_route **out, rotmos_error *error)
{
    return add_route(NULL, group, config, out, error);
}

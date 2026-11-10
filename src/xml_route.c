#include "xml_mapping_internal.h"
#include "router_internal.h"
#include "request_internal.h"
#include "upload_internal.h"
#include <stdlib.h>
#include <string.h>

struct xml_route {
    lonejson_schema_view input;
    knurl_xml_route_fn handler;
    knurl_xml_config xml;
    void *userdata;
    knurl_record_provider provider;
};
struct xml_records {
    lonejson_record_view input;
    int input_runtime;
    struct xml_route *route;
    knurl_record_storage storage;
    int acquired;
};
static void release_records(void *data)
{
    struct xml_records *r;
    r = data;
    if (r->acquired)
        r->route->provider.release(&r->storage, r->route->userdata);
    else if (r->input.record != NULL) {
        lonejson_cleanup(r->input.schema.map, r->input.record);
        free(r->input.record);
    }
    if (r->input_runtime)
        lonejson_free(r->input.schema.runtime);
    free(r);
}
static knurl_status allocate_record(const lonejson_schema_view *schema,
                                    const knurl_xml_config *xml,
                                    lonejson_record_view *view, int *owned,
                                    knurl_error *error)
{
    memset(view, 0, sizeof(*view));
    view->size = sizeof(*view);
    view->abi_version = LONEJSON_VIEW_ABI_VERSION;
    view->schema = *schema;
    if (schema->map == NULL)
        return KNURL_OK;
    *owned = schema->runtime == NULL;
    if (*owned)
        view->schema.runtime = knurl_xml_lonejson_new(xml, error);
    if (view->schema.runtime == NULL)
        return error != NULL ? error->status : KNURL_NOMEM;
    view->record = calloc(1, schema->record_size);
    if (view->record == NULL)
        return knurl_fail(error, KNURL_NOMEM, "xml_route",
                          "cannot allocate typed record");
    lonejson_init(view->schema.runtime, schema->map, view->record);
    return KNURL_OK;
}
static knurl_status open_records(knurl_request *request, void *data, void **out,
                                 knurl_error *error)
{
    struct xml_route *route;
    struct xml_records *records;
    knurl_status status;
    route = data;
    records = calloc(1, sizeof(*records));
    if (records == NULL)
        return knurl_fail(error, KNURL_NOMEM, "xml_route",
                          "cannot allocate route records");
    *out = records;
    records->route = route;
    if (route->provider.acquire != NULL) {
        records->acquired = 1;
        status =
            route->provider.acquire(request, 0, &route->input,
                                    &records->storage, route->userdata, error);
        records->input = records->storage.view;
        if (status == KNURL_OK)
            status =
                knurl_json_record_check(&route->input, &records->input, error);
    } else
        status = allocate_record(&route->input, &route->xml, &records->input,
                                 &records->input_runtime, error);
    return status;
}
static knurl_status parse(const knurl_upload_metadata *metadata,
                          knurl_source *reader, knurl_reply *reply, void *data,
                          knurl_error *error)
{
    struct xml_records *records = data;
    (void)metadata;
    (void)reply;
    return knurl_xml_parse_source(reader, &records->input.schema,
                                  &records->route->xml, records->input.record,
                                  error);
}
static knurl_status complete(knurl_request *request, knurl_reply *reply,
                             void *data, knurl_error *error)
{
    struct xml_records *records = data;
    struct xml_route *route = records->route;
    knurl_status status =
        route->handler(request, &records->input, reply, route->userdata, error);
    if (status == KNURL_OK && !knurl_reply_is_selected(reply))
        status = knurl_reply_status(reply, 204, error);
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
void knurl_xml_route_config_init(knurl_xml_route_config *config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    knurl_body_policy_init_upload(&config->body);
    knurl_xml_config_init(&config->xml);
}
static knurl_status add_route(knurl_router *router, knurl_route_group *group,
                              const knurl_xml_route_config *config,
                              knurl_route **out, knurl_error *error)
{
    struct xml_route *data;
    knurl_xml_config xml;
    knurl_upload_reader_config reader;
    struct knurl_upload_reader_codec codec;
    knurl_route *registered;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || config->handler == NULL || config->input == NULL ||
        (router == NULL && group == NULL))
        return knurl_fail(error, KNURL_INVALID, "xml_route",
                          "provide router, declaration and handler");
    if (knurl_json_schema_check(config->input, error) != KNURL_OK)
        return KNURL_INVALID;
    if (config->records != NULL &&
        (config->records->acquire == NULL || config->records->release == NULL))
        return knurl_fail(error, KNURL_INVALID, "xml_route",
                          "record provider needs acquire and release");
    if (config->body.mode != KNURL_BODY_STREAMING_UPLOAD &&
        config->body.mode != KNURL_BODY_STREAM)
        return knurl_fail(error, KNURL_INVALID, "xml_route",
                          "mapped input needs a live upload body policy");
    status = knurl_xml_effective_config(&config->xml, &xml, error);
    if (status != KNURL_OK)
        return status;
    data = calloc(1, sizeof(*data));
    if (data == NULL)
        return knurl_fail(error, KNURL_NOMEM, "xml_route", "copying XML route");
    data->input = *config->input;
    data->handler = config->handler;
    data->userdata = config->userdata;
    if (config->records != NULL)
        data->provider = *config->records;
    data->xml = xml;
    data->xml.root_element = knurl_copy_text(xml.root_element);
    data->xml.text_key = knurl_copy_text(xml.text_key);
    data->xml.attribute_prefix = knurl_copy_text(xml.attribute_prefix);
    if ((xml.root_element != NULL && data->xml.root_element == NULL) ||
        data->xml.text_key == NULL || data->xml.attribute_prefix == NULL) {
        release_route(data);
        return knurl_fail(error, KNURL_NOMEM, "xml_route",
                          "copying XML mapping options");
    }
    knurl_upload_reader_config_init(&reader);
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
    status = knurl_upload_reader_add(router, group, &reader, &codec,
                                     release_route, &registered, error);
    if (status != KNURL_OK)
        release_route(data);
    else {
        if (out != NULL)
            *out = registered;
    }
    return status;
}
knurl_status knurl_router_xml_route(knurl_router *router,
                                    const knurl_xml_route_config *config,
                                    knurl_route **out, knurl_error *error)
{
    return add_route(router, NULL, config, out, error);
}
knurl_status knurl_route_group_xml_route(knurl_route_group *group,
                                         const knurl_xml_route_config *config,
                                         knurl_route **out, knurl_error *error)
{
    return add_route(NULL, group, config, out, error);
}

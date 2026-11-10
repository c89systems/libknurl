#include "json_internal.h"
#include "router_internal.h"
#include "request_internal.h"
#include <stdlib.h>
#include <string.h>

struct json_route {
    lonejson_schema_view input;
    lonejson_schema_view output;
    knurl_json_route_fn handler;
    knurl_json_typed_route_fn typed;
    void *userdata;
    knurl_record_provider provider;
    size_t max_output_bytes;
};
struct json_records {
    lonejson_record_view input;
    lonejson_record_view output;
    knurl_record_storage input_storage, output_storage;
    knurl_record_provider provider;
    void *userdata;
    int input_acquired, output_acquired;
    int input_runtime;
    int output_runtime;
};
static void release_records(void *data)
{
    struct json_records *r;
    r = data;
    if (r->output_acquired)
        r->provider.release(&r->output_storage, r->userdata);
    else if (r->output.record != NULL) {
        lonejson_cleanup(r->output.schema.map, r->output.record);
        free(r->output.record);
    }
    if (r->input_acquired)
        r->provider.release(&r->input_storage, r->userdata);
    else if (r->input.record != NULL) {
        lonejson_cleanup(r->input.schema.map, r->input.record);
        free(r->input.record);
    }
    if (r->output_runtime)
        lonejson_free(r->output.schema.runtime);
    if (r->input_runtime)
        lonejson_free(r->input.schema.runtime);
    free(r);
}
static knurl_status allocate_record(const lonejson_schema_view *schema,
                                    lonejson_record_view *view, int *owned,
                                    knurl_error *error)
{
    knurl_status status;
    memset(view, 0, sizeof(*view));
    view->size = sizeof(*view);
    view->abi_version = LONEJSON_VIEW_ABI_VERSION;
    view->schema = *schema;
    if (schema->map == NULL)
        return KNURL_OK;
    status = knurl_json_runtime(schema, &view->schema.runtime, owned, error);
    if (status != KNURL_OK)
        return status;
    view->record = calloc(1, schema->record_size);
    if (view->record == NULL)
        return knurl_fail(error, KNURL_NOMEM, "json_route",
                          "cannot allocate typed record");
    lonejson_init(view->schema.runtime, schema->map, view->record);
    return KNURL_OK;
}
static knurl_status dispatch(knurl_request *request, knurl_reply *reply,
                             void *data, knurl_error *error)
{
    struct json_route *route;
    struct json_records *records;
    knurl_status status;
    const lonejson_record_view *input;
    route = data;
    records = calloc(1, sizeof(*records));
    if (records == NULL)
        return knurl_fail(error, KNURL_NOMEM, "json_route",
                          "cannot allocate route records");
    request->codec_data = records;
    request->codec_cleanup = release_records;
    records->provider = route->provider;
    records->userdata = route->userdata;
    if (route->input.map != NULL && route->provider.acquire != NULL) {
        records->input_acquired = 1;
        status = route->provider.acquire(request, 0, &route->input,
                                         &records->input_storage,
                                         route->userdata, error);
        records->input = records->input_storage.view;
        if (status == KNURL_OK)
            status =
                knurl_json_record_check(&route->input, &records->input, error);
    } else
        status = allocate_record(&route->input, &records->input,
                                 &records->input_runtime, error);
    if (status == KNURL_OK && records->input.record != NULL)
        status = knurl_request_json_into(request, &records->input.schema,
                                         records->input.record, error);
    if (status == KNURL_OK) {
        if (route->output.map != NULL && route->provider.acquire != NULL) {
            records->output_acquired = 1;
            status = route->provider.acquire(request, 1, &route->output,
                                             &records->output_storage,
                                             route->userdata, error);
            records->output = records->output_storage.view;
            if (status == KNURL_OK)
                status = knurl_json_record_check(&route->output,
                                                 &records->output, error);
        } else
            status = allocate_record(&route->output, &records->output,
                                     &records->output_runtime, error);
    }
    if (status != KNURL_OK)
        return status;
    input = records->input.record != NULL ? &records->input : NULL;
    if (route->typed != NULL)
        return route->typed(request, input, reply, route->userdata, error);
    status =
        route->handler(request, input,
                       records->output.record != NULL ? &records->output : NULL,
                       route->userdata, error);
    if (status != KNURL_OK)
        return status;
    if (records->output.record == NULL)
        status = knurl_reply_status(reply, 204, error);
    else
        /* Vectis freezes the ordinary JSON result before after-hooks. Live
         * generation is an explicit typed-reply choice. Do not add the
         * materializer's standalone 10 MiB default to an existing route. */
        status = knurl_reply_json_materialized(
            reply, 200, &records->output.schema, records->output.record,
            route->max_output_bytes != 0 ? route->max_output_bytes : (size_t)-1,
            error);
    request->codec_data = NULL;
    request->codec_cleanup = NULL;
    release_records(records);
    return status;
}
static void schema_init(lonejson_schema_view *schema)
{
    memset(schema, 0, sizeof(*schema));
    schema->size = sizeof(*schema);
    schema->abi_version = LONEJSON_VIEW_ABI_VERSION;
}
void knurl_json_route_config_init(knurl_json_route_config *config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    knurl_body_policy_init_materialized(&config->body);
    schema_init(&config->input);
    schema_init(&config->output);
}
void knurl_json_typed_route_config_init(knurl_json_typed_route_config *config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    knurl_body_policy_init_materialized(&config->body);
    schema_init(&config->input);
}
static knurl_status add_route(knurl_router *router, knurl_route_group *group,
                              const knurl_json_route_config *config,
                              knurl_json_typed_route_fn typed,
                              knurl_route **out, knurl_error *error)
{
    struct json_route *data;
    knurl_route_config route;
    knurl_route *registered;
    knurl_status status;
    if (out != NULL)
        *out = NULL;
    if (config == NULL || (config->handler == NULL && typed == NULL) ||
        (router == NULL && group == NULL))
        return knurl_fail(error, KNURL_INVALID, "json_route",
                          "provide router, declaration and handler");
    if (config->records != NULL &&
        (config->records->acquire == NULL || config->records->release == NULL))
        return knurl_fail(error, KNURL_INVALID, "json_route",
                          "record provider needs acquire and release");
    if (config->input.map != NULL &&
        knurl_json_schema_check(&config->input, error) != KNURL_OK)
        return KNURL_INVALID;
    if (config->output.map != NULL &&
        knurl_json_schema_check(&config->output, error) != KNURL_OK)
        return KNURL_INVALID;
    if (config->input.map != NULL && config->body.mode != KNURL_BODY_JSON &&
        config->body.mode != KNURL_BODY_MATERIALIZED &&
        config->body.mode != KNURL_BODY_BUFFERED)
        return knurl_fail(
            error, KNURL_INVALID, "json_route",
            "typed input needs BUFFERED or MATERIALIZED body policy");
    data = calloc(1, sizeof(*data));
    if (data == NULL)
        return knurl_fail(error, KNURL_NOMEM, "json_route",
                          "cannot copy JSON route declaration");
    data->input = config->input;
    data->output = config->output;
    data->handler = config->handler;
    data->typed = typed;
    data->userdata = config->userdata;
    data->max_output_bytes = config->max_output_bytes;
    if (config->records != NULL)
        data->provider = *config->records;
    knurl_route_config_init(&route);
    route.method = config->method;
    route.methods = config->methods;
    route.path = config->path;
    route.path_kind = config->path_kind;
    route.body = config->body;
    route.handler = dispatch;
    route.userdata = data;
    route.hooks = config->hooks;
    status = group != NULL
                 ? knurl_route_group_route(group, &route, &registered, error)
                 : knurl_router_route(router, &route, &registered, error);
    if (status != KNURL_OK)
        free(data);
    else {
        registered->release_userdata = free;
        if (out != NULL)
            *out = registered;
    }
    return status;
}
knurl_status knurl_router_json_route(knurl_router *router,
                                     const knurl_json_route_config *config,
                                     knurl_route **out, knurl_error *error)
{
    return add_route(router, NULL, config, NULL, out, error);
}
knurl_status knurl_route_group_json_route(knurl_route_group *group,
                                          const knurl_json_route_config *config,
                                          knurl_route **out, knurl_error *error)
{
    return add_route(NULL, group, config, NULL, out, error);
}
static knurl_status typed_route(knurl_router *router, knurl_route_group *group,
                                const knurl_json_typed_route_config *config,
                                knurl_route **out, knurl_error *error)
{
    knurl_json_route_config declaration;
    if (out != NULL)
        *out = NULL;
    if (config == NULL)
        return knurl_fail(error, KNURL_INVALID, "json_route",
                          "typed route declaration is required");
    knurl_json_route_config_init(&declaration);
    declaration.method = config->method;
    declaration.methods = config->methods;
    declaration.path = config->path;
    declaration.path_kind = config->path_kind;
    declaration.body = config->body;
    declaration.input = config->input;
    declaration.userdata = config->userdata;
    declaration.hooks = config->hooks;
    declaration.records = config->records;
    return add_route(router, group, &declaration, config->handler, out, error);
}
knurl_status
knurl_router_json_typed_route(knurl_router *router,
                              const knurl_json_typed_route_config *config,
                              knurl_route **out, knurl_error *error)
{
    return typed_route(router, NULL, config, out, error);
}
knurl_status
knurl_route_group_json_typed_route(knurl_route_group *group,
                                   const knurl_json_typed_route_config *config,
                                   knurl_route **out, knurl_error *error)
{
    return typed_route(NULL, group, config, out, error);
}

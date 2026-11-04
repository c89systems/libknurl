#include "vectis-forward.h"

struct array_forward {
    reference_array_item callback;
    void *userdata;
};
static rotmos_status forward_item(size_t index,
                                  const lonejson_record_view *view, void *data,
                                  rotmos_error *error)
{
    struct array_forward *forward = data;
    return forward->callback(forward->userdata, index, view->record, error);
}
rotmos_status reference_request_array_each(rotmos_request *request,
                                           const char *selector,
                                           const lonejson_schema_view *schema,
                                           void *item,
                                           reference_array_item callback,
                                           void *userdata, rotmos_error *error)
{
    struct array_forward forward;
    forward.callback = callback;
    forward.userdata = userdata;
    return rotmos_request_json_array_each(request, selector, schema, item,
                                          forward_item, &forward, error);
}

static rotmos_status forward(rotmos_request *request,
                             const lonejson_record_view *input,
                             const lonejson_record_view *output, void *data,
                             rotmos_error *error)
{
    struct reference_json_route *route = data;
    return route->handler(
        route->app, request, input != NULL ? input->record : NULL,
        output != NULL ? output->record : NULL, route->userdata, error);
}

rotmos_status reference_json_register(rotmos_router *router,
                                      const rotmos_json_route_config *input,
                                      struct reference_json_route *host,
                                      rotmos_error *error)
{
    rotmos_json_route_config config = *input;
    config.handler = forward;
    config.userdata = host;
    return rotmos_router_json_route(router, &config, NULL, error);
}
static rotmos_status forward_xml(rotmos_request *request,
                                 const lonejson_record_view *input,
                                 rotmos_reply *reply, void *data,
                                 rotmos_error *error)
{
    struct reference_xml_route *route = data;
    return route->handler(route->app, request, input->record, reply,
                          route->userdata, error);
}
rotmos_status reference_xml_register(rotmos_router *router,
                                     const rotmos_xml_route_config *input,
                                     struct reference_xml_route *host,
                                     rotmos_error *error)
{
    rotmos_xml_route_config config = *input;
    config.handler = forward_xml;
    config.userdata = host;
    return rotmos_router_xml_route(router, &config, NULL, error);
}

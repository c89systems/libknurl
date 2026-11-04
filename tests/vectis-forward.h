#ifndef ROTMOS_TEST_VECTIS_FORWARD_H
#define ROTMOS_TEST_VECTIS_FORWARD_H
#include <rotmos/lonejson.h>
#include <rotmos/xml.h>
typedef rotmos_status (*reference_array_item)(void *, size_t, void *,
                                              rotmos_error *);
rotmos_status reference_request_array_each(rotmos_request *, const char *,
                                           const lonejson_schema_view *, void *,
                                           reference_array_item, void *,
                                           rotmos_error *);

/* Reference consumer, not a shipped API or a copy of Vectis's app. Matches
 * vectis_json_route_handler_fn's app/request/raw-record/userdata shape. The
 * caller retains this forwarding context through router destruction. */
typedef rotmos_status (*reference_json_handler)(void *, rotmos_request *,
                                                void *, void *, void *,
                                                rotmos_error *);
struct reference_json_route {
    void *app;
    reference_json_handler handler;
    void *userdata;
};
rotmos_status reference_json_register(rotmos_router *,
                                      const rotmos_json_route_config *,
                                      struct reference_json_route *,
                                      rotmos_error *);
typedef rotmos_status (*reference_xml_handler)(void *, rotmos_request *, void *,
                                               rotmos_reply *, void *,
                                               rotmos_error *);
struct reference_xml_route {
    void *app;
    reference_xml_handler handler;
    void *userdata;
};
rotmos_status reference_xml_register(rotmos_router *,
                                     const rotmos_xml_route_config *,
                                     struct reference_xml_route *,
                                     rotmos_error *);
#endif

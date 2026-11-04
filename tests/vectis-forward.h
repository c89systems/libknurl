#ifndef ROTMOS_TEST_VECTIS_FORWARD_H
#define ROTMOS_TEST_VECTIS_FORWARD_H
#include <rotmos/lonejson.h>

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
#endif

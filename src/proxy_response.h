#ifndef ROTMOS_PROXY_RESPONSE_H
#define ROTMOS_PROXY_RESPONSE_H

#include "proxy_http.h"
#include "proxy_route_internal.h"

struct rotmos_proxy_response {
    const rotmos_proxy_http_response *upstream;
    rotmos_proxy_headers *headers;
    int status;
    rotmos_status failure;
};

/* Run the route hook against sanitized output headers. The original upstream
 * response remains the framing authority. The resulting status is used only
 * for the downstream status line and metrics. */
rotmos_status rotmos_proxy_response_apply(
    const rotmos_proxy_http_response *upstream, rotmos_proxy_headers *sanitized,
    rotmos_proxy_modify_response_fn modify, void *userdata,
    int *downstream_status, rotmos_error *error);

#endif

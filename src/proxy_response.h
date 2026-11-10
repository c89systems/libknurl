#ifndef KNURL_PROXY_RESPONSE_H
#define KNURL_PROXY_RESPONSE_H

#include "proxy_http.h"
#include "proxy_route_internal.h"

struct knurl_proxy_response {
    const knurl_proxy_http_response *upstream;
    knurl_proxy_headers *headers;
    int status;
    knurl_status failure;
};

/* Run the route hook against sanitized output headers. The original upstream
 * response remains the framing authority. The resulting status is used only
 * for the downstream status line and metrics. */
knurl_status knurl_proxy_response_apply(
    const knurl_proxy_http_response *upstream, knurl_proxy_headers *sanitized,
    knurl_proxy_modify_response_fn modify, void *userdata,
    int *downstream_status, knurl_error *error);

#endif

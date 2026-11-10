#ifndef KNURL_PROXY_LOCAL_H
#define KNURL_PROXY_LOCAL_H

#include "proxy_headers.h"
#include "proxy_route_internal.h"

struct knurl_proxy_local_response {
    knurl_proxy_headers headers;
    unsigned char *body;
    size_t body_length;
    int status;
    knurl_status failure;
};

void knurl_proxy_local_init(knurl_proxy_local_response *response);
void knurl_proxy_local_cleanup(knurl_proxy_local_response *response);

/* Run the optional hook and fill a complete 502/504 response on fallback. */
void knurl_proxy_local_error(const knurl_proxy_route_data *route,
                             const knurl_error *cause, int default_status,
                             knurl_proxy_local_response *response);

#endif

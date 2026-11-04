#ifndef ROTMOS_PROXY_LOCAL_H
#define ROTMOS_PROXY_LOCAL_H

#include "proxy_headers.h"
#include "proxy_route_internal.h"

struct rotmos_proxy_local_response {
    rotmos_proxy_headers headers;
    unsigned char *body;
    size_t body_length;
    int status;
    rotmos_status failure;
};

void rotmos_proxy_local_init(rotmos_proxy_local_response *response);
void rotmos_proxy_local_cleanup(rotmos_proxy_local_response *response);

/* Run the optional hook and fill a complete 502/504 response on fallback. */
void rotmos_proxy_local_error(const rotmos_proxy_route_data *route,
                              const rotmos_error *cause, int default_status,
                              rotmos_proxy_local_response *response);

#endif

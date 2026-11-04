#ifndef ROTMOS_PROXY_DIRECTOR_H
#define ROTMOS_PROXY_DIRECTOR_H

#include "proxy_headers.h"
#include "proxy_route_internal.h"

struct rotmos_proxy_inbound {
    rotmos_http_method method;
    const char *path;
    const char *query;
    const char *host;
    const rotmos_proxy_headers *headers;
    rotmos_request *matched_request;
    int websocket;
};

struct rotmos_proxy_outbound {
    const rotmos_proxy_route_data *route;
    rotmos_proxy_headers headers;
    char *path;
    char *query;
    char *host;
    rotmos_http_method method;
    size_t target_index;
    int websocket;
    rotmos_status failure;
};

/* Copy sanitized into out, invoke the optional route callback, then validate
 * the final target. On success target and authority are caller-owned. out
 * remains valid until cleanup; on failure cleanup is still required. */
rotmos_status rotmos_proxy_director_prepare(
    const rotmos_proxy_route_data *route, rotmos_http_method method,
    const char *path, const char *query, const char *host, int websocket,
    rotmos_request *matched_request, const rotmos_proxy_headers *inbound,
    const rotmos_proxy_headers *sanitized, rotmos_proxy_outbound *out,
    char **target, char **authority, rotmos_error *error);

void rotmos_proxy_director_cleanup(rotmos_proxy_outbound *out);

#endif

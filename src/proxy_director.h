#ifndef KNURL_PROXY_DIRECTOR_H
#define KNURL_PROXY_DIRECTOR_H

#include "proxy_headers.h"
#include "proxy_route_internal.h"

struct knurl_proxy_inbound {
    knurl_http_method method;
    const char *path;
    const char *query;
    const char *host;
    const knurl_proxy_headers *headers;
    knurl_request *matched_request;
    int websocket;
};

struct knurl_proxy_outbound {
    const knurl_proxy_route_data *route;
    knurl_proxy_headers headers;
    char *path;
    char *query;
    char *host;
    knurl_http_method method;
    size_t target_index;
    int websocket;
    knurl_status failure;
};

/* Copy sanitized into out, invoke the optional route callback, then validate
 * the final target. On success target and authority are caller-owned. out
 * remains valid until cleanup; on failure cleanup is still required. */
knurl_status knurl_proxy_director_prepare(
    const knurl_proxy_route_data *route, knurl_http_method method,
    const char *path, const char *query, const char *host, int websocket,
    knurl_request *matched_request, const knurl_proxy_headers *inbound,
    const knurl_proxy_headers *sanitized, knurl_proxy_outbound *out,
    char **target, char **authority, knurl_error *error);

void knurl_proxy_director_cleanup(knurl_proxy_outbound *out);

#endif

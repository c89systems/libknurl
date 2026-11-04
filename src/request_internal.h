#ifndef ROTMOS_REQUEST_INTERNAL_H
#define ROTMOS_REQUEST_INTERNAL_H

#include "router_internal.h"
#include <rotmos/request.h>
#include <rotmos/dispatch.h>

struct rotmos_request {
    rotmos_server *server;
    void *native;
    void *userdata;
    rotmos_http_method method;
    rotmos_bytes method_token;
    rotmos_bytes version;
    rotmos_bytes target;
    rotmos_bytes raw_path;
    rotmos_bytes query;
    rotmos_bytes authority;
    rotmos_bytes path;
    rotmos_header *headers;
    size_t header_count;
    rotmos_header *queries;
    size_t query_count;
    rotmos_header *params;
    size_t param_count;
    size_t listener;
    rotmos_endpoint peer;
    rotmos_endpoint local;
    char peer_address[46];
    char local_address[46];
    rotmos_u64 content_length;
    int has_length;
    int has_coding;
    int tls;
    rotmos_bytes body;
    int body_ready;
    rotmos_route *route;
    rotmos_reply *reply;
    rotmos_response *response;
    rotmos_channel *channel;
    rotmos_body *body_store;
    void *input;
    rotmos_request_plan plan;
    rotmos_request_outcome outcome;
    int dispatch_started;
    int router_dispatched;
    int close_notified;
    int rejection_status;
    int in_headers;
};

rotmos_bytes rotmos_view(const char *text);
rotmos_status rotmos_request_decode(rotmos_request *request,
                                    rotmos_error *error);
void rotmos_request_cleanup(rotmos_request *request);
rotmos_status rotmos_router_select(rotmos_router *router,
                                   rotmos_request *request,
                                   rotmos_error *error);
rotmos_status rotmos_router_invoke(rotmos_request *request,
                                   rotmos_error *error);

#endif

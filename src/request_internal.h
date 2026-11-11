#ifndef KNURL_REQUEST_INTERNAL_H
#define KNURL_REQUEST_INTERNAL_H

#include "router_internal.h"
#include <knurl/request.h>
#include <knurl/dispatch.h>

struct knurl_request_impl {
    knurl_request receiver;
    void *codec_data;
    void (*codec_cleanup)(void *);
    knurl_server *server;
    void *native;
    void *userdata;
    knurl_http_method method;
    knurl_bytes method_token;
    knurl_bytes version;
    knurl_bytes target;
    knurl_bytes raw_path;
    knurl_bytes query;
    knurl_bytes authority;
    knurl_bytes path;
    knurl_header *headers;
    size_t header_count;
    knurl_header *queries;
    size_t query_count;
    knurl_header *params;
    size_t param_count;
    size_t listener;
    knurl_endpoint peer;
    knurl_endpoint local;
    char peer_address[46];
    char local_address[46];
    char effective_storage[46], correlation_storage[129];
    knurl_bytes effective_address, correlation_id;
    int address_source;
    int lifecycle_started, observing;
    int disconnect_side;
    knurl_u64 content_length;
    int has_length;
    int has_coding;
    int tls;
    knurl_bytes body;
    int body_ready;
    knurl_route *route;
    knurl_reply *reply;
    knurl_response *response;
    knurl_channel *channel;
    knurl_websocket *websocket;
    knurl_body *body_store;
    void *input;
    knurl_request_plan plan;
    knurl_request_outcome outcome;
    int dispatch_started;
    int router_dispatched;
    int raw_selected;
    int close_notified;
    int rejection_status;
    int in_headers;
};

knurl_bytes knurl_view(const char *text);
knurl_status knurl_request_path_decode(const char *, knurl_bytes *,
                                       knurl_error *);
knurl_status knurl_request_decode(knurl_request *request, knurl_error *error);
void knurl_request_cleanup(knurl_request *request);
knurl_status knurl_router_select(knurl_router *router, knurl_request *request,
                                 knurl_error *error);
knurl_status knurl_router_select_raw(knurl_router *, knurl_request *,
                                     knurl_error *);
knurl_status knurl_router_hooks_before(knurl_request *, int *, knurl_error *);
knurl_status knurl_router_hooks_after(knurl_request *, knurl_error *);
knurl_status knurl_router_invoke(knurl_request *request, knurl_error *error);

#endif

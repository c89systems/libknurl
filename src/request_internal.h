#ifndef KNURL_REQUEST_INTERNAL_H
#define KNURL_REQUEST_INTERNAL_H

#include "router_internal.h"
#include <knurl/request.h>
#include <knurl/dispatch.h>
#include <knurl/auth.h>
#include <knurl/proxy.h>
#include <stdint.h>

enum { KNURL_HOOK_NONE, KNURL_HOOK_ADMISSION, KNURL_HOOK_AFTER };
#define KNURL_AFTER_HOOK_COUNT 3

struct knurl_request_impl {
    knurl_request receiver;
    void *codec_data;
    void (*codec_cleanup)(void *);
    knurl_status (*on_body_admitted)(knurl_request *, knurl_error *);
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
    knurl_header *trailers;
    size_t trailer_count;
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
    int forwarding_invalid;
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
    unsigned int route_phase;
    int route_pending;
    int blocking_job_pending;
    struct knurl_auth_job *auth_job;
    knurl_auth_identity auth_identity;
    int auth_pending_headers, expect_continue;
    int hook_admission_complete;
    int hook_invoking;
    int proxy_admission_invoked;
    char *proxy_authorization;
    uint64_t proxy_token_generation;
    knurl_status proxy_token_failure;
    unsigned int hook_stage, hook_before, hook_after;
    void (*hook_cancel)(void *);
    void *hook_userdata;
    knurl_proxy_rewrite_fn proxy_rewrite;
    void (*proxy_rewrite_release)(void *);
    void *proxy_rewrite_data;
    struct proxy_exchange *proxy_exchange;
};

knurl_bytes knurl_view(const char *text);
knurl_status knurl_request_path_decode(const char *, knurl_bytes *,
                                       knurl_error *);
knurl_status knurl_request_decode(knurl_request *request, knurl_error *error);
void knurl_request_cleanup(knurl_request *request);
void knurl_request_stop_input(knurl_request *, knurl_status);
void knurl_request_log_handler_error(knurl_request *, const knurl_error *);
knurl_status knurl_router_select(knurl_router *router, knurl_request *request,
                                 knurl_error *error);
knurl_status knurl_router_select_raw(knurl_router *, knurl_request *,
                                     knurl_error *);
knurl_status knurl_router_hooks_before(knurl_request *, int *, knurl_error *);
knurl_status knurl_router_hooks_after(knurl_request *, knurl_error *);
knurl_status knurl_router_invoke(knurl_request *request, knurl_error *error);
knurl_status knurl_request_hooks_admit(knurl_request *, knurl_error *);
knurl_status knurl_request_proxy_admit(knurl_request *, knurl_error *);
knurl_status knurl_request_proxy_authenticate(knurl_request *,
                                              struct knurl_oauth2_client *,
                                              knurl_error *);
int knurl_request_has_after_hooks(const knurl_request *);
knurl_status knurl_request_hooks_after(knurl_request *, knurl_error *);
void knurl_request_hook_cancel(knurl_request *);

#endif

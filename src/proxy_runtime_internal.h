#ifndef KNURL_PROXY_RUNTIME_INTERNAL_H
#define KNURL_PROXY_RUNTIME_INTERNAL_H
#include "proxy_route_internal.h"
#include "proxy_local.h"
#include "proxy_transport.h"
#include "proxy_upload.h"
#include "proxy_http_upstream.h"
#include "server_internal.h"
struct knurl_proxy_ws;
struct proxy_exchange;
struct knurl_proxy_worker {
    knurl_proxy_transport *transport;
    struct proxy_exchange *exchanges;
    size_t active;
    size_t refs;
    int closing;
};
struct proxy_interim {
    struct proxy_interim *next;
    knurl_proxy_headers headers;
    knurl_header views[KNURL_PROXY_HEADER_COUNT_LIMIT];
    int status;
};
struct proxy_exchange {
    struct proxy_exchange *next;
    struct knurl_proxy_worker *worker;
    const knurl_proxy_route_data *route;
    knurl_request *request;
    knurl_loop *loop;
    knurl_channel *channel;
    knurl_proxy_transfer *transfer;
    struct knurl_proxy_ws *ws;
    knurl_proxy_http_upstream upstream;
    knurl_proxy_upload_buffer upload;
    knurl_proxy_local_response local;
    struct curl_slist *request_headers;
    struct proxy_interim *interims;
    struct proxy_interim *interim_tail;
    knurl_timer *timer;
    char *target;
    char *authority;
    unsigned char *input;
    size_t input_length;
    size_t input_offset;
    size_t refs;
    size_t local_offset;
    size_t interim_bytes;
    unsigned int interim_count;
    int queued;
    int has_upload;
    int up_done;
    int final_ready;
    int final_prepared;
    int begun;
    int ended;
    int failed;
    int local_prepared;
    int local_begun;
    int local_status;
    knurl_error failure;
    knurl_u64 started;
    knurl_u64 progress;
    curl_off_t uploaded;
    curl_off_t downloaded;
    knurl_proxy_exchange accounting;
    knurl_u64 accounting_started;
    int accounting_active;
};

void knurl_proxy_schedule(struct proxy_exchange *);
void knurl_proxy_release(void *);
void knurl_proxy_progress(void *, size_t);
void knurl_proxy_count(knurl_u64 *, size_t);
void knurl_proxy_record_failure(struct proxy_exchange *, knurl_status, int,
                                const char *);
void knurl_proxy_upstream_ready(knurl_proxy_http_upstream *,
                                knurl_proxy_http_event, void *);
#endif

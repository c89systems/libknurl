#ifndef ROTMOS_PROXY_RUNTIME_INTERNAL_H
#define ROTMOS_PROXY_RUNTIME_INTERNAL_H
#include "proxy_route_internal.h"
#include "proxy_local.h"
#include "proxy_transport.h"
#include "proxy_upload.h"
#include "proxy_http_upstream.h"
#include "server_internal.h"
struct rotmos_proxy_ws;
struct proxy_exchange;
struct rotmos_proxy_worker {
    rotmos_proxy_transport *transport;
    struct proxy_exchange *exchanges;
    size_t active;
    size_t refs;
    int closing;
};
struct proxy_interim {
    struct proxy_interim *next;
    rotmos_proxy_headers headers;
    rotmos_header views[ROTMOS_PROXY_HEADER_COUNT_LIMIT];
    int status;
};
struct proxy_exchange {
    struct proxy_exchange *next;
    struct rotmos_proxy_worker *worker;
    const rotmos_proxy_route_data *route;
    rotmos_request *request;
    rotmos_loop *loop;
    rotmos_channel *channel;
    rotmos_proxy_transfer *transfer;
    struct rotmos_proxy_ws *ws;
    rotmos_proxy_http_upstream upstream;
    rotmos_proxy_upload_buffer upload;
    rotmos_proxy_local_response local;
    struct curl_slist *request_headers;
    struct proxy_interim *interims;
    struct proxy_interim *interim_tail;
    rotmos_timer *timer;
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
    rotmos_error failure;
    rotmos_u64 started;
    rotmos_u64 progress;
    curl_off_t uploaded;
    curl_off_t downloaded;
};

void rotmos_proxy_schedule(struct proxy_exchange *);
void rotmos_proxy_release(void *);
void rotmos_proxy_progress(void *, size_t);
void rotmos_proxy_record_failure(struct proxy_exchange *, rotmos_status, int,
                                 const char *);
void rotmos_proxy_upstream_ready(rotmos_proxy_http_upstream *,
                                 rotmos_proxy_http_event, void *);
#endif

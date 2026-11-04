#ifndef ROTMOS_PROXY_HTTP_UPSTREAM_H
#define ROTMOS_PROXY_HTTP_UPSTREAM_H

#include "proxy_http.h"

#include <curl/curl.h>
#include <rotmos/rotmos.h>

typedef struct rotmos_proxy_http_upstream rotmos_proxy_http_upstream;
#define ROTMOS_PROXY_HTTP_BODY_HEADROOM 32u
#define ROTMOS_PROXY_HTTP_BODY_TAILROOM 2u
typedef void (*rotmos_proxy_http_upstream_ready_fn)(
    rotmos_proxy_http_upstream *upstream, rotmos_proxy_http_event event,
    void *userdata);

/* A producer may return CURL_READFUNC_PAUSE while its bounded queue is empty.
 * Once input arrives, the owner resumes the easy handle. The producer and
 * userdata remain valid until the transfer is cancelled or completed. */
typedef struct rotmos_proxy_http_upload {
    size_t (*read)(char *buffer, size_t size, size_t count, void *userdata);
    int (*trailers)(struct curl_slist **list, void *userdata);
    void *userdata;
    uint64_t content_length;
    int known_length;
} rotmos_proxy_http_upload;

/* The owner retains easy, url, target, method and request_headers through the
 * transfer. One curl callback chunk is held until the downstream consumes it;
 * curl is paused before another chunk can be copied. Header metadata has its
 * separate 64 KiB cap. The transfer owner cancels before cleanup. */
struct rotmos_proxy_http_upstream {
    rotmos_proxy_http_response response;
    rotmos_proxy_headers outbound_headers;
    CURL *easy;
    unsigned char *body_storage;
    unsigned char *body;
    size_t body_capacity;
    size_t body_length;
    size_t body_offset;
    rotmos_proxy_http_upstream_ready_fn ready;
    void *userdata;
    int paused;
    int failed;
    int head_upload;
    int head_upload_aborted;
};

rotmos_status rotmos_proxy_http_upstream_init(
    rotmos_proxy_http_upstream *upstream, CURL *easy, const char *url,
    const char *request_target, const char *method,
    struct curl_slist *request_headers, size_t buffer_limit,
    long connect_timeout_ms, long total_timeout_ms,
    const rotmos_proxy_http_upload *upload,
    rotmos_proxy_http_upstream_ready_fn ready, void *userdata,
    rotmos_error *error);

/* The pointer and reserved framing space remain valid until the next consume
 * or cleanup. The caller may write only in that framing space. */
unsigned char *
rotmos_proxy_http_upstream_body(rotmos_proxy_http_upstream *upstream,
                                size_t *length);
void rotmos_proxy_http_upstream_consume(rotmos_proxy_http_upstream *upstream,
                                        size_t length);
/* Resume only after the entire held curl chunk has been consumed. */
rotmos_status
rotmos_proxy_http_upstream_resume(rotmos_proxy_http_upstream *upstream,
                                  rotmos_error *error);
rotmos_proxy_header_status
rotmos_proxy_http_upstream_finish(rotmos_proxy_http_upstream *upstream,
                                  CURLcode result, const char **reason);
void rotmos_proxy_http_upstream_cleanup(rotmos_proxy_http_upstream *upstream);

#endif

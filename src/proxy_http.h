#ifndef ROTMOS_PROXY_HTTP_H
#define ROTMOS_PROXY_HTTP_H

#include "proxy_headers.h"

#include <stdint.h>

typedef enum rotmos_proxy_http_event {
    ROTMOS_PROXY_HTTP_MORE = 0,
    ROTMOS_PROXY_HTTP_INFORMATIONAL = 1,
    ROTMOS_PROXY_HTTP_FINAL = 2,
    ROTMOS_PROXY_HTTP_TRAILERS = 3
} rotmos_proxy_http_event;

typedef struct rotmos_proxy_http_response {
    rotmos_proxy_headers headers;
    rotmos_proxy_headers trailers;
    uint64_t content_length;
    uint64_t body_bytes;
    int status;
    int http2;
    int has_content_length;
    int chunked;
    int body_allowed;
    int head_request;
    int phase;
    unsigned informational_count;
} rotmos_proxy_http_response;

void rotmos_proxy_http_response_init(rotmos_proxy_http_response *response,
                                     int head_request);
void rotmos_proxy_http_response_cleanup(rotmos_proxy_http_response *response);

/* Feed one complete libcurl header callback line, including its CRLF. The
 * event reports an informational block, final headers, or final trailers.
 * Metadata is bounded separately from streamed body bytes. */
rotmos_proxy_header_status rotmos_proxy_http_response_header(
    rotmos_proxy_http_response *response, const char *line, size_t length,
    rotmos_proxy_http_event *event, const char **reason);

/* Account for each received body chunk before passing it to a bounded writer.
 * finish checks declared length and a completed trailer block. */
rotmos_proxy_header_status
rotmos_proxy_http_response_body(rotmos_proxy_http_response *response,
                                size_t length, const char **reason);
/* libcurl delivers trailer fields but can omit their terminal empty line.
 * Call only after a successful curl transfer, which has verified wire framing.
 * A reported TRAILERS event can then be emitted before the downstream final
 * chunk. Direct parser users still need an explicit terminal empty line. */
rotmos_proxy_header_status
rotmos_proxy_http_response_curl_complete(rotmos_proxy_http_response *response,
                                         rotmos_proxy_http_event *event,
                                         const char **reason);
rotmos_proxy_header_status
rotmos_proxy_http_response_finish(const rotmos_proxy_http_response *response,
                                  const char **reason);

#endif

#ifndef KNURL_PROXY_HTTP_H
#define KNURL_PROXY_HTTP_H

#include "proxy_headers.h"

#include <stdint.h>

typedef enum knurl_proxy_http_event {
    KNURL_PROXY_HTTP_MORE = 0,
    KNURL_PROXY_HTTP_INFORMATIONAL = 1,
    KNURL_PROXY_HTTP_FINAL = 2,
    KNURL_PROXY_HTTP_TRAILERS = 3
} knurl_proxy_http_event;

typedef struct knurl_proxy_http_response {
    knurl_proxy_headers headers;
    knurl_proxy_headers trailers;
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
} knurl_proxy_http_response;

void knurl_proxy_http_response_init(knurl_proxy_http_response *response,
                                    int head_request);
void knurl_proxy_http_response_cleanup(knurl_proxy_http_response *response);

/* Feed one complete libcurl header callback line, including its CRLF. The
 * event reports an informational block, final headers, or final trailers.
 * Metadata is bounded separately from streamed body bytes. */
knurl_proxy_header_status knurl_proxy_http_response_header(
    knurl_proxy_http_response *response, const char *line, size_t length,
    knurl_proxy_http_event *event, const char **reason);

/* Account for each received body chunk before passing it to a bounded writer.
 * finish checks declared length and a completed trailer block. */
knurl_proxy_header_status
knurl_proxy_http_response_body(knurl_proxy_http_response *response,
                               size_t length, const char **reason);
/* libcurl delivers trailer fields but can omit their terminal empty line.
 * Call only after a successful curl transfer, which has verified wire framing.
 * A reported TRAILERS event can then be emitted before the downstream final
 * chunk. Direct parser users still need an explicit terminal empty line. */
knurl_proxy_header_status
knurl_proxy_http_response_curl_complete(knurl_proxy_http_response *response,
                                        knurl_proxy_http_event *event,
                                        const char **reason);
knurl_proxy_header_status
knurl_proxy_http_response_finish(const knurl_proxy_http_response *response,
                                 const char **reason);

#endif

#ifndef KNURL_PROXY_HEADERS_H
#define KNURL_PROXY_HEADERS_H

#include <stddef.h>
#include <stdint.h>

#define KNURL_PROXY_HEADER_COUNT_LIMIT 100u
#define KNURL_PROXY_HEADER_BLOCK_LIMIT 65536u

typedef enum knurl_proxy_header_status {
    KNURL_PROXY_HEADER_OK = 0,
    KNURL_PROXY_HEADER_INVALID = 1,
    KNURL_PROXY_HEADER_LIMIT = 2,
    KNURL_PROXY_HEADER_NOMEM = 3,
    KNURL_PROXY_HEADER_UNSUPPORTED_UPGRADE = 4
} knurl_proxy_header_status;

typedef struct knurl_proxy_header {
    /* Both strings share one allocation owned by name. */
    char *name;
    char *value;
} knurl_proxy_header;

/* A bounded, owned copy of metadata. Bodies never enter this container. */
typedef struct knurl_proxy_headers {
    knurl_proxy_header fields[KNURL_PROXY_HEADER_COUNT_LIMIT];
    size_t count;
    size_t bytes;
} knurl_proxy_headers;

typedef struct knurl_proxy_request_head {
    const char *host; /* Borrowed from the input headers. */
    uint64_t content_length;
    int has_content_length;
    int chunked;
    int expect_continue;
    int websocket_upgrade;
    int has_trailers;
} knurl_proxy_request_head;

void knurl_proxy_headers_init(knurl_proxy_headers *headers);
void knurl_proxy_headers_cleanup(knurl_proxy_headers *headers);
knurl_proxy_header_status knurl_proxy_headers_add(knurl_proxy_headers *headers,
                                                  const char *name,
                                                  const char *value);
/* Inputs may alias strings owned by headers. Set preserves headers on failure.
 */
void knurl_proxy_headers_remove(knurl_proxy_headers *headers, const char *name);
knurl_proxy_header_status knurl_proxy_headers_set(knurl_proxy_headers *headers,
                                                  const char *name,
                                                  const char *value);

/* Validate request framing and upgrade metadata before any upstream connect.
 * The returned head borrows from headers until headers_cleanup(). */
knurl_proxy_header_status
knurl_proxy_request_head_parse(const knurl_proxy_headers *headers,
                               knurl_proxy_request_head *head,
                               const char **reason);

/* Copy end-to-end fields into an initially empty destination. Host, framing,
 * forwarding, and connection-nominated fields are rebuilt by the transport. */
knurl_proxy_header_status
knurl_proxy_headers_sanitize_request(const knurl_proxy_headers *source,
                                     knurl_proxy_headers *destination);

/* Director edits may change end-to-end fields only. WebSocket handshake
 * negotiation fields remain tied to the validated inbound opening request. */
int knurl_proxy_request_header_editable(const char *name);
int knurl_proxy_response_header_editable(const char *name);

/* Copy upstream end-to-end response fields into an empty destination.
 * The response writer reconstructs framing and Connection fields. */
knurl_proxy_header_status
knurl_proxy_headers_sanitize_response(const knurl_proxy_headers *source,
                                      knurl_proxy_headers *destination);

/* Only a declared, otherwise permitted request trailer may be forwarded. */
int knurl_proxy_request_trailer_declared(const knurl_proxy_headers *headers,
                                         const char *name);

#endif

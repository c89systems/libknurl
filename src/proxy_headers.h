#ifndef ROTMOS_PROXY_HEADERS_H
#define ROTMOS_PROXY_HEADERS_H

#include <stddef.h>
#include <stdint.h>

#define ROTMOS_PROXY_HEADER_COUNT_LIMIT 100u
#define ROTMOS_PROXY_HEADER_BLOCK_LIMIT 65536u

typedef enum rotmos_proxy_header_status {
    ROTMOS_PROXY_HEADER_OK = 0,
    ROTMOS_PROXY_HEADER_INVALID = 1,
    ROTMOS_PROXY_HEADER_LIMIT = 2,
    ROTMOS_PROXY_HEADER_NOMEM = 3,
    ROTMOS_PROXY_HEADER_UNSUPPORTED_UPGRADE = 4
} rotmos_proxy_header_status;

typedef struct rotmos_proxy_header {
    /* Both strings share one allocation owned by name. */
    char *name;
    char *value;
} rotmos_proxy_header;

/* A bounded, owned copy of metadata. Bodies never enter this container. */
typedef struct rotmos_proxy_headers {
    rotmos_proxy_header fields[ROTMOS_PROXY_HEADER_COUNT_LIMIT];
    size_t count;
    size_t bytes;
} rotmos_proxy_headers;

typedef struct rotmos_proxy_request_head {
    const char *host; /* Borrowed from the input headers. */
    uint64_t content_length;
    int has_content_length;
    int chunked;
    int expect_continue;
    int websocket_upgrade;
    int has_trailers;
} rotmos_proxy_request_head;

void rotmos_proxy_headers_init(rotmos_proxy_headers *headers);
void rotmos_proxy_headers_cleanup(rotmos_proxy_headers *headers);
rotmos_proxy_header_status
rotmos_proxy_headers_add(rotmos_proxy_headers *headers, const char *name,
                         const char *value);
/* Inputs may alias strings owned by headers. Set preserves headers on failure.
 */
void rotmos_proxy_headers_remove(rotmos_proxy_headers *headers,
                                 const char *name);
rotmos_proxy_header_status
rotmos_proxy_headers_set(rotmos_proxy_headers *headers, const char *name,
                         const char *value);

/* Validate request framing and upgrade metadata before any upstream connect.
 * The returned head borrows from headers until headers_cleanup(). */
rotmos_proxy_header_status
rotmos_proxy_request_head_parse(const rotmos_proxy_headers *headers,
                                rotmos_proxy_request_head *head,
                                const char **reason);

/* Copy end-to-end fields into an initially empty destination. Host, framing,
 * forwarding, and connection-nominated fields are rebuilt by the transport. */
rotmos_proxy_header_status
rotmos_proxy_headers_sanitize_request(const rotmos_proxy_headers *source,
                                      rotmos_proxy_headers *destination);

/* Director edits may change end-to-end fields only. WebSocket handshake
 * negotiation fields remain tied to the validated inbound opening request. */
int rotmos_proxy_request_header_editable(const char *name);
int rotmos_proxy_response_header_editable(const char *name);

/* Copy upstream end-to-end response fields into an empty destination.
 * The response writer reconstructs framing and Connection fields. */
rotmos_proxy_header_status
rotmos_proxy_headers_sanitize_response(const rotmos_proxy_headers *source,
                                       rotmos_proxy_headers *destination);

/* Only a declared, otherwise permitted request trailer may be forwarded. */
int rotmos_proxy_request_trailer_declared(const rotmos_proxy_headers *headers,
                                          const char *name);

#endif
